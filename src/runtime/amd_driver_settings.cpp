#include "amd_driver_settings.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

#ifdef MODERNGEKKO_HAVE_ADLX
#include "ADLXHelper/Windows/Cpp/ADLXHelper.h"
#include "Include/I3DSettings.h"
#include "Include/I3DSettings1.h"
#endif

namespace moderngekko::amd {
namespace {

constexpr std::array<const char *, kFeatureCount> kNames{
    "Radeon Anti-Lag", "Radeon Chill", "Radeon Boost",
    "Radeon Image Sharpening", "Enhanced Sync", "AMD Fluid Motion Frames"};
constexpr std::array<const char *, kFeatureCount> kKeys{
    "AntiLag", "Chill", "Boost", "ImageSharpening", "EnhancedSync",
    "FluidMotionFrames"};

std::filesystem::path s_restore_file;
// Driver value before this game changed it: -1 unchanged, else 0 / 1.
std::array<int, kFeatureCount> s_original{-1, -1, -1, -1, -1, -1};
std::atomic<bool> s_handed_over{false};

void WriteRestoreFile() {
  if (s_restore_file.empty())
    return;
  bool any = false;
  for (const int value : s_original)
    any |= value >= 0;
  std::error_code ec;
  if (!any) {
    std::filesystem::remove(s_restore_file, ec);
    return;
  }
  std::ofstream out(s_restore_file, std::ios::trunc);
  for (int i = 0; i < kFeatureCount; ++i)
    if (s_original[i] >= 0)
      out << kKeys[i] << '=' << s_original[i] << '\n';
}

std::array<int, kFeatureCount> ReadRestoreFile() {
  std::array<int, kFeatureCount> values{-1, -1, -1, -1, -1, -1};
  std::ifstream in(s_restore_file);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq);
    for (int i = 0; i < kFeatureCount; ++i)
      if (key == kKeys[i])
        values[i] = line.compare(eq + 1, 1, "1") == 0 ? 1 : 0;
  }
  return values;
}

#ifdef MODERNGEKKO_HAVE_ADLX
using namespace adlx;

ADLXHelper s_adlx;
bool s_ready = false;
IADLX3DSettingsServicesPtr s_services;
IADLXGPUPtr s_gpu;

// Calls fn with the feature's ADLX interface (all share IsSupported /
// IsEnabled / SetEnabled); returns fallback when it is unavailable.
template <typename Fn, typename R>
R WithFeature(Feature feature, R fallback, Fn &&fn) {
  if (!s_ready)
    return fallback;
  switch (feature) {
  case Feature::AntiLag: {
    IADLX3DAntiLagPtr p;
    return ADLX_SUCCEEDED(s_services->GetAntiLag(s_gpu, &p)) && p ? fn(p) : fallback;
  }
  case Feature::Chill: {
    IADLX3DChillPtr p;
    return ADLX_SUCCEEDED(s_services->GetChill(s_gpu, &p)) && p ? fn(p) : fallback;
  }
  case Feature::Boost: {
    IADLX3DBoostPtr p;
    return ADLX_SUCCEEDED(s_services->GetBoost(s_gpu, &p)) && p ? fn(p) : fallback;
  }
  case Feature::ImageSharpening: {
    IADLX3DImageSharpeningPtr p;
    return ADLX_SUCCEEDED(s_services->GetImageSharpening(s_gpu, &p)) && p ? fn(p)
                                                                          : fallback;
  }
  case Feature::EnhancedSync: {
    IADLX3DEnhancedSyncPtr p;
    return ADLX_SUCCEEDED(s_services->GetEnhancedSync(s_gpu, &p)) && p ? fn(p)
                                                                       : fallback;
  }
  case Feature::FluidMotionFrames: {
    IADLX3DSettingsServices1Ptr services1(s_services);
    IADLX3DAMDFluidMotionFramesPtr p;
    return services1 && ADLX_SUCCEEDED(services1->GetAMDFluidMotionFrames(&p)) && p
               ? fn(p)
               : fallback;
  }
  default:
    return fallback;
  }
}

int Query(Feature feature) {
  return WithFeature(feature, -1, [](auto &p) {
    adlx_bool supported = false, enabled = false;
    if (ADLX_FAILED(p->IsSupported(&supported)) || !supported ||
        ADLX_FAILED(p->IsEnabled(&enabled)))
      return -1;
    return enabled ? 1 : 0;
  });
}

bool Apply(Feature feature, bool enabled) {
  return WithFeature(feature, false, [enabled](auto &p) {
    return ADLX_SUCCEEDED(p->SetEnabled(enabled));
  });
}

bool Load() {
  if (ADLX_FAILED(s_adlx.Initialize()))
    return false;
  IADLXSystem *const system = s_adlx.GetSystemServices();
  IADLXGPUListPtr gpus;
  if (!system || ADLX_FAILED(system->Get3DSettingsServices(&s_services)) ||
      ADLX_FAILED(system->GetGPUs(&gpus)) || !gpus || gpus->Empty())
    return false;
  // Prefer the discrete GPU (the integrated one has its own settings).
  for (adlx_uint i = gpus->Begin(); i != gpus->End(); ++i) {
    IADLXGPUPtr gpu;
    ADLX_GPU_TYPE type = GPUTYPE_UNDEFINED;
    if (ADLX_SUCCEEDED(gpus->At(i, &gpu)) && gpu &&
        ADLX_SUCCEEDED(gpu->Type(&type)) && (type == GPUTYPE_DISCRETE || !s_gpu))
      s_gpu = gpu;
  }
  return s_gpu != nullptr;
}
#else
int Query(Feature) { return -1; }
bool Apply(Feature, bool) { return false; }
#endif

} // namespace

const char *Name(Feature feature) { return kNames[static_cast<int>(feature)]; }
const char *Key(Feature feature) { return kKeys[static_cast<int>(feature)]; }

void Initialize(const std::filesystem::path &restore_file, bool adopt_previous) {
  s_restore_file = restore_file;
  s_handed_over = false;
#ifdef MODERNGEKKO_HAVE_ADLX
  s_ready = Load();
  if (!s_ready) {
    s_services = nullptr;
    s_gpu = nullptr;
    s_adlx.Terminate();
  }
  std::fprintf(stderr, "[amd] ADLX %s\n", s_ready ? "ready" : "unavailable");
#endif
  std::error_code ec;
  if (!std::filesystem::exists(s_restore_file, ec))
    return;
  const auto saved = ReadRestoreFile();
  if (adopt_previous) {
    s_original = saved;
    return;
  }
  // A previous session ended without restoring: put the user's values back.
  for (int i = 0; i < kFeatureCount; ++i)
    if (saved[i] >= 0)
      Apply(static_cast<Feature>(i), saved[i] == 1);
  std::filesystem::remove(s_restore_file, ec);
}

void Shutdown() {
  if (!s_handed_over) {
    for (int i = 0; i < kFeatureCount; ++i)
      if (s_original[i] >= 0)
        Apply(static_cast<Feature>(i), s_original[i] == 1);
    s_original.fill(-1);
    WriteRestoreFile();
  }
#ifdef MODERNGEKKO_HAVE_ADLX
  if (s_ready) {
    s_services = nullptr;
    s_gpu = nullptr;
    s_adlx.Terminate();
    s_ready = false;
  }
#endif
}

void HandOver() { s_handed_over = true; }

bool Available() {
#ifdef MODERNGEKKO_HAVE_ADLX
  return s_ready;
#else
  return false;
#endif
}

int State(Feature feature) { return Query(feature); }

bool Set(Feature feature, bool enabled) {
  const int index = static_cast<int>(feature);
  const int current = Query(feature);
  if (current < 0)
    return false;
  if (current == (enabled ? 1 : 0))
    return true;
  if (s_original[index] < 0) {
    s_original[index] = current;
    WriteRestoreFile();
  }
  if (!Apply(feature, enabled))
    return false;
  // Back at the user's value: nothing left to restore for this one.
  if (s_original[index] == (enabled ? 1 : 0)) {
    s_original[index] = -1;
    WriteRestoreFile();
  }
  return Query(feature) == (enabled ? 1 : 0);
}

void Restore(Feature feature) {
  const int index = static_cast<int>(feature);
  if (s_original[index] < 0)
    return;
  Apply(feature, s_original[index] == 1);
  s_original[index] = -1;
  WriteRestoreFile();
}

bool Overridden(Feature feature) {
  return s_original[static_cast<int>(feature)] >= 0;
}

} // namespace moderngekko::amd
