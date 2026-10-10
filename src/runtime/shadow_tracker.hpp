// Pokemon Colosseum Shadow Pokemon tracker: reads the game's Shadow Pokemon
// definitions, their seen / snagged / purified flags and the heart gauges of
// Shadow Pokemon in the party and PC straight from guest memory (read only).
// Layout research: C:/dev/colo-perf/research/shadow/REPORT.md.
#pragma once

#include <string>
#include <vector>

namespace moderngekko::shadow {

enum class Status { Unseen, Seen, Snagged, Purified };

struct Entry {
  int shadow_id = 0;
  int species = 0;
  std::string name;     // species name in the game's text (UTF-8)
  Status status = Status::Unseen;
  // Heart gauge of the snagged Pokemon while it is still Shadow, if it is in
  // the party or the PC (0..1 of the gauge remaining), else -1.
  float heart = -1.0f;
  bool in_party = false;
};

struct Summary {
  bool valid = false;  // no save loaded (title screen, loading)
  int total = 0, seen = 0, snagged = 0, purified = 0;
  std::vector<Entry> entries;
};

// Reads everything now. Safe to call from the host thread at menu rates.
Summary Read();

}  // namespace moderngekko::shadow
