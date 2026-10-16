#pragma once

#include "moderngekko/runtime.hpp"

// The window menu's Players and Audio panels: what is plugged into each of
// the four controller sockets (a GameCube controller and which device drives
// it, or an emulated Game Boy Advance), the GBA screens, and a volume mixer.
// Settings are kept in config.ini.
namespace moderngekko::players_ui
{
// Before boot: reads the saved settings and plugs in the saved sockets
// (sockets the command line or netplay set up are left as they are).
void Initialize(const RuntimeConfig& config);

// Inside the ImGui frame on the video thread: the panels and GBA screens.
void Draw();

// Often, on the window thread.
void Tick();
}  // namespace moderngekko::players_ui

// For the window menus (DolphinNoGUI).
void Host_OpenPlayersPanel();
void Host_OpenAudioMixer();
bool Host_IsGbaPlayerNumbersShown();
void Host_ToggleGbaPlayerNumbers();
// Whether a panel or GBA screen should get the mouse rather than the game.
bool Host_WantsMouse();
