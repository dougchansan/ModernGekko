#pragma once

// Pokemon Colosseum's text fonts, rebuilt from the player's own game files.
//
// The game keeps its fonts as 4bpp glyph lists inside sys/main.dol (the same
// data as files/common.fsys :: common_rel, linked in uncompressed). This turns
// them into the atlas + metrics pair the pause menu and the launcher load
// (<name>_atlas.png, <name>_glyphs.json), so nothing from the game ships with
// the frontend. Output matches tools/colosseum_font/extract_font.py exactly.

#include <filesystem>
#include <string>

namespace moderngekko::frontend
{
// Writes the five fonts (colo_font0_22px ... colo_font4_13px_condensed) into
// out_dir unless they are already there. game_root is the extracted game
// (the directory holding sys/main.dol). Returns true when the fonts exist
// afterwards.
bool EnsureColosseumFonts(const std::filesystem::path& game_root,
                          const std::filesystem::path& out_dir, std::string* error);
}  // namespace moderngekko::frontend
