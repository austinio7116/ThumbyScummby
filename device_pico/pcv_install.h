// LucasArts PCV/LFG! install-disk extractor — Stage A.
//
// One entry point: scan /scumm/ for .imgs containing PCV archives,
// identify which game each chain belongs to, decompress + XOR + write
// the game data into /scumm/<gd.subdir>/.  Each .img is f_unlink'd
// only after all its PCV bytes have been consumed.
//
// Stage B (later) will add incremental outer-cluster freeing during
// PCV reading so MI2/Indy4 can fit inside the 8 MB shared FAT during
// install.  This file is structured so that change is local to the
// "fetch next byte" path.

#pragma once

namespace tsb {

struct GameDescriptor;

namespace preload {

// Scan /scumm/ (root + one level of subdir) for any .img files.
// For every PCV chain we recognise (by descriptor table + filename
// pattern + content), run the full install pipeline: decompress the
// DCL streams of every FILE chunk, XOR per descriptor's xor_byte,
// write to /scumm/<gd.subdir>/<filename>.  Returns true if at least
// one game was installed (caller should re-scan).
bool install_pcv_imgs();

}  // namespace preload
}  // namespace tsb
