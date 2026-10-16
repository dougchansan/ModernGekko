#include "gc_controller_art.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <imgui_internal.h>  // ImGui::ShadeVertsLinearColorGradientKeepAlpha

namespace moderngekko::gc_art
{
namespace
{
struct Circle
{
  float x, y, r;
};
#include "gc_controller_shape.inc"

constexpr float kPi = 3.14159265f;

struct Color
{
  float r, g, b;
};
Color Mix(Color a, Color b, float t)
{
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
Color Scale(Color c, float k)
{
  return {std::min(255.0f, c.r * k), std::min(255.0f, c.g * k), std::min(255.0f, c.b * k)};
}

constexpr Color kGlow{255, 214, 80};

class Painter
{
public:
  Painter(ImDrawList* dl, ImVec2 centre, float width, float alpha)
      : m_dl(dl), m_c(centre), m_w(width), m_alpha(alpha)
  {
  }
  ImVec2 P(float x, float y) const { return {m_c.x + x * m_w, m_c.y + y * m_w}; }
  float S(float v) const { return v * m_w; }
  ImU32 Col(Color c, float a = 1.0f) const
  {
    return IM_COL32(static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b),
                    static_cast<int>(255 * std::clamp(a * m_alpha, 0.0f, 1.0f)));
  }
  ImDrawList* dl() const { return m_dl; }

  // A filled shape with a top-to-bottom light gradient.
  template <typename Fill>
  void Shaded(Fill fill, float top_y, float bottom_y, Color top, Color bottom, float a = 1.0f)
  {
    const int start = m_dl->VtxBuffer.Size;
    fill(Col(top, a));
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(m_dl, start, m_dl->VtxBuffer.Size,
                                                  P(0, top_y), P(0, bottom_y), Col(top, a),
                                                  Col(bottom, a));
  }

  void Poly(const ImVec2* pts, int n, float dx, float dy, ImVec2* out) const
  {
    for (int i = 0; i < n; ++i)
      out[i] = P(pts[i].x + dx, pts[i].y + dy);
  }

private:
  ImDrawList* m_dl;
  ImVec2 m_c;
  float m_w;
  float m_alpha;
};

void Octagon(Painter& p, float x, float y, float r, ImVec2* out)
{
  for (int i = 0; i < 8; ++i)
  {
    const float a = kPi / 8 + i * kPi / 4;
    out[i] = p.P(x + r * std::cos(a), y + r * std::sin(a));
  }
}

// A recessed octagonal well: dark at the top where the rim shades it, lighter
// at the bottom where light falls in.
void Well(Painter& p, float x, float y, float r)
{
  ImVec2 pts[8];
  Octagon(p, x, y + 0.003f, r + 0.004f, pts);
  p.dl()->AddConvexPolyFilled(pts, 8, p.Col({150, 120, 220}, 0.55f));  // lit lower lip
  Octagon(p, x, y, r, pts);
  p.Shaded([&](ImU32 c) { p.dl()->AddConvexPolyFilled(pts, 8, c); }, y - r, y + r,
           {34, 20, 66}, {86, 60, 150});
  p.dl()->AddPolyline(pts, 8, p.Col({30, 18, 60}, 0.9f), ImDrawFlags_Closed, p.S(0.004f));
}

// A domed round button or stick cap.
// `tint`: whether the glow also colours the face (buttons) or only rings it
// (sticks, which keep their colour while tilted).
void Dome(Painter& p, float x, float y, float r, Color base, bool pressed, float glow,
          bool tint = true)
{
  ImDrawList* dl = p.dl();
  const float sink = pressed ? 0.004f : 0.0f;
  y += sink;
  if (glow > 0)
    for (int i = 3; i >= 1; --i)
      dl->AddCircleFilled(p.P(x, y), p.S(r + 0.008f * i), p.Col(kGlow, 0.10f * glow), 48);
  dl->AddCircleFilled(p.P(x, y + (pressed ? 0.004f : 0.010f)), p.S(r), p.Col({16, 8, 30}, 0.55f),
                      48);
  const Color lit = glow > 0 && tint ? Mix(base, kGlow, 0.55f * glow) : base;
  p.Shaded([&](ImU32 c) { dl->AddCircleFilled(p.P(x, y), p.S(r), c, 48); }, y - r, y + r,
           Scale(lit, 1.25f), Scale(lit, 0.72f));
  // Specular highlight, upper left.
  dl->AddEllipseFilled(p.P(x - r * 0.32f, y - r * 0.42f), {p.S(r * 0.42f), p.S(r * 0.24f)},
                       p.Col({255, 255, 255}, 0.38f), -0.5f, 32);
  dl->AddCircle(p.P(x, y), p.S(r), p.Col(Scale(lit, 0.45f), 0.9f), 48, p.S(0.003f));
}

// Concentric grip rings on a stick cap.
void Rings(Painter& p, float x, float y, float r, Color base)
{
  for (const float k : {0.72f, 0.48f, 0.24f})
    p.dl()->AddCircle(p.P(x, y), p.S(r * k), p.Col(Scale(base, 0.7f), 0.55f), 40, p.S(0.0025f));
}

void Label(Painter& p, float x, float y, float size, const char* text, Color col)
{
  ImFont* font = ImGui::GetFont();
  const ImVec2 ext = font->CalcTextSizeA(p.S(size), FLT_MAX, 0, text);
  const ImVec2 at = p.P(x, y);
  p.dl()->AddText(font, p.S(size), {at.x - ext.x / 2, at.y - ext.y / 2}, p.Col(col, 0.8f), text);
}

// A traced button (X, Y): drop shadow, shaded face, rim and a soft highlight.
void Traced(Painter& p, const ImVec2* pts, int n, Color base, bool pressed, float glow)
{
  ImDrawList* dl = p.dl();
  ImVec2 out[64];
  n = std::min(n, static_cast<int>(std::size(out)));
  float top = 1, bottom = -1;
  for (int i = 0; i < n; ++i)
    top = std::min(top, pts[i].y), bottom = std::max(bottom, pts[i].y);
  const float sink = pressed ? 0.004f : 0.0f;
  p.Poly(pts, n, 0, pressed ? 0.005f : 0.011f, out);
  dl->AddConcavePolyFilled(out, n, p.Col({16, 8, 30}, 0.5f));
  const Color lit = glow > 0 ? Mix(base, kGlow, 0.6f * glow) : base;
  p.Poly(pts, n, 0, sink, out);
  p.Shaded([&](ImU32 c) { dl->AddConcavePolyFilled(out, n, c); }, top + sink, bottom + sink,
           Scale(lit, 1.12f), Scale(lit, 0.74f));
  dl->AddPolyline(out, n, p.Col(Scale(lit, 0.5f), 0.9f), ImDrawFlags_Closed, p.S(0.003f));
}

void Shoulder(Painter& p, float x0, float x1, float y0, float y1, float travel, Color base,
              float glow)
{
  ImDrawList* dl = p.dl();
  const float dy = travel * 0.014f;
  const Color lit = glow > 0 ? Mix(base, kGlow, 0.65f * glow) : base;
  p.Shaded([&](ImU32 c) { dl->AddRectFilled(p.P(x0, y0 + dy), p.P(x1, y1 + dy), c, p.S(0.03f)); },
           y0 + dy, y1 + dy, Scale(lit, 1.15f), Scale(lit, 0.7f));
  dl->AddRect(p.P(x0, y0 + dy), p.P(x1, y1 + dy), p.Col(Scale(lit, 0.45f), 0.9f), p.S(0.03f), 0,
              p.S(0.003f));
}
}  // namespace

float Height(float width)
{
  return width * (kBodyAspect + 0.05f);
}

void Draw(ImDrawList* dl, ImVec2 centre, float width, const State& s, float alpha)
{
  // The traced body is centred on its own box; shift down so the shoulders,
  // which stand above it, stay inside Height().
  centre.y += width * 0.025f;
  Painter p(dl, centre, width, alpha);
  const float top = -kBodyAspect / 2;
  const bool pa = s.buttons & kA, pb = s.buttons & kB, px = s.buttons & kX, py = s.buttons & kY;

  // Shoulders behind the body; Z on top of R.
  const float l = std::max(s.l, (s.buttons & kL) ? 1.0f : 0.0f);
  const float r = std::max(s.r, (s.buttons & kR) ? 1.0f : 0.0f);
  Shoulder(p, -0.40f, -0.13f, top + 0.005f, top + 0.08f, l, {176, 170, 196}, l);
  Shoulder(p, 0.13f, 0.40f, top + 0.005f, top + 0.08f, r, {176, 170, 196}, r);
  Shoulder(p, 0.17f, 0.31f, top - 0.012f, top + 0.03f, (s.buttons & kZ) ? 0.6f : 0.0f,
           {118, 104, 226}, (s.buttons & kZ) ? 1.0f : 0.0f);

  // Body: soft drop shadow, lit face, rim, and two broad glosses.
  constexpr int body_n = static_cast<int>(std::size(kShapeBody));
  ImVec2 body[body_n];
  p.Poly(kShapeBody, body_n, 0.0f, 0.018f, body);
  dl->AddConcavePolyFilled(body, body_n, p.Col({0, 0, 0}, 0.35f));
  p.Poly(kShapeBody, body_n, 0, 0, body);
  p.Shaded([&](ImU32 c) { dl->AddConcavePolyFilled(body, body_n, c); }, top, -top,
           {142, 106, 222}, {70, 44, 140});
  dl->AddPolyline(body, body_n, p.Col({40, 24, 84}, 0.95f), ImDrawFlags_Closed, p.S(0.004f));
  for (const float side : {-1.0f, 1.0f})
    for (int i = 0; i < 4; ++i)
      dl->AddEllipseFilled(p.P(side * 0.25f, top + 0.12f), {p.S(0.16f - i * 0.03f), p.S(0.07f - i * 0.012f)},
                           p.Col({255, 255, 255}, 0.035f), side * -0.15f, 40);

  // Wells for the stick, C-stick and d-pad.
  Well(p, kStickCircle.x, kStickCircle.y, kStickCircle.r * 1.32f);
  Well(p, kCCircle.x, kCCircle.y, kCCircle.r * 1.32f);
  Well(p, kDpadCircle.x, kDpadCircle.y, kDpadCircle.r * 1.18f);

  // Control stick: tilts within its well.
  {
    const float travel = kStickCircle.r * 0.45f;
    const float x = kStickCircle.x + std::clamp(s.stick.x, -1.0f, 1.0f) * travel;
    const float y = kStickCircle.y + std::clamp(s.stick.y, -1.0f, 1.0f) * travel;
    const float moved = std::min(1.0f, std::hypot(s.stick.x, s.stick.y));
    Dome(p, x, y, kStickCircle.r, {206, 206, 214}, false, moved > 0.25f ? moved * 0.8f : 0,
         false);
    Rings(p, x, y, kStickCircle.r, {206, 206, 214});
  }
  // C-stick.
  {
    const float travel = kCCircle.r * 0.42f;
    const float x = kCCircle.x + std::clamp(s.cstick.x, -1.0f, 1.0f) * travel;
    const float y = kCCircle.y + std::clamp(s.cstick.y, -1.0f, 1.0f) * travel;
    const float moved = std::min(1.0f, std::hypot(s.cstick.x, s.cstick.y));
    Dome(p, x, y, kCCircle.r * 0.86f, {238, 196, 40}, false, moved > 0.25f ? moved * 0.8f : 0,
         false);
    Rings(p, x, y, kCCircle.r * 0.86f, {238, 196, 40});
    Label(p, x, y, kCCircle.r * 0.7f, "C", {150, 110, 10});
  }
  // D-pad: a rounded cross; the pressed arm lights and dips.
  {
    const float cx = kDpadCircle.x, cy = kDpadCircle.y, reach = kDpadCircle.r * 0.92f,
                arm = kDpadCircle.r * 0.34f;
    const Color grey{208, 208, 216};
    const auto cross = [&](float dy, ImU32 c) {
      dl->AddRectFilled(p.P(cx - reach, cy - arm + dy), p.P(cx + reach, cy + arm + dy), c,
                        p.S(arm * 0.35f));
      dl->AddRectFilled(p.P(cx - arm, cy - reach + dy), p.P(cx + arm, cy + reach + dy), c,
                        p.S(arm * 0.35f));
    };
    cross(0.010f, p.Col({16, 8, 30}, 0.5f));
    p.Shaded([&](ImU32 c) { cross(0, c); }, cy - reach, cy + reach, Scale(grey, 1.12f),
             Scale(grey, 0.72f));
    const auto arm_lit = [&](float x0, float y0, float x1, float y1) {
      dl->AddRectFilled(p.P(x0, y0), p.P(x1, y1), p.Col(kGlow, 0.85f), p.S(arm * 0.3f));
    };
    if (s.buttons & kUp)
      arm_lit(cx - arm, cy - reach, cx + arm, cy - arm);
    if (s.buttons & kDown)
      arm_lit(cx - arm, cy + arm, cx + arm, cy + reach);
    if (s.buttons & kLeft)
      arm_lit(cx - reach, cy - arm, cx - arm, cy + arm);
    if (s.buttons & kRight)
      arm_lit(cx + arm, cy - arm, cx + reach, cy + arm);
    dl->AddCircleFilled(p.P(cx, cy), p.S(arm * 0.45f), p.Col(Scale(grey, 0.8f), 0.8f), 20);
  }
  // Start.
  Dome(p, kStartCircle.x, kStartCircle.y, kStartCircle.r, {214, 214, 222},
       s.buttons & kStart, (s.buttons & kStart) ? 1.0f : 0.0f);
  // Face buttons.
  Traced(p, kShapeY, static_cast<int>(std::size(kShapeY)), {216, 216, 224}, py, py ? 1.0f : 0.0f);
  Traced(p, kShapeX, static_cast<int>(std::size(kShapeX)), {216, 216, 224}, px, px ? 1.0f : 0.0f);
  Dome(p, kACircle.x, kACircle.y, kACircle.r, {34, 176, 116}, pa, pa ? 1.0f : 0.0f);
  Label(p, kACircle.x, kACircle.y + (pa ? 0.004f : 0.0f), kACircle.r * 0.85f, "A", {8, 70, 40});
  Dome(p, kBCircle.x, kBCircle.y, kBCircle.r, {214, 36, 48}, pb, pb ? 1.0f : 0.0f);
  Label(p, kBCircle.x, kBCircle.y + (pb ? 0.004f : 0.0f), kBCircle.r * 0.85f, "B", {100, 10, 16});
}
}  // namespace moderngekko::gc_art
