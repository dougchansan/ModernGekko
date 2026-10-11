// Quick boot: with Skip intro on, the first launch boots normally and saves a
// state at the title screen (<User>/StartupCache/title.sav); later launches
// load that state instead of booting, so the title appears almost at once.
//
// The snapshot is only used while everything that went into it is unchanged:
// the runner, the game module, the mods, the game's patch ini and the memory
// card contents (a state carries the card, so a stale one must never load over
// newer saves). Any change discards it and the next launch rebuilds it.
// MODERNGEKKO_QUICK_BOOT=0 turns it off.
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace moderngekko::quick_boot {

// Decides for this launch. Call before boot, only when booting from the title
// (no requested save state). Returns the snapshot to boot from, if valid.
std::optional<std::filesystem::path> Prepare(const std::filesystem::path &user_directory,
                                             const std::vector<std::filesystem::path> &inputs,
                                             bool skip_intro);

// This launch booted from the snapshot (the menu shows no boot cover).
bool Loaded();

// The game reached the title after a normal covered boot: save the snapshot
// if this launch should. Safe from the pause menu tick.
void TitleReached();

// Finishes a snapshot save (marks it valid once written). Pause menu tick.
void Tick();

}  // namespace moderngekko::quick_boot
