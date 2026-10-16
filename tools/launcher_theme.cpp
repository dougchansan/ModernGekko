#include "launcher_theme.hpp"

// Drawing follows the mockup script (launcher-theme/screens.py) shape for
// shape: same canvas coordinates, colours and font scales, so the launcher and
// the approved screens stay comparable side by side.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include <imgui_internal.h>  // ImGui::GetKeyData, for analog stick values

#include "../src/runtime/gc_controller_art.hpp"

namespace moderngekko::pause_menu
{
ImFont* ColosseumFont(const std::string& directory, const std::string& name);
}

namespace moderngekko::frontend::theme
{
namespace
{
ImFont* Or(ImFont* font)
{
  return font ? font : ImGui::GetFont();
}

void Chamfer(ImVec2* out, const Canvas& c, float x0, float y0, float x1, float y1, float k)
{
  out[0] = c.P(x0 + k, y0);
  out[1] = c.P(x1 - k, y0);
  out[2] = c.P(x1, y0 + k);
  out[3] = c.P(x1, y1 - k);
  out[4] = c.P(x1 - k, y1);
  out[5] = c.P(x0 + k, y1);
  out[6] = c.P(x0, y1 - k);
  out[7] = c.P(x0, y0 + k);
}

bool MouseIn(const Canvas& c, float x0, float y0, float x1, float y1)
{
  return ImGui::IsMouseHoveringRect(c.P(x0, y0), c.P(x1, y1), false);
}

bool ClickedIn(const Canvas& c, float x0, float y0, float x1, float y1)
{
  return MouseIn(c, x0, y0, x1, y1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void RedTriangle(ImDrawList* dl, const Canvas& c, float tip_x, float y, float dir)
{
  dl->AddTriangleFilled(c.P(tip_x, y + 17), c.P(tip_x + dir * 12, y + 7),
                        c.P(tip_x + dir * 12, y + 27), color::kRed);
}

void Selector(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
              const std::string& text, float w, bool enabled)
{
  dl->AddRectFilled(c.P(x, y - 2), c.P(x + w, y + 36), IM_COL32(8, 12, 12, 200));
  dl->AddRect(c.P(x, y - 2), c.P(x + w, y + 36), IM_COL32(120, 126, 132, 255), 0, 0, c.S(1));
  if (enabled)
  {
    RedTriangle(dl, c, x + 10, y, 1);
    RedTriangle(dl, c, x + w - 10, y, -1);
  }
  const float tw = TextWidth(f.menu, 36, text);
  const float size = tw > w - 60 ? 36 * (w - 60) / tw : 36;
  Text(dl, c, f.menu, size, x + w / 2 - TextWidth(f.menu, size, text) / 2, y + (36 - size) / 2,
       text, enabled ? color::kWhite : color::kDim);
}
}  // namespace

Fonts LoadFonts(const std::string& directory)
{
  using moderngekko::pause_menu::ColosseumFont;
  Fonts fonts;
  fonts.title = ColosseumFont(directory, "colo_font0_22px");
  fonts.menu = ColosseumFont(directory, "colo_font3_18px_condensed");
  fonts.small = ColosseumFont(directory, "colo_font4_13px_condensed");
  fonts.body = ColosseumFont(directory, "colo_font2_13px");
  return fonts;
}

void Background(ImDrawList* dl, const Canvas& c, ImVec2 screen)
{
  // The mockups sit on a darkened, blurred battle; without a screenshot to
  // hand, the same green-black wash and a faint horizon glow.
  dl->AddRectFilledMultiColor({0, 0}, screen, IM_COL32(16, 30, 28, 255), IM_COL32(16, 30, 28, 255),
                              IM_COL32(4, 9, 9, 255), IM_COL32(4, 9, 9, 255));
  const float mid = c.origin.y + c.S(kCanvasH * 0.42f);
  dl->AddRectFilledMultiColor({0, mid - c.S(160)}, {screen.x, mid}, IM_COL32(60, 90, 84, 0),
                              IM_COL32(60, 90, 84, 0), IM_COL32(60, 90, 84, 40),
                              IM_COL32(60, 90, 84, 40));
  dl->AddRectFilledMultiColor({0, mid}, {screen.x, mid + c.S(220)}, IM_COL32(60, 90, 84, 40),
                              IM_COL32(60, 90, 84, 40), IM_COL32(60, 90, 84, 0),
                              IM_COL32(60, 90, 84, 0));
}

void Backdrop(ImDrawList* dl, ImVec2 screen, ImTextureRef image, float image_w, float image_h)
{
  const float screen_aspect = screen.x / screen.y, image_aspect = image_w / image_h;
  ImVec2 uv0{0, 0}, uv1{1, 1};
  if (image_aspect > screen_aspect)
    uv0.x = (1 - screen_aspect / image_aspect) / 2, uv1.x = 1 - uv0.x;
  else
    uv0.y = (1 - image_aspect / screen_aspect) / 2, uv1.y = 1 - uv0.y;
  dl->AddImage(image, {0, 0}, screen, uv0, uv1);
  // As the mockups: brightness down to about 40%, then a shade from
  // (6, 14, 12, 90) at the top to (6, 14, 12, 170) at the bottom.
  dl->AddRectFilled({0, 0}, screen, IM_COL32(0, 0, 0, 155));
  dl->AddRectFilledMultiColor({0, 0}, screen, IM_COL32(6, 14, 12, 90), IM_COL32(6, 14, 12, 90),
                              IM_COL32(6, 14, 12, 170), IM_COL32(6, 14, 12, 170));
}

void SteelPanel(ImDrawList* dl, const Canvas& c, float x0, float y0, float x1, float y1,
                float k, ImU32 fill)
{
  ImVec2 p[8];
  Chamfer(p, c, x0 - 2, y0 - 2, x1 + 2, y1 + 2, k + 2);
  dl->AddConvexPolyFilled(p, 8, IM_COL32(12, 14, 16, 255));
  for (int i = 0; i < 7; ++i)
  {
    const int tone = static_cast<int>(205 - 70 * i / 6.0f);
    const float in = i * 0.6f;
    Chamfer(p, c, x0 + in, y0 + in, x1 - in, y1 - in, k);
    dl->AddPolyline(p, 8, IM_COL32(tone, tone + 4, tone + 8, 255), ImDrawFlags_Closed,
                    std::max(1.0f, c.S(2)));
  }
  Chamfer(p, c, x0 + 7, y0 + 7, x1 - 7, y1 - 7, std::max(k - 6, 4.0f));
  dl->AddConvexPolyFilled(p, 8, IM_COL32(10, 12, 12, 255));
  Chamfer(p, c, x0 + 9, y0 + 9, x1 - 9, y1 - 9, std::max(k - 7, 3.0f));
  dl->AddConvexPolyFilled(p, 8, fill);
  dl->AddRectFilledMultiColor(c.P(x0 + 14, y0 + 10), c.P(x1 - 14, y0 + 28),
                              IM_COL32(255, 255, 255, 16), IM_COL32(255, 255, 255, 16),
                              IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
}

void Chevron(ImDrawList* dl, const Canvas& c, float x, float y, float s)
{
  // Two arrowheads, each a concave quad drawn as two triangles.
  for (const float off : {0.0f, s * 0.55f})
  {
    const ImVec2 a = c.P(x + off, y), b = c.P(x + off + s * 0.5f, y + s * 0.6f),
                 d = c.P(x + off, y + s * 1.2f), m = c.P(x + off + s * 0.22f, y + s * 0.6f);
    dl->AddTriangleFilled(a, b, m, color::kRed);
    dl->AddTriangleFilled(m, b, d, color::kRed);
  }
}

void PokeBall(ImDrawList* dl, const Canvas& c, float x, float y, bool on, float r)
{
  const ImVec2 centre = c.P(x + r, y + r);
  const float R = c.S(r);
  const float line = std::max(1.0f, c.S(3));
  if (!on)
  {
    dl->AddCircle(centre, R - line / 2, IM_COL32(150, 156, 162, 255), 40, line);
    return;
  }
  dl->AddCircleFilled(c.P(x + r + 2, y + r + 3), R, IM_COL32(0, 0, 0, 160), 40);
  dl->AddCircleFilled(centre, R, IM_COL32(244, 244, 244, 255), 40);
  constexpr float pi = 3.14159265f;
  dl->PathArcTo(centre, R, pi, 2 * pi, 24);
  dl->PathFillConvex(IM_COL32(214, 28, 34, 255));
  dl->AddCircle(centre, R - line / 2, IM_COL32(20, 20, 20, 255), 40, line);
  dl->AddLine(c.P(x, y + r), c.P(x + 2 * r, y + r), IM_COL32(20, 20, 20, 255), line);
  dl->AddCircleFilled(centre, c.S(6), IM_COL32(244, 244, 244, 255), 20);
  dl->AddCircle(centre, c.S(6), IM_COL32(20, 20, 20, 255), 20, line);
}

float TextWidth(ImFont* font, float size, const std::string& text)
{
  return Or(font)->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
}

void Text(ImDrawList* dl, const Canvas& c, ImFont* font, float size, float x, float y,
          const std::string& text, ImU32 col, bool shadow)
{
  ImFont* f = Or(font);
  if (shadow)
    dl->AddText(f, c.S(size), c.P(x + 2, y + 2), IM_COL32(0, 0, 0, (col >> 24) & 0xFF),
                text.c_str());
  dl->AddText(f, c.S(size), c.P(x, y), col, text.c_str());
}

void SliderBar(ImDrawList* dl, const Canvas& c, float x, float y, float w, float frac)
{
  frac = std::clamp(frac, 0.0f, 1.0f);
  dl->AddRectFilled(c.P(x - 2, y - 2), c.P(x + w + 2, y + 14), IM_COL32(170, 176, 182, 255));
  dl->AddRectFilled(c.P(x, y), c.P(x + w, y + 12), IM_COL32(14, 20, 18, 255));
  const float end = x + 2 + (w - 4) * frac;
  dl->AddRectFilled(c.P(x + 2, y + 2), c.P(end, y + 10), IM_COL32(40, 196, 52, 255));
  dl->AddRectFilled(c.P(x + 2, y + 2), c.P(end, y + 4), IM_COL32(140, 240, 140, 255));
}

void MemoryCard(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
                const std::string& label, ImU32 body, bool selected, float w, float h)
{
  const float round = c.S(10);
  const ImU32 r = body & 0xFF, g = (body >> 8) & 0xFF, b = (body >> 16) & 0xFF;
  const ImU32 light = IM_COL32(std::min(255u, r + 18), std::min(255u, g + 18),
                               std::min(255u, b + 18), 255);
  dl->AddRectFilled(c.P(x + 6, y + 8), c.P(x + w + 6, y + h + 8), IM_COL32(0, 0, 0, 140), round);
  dl->AddRectFilled(c.P(x, y), c.P(x + w, y + h), body | IM_COL32_A_MASK, round);
  dl->AddRect(c.P(x, y), c.P(x + w, y + h), IM_COL32(20, 20, 24, 255), round, 0, c.S(2));
  dl->AddRectFilled(c.P(x + 3, y + 3), c.P(x + w - 3, y + 20), light, c.S(8));
  for (int i = 0; i < 9; ++i)
  {
    const float rx = x + 22 + i * 12;
    dl->AddRectFilled(c.P(rx, y + 6), c.P(rx + 6, y + 16), IM_COL32(30, 32, 36, 255));
  }
  dl->AddRectFilled(c.P(x + 14, y + 34), c.P(x + w - 14, y + h - 16), IM_COL32(28, 30, 34, 255),
                    c.S(6));
  dl->AddRectFilled(c.P(x + 18, y + 38), c.P(x + w - 18, y + h - 20),
                    IM_COL32(236, 232, 218, 255), c.S(4));
  dl->AddRectFilled(c.P(x + 18, y + 38), c.P(x + w - 18, y + 58), IM_COL32(196, 30, 40, 255));
  Text(dl, c, f.body, 13, x + w / 2 - TextWidth(f.body, 13, "MEMORY CARD") / 2, y + 40,
       "MEMORY CARD", color::kWhite, false);

  // The file name on the label, wrapped onto two lines near the middle.
  std::string name = label;
  for (char& ch : name)
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  const float room = w - 44;
  const float big = TextWidth(f.menu, 36, name) <= room ? 36.0f : 18.0f;
  std::vector<std::string> lines{name};
  if (TextWidth(f.menu, big, name) > room)
  {
    std::size_t cut = name.size() / 2;
    float best = 1e9f;
    for (std::size_t i = 0; i < name.size(); ++i)
      if ((name[i] == ' ' || name[i] == '-' || name[i] == '_') &&
          std::abs(static_cast<float>(i) - name.size() / 2.0f) < best)
      {
        best = std::abs(static_cast<float>(i) - name.size() / 2.0f);
        cut = i;
      }
    lines = {name.substr(0, cut + 1), name.substr(cut + 1)};
  }
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    std::string line = lines[i];
    while (!line.empty() && line.back() == ' ')
      line.pop_back();
    float size = big;
    if (TextWidth(f.menu, size, line) > room)
      size *= room / TextWidth(f.menu, size, line);
    Text(dl, c, f.menu, size, x + w / 2 - TextWidth(f.menu, size, line) / 2, y + 80 + i * 28,
         line, IM_COL32(30, 30, 34, 255), false);
  }
  if (selected)
    dl->AddRect(c.P(x - 6, y - 6), c.P(x + w + 6, y + h + 6), color::kRed, c.S(14), 0, c.S(4));
}

float Paragraph(ImDrawList* dl, const Canvas& c, ImFont* font, float size, float x, float y,
                float width, const std::string& text, ImU32 col, float line_step)
{
  ImFont* f = Or(font);
  const char* p = text.c_str();
  const char* end = p + text.size();
  float line_y = y;
  while (p < end)
  {
    const char* wrap = f->CalcWordWrapPosition(size, p, end, width);
    if (wrap == p)
      ++wrap;
    if (dl)  // null measures only
      dl->AddText(f, c.S(size), c.P(x, line_y), col, p, wrap);
    line_y += line_step;
    p = wrap;
    while (p < end && (*p == ' ' || *p == '\n'))
      ++p;
  }
  return line_y - y;
}

void GcController(ImDrawList* dl, const Canvas& c, float cx, float cy, float width,
                  unsigned pressed, ImVec2 stick, ImVec2 cstick)
{
  // The same traced, shaded controller the in-game overlay draws; the button
  // bits match gc_art's.
  moderngekko::gc_art::State state;
  state.buttons = pressed & 0xFFFu;
  state.stick = stick;
  state.cstick = cstick;
  state.l = (pressed & kGcL) ? 1.0f : 0.0f;
  state.r = (pressed & kGcR) ? 1.0f : 0.0f;
  moderngekko::gc_art::Draw(dl, c.P(cx, cy), c.S(width), state);
}

unsigned GamepadButtons(ImVec2* stick, ImVec2* cstick)
{
  // Dolphin's default GameCube layout on an SDL gamepad: A south, B west,
  // X east, Y north, Z the right shoulder, L and R the triggers.
  auto down = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
  auto analog = [](ImGuiKey k) { return ImGui::GetKeyData(k)->AnalogValue; };
  unsigned b = 0;
  b |= down(ImGuiKey_GamepadFaceDown) ? kGcA : 0;
  b |= down(ImGuiKey_GamepadFaceLeft) ? kGcB : 0;
  b |= down(ImGuiKey_GamepadFaceRight) ? kGcX : 0;
  b |= down(ImGuiKey_GamepadFaceUp) ? kGcY : 0;
  b |= down(ImGuiKey_GamepadR1) ? kGcZ : 0;
  b |= down(ImGuiKey_GamepadL2) ? kGcL : 0;
  b |= down(ImGuiKey_GamepadR2) ? kGcR : 0;
  b |= down(ImGuiKey_GamepadStart) ? kGcStart : 0;
  b |= down(ImGuiKey_GamepadDpadUp) ? kGcUp : 0;
  b |= down(ImGuiKey_GamepadDpadDown) ? kGcDown : 0;
  b |= down(ImGuiKey_GamepadDpadLeft) ? kGcLeft : 0;
  b |= down(ImGuiKey_GamepadDpadRight) ? kGcRight : 0;
  *stick = {analog(ImGuiKey_GamepadLStickRight) - analog(ImGuiKey_GamepadLStickLeft),
            analog(ImGuiKey_GamepadLStickDown) - analog(ImGuiKey_GamepadLStickUp)};
  *cstick = {analog(ImGuiKey_GamepadRStickRight) - analog(ImGuiKey_GamepadRStickLeft),
             analog(ImGuiKey_GamepadRStickDown) - analog(ImGuiKey_GamepadRStickUp)};
  if (std::abs(cstick->x) > 0.3f || std::abs(cstick->y) > 0.3f)
    b |= kGcC;
  return b;
}

void ValueSelector(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
                   const std::string& text, float w, bool enabled)
{
  Selector(dl, c, f, x, y, text, w, enabled);
}

void Portrait(ImDrawList* dl, const Canvas& c, const Fonts& f, ImTextureRef texture, bool has_image,
              ImVec2 uv0, ImVec2 uv1, float x, float y, float size, bool active,
              const std::string& fallback)
{
  SteelPanel(dl, c, x, y, x + size + 20, y + size + 20, 12, IM_COL32(0, 0, 0, 255));
  if (active)
  {
    ImVec2 p[8];
    Chamfer(p, c, x - 3, y - 3, x + size + 23, y + size + 23, 14);
    dl->AddPolyline(p, 8, IM_COL32(196, 32, 36, 255), ImDrawFlags_Closed, std::max(1.0f, c.S(3)));
  }
  if (has_image)
    dl->AddImage(texture, c.P(x + 10, y + 10), c.P(x + 10 + size, y + 10 + size), uv0, uv1);
  else
    Text(dl, c, f.menu, 36, x + 10 + size / 2 - TextWidth(f.menu, 36, fallback) / 2,
         y + 10 + size / 2 - 19, fallback, color::kGrey);
}

void NewCardSlot(ImDrawList* dl, const Canvas& c, const Fonts& f, float x, float y,
                 bool selected, float w, float h)
{
  dl->AddRectFilled(c.P(x, y), c.P(x + w, y + h), IM_COL32(20, 24, 26, 120), c.S(10));
  dl->AddRect(c.P(x, y), c.P(x + w, y + h), IM_COL32(150, 156, 162, 255), c.S(10), 0, c.S(3));
  Text(dl, c, f.menu, 36, x + w / 2 - TextWidth(f.menu, 36, "+ NEW") / 2, y + h / 2 - 19, "+ NEW",
       color::kGrey);
  if (selected)
    dl->AddRect(c.P(x - 6, y - 6), c.P(x + w + 6, y + h + 6), color::kRed, c.S(14), 0, c.S(4));
}

ImU32 CardColor(const std::string& name)
{
  static constexpr ImU32 palette[] = {
      IM_COL32(60, 62, 70, 255),   IM_COL32(36, 44, 92, 255),  IM_COL32(120, 30, 36, 255),
      IM_COL32(28, 90, 60, 255),   IM_COL32(84, 52, 120, 255), IM_COL32(150, 92, 30, 255),
      IM_COL32(170, 172, 180, 255)};
  unsigned hash = 2166136261u;
  for (const char ch : name)
    hash = (hash ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)))) *
           16777619u;
  return palette[hash % std::size(palette)];
}

void TransformSince(ImDrawList* dl, int vtx_start, float cx, float squash, float alpha,
                    float shade)
{
  for (int i = vtx_start; i < dl->VtxBuffer.Size; ++i)
  {
    ImDrawVert& v = dl->VtxBuffer[i];
    v.pos.x = cx + (v.pos.x - cx) * squash;
    const ImU32 col = v.col;
    const auto ch = [&](int shift) {
      return static_cast<ImU32>(((col >> shift) & 0xFF) * shade) << shift;
    };
    const ImU32 a = static_cast<ImU32>(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    v.col = ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) |
            (a << IM_COL32_A_SHIFT);
  }
}

void Starfield(ImDrawList* dl, const Canvas& c, ImVec2 screen, float t, float time)
{
  dl->AddRectFilled({0, 0}, screen, IM_COL32(2, 3, 10, static_cast<int>(232 * t)));
  // A soft indigo glow behind the ring, built from stacked translucent discs.
  const ImVec2 centre = c.P(640, 330);
  for (int i = 0; i < 14; ++i)
  {
    const float r = c.S(560 - i * 36.0f);
    dl->AddCircleFilled(centre, r, IM_COL32(46, 34, 110, static_cast<int>(9 * t)), 64);
  }
  // Stars: fixed pseudo-random positions drifting slowly left, twinkling.
  unsigned seed = 12345;
  auto next = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0f;
  };
  for (int i = 0; i < 140; ++i)
  {
    const float sx = next(), sy = next(), size = 0.6f + next() * 1.6f, phase = next() * 6.28f;
    const float speed = 4 + size * 6;
    float x = std::fmod(sx * screen.x - time * speed * c.scale, screen.x);
    if (x < 0)
      x += screen.x;
    const float twinkle = 0.55f + 0.45f * std::sin(time * (1.2f + size) + phase);
    const int a = static_cast<int>(220 * t * twinkle);
    dl->AddCircleFilled({x, sy * screen.y}, c.S(size), IM_COL32(200, 210, 255, a), 8);
  }
}

Input ReadInput(bool* last_was_gamepad)
{
  Input in;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput)
    return in;  // a text box owns the keyboard
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  auto once = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
  const bool kb_up = key(ImGuiKey_UpArrow), kb_down = key(ImGuiKey_DownArrow);
  const bool kb_left = key(ImGuiKey_LeftArrow), kb_right = key(ImGuiKey_RightArrow);
  const bool kb_accept = once(ImGuiKey_Enter) || once(ImGuiKey_KeypadEnter) || once(ImGuiKey_Space);
  const bool kb_back = once(ImGuiKey_Escape) || once(ImGuiKey_Backspace);
  const bool kb_help = once(ImGuiKey_H);
  const bool kb_rename = once(ImGuiKey_R) || once(ImGuiKey_F2);
  const bool kb_duplicate = once(ImGuiKey_D);
  const bool kb_erase = once(ImGuiKey_Delete);
  const bool pad_up = key(ImGuiKey_GamepadDpadUp) || key(ImGuiKey_GamepadLStickUp);
  const bool pad_down = key(ImGuiKey_GamepadDpadDown) || key(ImGuiKey_GamepadLStickDown);
  const bool pad_left = key(ImGuiKey_GamepadDpadLeft) || key(ImGuiKey_GamepadLStickLeft);
  const bool pad_right = key(ImGuiKey_GamepadDpadRight) || key(ImGuiKey_GamepadLStickRight);
  const bool pad_accept = once(ImGuiKey_GamepadFaceDown) || once(ImGuiKey_GamepadStart);
  const bool pad_back = once(ImGuiKey_GamepadFaceRight);
  // SDL maps a GameCube pad's Z to the right shoulder.
  const bool pad_help = once(ImGuiKey_GamepadR1) || once(ImGuiKey_GamepadBack);
  const bool pad_rename = once(ImGuiKey_GamepadFaceLeft);    // X
  const bool pad_duplicate = once(ImGuiKey_GamepadFaceUp);   // Y
  in.up = kb_up || pad_up;
  in.down = kb_down || pad_down;
  in.left = kb_left || pad_left;
  in.right = kb_right || pad_right;
  in.accept = kb_accept || pad_accept;
  in.back = kb_back || pad_back;
  in.help = kb_help || pad_help;
  in.rename = kb_rename || pad_rename;
  in.duplicate = kb_duplicate || pad_duplicate;
  in.erase = kb_erase;
  const bool pad = pad_up || pad_down || pad_left || pad_right || pad_accept || pad_back ||
                   pad_help || pad_rename || pad_duplicate;
  const bool kb = kb_up || kb_down || kb_left || kb_right || kb_accept || kb_back || kb_help ||
                  kb_erase ||
                  ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (pad)
    *last_was_gamepad = true;
  else if (kb)
    *last_was_gamepad = false;
  in.gamepad = *last_was_gamepad;
  return in;
}

std::string DrawRows(ImDrawList* dl, const Canvas& c, const Fonts& f, std::vector<Row>& rows,
                     int* focus, bool page_has_focus, const Input& in, float x0, float y0,
                     float label_w, float step)
{
  using Kind = Row::Kind;
  const float vx = x0 + label_w;
  auto focusable = [&](int i) {
    return i >= 0 && i < static_cast<int>(rows.size()) && rows[i].kind != Kind::Info &&
           rows[i].enabled;
  };
  // Keep focus on a focusable row.
  if (!focusable(*focus))
  {
    int best = -1;
    for (int i = 0; i < static_cast<int>(rows.size()) && best < 0; ++i)
      if (focusable(i))
        best = i;
    *focus = best;
  }
  if (page_has_focus && *focus >= 0)
  {
    const int dir = in.down ? 1 : in.up ? -1 : 0;
    if (dir)
      for (int i = *focus + dir; i >= 0 && i < static_cast<int>(rows.size()); i += dir)
        if (focusable(i))
        {
          *focus = i;
          break;
        }
  }

  auto commit = [](Row& row, const Row& before) {
    if (row.changed && !row.changed(row))
      row = before;
  };

  std::string help;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i)
  {
    Row& row = rows[i];
    const float y = y0 + i * step;
    const bool hovered = row.kind != Kind::Info && row.enabled && MouseIn(c, x0 - 20, y - 4, 1228, y + 42);
    if (hovered && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0))
      *focus = i;
    const bool focused = *focus == i;
    if (focused)
    {
      help = row.help;
      dl->AddRectFilled(c.P(x0 - 18, y - 6), c.P(1228, y + 44), IM_COL32(255, 255, 255, 14));
      if (page_has_focus)
        Chevron(dl, c, x0 - 23, y + 9, 14);
    }
    if (row.kind == Kind::Info)
    {
      Text(dl, c, f.body, 13, x0 + 4, y + 8, row.label, row.text_color, false);
      continue;
    }
    const ImU32 label_col = row.enabled ? color::kGold : color::kDim;
    Text(dl, c, f.small, 26, x0, y + 6, row.label, label_col);
    const bool act = focused && page_has_focus && row.enabled;
    const Row before = row;
    switch (row.kind)
    {
    case Kind::Toggle:
    {
      PokeBall(dl, c, vx + 4, y + 3, row.on);
      Text(dl, c, f.menu, 36, vx + 50, y, row.on ? "ON" : "OFF",
           row.on ? color::kWhite : color::kDim);
      const bool clicked = row.enabled && ClickedIn(c, x0, y - 4, vx + 200, y + 42);
      if ((act && (in.accept || in.left || in.right)) || clicked)
      {
        row.on = !row.on;
        commit(row, before);
      }
      break;
    }
    case Kind::Choice:
    {
      const int n = static_cast<int>(row.options.size());
      Selector(dl, c, f, vx, y, n ? row.options[std::clamp(row.index, 0, n - 1)] : "",
               340, row.enabled);
      int dir = 0;
      if (act && (in.right || in.accept))
        dir = 1;
      else if (act && in.left)
        dir = -1;
      else if (row.enabled && ClickedIn(c, vx, y - 2, vx + 170, y + 36))
        dir = -1;
      else if (row.enabled && ClickedIn(c, vx + 170, y - 2, vx + 340, y + 36))
        dir = 1;
      if (dir && n > 1)
      {
        row.index = (row.index + dir + n) % n;
        commit(row, before);
      }
      break;
    }
    case Kind::Slider:
    {
      const float frac = row.max > row.min ?
                             static_cast<float>(row.value - row.min) / (row.max - row.min) : 0;
      SliderBar(dl, c, vx + 4, y + 12, 240, frac);
      Text(dl, c, f.small, 26, vx + 260, y + 2,
           row.value_text.empty() ? std::to_string(row.value) : row.value_text,
           row.enabled ? color::kWhite : color::kDim);
      int value = row.value;
      if (act && in.right)
        value += row.step;
      else if (act && in.left)
        value -= row.step;
      if (row.enabled && MouseIn(c, vx, y, vx + 252, y + 30) &&
          ImGui::IsMouseDown(ImGuiMouseButton_Left))
      {
        const float mx = (ImGui::GetIO().MousePos.x - c.origin.x) / c.scale;
        const float t = std::clamp((mx - vx - 6) / 236, 0.0f, 1.0f);
        value = row.min + static_cast<int>(std::lround(t * (row.max - row.min) / row.step)) * row.step;
      }
      value = std::clamp(value, row.min, row.max);
      if (value != row.value)
      {
        row.value = value;
        commit(row, before);
      }
      break;
    }
    case Kind::Action:
    {
      SteelPanel(dl, c, vx, y - 4, vx + 340, y + 42, 10,
                 focused ? color::kPlay : color::kPanelDark);
      const std::string& text = row.options.empty() ? row.label : row.options.front();
      Text(dl, c, f.small, 26, vx + 170 - TextWidth(f.small, 26, text) / 2, y + 4, text,
           row.enabled ? color::kWhite : color::kDim);
      if (((act && in.accept) || (row.enabled && ClickedIn(c, vx, y - 4, vx + 340, y + 42))) &&
          row.activated)
        row.activated();
      break;
    }
    case Kind::TextInput:
    {
      Selector(dl, c, f, vx, y, "", 340, false);
      ImGui::PushID(i);
      ImGui::SetCursorScreenPos(c.P(vx + 14, y));
      const ImGuiStyle& style = ImGui::GetStyle();
      ImGui::PushFont(Or(f.menu), c.S(36) / (style.FontScaleMain * style.FontScaleDpi));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Text, color::kWhite);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
      ImGui::SetNextItemWidth(c.S(312));
      if (act && in.accept)
        ImGui::SetKeyboardFocusHere();
      ImGui::InputText("##text", row.buffer, row.buffer_size,
                       row.numeric ? ImGuiInputTextFlags_CharsDecimal : 0);
      if (ImGui::IsItemActivated())
        *focus = i;
      if (ImGui::IsItemDeactivatedAfterEdit() && row.changed)
        row.changed(row);
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(2);
      ImGui::PopFont();
      ImGui::PopID();
      break;
    }
    case Kind::Info:
      break;
    }
  }
  return help;
}
}  // namespace moderngekko::frontend::theme
