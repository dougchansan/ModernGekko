#pragma once

// GameCube controller bindings shared by the launcher, the runner and the
// runtime's pause menu: which host device is a keyboard, the default layout
// for a keyboard or a gamepad, the key names each of Dolphin's keyboard
// backends uses, and a readable form of a binding for the menus.
//
// Key names are the strings Dolphin's ControllerInterface gives its inputs:
//   DInput   (Windows) DInput/DInputKeyboardMouse.cpp, names from DInput/NamedKeys.h
//   Quartz   (macOS)   Quartz/QuartzKeyboardAndMouse.mm, KeycodeToName()
//   XInput2  (Linux)   Xlib/XInput2.cpp, XKeysymToString() of each key's keysym
// Gamepad names are the SDL backend's (SDL/SDLGamepad.h), with the face
// buttons written positionally ("Button A" matches the south button of any
// pad, see Gamepad::Button::IsMatchingName).

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moderngekko::frontend
{
// Every GameCube input a player can bind, in the order the mapping screens
// list them. `key` is the GCPadNew.ini key.
struct GcInput
{
  std::string_view key;
  std::string_view label;
};
inline constexpr std::array<GcInput, 20> kGcInputs = {{
    {"Buttons/A", "A"},
    {"Buttons/B", "B"},
    {"Buttons/X", "X"},
    {"Buttons/Y", "Y"},
    {"Buttons/Z", "Z"},
    {"Buttons/Start", "START"},
    {"Triggers/L", "L"},
    {"Triggers/R", "R"},
    {"D-Pad/Up", "D-PAD UP"},
    {"D-Pad/Down", "D-PAD DOWN"},
    {"D-Pad/Left", "D-PAD LEFT"},
    {"D-Pad/Right", "D-PAD RIGHT"},
    {"Main Stick/Up", "STICK UP"},
    {"Main Stick/Down", "STICK DOWN"},
    {"Main Stick/Left", "STICK LEFT"},
    {"Main Stick/Right", "STICK RIGHT"},
    {"C-Stick/Up", "C-STICK UP"},
    {"C-Stick/Down", "C-STICK DOWN"},
    {"C-Stick/Left", "C-STICK LEFT"},
    {"C-Stick/Right", "C-STICK RIGHT"},
}};

enum class KeyboardBackend
{
  None,
  DInput,
  Quartz,
  XInput2,
};

inline KeyboardBackend KeyboardBackendOf(std::string_view device)
{
  if (device.starts_with("DInput/") && device.find("Keyboard") != std::string_view::npos)
    return KeyboardBackend::DInput;
  if (device.starts_with("Quartz/"))
    return KeyboardBackend::Quartz;
  if (device.starts_with("XInput2/"))
    return KeyboardBackend::XInput2;
  return KeyboardBackend::None;
}

inline bool IsKeyboardDevice(std::string_view device)
{
  return KeyboardBackendOf(device) != KeyboardBackend::None;
}

inline bool IsGamepadDevice(std::string_view device)
{
  return device.starts_with("SDL/") || device.starts_with("XInput/");
}

// This platform's keyboard as ControllerInterface names it. On Linux it is
// the X server's first master pointer/keyboard pair.
inline constexpr std::string_view KeyboardDevice()
{
#if defined(_WIN32)
  return "DInput/0/Keyboard Mouse";
#elif defined(__APPLE__)
  return "Quartz/0/Keyboard & Mouse";
#else
  return "XInput2/0/Virtual core pointer";
#endif
}

// A key under each backend's name. `hid` is the USB HID usage, which is also
// the SDL scancode, so the launcher can name the key it sees pressed. A null
// name is a key that backend does not have.
struct KeyName
{
  int hid;
  const char* dinput;
  const char* quartz;
  const char* xinput2;
  const char* display;
};
inline constexpr KeyName kKeyNames[] = {
    {4, "A", "A", "A", "A"},
    {5, "B", "B", "B", "B"},
    {6, "C", "C", "C", "C"},
    {7, "D", "D", "D", "D"},
    {8, "E", "E", "E", "E"},
    {9, "F", "F", "F", "F"},
    {10, "G", "G", "G", "G"},
    {11, "H", "H", "H", "H"},
    {12, "I", "I", "I", "I"},
    {13, "J", "J", "J", "J"},
    {14, "K", "K", "K", "K"},
    {15, "L", "L", "L", "L"},
    {16, "M", "M", "M", "M"},
    {17, "N", "N", "N", "N"},
    {18, "O", "O", "O", "O"},
    {19, "P", "P", "P", "P"},
    {20, "Q", "Q", "Q", "Q"},
    {21, "R", "R", "R", "R"},
    {22, "S", "S", "S", "S"},
    {23, "T", "T", "T", "T"},
    {24, "U", "U", "U", "U"},
    {25, "V", "V", "V", "V"},
    {26, "W", "W", "W", "W"},
    {27, "X", "X", "X", "X"},
    {28, "Y", "Y", "Y", "Y"},
    {29, "Z", "Z", "Z", "Z"},
    {30, "1", "1", "1", "1"},
    {31, "2", "2", "2", "2"},
    {32, "3", "3", "3", "3"},
    {33, "4", "4", "4", "4"},
    {34, "5", "5", "5", "5"},
    {35, "6", "6", "6", "6"},
    {36, "7", "7", "7", "7"},
    {37, "8", "8", "8", "8"},
    {38, "9", "9", "9", "9"},
    {39, "0", "0", "0", "0"},
    {40, "RETURN", "Return", "Return", "ENTER"},
    {41, "ESCAPE", "Escape", "Escape", "ESCAPE"},
    {42, "BACK", "Backspace", "BackSpace", "BACKSPACE"},
    {43, "TAB", "Tab", "Tab", "TAB"},
    {44, "SPACE", "Space", "space", "SPACE"},
    {45, "MINUS", "-", "minus", "MINUS"},
    {46, "EQUALS", "=", "equal", "EQUALS"},
    {47, "LBRACKET", "[", "bracketleft", "LEFT BRACKET"},
    {48, "RBRACKET", "]", "bracketright", "RIGHT BRACKET"},
    {49, "BACKSLASH", "\\", "backslash", "BACKSLASH"},
    {51, "SEMICOLON", ";", "semicolon", "SEMICOLON"},
    {52, "APOSTROPHE", "'", "apostrophe", "APOSTROPHE"},
    {53, "GRAVE", "Tilde", "grave", "GRAVE"},
    {54, "COMMA", ",", "comma", "COMMA"},
    {55, "PERIOD", ".", "period", "PERIOD"},
    {56, "SLASH", "/", "slash", "SLASH"},
    {57, "CAPITAL", "Caps Lock", "Caps_Lock", "CAPS LOCK"},
    {58, "F1", "F1", "F1", "F1"},
    {59, "F2", "F2", "F2", "F2"},
    {60, "F3", "F3", "F3", "F3"},
    {61, "F4", "F4", "F4", "F4"},
    {62, "F5", "F5", "F5", "F5"},
    {63, "F6", "F6", "F6", "F6"},
    {64, "F7", "F7", "F7", "F7"},
    {65, "F8", "F8", "F8", "F8"},
    {66, "F9", "F9", "F9", "F9"},
    {67, "F10", "F10", "F10", "F10"},
    {68, "F11", "F11", "F11", "F11"},
    {69, "F12", "F12", "F12", "F12"},
    {73, "INSERT", nullptr, "Insert", "INSERT"},
    {74, "HOME", "Home", "Home", "HOME"},
    {75, "PRIOR", "Page Up", "Prior", "PAGE UP"},
    {76, "DELETE", "Delete", "Delete", "DELETE"},
    {77, "END", "End", "End", "END"},
    {78, "NEXT", "Page Down", "Next", "PAGE DOWN"},
    {79, "RIGHT", "Right Arrow", "Right", "RIGHT ARROW"},
    {80, "LEFT", "Left Arrow", "Left", "LEFT ARROW"},
    {81, "DOWN", "Down Arrow", "Down", "DOWN ARROW"},
    {82, "UP", "Up Arrow", "Up", "UP ARROW"},
    {84, "DIVIDE", "Keypad /", "KP_Divide", "KEYPAD DIVIDE"},
    {85, "MULTIPLY", "Keypad *", "KP_Multiply", "KEYPAD MULTIPLY"},
    {86, "SUBTRACT", "Keypad -", "KP_Subtract", "KEYPAD MINUS"},
    {87, "ADD", "Keypad +", "KP_Add", "KEYPAD PLUS"},
    {88, "NUMPADENTER", "Keypad Enter", "KP_Enter", "KEYPAD ENTER"},
    // X11 names the keypad digits by their unshifted (Num Lock off) keysyms.
    {89, "NUMPAD1", "Keypad 1", "KP_End", "KEYPAD 1"},
    {90, "NUMPAD2", "Keypad 2", "KP_Down", "KEYPAD 2"},
    {91, "NUMPAD3", "Keypad 3", "KP_Next", "KEYPAD 3"},
    {92, "NUMPAD4", "Keypad 4", "KP_Left", "KEYPAD 4"},
    {93, "NUMPAD5", "Keypad 5", "KP_Begin", "KEYPAD 5"},
    {94, "NUMPAD6", "Keypad 6", "KP_Right", "KEYPAD 6"},
    {95, "NUMPAD7", "Keypad 7", "KP_Home", "KEYPAD 7"},
    {96, "NUMPAD8", "Keypad 8", "KP_Up", "KEYPAD 8"},
    {97, "NUMPAD9", "Keypad 9", "KP_Prior", "KEYPAD 9"},
    {98, "NUMPAD0", "Keypad 0", "KP_Insert", "KEYPAD 0"},
    {99, "DECIMAL", "Keypad .", "KP_Delete", "KEYPAD PERIOD"},
    {224, "LCONTROL", "Left Control", "Control_L", "LEFT CTRL"},
    {225, "LSHIFT", "Left Shift", "Shift_L", "LEFT SHIFT"},
    {226, "LMENU", "Left Alt", "Alt_L", "LEFT ALT"},
    {227, "LWIN", "Command", "Super_L", "LEFT SUPER"},
    {228, "RCONTROL", "Right Control", "Control_R", "RIGHT CTRL"},
    {229, "RSHIFT", "Right Shift", "Shift_R", "RIGHT SHIFT"},
    {230, "RMENU", "Right Alt", "Alt_R", "RIGHT ALT"},
    {231, "RWIN", nullptr, "Super_R", "RIGHT SUPER"},
};

inline const char* KeyNameFor(const KeyName& key, KeyboardBackend backend)
{
  switch (backend)
  {
  case KeyboardBackend::DInput:
    return key.dinput;
  case KeyboardBackend::Quartz:
    return key.quartz;
  case KeyboardBackend::XInput2:
    return key.xinput2;
  default:
    return nullptr;
  }
}

// The backend's name for the key with this HID usage / SDL scancode, or empty.
inline std::string KeyNameForHid(int hid, KeyboardBackend backend)
{
  for (const KeyName& key : kKeyNames)
    if (key.hid == hid)
    {
      const char* name = KeyNameFor(key, backend);
      return name ? name : std::string();
    }
  return {};
}

// A binding expression for one input name: `name`.
inline std::string QuoteInput(std::string_view name)
{
  return "`" + std::string(name) + "`";
}

// The default layout for a device, as GCPadNew.ini keys and values (Device
// not included). A keyboard gets the README's layout under its backend's key
// names; anything else gets the positional gamepad layout.
inline std::vector<std::pair<std::string, std::string>> DefaultBindings(std::string_view device)
{
  const KeyboardBackend backend = KeyboardBackendOf(device);
  if (backend == KeyboardBackend::None)
    return {
        {"Buttons/A", "`Button A`"},
        {"Buttons/B", "`Button B`"},
        {"Buttons/X", "`Button X`"},
        {"Buttons/Y", "`Button Y`"},
        {"Buttons/Z", "`Shoulder R`"},
        {"Buttons/Start", "Start"},
        {"Main Stick/Up", "`Left Y+`"},
        {"Main Stick/Down", "`Left Y-`"},
        {"Main Stick/Left", "`Left X-`"},
        {"Main Stick/Right", "`Left X+`"},
        {"Main Stick/Calibration", "100.00"},
        {"C-Stick/Up", "`Right Y+`"},
        {"C-Stick/Down", "`Right Y-`"},
        {"C-Stick/Left", "`Right X-`"},
        {"C-Stick/Right", "`Right X+`"},
        {"C-Stick/Calibration", "100.00"},
        {"Triggers/L", "`Trigger L`"},
        {"Triggers/R", "`Trigger R`"},
        {"Triggers/L-Analog", "`Trigger L`"},
        {"Triggers/R-Analog", "`Trigger R`"},
        {"D-Pad/Up", "`Pad N`"},
        {"D-Pad/Down", "`Pad S`"},
        {"D-Pad/Left", "`Pad W`"},
        {"D-Pad/Right", "`Pad E`"},
        {"Rumble/Motor", "`Motor L` | `Motor R`"},
    };
  const auto key = [backend](int hid) { return QuoteInput(KeyNameForHid(hid, backend)); };
  // HID usages: W 26, S 22, A 4, D 7, I 12, K 14, J 13, L 15, X 27, Z 29,
  // C 6, V 25, Left Shift 225, Return 40, Q 20, E 8, arrows 82 81 80 79.
  return {
      {"Buttons/A", key(27)},
      {"Buttons/B", key(29)},
      {"Buttons/X", key(6)},
      {"Buttons/Y", key(25)},
      {"Buttons/Z", key(225)},
      {"Buttons/Start", key(40)},
      {"Main Stick/Up", key(26)},
      {"Main Stick/Down", key(22)},
      {"Main Stick/Left", key(4)},
      {"Main Stick/Right", key(7)},
      {"Main Stick/Calibration", "100.00"},
      {"C-Stick/Up", key(12)},
      {"C-Stick/Down", key(14)},
      {"C-Stick/Left", key(13)},
      {"C-Stick/Right", key(15)},
      {"C-Stick/Calibration", "100.00"},
      {"Triggers/L", key(20)},
      {"Triggers/R", key(8)},
      {"Triggers/L-Analog", key(20)},
      {"Triggers/R-Analog", key(8)},
      {"D-Pad/Up", key(82)},
      {"D-Pad/Down", key(81)},
      {"D-Pad/Left", key(80)},
      {"D-Pad/Right", key(79)},
  };
}

namespace detail
{
inline std::string Upper(std::string_view text)
{
  std::string out(text);
  for (char& c : out)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// One input name (no backticks) as the menus show it.
inline std::string ReadableInput(std::string_view name)
{
  for (const KeyName& key : kKeyNames)
    for (const char* candidate : {key.dinput, key.quartz, key.xinput2})
      if (candidate && name == candidate)
        return key.display;
  static constexpr std::pair<std::string_view, std::string_view> kPad[] = {
      {"Button S", "BUTTON A"},       {"Button E", "BUTTON B"},
      {"Button W", "BUTTON X"},       {"Button N", "BUTTON Y"},
      {"Shoulder L", "LEFT BUMPER"},  {"Shoulder R", "RIGHT BUMPER"},
      {"Trigger L", "LEFT TRIGGER"},  {"Trigger R", "RIGHT TRIGGER"},
      {"Thumb L", "LEFT STICK CLICK"}, {"Thumb R", "RIGHT STICK CLICK"},
      {"Pad N", "D-PAD UP"},          {"Pad S", "D-PAD DOWN"},
      {"Pad W", "D-PAD LEFT"},        {"Pad E", "D-PAD RIGHT"},
      {"Left Y+", "LEFT STICK UP"},   {"Left Y-", "LEFT STICK DOWN"},
      {"Left X-", "LEFT STICK LEFT"}, {"Left X+", "LEFT STICK RIGHT"},
      {"Right Y+", "RIGHT STICK UP"}, {"Right Y-", "RIGHT STICK DOWN"},
      {"Right X-", "RIGHT STICK LEFT"}, {"Right X+", "RIGHT STICK RIGHT"},
      {"Shift", "SHIFT"},             {"Ctrl", "CTRL"},
      {"Alt", "ALT"},
  };
  for (const auto& [raw, shown] : kPad)
    if (name == raw)
      return std::string(shown);
  return Upper(name);
}
}  // namespace detail

// A binding expression as the menus show it: backticks gone, names spelled
// out ("LEFT SHIFT", "BUTTON A"), alternatives joined with " / ".
inline std::string ReadableBinding(std::string_view expression)
{
  std::string out, token;
  bool quoted = false;
  const auto flush = [&]
  {
    std::string_view t = token;
    while (!t.empty() && t.front() == ' ')
      t.remove_prefix(1);
    while (!t.empty() && t.back() == ' ')
      t.remove_suffix(1);
    if (!t.empty())
      out += detail::ReadableInput(t);
    token.clear();
  };
  for (const char c : expression)
  {
    if (c == '`')
    {
      if (quoted)
        flush();
      quoted = !quoted;
      continue;
    }
    if (!quoted && (c == '|' || c == '&' || c == '!' || c == '(' || c == ')' || c == '+'))
    {
      flush();
      if (c == '|')
        out += " / ";
      else if (c == '&' || c == '+')
        out += " + ";
      else if (c == '!')
        out += "NOT ";
      continue;
    }
    token += c;
  }
  flush();
  return out;
}
}  // namespace moderngekko::frontend
