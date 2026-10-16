#pragma once

// A controller keyboard for naming saves and memory cards, shared by the
// launcher and the in-game prompt (each draws it its own way). Four rows of
// characters and a row of actions:
//
//   A B C D E F G H I J
//   K L M N O P Q R S T
//   U V W X Y Z - _ 0 1
//   2 3 4 5 6 7 8 9
//   [ SPACE ][ DELETE ][ DONE ]
//
// Letters type as words in title case ("Snag Test"), so names read well
// without a shift key.

#include <algorithm>
#include <cctype>
#include <string>

namespace moderngekko::onscreen_keyboard
{
inline constexpr int kColumns = 10;
inline constexpr int kRows = 5;
inline constexpr const char* kKeys[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_01", "23456789"};

enum class Key
{
  Character,
  Space,
  Delete,
  Done,
};

struct Cursor
{
  int row = 0, column = 0;
};

inline int RowLength(int row)
{
  return row < 4 ? static_cast<int>(std::char_traits<char>::length(kKeys[row])) : kColumns;
}

// The action-row key under a column: SPACE spans 0-3, DELETE 4-6, DONE 7-9.
inline Key ActionAt(int column)
{
  return column < 4 ? Key::Space : column < 7 ? Key::Delete : Key::Done;
}

inline Key KeyAt(const Cursor& c)
{
  return c.row < 4 ? Key::Character : ActionAt(c.column);
}

inline char CharacterAt(const Cursor& c)
{
  return c.row < 4 ? kKeys[c.row][std::clamp(c.column, 0, RowLength(c.row) - 1)] : ' ';
}

inline void Move(Cursor* c, int dx, int dy)
{
  if (dy)
    c->row = (c->row + dy + kRows) % kRows;
  if (dx)
  {
    if (c->row == 4)
    {
      // Jump between the three action keys.
      const int key = c->column < 4 ? 0 : c->column < 7 ? 1 : 2;
      const int next = (key + dx + 3) % 3;
      c->column = next == 0 ? 1 : next == 1 ? 5 : 8;
    }
    else
    {
      c->column = (c->column + dx + RowLength(c->row)) % RowLength(c->row);
    }
  }
  c->column = std::clamp(c->column, 0, RowLength(c->row) - 1);
}

// Applies a key to `text` (at most `limit` characters). Returns true for DONE.
inline bool Press(const Cursor& c, std::string* text, std::size_t limit)
{
  switch (KeyAt(c))
  {
  case Key::Done:
    return true;
  case Key::Delete:
    if (!text->empty())
      text->pop_back();
    return false;
  case Key::Space:
    if (!text->empty() && text->back() != ' ' && text->size() < limit)
      text->push_back(' ');
    return false;
  case Key::Character:
  {
    if (text->size() >= limit)
      return false;
    char ch = CharacterAt(c);
    const bool word_start = text->empty() || text->back() == ' ' || text->back() == '-' ||
                            text->back() == '_';
    if (std::isalpha(static_cast<unsigned char>(ch)) && !word_start)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    text->push_back(ch);
    return false;
  }
  }
  return false;
}

// Labels for drawing.
inline const char* ActionLabel(Key key)
{
  return key == Key::Space ? "SPACE" : key == Key::Delete ? "DELETE" : "DONE";
}
}  // namespace moderngekko::onscreen_keyboard
