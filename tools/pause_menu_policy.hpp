// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace moderngekko::pause_menu {

enum class Page { Closed, Home, Settings, Controls, QuitConfirm };
enum class Action {
  None,
  Resume,
  Settings,
  Controls,
  SaveCheckpoint,
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
  void set_rows(std::size_t rows) {
    if (page_ == Page::Settings || page_ == Page::Controls)
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
    if (custom_rows_ && (page_ == Page::Settings || page_ == Page::Controls))
      return custom_rows_;
    switch (page_) {
    case Page::Home:
      return 5;
    case Page::Settings:
      return 4;
    case Page::Controls:
      return 1;
    case Page::QuitConfirm:
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
        return Action::SaveCheckpoint;
      case 4:
        page_ = Page::QuitConfirm;
        focus_ = 0;
        return Action::None;
      }
      break;
    case Page::Settings:
      return row + 1 == row_count() ? back() : Action::AdjustSetting;
    case Page::Controls:
      return row + 1 == row_count() ? back() : Action::AdjustSetting;
    case Page::QuitConfirm:
      if (row == 0)
        return back();
      close();
      return Action::Quit;
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
    if (rows && (page_ == Page::Settings || page_ == Page::Controls))
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
