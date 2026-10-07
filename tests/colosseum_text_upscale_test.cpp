#include "VideoCommon/ColosseumTextUpscale.h"

#include <cmath>

int main()
{
  const auto enabled = ColosseumTextUpscale::GetPlan(true, true, false, 512, 512, 1, 0);
  if (!enabled.enabled || enabled.width != 3072 || enabled.height != 3072 ||
      enabled.filter != ColosseumTextUpscale::Filter::Coverage)
    return 1;

  if (ColosseumTextUpscale::GetPlan(false, true, false, 512, 512, 1, 0).enabled)
    return 2;
  if (ColosseumTextUpscale::GetPlan(true, false, false, 512, 512, 1, 0).enabled)
    return 3;
  if (ColosseumTextUpscale::GetPlan(true, true, false, 256, 512, 1, 0).enabled)
    return 4;
  if (ColosseumTextUpscale::GetPlan(true, true, false, 512, 512, 2, 0).enabled)
    return 5;

  const auto battle_hud = ColosseumTextUpscale::GetPlan(
      true, false, true, 256, 239, 1, ColosseumTextUpscale::BATTLE_STATUS_ATLAS_HASH);
  if (!battle_hud.enabled || battle_hud.width != 1536 || battle_hud.height != 1434 ||
      battle_hud.filter != ColosseumTextUpscale::Filter::ColorCoverage)
    return 10;
  if (ColosseumTextUpscale::GetPlan(true, false, true, 256, 239, 1, 0).enabled)
    return 11;
  if (ColosseumTextUpscale::GetPlan(true, false, false, 256, 239, 1,
                                    ColosseumTextUpscale::BATTLE_STATUS_ATLAS_HASH)
          .enabled)
    return 12;

  // The coverage pass must preserve transparent counters while retaining an antialiased ramp.
  // This guards against threshold-style sharpening that exposes native texel squares.
  if (ColosseumTextUpscale::ReconstructCoverage(0.0f) != 0.0f ||
      ColosseumTextUpscale::ReconstructCoverage(1.0f) != 1.0f)
    return 6;
  if (ColosseumTextUpscale::ReconstructCoverage(0.39f) != 0.0f ||
      ColosseumTextUpscale::ReconstructCoverage(0.69f) != 1.0f)
    return 7;
  if (ColosseumTextUpscale::ReconstructCoverage(0.41f) <= 0.0f ||
      ColosseumTextUpscale::ReconstructCoverage(0.67f) >= 1.0f)
    return 8;
  if (std::abs(ColosseumTextUpscale::ReconstructCoverage(0.54f) - 0.5f) > 0.0001f)
    return 9;

  // Nine normalized taps must retain a flat field and the source's 50% contour.
  if (std::abs(ColosseumTextUpscale::CombineCoverage(1.0f, 4.0f, 4.0f) - 1.0f) >
          0.0001f ||
      std::abs(ColosseumTextUpscale::CombineCoverage(0.5f, 2.0f, 2.0f) - 0.5f) >
          0.0001f)
    return 13;

  // An empty counter texel must remain transparent even when all eight neighboring texels are
  // opaque. Conversely, an opaque center must remain opaque without neighbor support.
  if (ColosseumTextUpscale::ReconstructCoverage(
          ColosseumTextUpscale::CombineCoverage(0.0f, 4.0f, 4.0f)) != 0.0f ||
      ColosseumTextUpscale::ReconstructCoverage(
          ColosseumTextUpscale::CombineCoverage(1.0f, 0.0f, 0.0f)) != 1.0f)
    return 14;

  return 0;
}
