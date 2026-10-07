#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace moderngekko
{
struct CommunityHdTexturePackStatus
{
  bool available = false;
  bool installed = false;
  std::size_t texture_count = 0;
  // Files sitting in the active texture directory that the pack does not
  // provide -- this frontend's own curated overrides. Counted rather than
  // assumed, because whether any ship is a packaging decision.
  std::size_t override_count = 0;
  std::string name;
  std::string version;
  std::string error;
};

CommunityHdTexturePackStatus
GetCommunityHdTexturePackStatus(const std::filesystem::path& user_directory);

bool SetCommunityHdTexturePackEnabled(const std::filesystem::path& user_directory,
                                      bool enabled, std::string* error);
}  // namespace moderngekko
