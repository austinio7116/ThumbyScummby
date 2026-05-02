#include "chunk.h"

namespace tsb {

bool chunk_read(Span s, Chunk *out) {
    if (s.size < 8) return false;
    uint32_t tag  = read_be32(s.data);
    uint32_t size = read_be32(s.data + 4);
    if (size < 8 || size > s.size) return false;
    out->tag      = tag;
    out->full     = Span{s.data, size};
    out->payload  = Span{s.data + 8, size - 8};
    return true;
}

bool chunk_find(Span parent_payload, uint32_t tag, Chunk *out) {
    size_t cursor = 0;
    Chunk  c{};
    while (chunk_next(parent_payload, &cursor, &c)) {
        if (c.tag == tag) {
            *out = c;
            return true;
        }
    }
    return false;
}

bool chunk_next(Span parent_payload, size_t *cursor, Chunk *out) {
    if (*cursor >= parent_payload.size) return false;
    Span rest = parent_payload.sub(*cursor);
    Chunk c{};
    if (!chunk_read(rest, &c)) return false;
    *out = c;
    // Advance cursor by the chunk's full inclusive size.
    *cursor += c.full.size;
    return true;
}

}  // namespace tsb
