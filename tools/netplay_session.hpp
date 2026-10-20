#pragma once

#include "frontend_config.hpp"
#include "moderngekko/runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace moderngekko::frontend {
enum class NetplayRole {
  Host,
  Join,
};

enum class NetplayExitCode {
  Failed = 1,
  InvalidConfiguration = 2,
  HostUnavailable = 10,
  VersionMismatch = 11,
  CompatibilityMismatch = 12,
  RoomFull = 13,
  GameRunning = 14,
  ServerFull = 15,
  NicknameRejected = 16,
};

struct NetplayOptions {
  NetplayRole role = NetplayRole::Join;
  std::string address = "127.0.0.1";
  std::uint16_t port = 2626;
  std::string nickname = "Player";
  std::string buffer = "auto";
  std::vector<std::string> controllers;
  // Unattended sessions (testing two machines from a script): mark this
  // player ready on its own, and on the host start once everyone is ready.
  bool auto_ready = false;
  bool auto_start = false;
  // The host alone can satisfy CanStart(), because it holds both controller
  // slots until someone else takes one - so an unattended host starts before
  // the other machine has finished connecting, and the joiner is told the
  // game is already running. Wait for the lobby to hold this many players.
  int auto_start_players = 1;
  // Game Boy Advance link: every player runs the GBA in lockstep, so all of
  // them need this ROM. The host also names the port (0-3) that is a GBA and
  // sends its save for that port to the others.
  std::string gba_rom;
  int gba_port = -1;
};

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options);
} // namespace moderngekko::frontend
