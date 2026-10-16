#pragma once

// The screens of the emulated Game Boy Advances (Colosseum's link battles),
// drawn in a corner of the game window.
namespace moderngekko::gba_screen
{
// Called inside the ImGui frame on the video thread.
void Draw();

// Turns the GBA on a port off and on, now: its BIOS boots again, which is when
// it listens for a program sent over the link cable. False with no GBA there.
bool ResetGba(int port);
}  // namespace moderngekko::gba_screen
