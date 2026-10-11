// Replacement for the title screen's attract movie (files/movie/autodemo01.thp):
// while the game plays it, a remade high-resolution video from
// <User>/Load/Attract/autodemo01.mp4 is drawn over it, kept in step with the
// game's own movie frame (the game still plays, times and cleans up its movie,
// and plays the music). Without the file the original movie shows.
// Research: C:/dev/colo-perf/research/attract/playback/REPORT.md.
#pragma once

namespace moderngekko::attract {

// Draws the replacement if the attract movie is playing. Call from the ImGui
// overlay callback (render thread), before the menus.
void Draw();

}  // namespace moderngekko::attract
