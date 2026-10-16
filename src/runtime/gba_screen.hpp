#pragma once

#include <array>
#include <functional>

// The screens of the emulated Game Boy Advances (Colosseum's link battles):
// see-through panels over the game window that can be dragged, resized, or
// moved out into their own windows, with the player's number on each.
namespace moderngekko::gba_screen
{
struct View
{
  int opacity = 80;  // percent, 10..100
  bool detached = false;
  // Position and width as fractions of the game window; x < 0 means the
  // default place for that player.
  float x = -1.0f;
  float y = -1.0f;
  float width = 0.0f;
};

struct Settings
{
  std::array<View, 4> views;
  bool show_numbers = true;
};

// A copy of the settings, and a replacement for them. The callback (set once,
// by the players UI) is told when the player changes them from a GBA screen,
// so they can be saved.
Settings GetSettings();
void SetSettings(const Settings& settings);
void SetChangedCallback(std::function<void()> callback);

// Whether that port's GBA is running.
bool IsActive(int port);

// Called inside the ImGui frame on the video thread.
void Draw();

// Called often on the window (main) thread: separate GBA windows, and the
// L+R+Start+Select reset combo.
void Tick();

// Turns the GBA on a port off and on: its BIOS boots again, which is when it
// listens for a program sent over the link cable. False with no GBA there.
// Over netplay only the GBA's own player can do this (with the combo).
bool ResetGba(int port);
}  // namespace moderngekko::gba_screen
