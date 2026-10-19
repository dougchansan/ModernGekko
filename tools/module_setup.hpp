// SPDX-License-Identifier: GPL-2.0-or-later
//
// Builds the native game module on the player's machine from their own extracted disc, so a
// release never ships game-derived code: downloads a pinned, checksum-verified compiler (Zig)
// once, then runs the bundled moderngekko-port over the extracted game.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace moderngekko::frontend
{
namespace fs = std::filesystem;

// The module the runner loads for this game, under the user directory (both the normal and
// the 60 FPS name: the two are the same build).
fs::path InstalledModule(const fs::path& user_directory, const std::string& disc_id,
                         bool sixty_fps = false);

// True if the runner will find a module for this game (beside it or in the user directory).
bool HasGameModule(const fs::path& release_directory, const fs::path& user_directory,
                   const std::string& disc_id);

// True if this release carries what a module build needs (Tools/ beside the launcher).
bool CanBuildGameModule(const fs::path& release_directory);

// Reports progress: what is happening, and done/total for a bar (total 0 = no bar yet).
using ModuleProgress = std::function<void(const std::string& status, unsigned done, unsigned total)>;

// Downloads the compiler if needed and builds and installs the module for the game extracted
// at game_root. Blocks for several minutes; run it off the UI thread.
bool BuildGameModule(const fs::path& game_root, const std::string& disc_id,
                     const fs::path& release_directory, const fs::path& user_directory,
                     const ModuleProgress& progress, std::string* error);
}  // namespace moderngekko::frontend
