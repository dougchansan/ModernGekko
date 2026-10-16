#pragma once

// A GameCube controller drawn with ImGui, traced from a front photo of the
// real thing (gc_controller_shape.inc) and shaded so it reads as an object:
// lit body, recessed stick wells, domed buttons. Pressed buttons light up and
// sink; the sticks tilt; the shoulders slide with the analog triggers. Used by
// the in-game input overlay and the launcher's controller preview.

#include <imgui.h>

namespace moderngekko::gc_art
{
enum Button : unsigned
{
  kA = 1u << 0,
  kB = 1u << 1,
  kX = 1u << 2,
  kY = 1u << 3,
  kZ = 1u << 4,
  kL = 1u << 5,
  kR = 1u << 6,
  kStart = 1u << 7,
  kUp = 1u << 8,
  kDown = 1u << 9,
  kLeft = 1u << 10,
  kRight = 1u << 11,
};

struct State
{
  unsigned buttons = 0;
  ImVec2 stick{0, 0};   // -1..1, +y down
  ImVec2 cstick{0, 0};  // -1..1, +y down
  float l = 0, r = 0;   // analog triggers, 0..1
};

// Height of the drawing (shoulders included) for a body `width` wide.
float Height(float width);

// Draws the controller with its body centred on `centre`, `width` pixels wide.
void Draw(ImDrawList* dl, ImVec2 centre, float width, const State& state, float alpha = 1.0f);
}  // namespace moderngekko::gc_art
