// Drawing helpers for the pause menu in the style of Pokemon Colosseum's own
// menus: chamfered plates with chrome bevels, deep teal / green fills, the
// focused entry in bright green or framed in red, outlined text.
#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h>

namespace moderngekko::pause_menu::style {

inline ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// Colosseum palette (sampled from the game's Start menu and party screen).
namespace color {
inline const ImU32 kChromeLight = Rgb(244, 247, 250);
inline const ImU32 kChromeDark = Rgb(138, 144, 152);
inline const ImU32 kChromeEdge = Rgb(44, 48, 54);
inline const ImU32 kTealTop = Rgb(18, 76, 94);
inline const ImU32 kTealBottom = Rgb(8, 40, 54);
inline const ImU32 kGreenTop = Rgb(22, 74, 52);
inline const ImU32 kGreenBottom = Rgb(10, 44, 32);
inline const ImU32 kPanelFrameLight = Rgb(170, 186, 178);
inline const ImU32 kPanelFrameDark = Rgb(84, 100, 92);
inline const ImU32 kSelectRed = Rgb(206, 26, 30);
inline const ImU32 kFocusGreen = Rgb(64, 255, 64);
inline const ImU32 kDimTeal = Rgb(46, 104, 122);
inline const ImU32 kWhite = Rgb(246, 248, 250);
inline const ImU32 kYellow = Rgb(246, 206, 48);
inline const ImU32 kRed = Rgb(214, 46, 40);
inline const ImU32 kOutline = Rgb(6, 16, 20, 235);
inline const ImU32 kHeaderGreen = Rgb(120, 214, 150);
}  // namespace color

// Eight corners of a rectangle with chamfered (cut) corners, clockwise.
inline void ChamferPoints(ImVec2 a, ImVec2 b, float c, ImVec2 out[8]) {
  c = std::min({c, (b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f});
  out[0] = ImVec2(a.x + c, a.y);
  out[1] = ImVec2(b.x - c, a.y);
  out[2] = ImVec2(b.x, a.y + c);
  out[3] = ImVec2(b.x, b.y - c);
  out[4] = ImVec2(b.x - c, b.y);
  out[5] = ImVec2(a.x + c, b.y);
  out[6] = ImVec2(a.x, b.y - c);
  out[7] = ImVec2(a.x, a.y + c);
}

// Chamfered plate filled with a vertical gradient.
inline void FillChamfer(ImDrawList *draw, ImVec2 a, ImVec2 b, float c, ImU32 top, ImU32 bottom) {
  ImVec2 points[8];
  ChamferPoints(a, b, c, points);
  const int start = draw->VtxBuffer.Size;
  // The gradient keeps each vertex's alpha, so the base carries the top alpha.
  draw->AddConvexPolyFilled(points, 8, top);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, a,
                                                ImVec2(a.x, b.y), top, bottom);
}

inline void StrokeChamfer(ImDrawList *draw, ImVec2 a, ImVec2 b, float c, ImU32 color,
                          float thickness) {
  ImVec2 points[8];
  ChamferPoints(a, b, c, points);
  draw->AddPolyline(points, 8, color, ImDrawFlags_Closed, thickness);
}

// A plate with a chrome bevel (the Start menu buttons): bright metal frame,
// dark seam, then the gradient fill.
inline void ChromePlate(ImDrawList *draw, ImVec2 a, ImVec2 b, float c, float bevel, ImU32 fill_top,
                        ImU32 fill_bottom) {
  FillChamfer(draw, ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), c + 1, color::kChromeEdge,
              color::kChromeEdge);
  FillChamfer(draw, a, b, c, color::kChromeLight, color::kChromeDark);
  const ImVec2 ia(a.x + bevel, a.y + bevel), ib(b.x - bevel, b.y - bevel);
  FillChamfer(draw, ImVec2(ia.x - 1, ia.y - 1), ImVec2(ib.x + 1, ib.y + 1),
              std::max(c - bevel * 0.6f, 1.0f) + 1, color::kChromeEdge, color::kChromeEdge);
  FillChamfer(draw, ia, ib, std::max(c - bevel * 0.6f, 1.0f), fill_top, fill_bottom);
  // Gloss on the upper half of the fill.
  FillChamfer(draw, ia, ImVec2(ib.x, ia.y + (ib.y - ia.y) * 0.45f), std::max(c - bevel * 0.6f, 1.0f),
              Rgb(255, 255, 255, 28), Rgb(255, 255, 255, 4));
}

// A party-screen style panel: grey-green bevel around a dark green fill, or a
// red frame when selected.
inline void Panel(ImDrawList *draw, ImVec2 a, ImVec2 b, float c, float bevel, bool selected,
                  ImU32 fill_top = color::kGreenTop, ImU32 fill_bottom = color::kGreenBottom) {
  FillChamfer(draw, ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), c + 1, color::kChromeEdge,
              color::kChromeEdge);
  if (selected)
    FillChamfer(draw, a, b, c, Rgb(236, 60, 56), Rgb(150, 14, 18));
  else
    FillChamfer(draw, a, b, c, color::kPanelFrameLight, color::kPanelFrameDark);
  const ImVec2 ia(a.x + bevel, a.y + bevel), ib(b.x - bevel, b.y - bevel);
  FillChamfer(draw, ia, ib, std::max(c - bevel * 0.6f, 1.0f), fill_top, fill_bottom);
}

// Text with a dark outline, as all of the game's menu text has.
inline void OutlinedText(ImDrawList *draw, ImFont *font, float size, ImVec2 pos, ImU32 color,
                         std::string_view text, float outline = 1.5f,
                         ImU32 outline_color = color::kOutline) {
  const char *begin = text.data(), *end = text.data() + text.size();
  static const ImVec2 kOffsets[] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0},
                                    {-1, 1},  {0, 1},  {1, 1},  {1.5f, 2.0f}};
  for (const ImVec2 &o : kOffsets)
    draw->AddText(font, size, ImVec2(pos.x + o.x * outline, pos.y + o.y * outline), outline_color,
                  begin, end);
  draw->AddText(font, size, pos, color, begin, end);
}

// Engraved text (the unfocused Start menu entries): dark above, light below.
inline void EngravedText(ImDrawList *draw, ImFont *font, float size, ImVec2 pos, ImU32 color,
                         std::string_view text, float depth = 1.5f) {
  const char *begin = text.data(), *end = text.data() + text.size();
  draw->AddText(font, size, ImVec2(pos.x, pos.y - depth), Rgb(2, 14, 20, 220), begin, end);
  draw->AddText(font, size, ImVec2(pos.x, pos.y + depth), Rgb(120, 190, 210, 70), begin, end);
  draw->AddText(font, size, pos, color, begin, end);
}

inline ImVec2 TextSize(ImFont *font, float size, std::string_view text) {
  return font->CalcTextSizeA(size, 1e9f, 0.0f, text.data(), text.data() + text.size());
}

// Letter-spaced text (the game's message box), drawn one character at a time.
inline float SpacedWidth(ImFont *font, float size, std::string_view text, float spacing) {
  float width = 0;
  for (char ch : text)
    width += TextSize(font, size, std::string_view(&ch, 1)).x + spacing;
  return text.empty() ? 0 : width - spacing;
}

inline void SpacedText(ImDrawList *draw, ImFont *font, float size, ImVec2 pos, ImU32 color,
                       std::string_view text, float spacing, float outline = 1.5f) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    // Keep UTF-8 sequences (é) together.
    std::size_t n = 1;
    while (i + n < text.size() && (static_cast<unsigned char>(text[i + n]) & 0xC0) == 0x80)
      ++n;
    const std::string_view ch = text.substr(i, n);
    OutlinedText(draw, font, size, pos, color, ch, outline);
    pos.x += TextSize(font, size, ch).x + spacing;
    i += n - 1;
  }
}

}  // namespace moderngekko::pause_menu::style
