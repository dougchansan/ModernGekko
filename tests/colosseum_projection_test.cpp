#include "VideoCommon/ColosseumProjection.h"

#include <cmath>
#include <limits>

namespace
{
bool NearlyEqual(float left, float right)
{
  return std::abs(left - right) < 0.0001f;
}
}

int main()
{
  using VideoCommon::GetColosseumAuthoredMenuHorizontalScale;
  using VideoCommon::GetColosseumNamingHorizontalScale;
  using VideoCommon::GetColosseumPresentationAspect;
  using VideoCommon::IsColosseumSquareOffscreenViewport;

  if (!NearlyEqual(VideoCommon::COLOSSEUM_AUTHORED_MENU_MINIMUM_SCALE,
                   4.0f / 3.0f))
  {
    return 4;
  }

  if (!NearlyEqual(GetColosseumAuthoredMenuHorizontalScale(32.0f / 9.0f, false), 1.0f) ||
      !NearlyEqual(GetColosseumAuthoredMenuHorizontalScale(0.0f, true), 1.0f) ||
      !NearlyEqual(GetColosseumAuthoredMenuHorizontalScale(
                       std::numeric_limits<float>::infinity(), true),
                   1.0f))
  {
    return 1;
  }

  if (!NearlyEqual(GetColosseumAuthoredMenuHorizontalScale(4.0f / 3.0f, true), 1.0f) ||
      !NearlyEqual(GetColosseumAuthoredMenuHorizontalScale(16.0f / 9.0f, true),
                   4.0f / 3.0f) ||
      !NearlyEqual(GetColosseumAuthoredMenuHorizontalScale(32.0f / 9.0f, true),
                   8.0f / 3.0f))
  {
    return 2;
  }

  if (!IsColosseumSquareOffscreenViewport(192.0f, -192.0f) ||
      !IsColosseumSquareOffscreenViewport(-128.0f, 128.0f) ||
      IsColosseumSquareOffscreenViewport(320.0f, -240.0f) ||
      IsColosseumSquareOffscreenViewport(16.0f, -16.0f) ||
      IsColosseumSquareOffscreenViewport(std::numeric_limits<float>::infinity(), 192.0f))
  {
    return 3;
  }

  // Colosseum's VI exposes only 602 of the 640 authored horizontal pixels, so
  // the raw VI aspect is 602:480 and stock Dolphin presents it that way. This
  // override forces the full 640:480 canvas instead.
  //
  // Read the flag as "the naming screens", not "every authored menu". The
  // caller in dolphin_runtime.cpp feeds GFX_WIDESCREEN_AUTHORED_MENU from
  // character_naming_active alone, so ordinary authored menus and gameplay do
  // present at 602:480. That was checked against the running game and looks
  // correct on screen -- the 38-pixel difference is the game's own horizontal
  // blanking, not a squeeze. Do not "fix" the wiring to cover all authored
  // menus on the strength of this test: it would change presentation for the
  // whole game, and the current output is the one that was eyeballed and
  // accepted.
  if (!NearlyEqual(GetColosseumPresentationAspect(602.0f / 480.0f, false),
                   602.0f / 480.0f) ||
      !NearlyEqual(GetColosseumPresentationAspect(602.0f / 480.0f, true),
                   4.0f / 3.0f) ||
      !NearlyEqual(GetColosseumPresentationAspect(16.0f / 9.0f, true),
                   4.0f / 3.0f))
  {
    return 5;
  }

  if (!NearlyEqual(GetColosseumNamingHorizontalScale(false), 1.0f) ||
      !NearlyEqual(GetColosseumNamingHorizontalScale(true), 602.0f / 640.0f))
  {
    return 6;
  }

  return 0;
}
