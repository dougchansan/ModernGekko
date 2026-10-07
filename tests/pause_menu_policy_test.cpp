// SPDX-License-Identifier: GPL-3.0-or-later
#include "pause_menu_policy.hpp"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      return __LINE__;                                                         \
  } while (false)

using namespace moderngekko::pause_menu;

int main() {
  Policy menu;
  Input input;
  input.accept = true;
  CHECK(menu.update(input, 0) == Action::None);
  CHECK(!menu.is_open());
  input = {};
  input.escape = true;
  menu.update(input, 1);
  CHECK(menu.page() == Page::Home && menu.focus() == 0);
  menu.update(input, 2);
  CHECK(menu.is_open());
  input = {};
  menu.update(input, 2.1);
  input.up = true;
  menu.update(input, 3);
  CHECK(menu.focus() == 4);
  menu.update(input, 3.2);
  CHECK(menu.focus() == 4);
  menu.update(input, 3.36);
  CHECK(menu.focus() == 3);
  menu.update(input, 3.40);
  CHECK(menu.focus() == 3);
  menu.update(input, 3.47);
  CHECK(menu.focus() == 2);
  input.down = true;
  menu.update(input, 4);
  CHECK(menu.focus() == 2);
  input = {};
  menu.update(input, 4.1);
  menu.activate(4);
  CHECK(menu.page() == Page::QuitConfirm && menu.focus() == 0);
  CHECK(menu.activate(0) == Action::None && menu.page() == Page::Home);
  menu.activate(1);
  CHECK(menu.page() == Page::Settings);
  CHECK(menu.activate(0) == Action::AdjustSetting);
  menu.update({}, 4.2, 6);
  CHECK(menu.activate(4) == Action::AdjustSetting);
  menu.activate(5);
  CHECK(menu.page() == Page::Home);
  menu.activate(2);
  CHECK(menu.page() == Page::Controls);
  menu.back();
  CHECK(menu.activate(3) == Action::SaveCheckpoint);
  CHECK(menu.is_open());
  input.escape = true;
  CHECK(menu.update(input, 5) == Action::Resume);
  CHECK(!menu.is_open() && menu.blocks_game_input());
  input = {};
  input.any_game_button = true;
  menu.update(input, 5.1);
  CHECK(menu.blocks_game_input());
  input = {};
  menu.update(input, 5.2);
  CHECK(!menu.blocks_game_input());
  input.start = true;
  menu.update(input, 6);
  CHECK(!menu.is_open());
  input.select = true;
  menu.update(input, 6.1);
  CHECK(menu.is_open());
  menu.update(input, 7);
  CHECK(menu.is_open());
  input.select = false;
  menu.update(input, 7.1);
  input.select = true;
  menu.update(input, 7.2);
  CHECK(menu.is_open());
  input = {};
  menu.update(input, 7.3);
  input.start = input.select = true;
  CHECK(menu.update(input, 8) == Action::Resume);
  input = {};
  menu.update(input, 8.1);
  input.game_running = false;
  input.escape = true;
  CHECK(menu.update(input, 9) == Action::None && !menu.is_open());
  input = {};
  menu.update(input, 9.1);
  menu.open();
  menu.activate(4);
  CHECK(menu.activate(1) == Action::Quit && !menu.is_open());
  menu.open();
  input = {};
  input.accept = true;
  CHECK(menu.update(input, 10) == Action::Resume);
  CHECK(menu.update(input, 11) == Action::None);
  menu.open();
  input = {};
  input.down = true;
  menu.update(input, 12);
  CHECK(menu.focus() == 1);
  menu.reset_edges(input); // Key-up/down may both arrive between UI polls.
  menu.update(input, 12.01);
  CHECK(menu.focus() == 2);
  menu.open();
  menu.activate(1);
  menu.set_rows(17);
  CHECK(menu.activate(15) == Action::AdjustSetting);
  CHECK(menu.activate(16) == Action::None);
  CHECK(menu.page() == Page::Home);
  return 0;
}
