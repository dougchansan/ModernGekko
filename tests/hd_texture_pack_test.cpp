#include "moderngekko/hd_texture_pack.hpp"

#include <chrono>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
  namespace fs = std::filesystem;
  const fs::path directory =
      fs::temp_directory_path() /
      ("moderngekko-hd-pack-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(directory / "ResourcePacks");

  if (argc == 1)
  {
    const auto status = moderngekko::GetCommunityHdTexturePackStatus(directory);
    fs::remove_all(directory);
    return status.available || status.installed ? 1 : 0;
  }

  fs::copy_file(argv[1], directory / "ResourcePacks" / "Pokemon Colosseum HD Pack.zip");
  auto status = moderngekko::GetCommunityHdTexturePackStatus(directory);
  if (!status.available || status.installed || status.texture_count != 7045)
    return 2;

  std::string error;
  if (!moderngekko::SetCommunityHdTexturePackEnabled(directory, true, &error))
    return 3;
  status = moderngekko::GetCommunityHdTexturePackStatus(directory);
  if (!status.installed || !fs::is_directory(directory / "Load" / "Textures" / "GC6"))
    return 4;

  if (!moderngekko::SetCommunityHdTexturePackEnabled(directory, false, &error))
    return 5;
  status = moderngekko::GetCommunityHdTexturePackStatus(directory);
  if (status.installed || fs::exists(directory / "Load" / "Textures" / "GC6"))
    return 6;

  fs::remove_all(directory);
  return 0;
}
