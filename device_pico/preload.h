// Preload pipeline — runs once after FatFs mount, before the engine
// starts.  Walks the game table, finds any /scumm/<subdir>/ that has
// the required raw files but no `.thumbyscummby` marker, and:
//
//   1. XOR-decrypts each encrypted file in place (xor_byte from
//      GameDescriptor::files).
//   2. Applies copy-protection patches (e.g. MI2 Mix-N-Mojo skip).
//   3. Drops a `.thumbyscummby` marker on success.
//
// Subsequent boots see the marker and skip the work.  The whole pass
// is idempotent — partial completion on power loss leaves the marker
// absent, so the next boot retries.
//
// THUMBYONE_SLOT_PLAN phase C3 step A.  Step B (.img mount +
// streaming extract) will plug in at the front of this same pipeline
// and feed files into /scumm/<subdir>/ before decrypt runs.

#pragma once

namespace tsb {
namespace preload {

// Run any pending preload work for installed games.  Caller should
// re-scan /scumm/* afterwards (a fresh game may have appeared).
// Returns true if anything was decoded; false if nothing to do.
bool maybe_run();

}  // namespace preload
}  // namespace tsb
