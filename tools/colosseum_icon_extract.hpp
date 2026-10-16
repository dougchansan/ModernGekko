#pragma once

// Pokemon Colosseum's Pokemon face portraits, rebuilt from the player's own
// game files (files/poke_face.fsys, mapped through common.fsys::common_rel)
// into the atlas the pause menu and the launcher draw from:
// <out>/icons_atlas.png and <out>/icons.json. Output matches
// tools/colosseum_icons/extract_icons.py (GC6E01 USA only).

#include <filesystem>
#include <string>

namespace moderngekko::frontend
{
inline constexpr int kColosseumIconCell = 42;
inline constexpr int kColosseumIconColumns = 16;

// Writes the atlas unless it is already there. Returns true when it exists
// afterwards.
bool EnsureColosseumIcons(const std::filesystem::path& game_root,
                          const std::filesystem::path& out_dir, std::string* error);

// Where a species' face sits in the atlas: its cell index, or -1. Species are
// the game's internal ids (1-251 as the national dex, then 277-411).
int ColosseumIconIndex(int species);
}  // namespace moderngekko::frontend
