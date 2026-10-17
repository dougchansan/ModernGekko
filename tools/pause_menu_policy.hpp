// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace moderngekko::pause_menu {

enum class Page { Closed, Home, Settings, Controls, Online, Shadow, Developer, QuitConfirm, RestartConfirm };
enum class Action {
  None,
  Resume,
  Settings,
  Controls,
  Online,
  Shadow,
  Developer,
  PhotoMode,
  SaveCheckpoint,
  Restart,
  Quit,
  AdjustSetting,
  ToggleFullscreen,
  ToggleVsync,
  ToggleFps
};

struct Input {
  bool escape = false;
  bool up = false;
  bool down = false;
  bool accept = false;
  bool back = false;
  bool start = false;
  bool select = false;
  // Include every held game button, not only the menu bindings.
  bool any_game_button = false;
  bool game_running = true;
};

class Policy {
public:
  Page page() const { return page_; }
  std::size_t focus() const { return focus_; }
  bool is_open() const { return page_ != Page::Closed; }
  bool blocks_game_input() const { return is_open() || release_gate_; }
  // Pages whose row count the runtime supplies.
  bool custom_page() const {
    return page_ == Page::Settings || page_ == Page::Controls || page_ == Page::Online ||
           page_ == Page::Shadow || page_ == Page::Developer;
  }
  void set_rows(std::size_t rows) {
    if (custom_page())
      custom_rows_ = rows;
  }
  void reset_edges(const Input &pressed) {
    if (pressed.escape)
      previous_.escape = false;
    if (pressed.accept)
      previous_.accept = false;
    if (pressed.back)
      previous_.back = false;
    if (pressed.up || pressed.down)
      direction_ = 0;
  }

  std::size_t row_count() const {
    if (custom_rows_ && custom_page())
      return custom_rows_;
    switch (page_) {
    case Page::Home:
      return 10;
    case Page::Settings:
      return 4;
    case Page::Controls:
    case Page::Online:
    case Page::Shadow:
    case Page::Developer:
      return 1;
    case Page::QuitConfirm:
    case Page::RestartConfirm:
      return 2;
    default:
      return 0;
    }
  }

  void open() {
    page_ = Page::Home;
    focus_ = 0;
    custom_rows_ = 0;
    release_gate_ = true;
  }
  void close() {
    page_ = Page::Closed;
    focus_ = 0;
    release_gate_ = true;
  }

  Action back() {
    if (!is_open())
      return Action::None;
    if (page_ == Page::Home) {
      close();
      return Action::Resume;
    }
    page_ = Page::Home;
    focus_ = 0;
    custom_rows_ = 0;
    return Action::None;
  }

  Action activate(std::size_t row) {
    if (row >= row_count())
      return Action::None;
    focus_ = row;
    switch (page_) {
    case Page::Home:
      switch (row) {
      case 0:
        close();
        return Action::Resume;
      case 1:
        page_ = Page::Settings;
        focus_ = 0;
        return Action::Settings;
      case 2:
        page_ = Page::Controls;
        focus_ = 0;
        return Action::Controls;
      case 3:
        page_ = Page::Online;
        focus_ = 0;
        return Action::Online;
      case 4:
        page_ = Page::Shadow;
        focus_ = 0;
        return Action::Shadow;
      case 5:
        close();
        return Action::PhotoMode;
      case 6:
        return Action::SaveCheckpoint;
      case 7:
        page_ = Page::Developer;
        focus_ = 0;
        return Action::Developer;
      case 8:
        page_ = Page::RestartConfirm;
        focus_ = 0;
        return Action::None;
      case 9:
        page_ = Page::QuitConfirm;
        focus_ = 0;
        return Action::None;
      }
      break;
    case Page::Settings:
      return row + 1 == row_count() ? back() : Action::AdjustSetting;
    case Page::Controls:
    case Page::Online:
      return row + 1 == row_count() ? back() : Action::AdjustSetting;
    case Page::Shadow:
      return row + 1 == row_count() ? back() : Action::None;
    case Page::Developer:
      return row + 1 == row_count() ? back() : Action::AdjustSetting;
    case Page::QuitConfirm:
      if (row == 0)
        return back();
      close();
      return Action::Quit;
    case Page::RestartConfirm:
      if (row == 0)
        return back();
      close();
      return Action::Restart;
    default:
      break;
    }
    return Action::None;
  }

  // now_seconds is an absolute monotonic timestamp. Held navigation repeats
  // after 350 ms, then every 100 ms. Accept/back/toggle remain edge triggered.
  Action update(const Input &input, double now_seconds, std::size_t rows = 0) {
    const bool held = input.escape || input.up || input.down || input.accept ||
                      input.back || input.start || input.select ||
                      input.any_game_button;
    if (!held)
      release_gate_ = false;
    const bool escape_edge = input.escape && !previous_.escape;
    const bool accept_edge = input.accept && !previous_.accept;
    const bool back_edge = input.back && !previous_.back;
    const bool chord = input.start && input.select && chord_armed_;
    if (!input.start && !input.select)
      chord_armed_ = true;
    if (chord)
      chord_armed_ = false;
    previous_ = input;
    if (!input.game_running) {
      if (is_open())
        close();
      direction_ = 0;
      return Action::None;
    }
    if (escape_edge || chord) {
      direction_ = 0;
      if (is_open()) {
        close();
        return Action::Resume;
      }
      open();
      return Action::None;
    }
    if (!is_open()) {
      direction_ = 0;
      return Action::None;
    }
    if (back_edge)
      return back();
    if (rows && custom_page())
      custom_rows_ = rows;
    const auto count = rows ? rows : row_count();
    if (count == 0)
      return Action::None;
    focus_ %= count;
    const int direction = input.up == input.down ? 0 : input.up ? -1 : 1;
    if (direction == 0)
      direction_ = 0;
    else if (direction != direction_ || now_seconds >= next_repeat_) {
      focus_ =
          direction < 0 ? (focus_ + count - 1) % count : (focus_ + 1) % count;
      next_repeat_ = now_seconds + (direction != direction_ ? 0.35 : 0.10);
      direction_ = direction;
    }
    if (accept_edge)
      return activate(focus_);
    return Action::None;
  }

private:
  Page page_ = Page::Closed;
  std::size_t focus_ = 0;
  std::size_t custom_rows_ = 0;
  Input previous_{};
  bool release_gate_ = false;
  bool chord_armed_ = true;
  int direction_ = 0;
  double next_repeat_ = 0;
};

} // namespace moderngekko::pause_menu
