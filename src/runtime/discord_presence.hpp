// Discord Rich Presence for Pokemon Colosseum: shows the place the player is
// in, whether a battle is on and the Shadow Pokemon snag / purify counts on the
// player's Discord profile. Talks to the local Discord client over its IPC pipe
// (no SDK); does nothing when Discord isn't running.
//
// config.ini [Discord]: Enabled (default true), ApplicationId (the Discord
// application whose name and art the profile shows; empty disables it),
// LargeImage (cover art key, default colosseum), AreaImages (default true: the
// area's art key, such as pyrite_town, as the large image), BattleImage (small
// badge key shown during battles, default battle; empty for none).
#pragma once

#include <filesystem>

namespace moderngekko::discord {

// Starts the presence thread for a booted GC6E01. Safe to call once per boot.
void Start(const std::filesystem::path& config_ini);
// Clears the presence and joins the thread. Safe to call when not started.
void Stop();

}  // namespace moderngekko::discord
