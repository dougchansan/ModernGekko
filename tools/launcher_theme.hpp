#pragma once

// The launcher's Pokemon Colosseum look: the game's own fonts and the battle
// menu's steel frames, a red chevron cursor, Poke Ball toggles. Everything is
// laid out on a fixed 1280x760 canvas and scaled to the window, so pages are
// written in canvas units and stay in proportion at any size.
//
// Pages are immediate-mode lists of rows. The theme owns focus and input:
// keyboard, mouse and gamepad all move the same cursor (arrows / d-pad, Enter
// / A, Esc / B), and H on the keyboard or Z on a controller opens the help for
// whatever is highlighted.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

namespace moderngekko::frontend::theme
{
inline constexpr float kCanvasW = 1280.0f;
inline constexpr float kCanvasH = 760.0f;

struct Fonts
{
  ImFont* title = nullptr;  // colo_font0_22px, the main menu font
  ImFont* menu = nullptr;   // colo_font3_18px_condensed, the battle HUD font
  ImFont* small = nullptr;  // colo_font4_13px_condensed
  ImFont* body = nullptr;   // colo_font2_13px, prose
};

// Loads the game fonts from `directory` (ending in a separator). Missing fonts
// fall back to ImGui's default, so the launcher still works before a game is
// set up.
Fonts LoadFonts(const std::string& directory);

namespace color
{
inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kText = IM_COL32(214, 220, 222, 255);
inline constexpr ImU32 kDim = IM_COL32(150, 156, 160, 255);
inline constexpr ImU32 kGold = IM_COL32(232, 200, 64, 255);
inline constexpr ImU32 kGrey = IM_COL32(196, 204, 208, 255);
inline constexpr ImU32 kRed = IM_COL32(200, 18, 22, 255);
inline constexpr ImU32 kWarn = IM_COL32(220, 120, 60, 255);
inline constexpr ImU32 kPanel = IM_COL32(26, 40, 37, 215);
inline constexpr ImU32 kPanelDark = IM_COL32(20, 30, 28, 220);
inline constexpr ImU32 kPlay = IM_COL32(70, 20, 22, 230);
}  // namespace color

// Canvas -> screen mapping for this frame.
struct Canvas
{
  ImVec2 origin{0, 0};
  float scale = 1.0f;
  ImVec2 P(float x, float y) const { return {origin.x + x * scale, origin.y + y * scale}; }
  float S(float v) const { return v * scale; }
};

// Drawing primitives, all in canvas units.
void Background(ImDrawList* dl, const Canvas& c, ImVec2 screen_size);
// The backdrop the mockups use: a game picture filling the window (cropped to
// cover it), darkened, with a green-black shade deepening towards the bottom.
void Backdrop(ImDrawList* dl, ImVec2 screen_size, ImTextureRef image, float image_w,
              float image_h);
void SteelPanel(ImDrawList* dl, const Canvas& c, float x0, float y0, float x1, float y1,
                float chamfer = 14.0f, ImU32 fill = color::kPanel);
void Chevron(ImDrawList* dl, const Canvas& c, float x, float y, float size = 16.0f);
void PokeBall(ImDrawList* dl, const Canvas& c, float x, float y, bool on, float radius = 15.0f);
void Text(ImDrawList* dl, const Canvas& c, ImFont* font, float size, float x, float y,
          const std::string& text, ImU32 col = color::kWhite, bool shadow = true);
float TextWidth(ImFont* font, float size, const std::string& text);  // canvas units
void SliderBar(ImDrawList* dl, const Canvas& c, float x, float y, float w, float fraction);
void MemoryCard(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
                const std::string& label, ImU32 body, bool selected, float w = 164.0f,
                float h = 190.0f);

// Word-wrapped prose with comfortable line spacing; returns the height used.
// With a null draw list it only measures.
float Paragraph(ImDrawList* dl, const Canvas& c, ImFont* font, float size, float x, float y,
                float width, const std::string& text, ImU32 col, float line_step);

// A GameCube controller seen from the front, centred on (cx, cy). Buttons
// whose bit is set in `pressed` light up gold; sticks are -1..1.
enum GcButton : unsigned
{
  kGcA = 1u << 0, kGcB = 1u << 1, kGcX = 1u << 2, kGcY = 1u << 3, kGcZ = 1u << 4,
  kGcL = 1u << 5, kGcR = 1u << 6, kGcStart = 1u << 7, kGcUp = 1u << 8, kGcDown = 1u << 9,
  kGcLeft = 1u << 10, kGcRight = 1u << 11, kGcC = 1u << 12,
};
// `width` is the controller body's width in canvas units.
void GcController(ImDrawList* dl, const Canvas& c, float cx, float cy, float width,
                  unsigned pressed, ImVec2 stick, ImVec2 cstick);

// What a connected gamepad is pressing right now, in GameCube terms.
unsigned GamepadButtons(ImVec2* stick, ImVec2* cstick);

// A "< VALUE >" picker in a dark inset, `w` wide.
void ValueSelector(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
                   const std::string& text, float w, bool enabled = true);

// A Pokemon face in a small steel frame (size + 20 square), outlined red when
// `active`. A null texture draws the frame with `fallback` text instead.
void Portrait(ImDrawList* dl, const Canvas& c, const Fonts& f, ImTextureRef texture, bool has_image,
              ImVec2 uv0, ImVec2 uv1, float x, float y, float size, bool active,
              const std::string& fallback);

// The empty "+ NEW" slot beside the cards.
void NewCardSlot(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
                 bool selected, float w = 164.0f, float h = 190.0f);

// A colour for a card, picked from GameCube card colours by its name.
ImU32 CardColor(const std::string& name);

// Squashes everything drawn since `vtx_start` horizontally about `cx` (screen
// x) and fades/darkens it: how a card turns away on the carousel ring.
void TransformSince(ImDrawList* dl, int vtx_start, float cx, float squash, float alpha,
                    float shade);

// The carousel's backdrop: dimmed screen, indigo glow and slow twinkling stars.
void Starfield(ImDrawList* dl, const Canvas& c, ImVec2 screen, float t, float time);

// Per-frame input, gathered once from keyboard, mouse and gamepad.
struct Input
{
  bool up = false, down = false, left = false, right = false;
  bool accept = false, back = false, help = false;
  bool rename = false, duplicate = false;  // R / X and D / Y
  bool erase = false;                      // Delete
  bool gamepad = false;  // a gamepad was used last, for button hints
};
Input ReadInput(bool* last_was_gamepad);

// A settings row. Pages describe their rows each frame; the UI handles focus,
// drawing and input, and calls back when a value changes.
struct Row
{
  enum class Kind
  {
    Toggle,   // Poke Ball on/off
    Choice,   // < value >, left/right cycles
    Slider,   // integer range, left/right steps
    Action,   // a button (Enter / A)
    TextInput,  // a text box (Enter / A edits, Enter commits)
    Info,     // a line of text, not focusable
  };
  Kind kind = Kind::Info;
  std::string label;
  std::string help;  // shown in the help panel and the H / Z card
  bool enabled = true;
  // Toggle
  bool on = false;
  // Choice
  std::vector<std::string> options;
  int index = 0;
  // Slider
  int value = 0, min = 0, max = 0, step = 1;
  std::string value_text;  // e.g. "2x", "30 S"
  // TextInput: edits uffer in place; changed runs when editing ends.
  char* buffer = nullptr;
  std::size_t buffer_size = 0;
  bool numeric = false;
  // Info / action colour
  ImU32 text_color = color::kText;
  // Called with the new state; return false to reject (the row keeps its value).
  std::function<bool(const Row&)> changed;
  std::function<void()> activated;  // Action
};

// Lays out and handles a column of rows inside the page panel. `focus` is the
// focused row (kept in range). Returns the help text of the focused row.
std::string DrawRows(ImDrawList* dl, const Canvas& c, const Fonts& f, std::vector<Row>& rows,
                     int* focus, bool page_has_focus, const Input& in, float x0, float y0,
                     float label_w = 410.0f, float step = 50.0f);
}  // namespace moderngekko::frontend::theme
