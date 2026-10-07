// Radeon driver settings (Anti-Lag, Chill, Boost, Image Sharpening, Enhanced
// Sync, Fluid Motion Frames) through AMD ADLX.
//
// These are driver-wide settings, so the values found before the first change
// are kept in a restore file and put back on exit (or on the next launch, if
// the game did not exit cleanly).
#pragma once

#include <filesystem>

namespace moderngekko::amd {

enum class Feature {
  AntiLag,
  Chill,
  Boost,
  ImageSharpening,
  EnhancedSync,
  FluidMotionFrames,
  Count,
};

constexpr int kFeatureCount = static_cast<int>(Feature::Count);

const char *Name(Feature feature);
// Ini key, e.g. "AntiLag".
const char *Key(Feature feature);

// Loads ADLX and restores settings a previous session left changed. During a
// graphics API switch (adopt_previous) the previous session's saved values are
// taken over instead, since it hands the overrides to this session.
void Initialize(const std::filesystem::path &restore_file, bool adopt_previous);
// Puts every changed setting back and removes the restore file.
void Shutdown();
// Keeps the current overrides and restore file for the next session.
void HandOver();

bool Available();
// -1 not supported, 0 off, 1 on (as the driver reports it).
int State(Feature feature);
// Turns a setting on or off, saving its original value first. Returns false if
// the driver refused (e.g. Chill and Anti-Lag exclude each other).
bool Set(Feature feature, bool enabled);
// Puts one setting back to the value it had before this game changed it.
void Restore(Feature feature);
// True while this game has the setting changed from the driver's value.
bool Overridden(Feature feature);

} // namespace moderngekko::amd
