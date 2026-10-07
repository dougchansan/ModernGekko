// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace moderngekko {
struct RuntimeConfig;
class ModManager;
namespace pause_menu {
void Initialize(const RuntimeConfig &config, ModManager *mods);
void Shutdown();
void Draw();
} // namespace pause_menu
} // namespace moderngekko

void Host_PauseMenuTick();
void Host_PauseMenuKey(int virtual_key, bool down);
void Host_PauseMenuFocusLost();
bool Host_IsPauseMenuOpen();
void Host_RequestPauseMenuToggle();
bool Host_IsPauseMenuInputBlocked();
void Host_TogglePauseMenuFullscreen();
