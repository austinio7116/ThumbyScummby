// PKWARE DCL "explode" decoder — see dcl.h.
//
// Direct port of ScummVM common/compression/dcl.cpp.  Logic and
// Huffman tables are unchanged; the only adaptation is the
// streaming I/O via callbacks (versus ScummVM's SeekableReadStream
// + WriteStream) and dictionary-passed-from-caller (we don't
// allocate the 4 KB dict ourselves — caller owns it so the same
// instance can be reused across multiple FILE chunks in one PCV
// install run).
//
// ASCII mode is not ported — the LucasArts installers all use
// binary mode.  Detection still works (we error cleanly if asked).

#include "dcl.h"
#include "types.h"

namespace tsb {

namespace {

constexpr uint32_t HUFFMAN_LEAF = 0x40000000u;

#define BN(left, right) (((uint32_t)(left) << 12) | (uint32_t)(right))
#define LN(value)       ((uint32_t)(value) | HUFFMAN_LEAF)

// Huffman trees — bit-for-bit copy of ScummVM's tables.
static const uint32_t kLengthTree[] = {
    BN(1,2),
    BN(3,4),       BN(5,6),
    BN(7,8),       BN(9,10),     BN(11,12),     LN(1),
    BN(13,14),     BN(15,16),    BN(17,18),     LN(3),    LN(2),    LN(0),
    BN(19,20),     BN(21,22),    BN(23,24),     LN(6),    LN(5),    LN(4),
    BN(25,26),     BN(27,28),    LN(10),        LN(9),    LN(8),    LN(7),
    BN(29,30),     LN(13),       LN(12),        LN(11),
    LN(15),        LN(14),
};

static const uint32_t kDistanceTree[] = {
    BN(1,2),
    BN(3,4),       BN(5,6),
    BN(7,8),       BN(9,10),     BN(11,12),     LN(0),
    BN(13,14),     BN(15,16),    BN(17,18),     BN(19,20),
    BN(21,22),     BN(23,24),
    BN(25,26),     BN(27,28),    BN(29,30),     BN(31,32),
    BN(33,34),     BN(35,36),    BN(37,38),     BN(39,40),
    BN(41,42),     BN(43,44),    LN(2),         LN(1),
    BN(45,46),     BN(47,48),    BN(49,50),     BN(51,52),
    BN(53,54),     BN(55,56),    BN(57,58),     BN(59,60),
    BN(61,62),     BN(63,64),    BN(65,66),     BN(67,68),
    BN(69,70),     BN(71,72),    BN(73,74),     BN(75,76),
    LN(6),         LN(5),        LN(4),         LN(3),
    BN(77,78),     BN(79,80),    BN(81,82),     BN(83,84),
    BN(85,86),     BN(87,88),    BN(89,90),     BN(91,92),
    BN(93,94),     BN(95,96),    BN(97,98),     BN(99,100),
    BN(101,102),   BN(103,104),  BN(105,106),   BN(107,108),
    BN(109,110),   LN(21),       LN(20),        LN(19),
    LN(18),        LN(17),       LN(16),        LN(15),
    LN(14),        LN(13),       LN(12),        LN(11),
    LN(10),        LN(9),        LN(8),         LN(7),
    BN(111,112),   BN(113,114),  BN(115,116),   BN(117,118),
    BN(119,120),   BN(121,122),  BN(123,124),   BN(125,126),
    LN(47),        LN(46),       LN(45),        LN(44),
    LN(43),        LN(42),       LN(41),        LN(40),
    LN(39),        LN(38),       LN(37),        LN(36),
    LN(35),        LN(34),       LN(33),        LN(32),
    LN(31),        LN(30),       LN(29),        LN(28),
    LN(27),        LN(26),       LN(25),        LN(24),
    LN(23),        LN(22),       LN(63),        LN(62),
    LN(61),        LN(60),       LN(59),        LN(58),
    LN(57),        LN(56),       LN(55),        LN(54),
    LN(53),        LN(52),       LN(51),        LN(50),
    LN(49),        LN(48),
};

#undef BN
#undef LN

struct State {
    DclDecoder::ReadByteFn  read_byte;
    void                   *read_user;
    DclDecoder::WriteByteFn write_byte;
    void                   *write_user;

    uint32_t bits;
    int      nbits;
    bool     read_eof;
    bool     write_err;
};

static void fetch_bits(State &s) {
    while (s.nbits <= 24 && !s.read_eof) {
        int b = s.read_byte(s.read_user);
        if (b < 0) { s.read_eof = true; break; }
        s.bits |= ((uint32_t)b) << s.nbits;
        s.nbits += 8;
    }
}

static uint32_t get_bits(State &s, int n) {
    if (s.nbits < n) fetch_bits(s);
    uint32_t v = s.bits & ((1u << n) - 1u);
    s.bits >>= n;
    s.nbits -= n;
    return v;
}

static int huffman_lookup(State &s, const uint32_t *tree) {
    int node = 0;
    for (;;) {
        uint32_t v = tree[node];
        if (v & HUFFMAN_LEAF) return (int)(v & ~HUFFMAN_LEAF);
        uint32_t bit = get_bits(s, 1);
        if (bit) node = (int)(v & 0xFFF);
        else     node = (int)((v >> 12) & 0xFFF);
    }
}

static bool put_byte(State &s, uint8_t b) {
    if (!s.write_byte(b, s.write_user)) {
        s.write_err = true;
        return false;
    }
    return true;
}

}  // anonymous

bool DclDecoder::decode(ReadByteFn read_byte, void *read_user,
                       WriteByteFn write_byte, void *write_user,
                       uint8_t *dictionary,
                       uint32_t expected_size) {
    State s = {
        read_byte, read_user,
        write_byte, write_user,
        0, 0, false, false
    };

    uint8_t mode         = (uint8_t)get_bits(s, 8);
    uint8_t dict_type    = (uint8_t)get_bits(s, 8);
    if (s.read_eof) return false;
    if (mode != 0) return false;          // binary only; ASCII unused by PCV
    uint16_t dict_size = 0;
    if      (dict_type == 4) dict_size = 1024;
    else if (dict_type == 5) dict_size = 2048;
    else if (dict_type == 6) dict_size = 4096;
    else return false;
    uint16_t dict_mask = dict_size - 1;

    // The first 1..4 KB of dictionary[] is our sliding window.  Zero
    // it so any token referencing-before-first-write reads predictably.
    for (uint32_t i = 0; i < dict_size; ++i) dictionary[i] = 0;

    uint16_t dict_pos    = 0;
    uint32_t written     = 0;
    const bool fixed_size = (expected_size > 0);

    while (!fixed_size || written < expected_size) {
        if (s.write_err) return false;

        if (get_bits(s, 1)) {
            // length/distance pair
            int v = huffman_lookup(s, kLengthTree);
            uint16_t token_length;
            if (v < 8) {
                token_length = (uint16_t)(v + 2);
            } else {
                token_length = (uint16_t)(8u + (1u << (v - 7)) + get_bits(s, v - 7));
            }
            if (token_length == 519) break;   // end-of-stream

            v = huffman_lookup(s, kDistanceTree);
            uint16_t token_offset;
            if (token_length == 2) {
                token_offset = (uint16_t)((v << 2) | get_bits(s, 2));
            } else {
                token_offset = (uint16_t)((v << dict_type) | get_bits(s, dict_type));
            }
            token_offset++;

            if (fixed_size && token_length + written > expected_size) {
                // declared size lie — let decoded length take precedence
                token_length = (uint16_t)(expected_size - written);
                if (token_length == 0) break;
            }

            uint16_t base_idx = (uint16_t)((dict_pos - token_offset) & dict_mask);
            uint16_t di       = base_idx;
            uint16_t dni      = dict_pos;
            while (token_length) {
                uint8_t b = dictionary[di];
                if (!put_byte(s, b)) return false;
                written++;
                dictionary[dni] = b;
                dni = (uint16_t)((dni + 1) & dict_mask);
                di  = (uint16_t)((di + 1) & dict_mask);
                if (di == dict_pos)   di  = base_idx;
                if (dni == dict_size) dni = 0;
                token_length--;
            }
            dict_pos = dni;
        } else {
            // literal
            uint8_t b = (uint8_t)get_bits(s, 8);
            if (s.read_eof) break;
            if (!put_byte(s, b)) return false;
            written++;
            dictionary[dict_pos] = b;
            dict_pos = (uint16_t)((dict_pos + 1) & dict_mask);
        }
    }

    if (fixed_size) return written == expected_size;
    return true;
}

}  // namespace tsb
