#pragma once

#include <cstdint>
#include <vector>

#include "gba_screen.hpp"

// Between the GBA screens and their separate windows (gba_window_win32.cpp),
// which is kept apart so <windows.h> never meets mGBA's headers.
namespace moderngekko::gba_screen::detail
{
// The latest frame of a port's GBA as BGRA, when it is newer than *frame.
// False when that GBA is not running.
bool CopyFrame(int port, std::vector<std::uint32_t>* bgra, std::uint32_t* width,
               std::uint32_t* height, std::uint64_t* frame);

// Change one GBA's view (or the player numbers) and save.
void SetView(int port, const View& view, bool place_again);
void SetShowNumbers(bool show);

// Creates, updates and closes the separate windows; window thread.
void TickWindows();
}  // namespace moderngekko::gba_screen::detail
