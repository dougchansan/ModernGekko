#include "moderngekko/hd_texture_pack.hpp"

#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_set>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/Manifest.h"
#include "UICommon/UICommon.h"

namespace moderngekko
{
namespace
{
std::mutex s_pack_mutex;

// Counting walks the whole active texture directory -- 7045 files with the pack
// installed -- and the launcher asks for status on startup and after every
// toggle. Within one process the only thing that changes that directory is an
// install or uninstall performed through this file, so cache the count against
// the directory and the installed state. A toggle flips the key and recomputes
// exactly once; repeated queries cost nothing. Guarded by s_pack_mutex, which
// both entry points already hold.
struct OverrideCountCache
{
  std::filesystem::path user_directory;
  bool installed = false;
  bool valid = false;
  std::size_t count = 0;
};

OverrideCountCache s_override_cache;

bool IsColosseumHdPack(const ResourcePack::ResourcePack& pack)
{
  const ResourcePack::Manifest* const manifest = pack.GetManifest();
  if (!manifest)
    return false;

  const bool matching_name =
      manifest->GetName().find("Colosseum HD Pack") != std::string::npos;
  const bool matching_author =
      manifest->GetAuthors() && manifest->GetAuthors()->find("Sephie") != std::string::npos;
  return matching_name && matching_author;
}

ResourcePack::ResourcePack* FindColosseumHdPack()
{
  for (ResourcePack::ResourcePack& pack : ResourcePack::GetPacks())
  {
    if (IsColosseumHdPack(pack))
      return &pack;
  }
  return nullptr;
}

// Installing the pack copies its textures into the same directory the curated
// overrides live in, so the overrides cannot be counted by listing that
// directory alone. Exclude every filename the pack provides and what remains is
// this frontend's own. A pack texture and an override that share a filename are
// the same file on disk -- the override wins -- so such a file counts as the
// pack's; the number is therefore a floor, never an overstatement.
std::size_t CountCuratedOverrides(const ResourcePack::ResourcePack& pack)
{
  std::unordered_set<std::string> from_pack;
  for (const std::string& texture : pack.GetTextures())
    from_pack.insert(std::filesystem::path(texture).filename().string());

  const std::filesystem::path textures =
      std::filesystem::path(File::GetUserPath(D_LOAD_IDX)) / "Textures";
  std::error_code ec;
  if (!std::filesystem::is_directory(textures, ec))
    return 0;

  std::size_t count = 0;
  for (std::filesystem::recursive_directory_iterator it(textures, ec), end;
       it != end && !ec; it.increment(ec))
  {
    if (it->is_regular_file(ec) &&
        !from_pack.contains(it->path().filename().string()))
    {
      ++count;
    }
  }
  return count;
}

CommunityHdTexturePackStatus QueryStatus(const std::filesystem::path& user_directory)
{
  UICommon::SetUserDirectory(user_directory.string());
  const bool all_packs_valid = ResourcePack::Init();
  ResourcePack::ResourcePack* const pack = FindColosseumHdPack();

  CommunityHdTexturePackStatus status;
  if (!pack)
  {
    if (!all_packs_valid)
      status.error = "One or more resource-pack archives are invalid.";
    return status;
  }

  status.available = true;
  status.installed = ResourcePack::IsInstalled(*pack);
  status.texture_count = pack->GetTextures().size();
  if (s_override_cache.valid && s_override_cache.installed == status.installed &&
      s_override_cache.user_directory == user_directory)
  {
    status.override_count = s_override_cache.count;
  }
  else
  {
    status.override_count = CountCuratedOverrides(*pack);
    s_override_cache = {user_directory, status.installed, true,
                        status.override_count};
  }
  status.name = pack->GetManifest()->GetName();
  status.version = pack->GetManifest()->GetVersion();
  return status;
}
}  // namespace

CommunityHdTexturePackStatus
GetCommunityHdTexturePackStatus(const std::filesystem::path& user_directory)
{
  std::scoped_lock lock(s_pack_mutex);
  return QueryStatus(user_directory);
}

bool SetCommunityHdTexturePackEnabled(const std::filesystem::path& user_directory,
                                      bool enabled, std::string* error)
{
  std::scoped_lock lock(s_pack_mutex);
  const CommunityHdTexturePackStatus status = QueryStatus(user_directory);
  ResourcePack::ResourcePack* const pack = FindColosseumHdPack();

  if (!pack)
  {
    if (!enabled)
      return true;
    if (error)
      *error = status.error.empty() ?
                   "Pokemon Colosseum HD Pack.zip is missing from User/ResourcePacks." :
                   status.error;
    return false;
  }

  if (status.installed == enabled)
    return true;

  const bool succeeded = enabled ? pack->Install(File::GetUserPath(D_LOAD_IDX)) :
                                   pack->Uninstall(File::GetUserPath(D_LOAD_IDX));
  if (!succeeded && error)
  {
    *error = pack->GetError().empty() ? "The HD texture pack operation failed." :
                                       pack->GetError();
  }
  return succeeded;
}
}  // namespace moderngekko
