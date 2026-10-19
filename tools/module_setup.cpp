// SPDX-License-Identifier: GPL-2.0-or-later

#include "module_setup.hpp"

#include "moderngekko/game.hpp"

#include "Common/HttpRequest.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace moderngekko::frontend
{
namespace
{
// The compiler, pinned: Zig's bundled clang, lld and C runtime headers build the module on
// every platform without Visual Studio, Xcode or a system compiler. Checksums are the ones
// ziglang.org publishes for each release file.
struct ZigRelease
{
  std::string_view platform;  // directory name inside the archive, after "zig-"
  std::string_view url;
  std::string_view sha256;
};
constexpr std::string_view kZigVersion = "0.14.1";
constexpr std::array kZigReleases = {
    ZigRelease{"x86_64-windows", "https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip",
               "554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c"},
    ZigRelease{"aarch64-windows",
               "https://ziglang.org/download/0.14.1/zig-aarch64-windows-0.14.1.zip",
               "b5aac0ccc40dd91e8311b1f257717d8e3903b5fefb8f659de6d65a840ad1d0e7"},
    ZigRelease{"x86_64-linux", "https://ziglang.org/download/0.14.1/zig-x86_64-linux-0.14.1.tar.xz",
               "24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c"},
    ZigRelease{"aarch64-linux",
               "https://ziglang.org/download/0.14.1/zig-aarch64-linux-0.14.1.tar.xz",
               "f7a654acc967864f7a050ddacfaa778c7504a0eca8d2b678839c21eea47c992b"},
    ZigRelease{"aarch64-macos",
               "https://ziglang.org/download/0.14.1/zig-aarch64-macos-0.14.1.tar.xz",
               "39f3dc5e79c22088ce878edc821dedb4ca5a1cd9f5ef915e9b3cc3053e8faefa"},
    ZigRelease{"x86_64-macos", "https://ziglang.org/download/0.14.1/zig-x86_64-macos-0.14.1.tar.xz",
               "b0f8bdfb9035783db58dd6c19d7dea89892acc3814421853e5752fe4573e5f43"},
};

constexpr std::string_view HostPlatform()
{
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
  return "aarch64-windows";
#elif defined(_WIN32)
  return "x86_64-windows";
#elif defined(__APPLE__) && defined(__aarch64__)
  return "aarch64-macos";
#elif defined(__APPLE__)
  return "x86_64-macos";
#elif defined(__aarch64__)
  return "aarch64-linux";
#else
  return "x86_64-linux";
#endif
}

std::string LibrarySuffix()
{
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::string ExecutableName(std::string name)
{
#if defined(_WIN32)
  name += ".exe";
#endif
  return name;
}

fs::path ToolsDirectory(const fs::path& release_directory)
{
  return release_directory / "Tools";
}

fs::path ZigExecutable(const fs::path& user_directory, const ZigRelease& release)
{
  return user_directory / "Toolchains" /
         ("zig-" + std::string(release.platform) + "-" + std::string(kZigVersion)) /
         ExecutableName("zig");
}

// Runs a program to completion with arguments; its combined output goes to `output_line` line
// by line as it arrives. Returns the exit code, or -1 if it could not start.
int RunProcess(const std::vector<std::string>& args, const std::vector<std::pair<std::string,
                                                                                  std::string>>& env,
               const std::function<void(const std::string&)>& output_line)
{
  std::vector<const char*> argv;
  for (const std::string& arg : args)
    argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  SDL_Environment* environment = SDL_CreateEnvironment(true);
  for (const auto& [name, value] : env)
    SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
  const SDL_PropertiesID properties = SDL_CreateProperties();
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  SDL_Process* process = SDL_CreateProcessWithProperties(properties);
  SDL_DestroyProperties(properties);
  SDL_DestroyEnvironment(environment);
  if (!process)
    return -1;
  SDL_IOStream* output = SDL_GetProcessOutput(process);
  std::string pending;
  char buffer[4096];
  for (;;)
  {
    const size_t read = output ? SDL_ReadIO(output, buffer, sizeof(buffer)) : 0;
    if (read == 0)
    {
      if (!output || SDL_GetIOStatus(output) != SDL_IO_STATUS_NOT_READY)
        break;
      SDL_Delay(20);
      continue;
    }
    pending.append(buffer, read);
    for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;)
    {
      std::string line = pending.substr(0, newline);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      pending.erase(0, newline + 1);
      if (output_line)
        output_line(line);
    }
  }
  if (!pending.empty() && output_line)
    output_line(pending);
  int exit_code = -1;
  SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  return exit_code;
}

bool EnsureZig(const fs::path& user_directory, const ModuleProgress& progress, fs::path* zig,
               std::string* error)
{
  const ZigRelease* release = nullptr;
  for (const ZigRelease& candidate : kZigReleases)
  {
    if (candidate.platform == HostPlatform())
      release = &candidate;
  }
  if (!release)
  {
    *error = "building the game is not supported on this platform";
    return false;
  }
  *zig = ZigExecutable(user_directory, *release);
  std::error_code ec;
  if (fs::is_regular_file(*zig, ec))
    return true;

  const fs::path toolchains = user_directory / "Toolchains";
  fs::create_directories(toolchains, ec);
  const std::string url(release->url);
  const fs::path archive = toolchains / url.substr(url.rfind('/') + 1);
  progress("Downloading the compiler (one time)", 0, 0);
  Common::HttpRequest request(std::chrono::minutes(30),
                              [&](s64 total, s64 now, s64, s64) {
                                if (total > 0)
                                {
                                  progress("Downloading the compiler (one time)",
                                           static_cast<unsigned>(now / 1024),
                                           static_cast<unsigned>(total / 1024));
                                }
                                return true;
                              });
  const Common::HttpRequest::Response response = request.Get(url);
  if (!response || response->empty())
  {
    *error = "could not download the compiler from " + url +
             ". Check the internet connection and try again.";
    return false;
  }
  {
    std::ofstream out(archive, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(response->data()),
              static_cast<std::streamsize>(response->size()));
    if (!out)
    {
      *error = "could not save the compiler download to " + archive.string();
      return false;
    }
  }
  progress("Checking the compiler download", 0, 0);
  const auto hash = moderngekko::HashFileSha256(archive);
  if (!hash || *hash != release->sha256)
  {
    fs::remove(archive, ec);
    *error = "the compiler download did not match its published checksum; try again";
    return false;
  }

  // System tar unpacks both: Windows 10 and later ship bsdtar, which reads .zip.
  progress("Unpacking the compiler", 0, 0);
  const fs::path staging = toolchains / "unpacking";
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  std::string tar_output;
  const int tar = RunProcess({"tar", "-xf", archive.string(), "-C", staging.string()}, {},
                             [&](const std::string& line) { tar_output += line + "\n"; });
  const fs::path unpacked = staging / ("zig-" + std::string(release->platform) + "-" +
                                       std::string(kZigVersion));
  if (tar != 0 || !fs::is_regular_file(unpacked / ExecutableName("zig"), ec))
  {
    fs::remove_all(staging, ec);
    *error = "could not unpack the compiler (tar exited with " + std::to_string(tar) + ")\n" +
             tar_output;
    return false;
  }
  const fs::path destination = zig->parent_path();
  fs::remove_all(destination, ec);
  fs::rename(unpacked, destination, ec);
  fs::remove_all(staging, ec);
  fs::remove(archive, ec);
  if (ec || !fs::is_regular_file(*zig, ec))
  {
    *error = "could not install the compiler into " + destination.string();
    return false;
  }
  return true;
}

// Reads a two-column CSV (with a header) as "first<sep>second" entries.
std::vector<std::string> CsvPairs(const fs::path& path, std::string_view first,
                                  std::string_view second, char separator)
{
  std::vector<std::string> entries;
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line))
    return entries;
  const auto split = [](const std::string& text) {
    std::vector<std::string> fields;
    std::string field;
    for (char c : text)
    {
      if (c == ',')
      {
        fields.push_back(field);
        field.clear();
      }
      else if (c != '\r' && c != '"')
        field += c;
    }
    fields.push_back(field);
    return fields;
  };
  const std::vector<std::string> header = split(line);
  std::size_t a = header.size(), b = header.size();
  for (std::size_t i = 0; i < header.size(); ++i)
  {
    if (header[i] == first)
      a = i;
    if (header[i] == second)
      b = i;
  }
  if (a == header.size() || b == header.size())
    return entries;
  while (std::getline(in, line))
  {
    const std::vector<std::string> fields = split(line);
    if (fields.size() > std::max(a, b) && !fields[a].empty())
      entries.push_back(fields[a] + separator + fields[b]);
  }
  return entries;
}
}  // namespace

fs::path InstalledModule(const fs::path& user_directory, const std::string& disc_id,
                         bool sixty_fps)
{
  return user_directory / "StaticRecompModules" /
         ("g" + disc_id + (sixty_fps ? "_recomp_60fps" : "_recomp") + LibrarySuffix());
}

// The hook list a module was built with, kept beside it: a release whose mods hook other
// addresses needs the module built again, or those hooks are silently skipped.
static fs::path HookStamp(const fs::path& user_directory, const std::string& disc_id)
{
  return user_directory / "StaticRecompModules" / (disc_id + "-module-hooks.txt");
}

static std::string ReadWhole(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool HasGameModule(const fs::path& release_directory, const fs::path& user_directory,
                   const std::string& disc_id)
{
  std::error_code ec;
  const std::string name = "g" + disc_id + "_recomp" + LibrarySuffix();
  if (fs::is_regular_file(release_directory / name, ec))
    return true;
  if (!fs::is_regular_file(InstalledModule(user_directory, disc_id), ec))
    return false;
  const fs::path hooks = ToolsDirectory(release_directory) / (disc_id + "-module-hooks.txt");
  if (!fs::is_regular_file(hooks, ec))
    return true;  // a release that does not build modules: take what is there
  return ReadWhole(hooks) == ReadWhole(HookStamp(user_directory, disc_id));
}

bool CanBuildGameModule(const fs::path& release_directory)
{
  std::error_code ec;
  const fs::path tools = ToolsDirectory(release_directory);
  return fs::is_regular_file(tools / ExecutableName("moderngekko-port"), ec) &&
         fs::is_regular_file(tools / ExecutableName("dolrecomp"), ec) &&
         fs::is_directory(tools / "ModuleSource", ec);
}

bool BuildGameModule(const fs::path& game_root, const std::string& disc_id,
                     const fs::path& release_directory, const fs::path& user_directory,
                     const ModuleProgress& progress, std::string* error)
{
  if (!CanBuildGameModule(release_directory))
  {
    *error = "this release cannot build the game module (Tools folder missing)";
    return false;
  }
  fs::path zig;
  if (!EnsureZig(user_directory, progress, &zig, error))
    return false;

  const fs::path tools = ToolsDirectory(release_directory);
  const fs::path cache = user_directory / "StaticRecompModules" / "build";
  std::vector<std::string> args = {(tools / ExecutableName("moderngekko-port")).string(),
                                   "build", game_root.string(), "--backend", "c",
                                   "--opt-level", "2", "--output", cache.string(), "--zig",
                                   zig.string(), "--progress"};
  std::error_code ec;
  const fs::path hooks = tools / (disc_id + "-module-hooks.txt");
  if (fs::is_regular_file(hooks, ec))
  {
    args.insert(args.end(), {"--hook-list", hooks.string()});
    // The game's task scheduler: its idle loop has to come back through the dispatcher.
    if (disc_id == "GC6E01")
      args.insert(args.end(), {"--idle-continuation", "800FEB9C"});
  }
  for (const std::string& patch : CsvPairs(tools / (disc_id + "-native-widescreen.csv"),
                                           "address", "replacement", '='))
    args.insert(args.end(), {"--dol-patch", patch});
  for (const std::string& substitution :
       CsvPairs(tools / (disc_id + "-native-substitutions.csv"), "guest", "symbol", ':'))
    args.insert(args.end(), {"--native-subst", substitution});

  const fs::path log_path = user_directory / "Logs" / "module-build.log";
  fs::create_directories(log_path.parent_path(), ec);
  std::ofstream log(log_path, std::ios::trunc);
  std::deque<std::string> recent;
  std::optional<fs::path> built;
  progress("Converting the game's code", 0, 0);
  const int exit_code = RunProcess(
      args, {{"MODERNGEKKO_SOURCE_DIR", (tools / "ModuleSource").string()}},
      [&](const std::string& line) {
        log << line << '\n';
        recent.push_back(line);
        if (recent.size() > 30)
          recent.pop_front();
        unsigned done = 0, total = 0;
        char stage[64] = {};
        if (std::sscanf(line.c_str(), "progress %u %u %63s", &done, &total, stage) >= 2)
        {
          if (total == 0)
            progress("Converting the game's code", 0, 0);
          else if (done + 1 >= total && std::string_view(stage) == "linking")
            progress("Linking the game (the last step takes a few minutes)", done, total);
          else
            progress("Compiling the game", done, total);
        }
        for (std::string_view prefix : {"built module: ", "cache hit: "})
        {
          if (line.starts_with(prefix))
          {
            // Printed as a quoted path (std::quoted escaping: \" and \\).
            std::string path = line.substr(prefix.size());
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            {
              std::string unescaped;
              for (std::size_t i = 1; i + 1 < path.size(); ++i)
              {
                if (path[i] == '\\' && i + 2 < path.size() &&
                    (path[i + 1] == '\\' || path[i + 1] == '"'))
                  ++i;
                unescaped += path[i];
              }
              path = std::move(unescaped);
            }
            built = fs::path(path);
          }
        }
      });
  log.flush();
  if (exit_code != 0 || !built || !fs::is_regular_file(*built, ec))
  {
    std::string tail;
    for (const std::string& line : recent)
      tail += line + '\n';
    *error = "building the game module failed (exit " + std::to_string(exit_code) +
             "); details are in " + log_path.string() + "\n" + tail;
    return false;
  }
  // The 60 FPS module is the same build: the frame period is set host-side.
  for (const bool sixty : {false, true})
  {
    fs::copy_file(*built, InstalledModule(user_directory, disc_id, sixty),
                  fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
      *error = "could not install the game module: " + ec.message();
      return false;
    }
  }
  if (fs::is_regular_file(hooks, ec))
    fs::copy_file(hooks, HookStamp(user_directory, disc_id), fs::copy_options::overwrite_existing,
                  ec);
  progress("Game ready", 1, 1);
  return true;
}
}  // namespace moderngekko::frontend
