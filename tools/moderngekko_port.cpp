#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

#ifndef MODERNGEKKO_RECOMPCORE_REVISION
#define MODERNGEKKO_RECOMPCORE_REVISION "unknown"
#endif
#ifndef MODERNGEKKO_DOLRECOMP_REVISION
#define MODERNGEKKO_DOLRECOMP_REVISION "unknown"
#endif

namespace
{
constexpr std::string_view RECOMPCORE_REVISION = MODERNGEKKO_RECOMPCORE_REVISION;
constexpr std::string_view DOLRECOMP_REVISION = MODERNGEKKO_DOLRECOMP_REVISION;

unsigned BuildJobs()
{
  if (const char* value = std::getenv("MODERNGEKKO_BUILD_JOBS"))
  {
    unsigned jobs = 0;
    const std::string_view text(value);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), jobs);
    if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && jobs > 0)
      return jobs;
    std::cerr << "ignoring invalid MODERNGEKKO_BUILD_JOBS=" << text << '\n';
  }
  return std::max(1u, std::thread::hardware_concurrency());
}

struct BuildOptions
{
  std::string toolchain = "auto";
  // Empty means "leave the module template's own default alone". Any other
  // value is forwarded as RECOMPCORE_MODULE_OPT_LEVEL and folded into the
  // cache key, so an -O3 module cannot collide with an -O2 one.
  std::string opt_level;
  // Extra architecture flags for the generated translation units, e.g. -mfma so
  // the native SDK substitutions' fmaf calls become single instructions instead
  // of libm calls. Empty by default: these flags raise the CPU baseline, and a
  // module built with -mfma faults with an illegal instruction on a host
  // without FMA3. Folded into the cache key below, so an -mfma module can never
  // be served from the cache to a build that did not ask for one.
  std::string arch_flags;
#if defined(MODERNGEKKO_DOLRECOMP_LLVM)
  std::string backend = "llvm";
#else
  std::string backend = "c";
#endif
  // Keep non-native guest state in CPUState instead of promoting it to allocas
  // at region entry. LLVM-backend only, and the difference between an LLVM
  // module that loses to the C backend and one that beats it, so it has to be
  // selectable per build and folded into the cache key below.
  bool state_in_memory = false;
  fs::path output;
  std::vector<std::string> dol_patches;
  // "address:symbol" entries naming guest functions whose entry point the
  // emitter should guard with a call to a native routine in the GX runtime.
  // Passed explicitly rather than left to the DOLRECOMP_NATIVE_SUBST
  // environment variable, so a build is reproducible from its command line and
  // a packaging script cannot silently omit it and ship a slower module.
  std::vector<std::string> native_substitutions;
  // Profile-guided optimization, in two phases against the user's own copy of
  // the game. The profile cannot be shipped: it is instrumentation counters
  // over the recompiled game code, so it is derived from the game itself and is
  // not ours to distribute. pgo_generate names the directory the instrumented
  // module writes .profraw files to; pgo_use names the merged .profdata to
  // optimize against. Exactly one may be set.
  std::string pgo_generate;
  std::string pgo_use;
  // The module links -nostdlib -nostartfiles, so an instrumented build has to
  // be told where clang's profiling runtime is or the link fails on
  // undefined __llvm_profile_*. Discovered from the compiler when not given.
  std::string profile_runtime;
  std::vector<std::string> runner_arguments;
};

fs::path DefaultOutput()
{
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
    return fs::path(xdg) / "moderngekko" / "modules";
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".cache" / "moderngekko" / "modules";
  return "moderngekko-modules";
}

std::string Suffix()
{
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::string Quote(const fs::path& value)
{
#if defined(_WIN32)
  std::string text = value.string();
  return '"' + text + '"';
#else
  std::string text = value.string();
  std::string result = "'";
  for (char c : text)
    result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
#endif
}

std::string Trim(std::string value);
std::string ReadCommand(const std::string& command);

bool CommandOnPath(const std::string& command)
{
  return !Trim(ReadCommand(command + " --version 2>&1")).empty();
}

bool IsClangCommand(std::string_view command)
{
  const fs::path path(command);
  std::string stem = path.stem().string();
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return stem == "clang";
}

std::optional<std::string> FindClangCommand()
{
#if defined(_WIN32)
  if (const char* llvm_dir = std::getenv("LLVM_DIR"))
  {
    const fs::path candidate = fs::path(llvm_dir) / "bin" / "clang.exe";
    if (fs::is_regular_file(candidate))
      return candidate.string();
  }

  constexpr std::array common_candidates = {
      "C:\\Program Files\\LLVM\\bin\\clang.exe",
      "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe",
  };
  for (const char* candidate : common_candidates)
  {
    if (fs::is_regular_file(candidate))
      return std::string(candidate);
  }
#endif

  if (CommandOnPath("clang"))
    return std::string("clang");

  return std::nullopt;
}

std::optional<fs::path> FindWindowsResourceCompiler()
{
#if !defined(_WIN32)
  return std::nullopt;
#else
  const fs::path sdk_root = "C:\\Program Files (x86)\\Windows Kits\\10\\bin";
  std::error_code ec;
  if (!fs::is_directory(sdk_root, ec))
    return std::nullopt;

  std::optional<fs::path> best;
  for (const auto& entry : fs::directory_iterator(sdk_root, ec))
  {
    if (ec)
      break;
    if (!entry.is_directory(ec))
      continue;
    const fs::path candidate = entry.path() / "x64" / "rc.exe";
    if (!fs::is_regular_file(candidate, ec))
      continue;
    if (!best || entry.path().filename().string() > best->parent_path().parent_path().filename().string())
      best = candidate;
  }
  return best;
#endif
}

std::uint64_t Fnv1a(std::string_view value)
{
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char c : value)
    hash = (hash ^ c) * 0x100000001b3ULL;
  return hash;
}

std::string Trim(std::string value)
{
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

std::uint32_t ReadBE32(const std::uint8_t* data)
{
  return (std::uint32_t{data[0]} << 24) | (std::uint32_t{data[1]} << 16) |
         (std::uint32_t{data[2]} << 8) | data[3];
}

void WriteBE32(std::uint8_t* data, std::uint32_t value)
{
  data[0] = static_cast<std::uint8_t>(value >> 24);
  data[1] = static_cast<std::uint8_t>(value >> 16);
  data[2] = static_cast<std::uint8_t>(value >> 8);
  data[3] = static_cast<std::uint8_t>(value);
}

bool ParseHex32(std::string_view value, std::uint32_t* parsed)
{
  if (value.starts_with("0x") || value.starts_with("0X"))
    value.remove_prefix(2);
  const auto result = std::from_chars(value.data(), value.data() + value.size(), *parsed, 16);
  return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

struct DolPatch
{
  std::uint32_t address;
  std::uint32_t value;
};

struct DolPatchSet
{
  std::vector<DolPatch> entries;
  std::string fingerprint = "none";
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

void RefreshDolPatchFingerprint(DolPatchSet* patches)
{
  std::ranges::sort(patches->entries, {}, &DolPatch::address);
  std::ostringstream identity;
  identity << std::hex << std::setfill('0');
  for (const DolPatch& patch : patches->entries)
    identity << std::setw(8) << patch.address << std::setw(8) << patch.value;
  std::ostringstream fingerprint;
  fingerprint << std::hex << std::setfill('0') << std::setw(16) << Fnv1a(identity.str());
  patches->fingerprint = patches->entries.empty() ? "none" : fingerprint.str();
}

DolPatchSet LoadDefaultDolPatches(const fs::path& path)
{
  std::ifstream input(path);
  if (!input)
    return {};

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line))
    lines.push_back(Trim(std::move(line)));

  std::unordered_set<std::string> enabled;
  std::string section;
  for (const std::string& current : lines)
  {
    if (current.starts_with('[') && current.ends_with(']'))
      section = current.substr(1, current.size() - 2);
    else if (section == "OnFrame_Enabled" && current.starts_with('$'))
      enabled.insert(current.substr(1));
  }
  if (enabled.empty())
    return {};

  DolPatchSet patches;
  std::string patch_name;
  for (const std::string& current : lines)
  {
    if (current.starts_with('[') && current.ends_with(']'))
    {
      section = current.substr(1, current.size() - 2);
      patch_name.clear();
      continue;
    }
    if (section != "OnFrame")
      continue;
    if (current.starts_with('$'))
    {
      patch_name = current.substr(1);
      continue;
    }
    if (current.empty() || current.starts_with('#') || current.starts_with(';') ||
        !enabled.contains(patch_name))
      continue;

    const std::size_t first = current.find(':');
    const std::size_t second = current.find(':', first == std::string::npos ? first : first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        current.find(':', second + 1) != std::string::npos ||
        current.substr(first + 1, second - first - 1) != "dword")
    {
      patches.error = "unsupported enabled DOL patch line in " + path.string();
      return patches;
    }
    DolPatch patch{};
    if (!ParseHex32(std::string_view(current).substr(0, first), &patch.address) ||
        !ParseHex32(std::string_view(current).substr(second + 1), &patch.value))
    {
      patches.error = "malformed enabled DOL patch line in " + path.string();
      return patches;
    }
    patches.entries.push_back(patch);
  }

  RefreshDolPatchFingerprint(&patches);
  return patches;
}

bool PatchDol(const fs::path& input_path, const fs::path& output_path,
              const DolPatchSet& patches, std::string* error)
{
  std::ifstream input(input_path, std::ios::binary | std::ios::ate);
  if (!input)
  {
    *error = "can't open " + input_path.string();
    return false;
  }
  const std::streamoff input_size = input.tellg();
  if (input_size < 0x100)
  {
    *error = "malformed DOL " + input_path.string();
    return false;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input_size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(bytes.data()), input_size))
  {
    *error = "can't read " + input_path.string();
    return false;
  }

  for (const DolPatch& patch : patches.entries)
  {
    bool applied = false;
    for (std::size_t section_index = 0; section_index < 18; ++section_index)
    {
      const std::uint32_t offset = ReadBE32(bytes.data() + section_index * 4);
      const std::uint32_t address = ReadBE32(bytes.data() + 0x48 + section_index * 4);
      const std::uint32_t size = ReadBE32(bytes.data() + 0x90 + section_index * 4);
      if (patch.address < address ||
          static_cast<std::uint64_t>(patch.address) + 4 >
              static_cast<std::uint64_t>(address) + size)
        continue;
      const std::uint64_t patch_offset =
          static_cast<std::uint64_t>(offset) + patch.address - address;
      if (patch_offset + 4 > bytes.size())
      {
        *error = "DOL patch points outside the file";
        return false;
      }
      WriteBE32(bytes.data() + patch_offset, patch.value);
      applied = true;
      break;
    }
    if (!applied)
    {
      std::ostringstream message;
      message << "DOL patch address 0x" << std::hex << patch.address
              << " is outside every section";
      *error = message.str();
      return false;
    }
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
  {
    *error = "can't write " + output_path.string();
    return false;
  }
  return true;
}

std::string ReadCommand(const std::string& command)
{
#if defined(_WIN32)
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe)
    return {};
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe))
    output += buffer;
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return output;
}

bool RunCommand(const std::string& command)
{
  std::cout << "+ " << command << '\n';
#if defined(_WIN32)
  std::vector<char> command_line(command.begin(), command.end());
  command_line.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                      &startup, &process))
  {
    std::cerr << "failed to launch command: Windows error " << GetLastError() << '\n';
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  const bool got_exit_code = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return got_exit_code && exit_code == 0;
#else
  return std::system(command.c_str()) == 0;
#endif
}

fs::path SiblingExecutable(const char* argv0, std::string name)
{
  std::error_code ec;
  fs::path self = fs::weakly_canonical(argv0, ec);
#if defined(_WIN32)
  name += ".exe";
#endif
  const fs::path sibling = self.parent_path() / name;
  return fs::is_regular_file(sibling) ? sibling : fs::path(std::move(name));
}

// Find clang's profiling runtime for an instrumented (PGO training) module.
//
// The module links -nostdlib -nostartfiles, so nothing pulls this in
// automatically and the link fails on undefined __llvm_profile_instrument_target
// / __llvm_profile_runtime. The library must come from the same clang whose LLVM
// did the instrumenting, so it is derived from the compiler in use rather than
// searched for on the system: a mismatched profiling runtime is worse than a
// missing one, because it links and then misbehaves.
// CMake reads a -D cache value as an escaped string, so a Windows path lands as
// escape sequences: -fprofile-use=C:\mgt\merged.profdata loses \m and the
// compiler check fails at project() with nothing but "CMake will not be able to
// correctly generate this project". CMake accepts forward slashes on Windows,
// so normalize rather than double the backslashes.
std::string CMakePathValue(const fs::path& value)
{
  std::string text = value.string();
  for (char& character : text)
  {
    if (character == '\\')
      character = '/';
  }
  return text;
}

fs::path FindProfileRuntime(const std::string& compiler)
{
  const std::string resource_dir =
      ReadCommand(Quote(fs::path(compiler)) + " -print-resource-dir 2>&1");
  if (resource_dir.empty())
    return {};
  std::string trimmed = resource_dir;
  while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                              trimmed.back() == ' '))
    trimmed.pop_back();
  std::error_code ec;
  const fs::path base(trimmed);
#if defined(_WIN32)
  const fs::path candidates[] = {base / "lib/windows/clang_rt.profile-x86_64.lib",
                                 base / "lib/windows/clang_rt.profile-aarch64.lib"};
#elif defined(__APPLE__)
  const fs::path candidates[] = {base / "lib/darwin/libclang_rt.profile_osx.a"};
#else
  const fs::path candidates[] = {base / "lib/linux/libclang_rt.profile-x86_64.a",
                                 base / "lib/linux/libclang_rt.profile-aarch64.a"};
#endif
  for (const fs::path& candidate : candidates)
  {
    if (fs::is_regular_file(candidate, ec))
      return candidate;
  }
  return {};
}

// Locate the ModernGekko tree whose runtime sources this build should use.
//
// MODERNGEKKO_SOURCE_DIR is baked in at configure time as
// CMAKE_CURRENT_SOURCE_DIR, which stops being true the moment a build directory
// is copied between projects. Every game project here inherited a build dir
// configured against another game project, so all copies of this tool pointed
// GXRUNTIME_DIR and CHASSIS_ABI_DIR at that one tree: a module would be compiled
// against another game's runtime headers. That works only while the trees happen
// to agree, and when they diverge it surfaces as silent ABI skew at runtime
// rather than as a build error.
//
// Prefer the tree this executable actually lives in. The tool is installed at
// <source_root>/build/moderngekko-port, so walk up from it and take the first
// ancestor that looks like a ModernGekko checkout. An explicit environment
// override wins over both, and the configure-time path stays as a last resort so
// an unusual layout still builds.
fs::path ResolveSourceRoot(const char* argv0)
{
  const auto looks_like_tree = [](const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir / "vendor/dolphin/GXRuntime", ec) &&
           fs::is_directory(dir / "vendor/dolphin/module-template", ec);
  };

  if (const char* env = std::getenv("MODERNGEKKO_SOURCE_DIR"); env && *env)
  {
    const fs::path root(env);
    if (looks_like_tree(root))
      return root;
    std::cerr << "warning: MODERNGEKKO_SOURCE_DIR=" << env
              << " is not a ModernGekko tree; ignoring it\n";
  }

  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  if (!ec)
  {
    for (fs::path dir = self.parent_path();
         !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path())
    {
      if (looks_like_tree(dir))
        return dir;
    }
  }

  const fs::path configured(MODERNGEKKO_SOURCE_DIR);
  if (!looks_like_tree(configured))
  {
    std::cerr << "warning: falling back to the configure-time source directory "
              << configured
              << ", which does not look like a ModernGekko tree\n";
  }
  return configured;
}

std::string PlatformName(moderngekko::GamePlatform platform)
{
  return platform == moderngekko::GamePlatform::Wii ? "Wii (Broadway)" : "GameCube (Gekko)";
}

std::string ActiveModule(const fs::path& output, std::string_view id)
{
  std::ifstream file(output / id / "active-module.txt");
  std::string value;
  std::getline(file, value);
  return value;
}

void WriteActiveModuleMarkers(const fs::path& output, std::string_view id, std::string_view backend,
                              const fs::path& module)
{
  fs::create_directories(output / id);
  for (const fs::path marker : {output / id / "active-module.txt",
                                output / id / ("active-module-" + std::string(backend) + ".txt")})
  {
    std::ofstream active(marker);
    active << module.string() << '\n';
  }
}

std::string CachedModuleStatus(const fs::path& output,
                               const moderngekko::GameMetadata& game)
{
  const std::string active = ActiveModule(output, game.disc_id);
  if (active.empty())
    return "none";

  const fs::path module = active;
  if (!fs::is_regular_file(module))
    return "missing: " + module.string();

  std::ifstream manifest(module.parent_path() / "manifest.txt");
  std::string line;
  while (std::getline(manifest, line))
  {
    constexpr std::string_view prefix = "dol_sha256=";
    if (line.starts_with(prefix))
    {
      const bool current = line.substr(prefix.size()) == game.dol_sha256;
      return std::string(current ? "current: " : "stale: ") + module.string();
    }
  }
  return "unverified: " + module.string();
}

int Inspect(const fs::path& root, const fs::path& output)
{
  const auto result = moderngekko::InspectGame(root);
  if (!result)
  {
    std::cerr << "invalid extracted game: " << result.error << '\n';
    return 1;
  }
  const auto& game = *result.metadata;
  std::cout << "Game name: " << game.game_name << '\n'
            << "Disc ID:   " << game.disc_id << '\n'
            << "Platform:  " << PlatformName(game.platform) << '\n'
            << "Entry:     0x" << std::hex << std::setw(8) << std::setfill('0')
            << game.entry_point << std::dec << '\n'
            << "DOL SHA-256: " << game.dol_sha256 << '\n'
            << "Cached module: " << CachedModuleStatus(output, game) << '\n';
  return 0;
}

std::optional<fs::path> Build(const char* argv0, const fs::path& root,
                              BuildOptions options)
{
  const auto inspected = moderngekko::InspectGame(root);
  if (!inspected)
  {
    std::cerr << "invalid extracted game: " << inspected.error << '\n';
    return std::nullopt;
  }
  const auto& game = *inspected.metadata;
  if (options.output.empty())
    options.output = DefaultOutput();
  const fs::path source_root = ResolveSourceRoot(argv0);
  DolPatchSet patches = LoadDefaultDolPatches(
      source_root / "vendor/dolphin/Data/Sys/GameSettings" / (game.disc_id + ".ini"));
  if (!patches)
  {
    std::cerr << patches.error << '\n';
    return std::nullopt;
  }
  for (const std::string& specification : options.dol_patches)
  {
    const std::size_t separator = specification.find('=');
    DolPatch patch{};
    if (separator == std::string::npos ||
        !ParseHex32(std::string_view(specification).substr(0, separator), &patch.address) ||
        !ParseHex32(std::string_view(specification).substr(separator + 1), &patch.value))
    {
      std::cerr << "invalid --dol-patch (expected address=value): " << specification << '\n';
      return std::nullopt;
    }
    const auto existing = std::ranges::find(patches.entries, patch.address, &DolPatch::address);
    if (existing == patches.entries.end())
      patches.entries.push_back(patch);
    else
      existing->value = patch.value;
  }
  RefreshDolPatchFingerprint(&patches);

  std::string compiler;
  if (options.toolchain == "auto")
#if defined(_MSC_VER)
    compiler = FindClangCommand().value_or("cl");
#elif defined(__clang__)
    compiler = "clang";
#elif defined(__GNUC__)
    compiler = "gcc";
#else
    compiler = FindClangCommand().value_or("gcc");
#endif
  else if (options.toolchain == "clang")
  {
    const auto clang = FindClangCommand();
    compiler = clang.value_or("clang");
  }
  else if (options.toolchain == "gcc")
    compiler = "gcc";
  else if (options.toolchain == "msvc")
  {
#if defined(_WIN32)
    compiler = "cl";
#else
    std::cerr << "MSVC modules can only be built on Windows\n";
    return std::nullopt;
#endif
  }
  else
  {
    std::cerr << "unknown toolchain: " << options.toolchain << '\n';
    return std::nullopt;
  }

  const std::string compiler_identity =
      ReadCommand(Quote(fs::path(compiler)) + " --version 2>&1");
  if (compiler_identity.empty())
  {
    std::cerr << "compiler is unavailable: " << compiler << '\n';
    return std::nullopt;
  }
  const fs::path dolrecomp = SiblingExecutable(argv0, "dolrecomp");
  const auto dolrecomp_hash = moderngekko::HashFileSha256(dolrecomp);
  if (!dolrecomp_hash)
  {
    std::cerr << "DolRecomp compiler is unavailable: " << dolrecomp << '\n';
    return std::nullopt;
  }
#if defined(__x86_64__) || defined(_M_X64)
  constexpr std::string_view architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  constexpr std::string_view architecture = "aarch64";
#else
  constexpr std::string_view architecture = "unsupported";
#endif
  // The module template appends -O<level> after CMake's own
  // CMAKE_C_FLAGS_RELEASE, so this is the level that actually reaches the
  // per-translation-unit compiles.
  const std::string opt = options.opt_level.empty() ? std::string("2") : options.opt_level;
  std::string flags;
  if (IsClangCommand(compiler))
  {
    flags = "compile:-O" + opt +
            " -flto=thin -fvisibility=hidden -ffp-contract=off -fno-fast-math "
            "link:-flto=thin";
#if defined(__linux__)
    flags += " -fuse-ld=lld";
#endif
  }
  else if (compiler == "gcc")
  {
    flags = "compile:-O" + opt +
            " -fvisibility=hidden -ffp-contract=off -fno-fast-math link:no-lto";
  }
  else
  {
    flags = opt == "0" ? "compile:/Od /fp:strict" : "compile:/O2 /fp:strict";
  }
  if (!options.arch_flags.empty())
    flags += " arch:" + options.arch_flags;
  // The substitution set changes the emitted code itself - each named entry
  // point gets a guarded call to a native routine - so two builds that differ
  // only in it must not share a cache entry. Without this, switching
  // substitution sets silently returns whichever module was built first.
  //
  // --native-subst wins over the environment variable when both are given: the
  // command line is the reproducible half.
  std::string native_subst_list;
  for (const std::string& entry : options.native_substitutions)
  {
    if (!native_subst_list.empty())
      native_subst_list += ',';
    native_subst_list += entry;
  }
  if (native_subst_list.empty())
  {
    if (const char* const from_env = std::getenv("DOLRECOMP_NATIVE_SUBST"))
      native_subst_list = from_env;
  }
  // DOLRECOMP_INLINE_CACHE_OPS is read from the environment by dolrecomp and is
  // otherwise invisible to the cache key, so two builds differing only in it
  // collide in one output root and the second is silently served the first's
  // module. Fold it in so that cannot happen.
  const char* const inline_cache_ops = std::getenv("DOLRECOMP_INLINE_CACHE_OPS");
  const char* const inline_fp = std::getenv("DOLRECOMP_INLINE_FP");
  const std::string inline_fp_identity =
      (inline_fp && *inline_fp && *inline_fp != '0')
          ? std::string("|inline_fp=") + inline_fp
          : std::string();
  const std::string codegen_env_identity =
      (inline_cache_ops && *inline_cache_ops && *inline_cache_ops != '0')
          ? std::string("|inline_cache_ops=1")
          : std::string();
  const std::string subst_identity =
      native_subst_list.empty() ? std::string() : "|native_subst=" + native_subst_list;
  // An instrumented module and an optimized one are different binaries built
  // from the same sources, and an optimized one depends on the exact profile,
  // so all three cases have to be distinguishable. The profile is hashed rather
  // than named: two users' profiles share a path and describe different play.
  std::string pgo_identity;
  if (!options.pgo_generate.empty())
  {
    pgo_identity = "|pgo=generate";
  }
  else if (!options.pgo_use.empty())
  {
    const auto profile_hash = moderngekko::HashFileSha256(options.pgo_use);
    if (!profile_hash)
    {
      std::cerr << "cannot read PGO profile: " << options.pgo_use << '\n';
      return std::nullopt;
    }
    pgo_identity = "|pgo=use:" + *profile_hash;
  }
  const std::string identity = std::string(RECOMPCORE_REVISION) + subst_identity + pgo_identity + "|dolrecomp=" +
      std::string(DOLRECOMP_REVISION) + "|module-abi=" +
      std::to_string(MODERNGEKKO_MODULE_ABI_VERSION) + "|cpu-abi=" +
      std::to_string(MODERNGEKKO_CPU_ABI_VERSION) + "|" + compiler_identity + "|" +
      std::string(architecture) + "|" + flags + "|backend=" + options.backend +
      (options.state_in_memory ? std::string("|state_in_memory=1") : std::string()) +
      codegen_env_identity + inline_fp_identity + "|patches=" + patches.fingerprint + "|dolrecomp_binary=" +
      *dolrecomp_hash;
  std::ostringstream key_tail;
  key_tail << std::hex << std::setfill('0') << std::setw(16) << Fnv1a(identity);
  const std::string cache_key = game.dol_sha256 + "-" + key_tail.str();
  const fs::path artifact = options.output / game.disc_id / cache_key;
  const fs::path module = artifact / ("g" + game.disc_id + "_recomp" + Suffix());
  const fs::path module_build = artifact / "module-build";
  const fs::path built = module_build / ("g" + game.disc_id + "_recomp" + Suffix());
  if (fs::is_regular_file(module))
  {
    WriteActiveModuleMarkers(options.output, game.disc_id, options.backend, module);
    std::cout << "cache hit: " << module << '\n';
    return module;
  }

  const auto publish_module = [&]() -> std::optional<fs::path> {
    fs::create_directories(artifact);
    fs::copy_file(built, module, fs::copy_options::overwrite_existing);
    std::ofstream manifest(artifact / "manifest.txt");
    manifest << "disc_id=" << game.disc_id << '\n' << "dol_sha256=" << game.dol_sha256 << '\n'
             << "recompcore_revision=" << RECOMPCORE_REVISION << '\n'
             << "dolrecomp_revision=" << DOLRECOMP_REVISION << '\n'
             << "dolrecomp_binary_sha256=" << *dolrecomp_hash << '\n'
             << "module_abi=" << MODERNGEKKO_MODULE_ABI_VERSION << '\n'
             << "cpu_abi=" << MODERNGEKKO_CPU_ABI_VERSION << '\n'
             << "compiler=" << compiler_identity << '\n'
             << "architecture=" << architecture << '\n'
             << "flags=" << flags << '\n'
             << "backend=" << options.backend << '\n'
             << "state_in_memory=" << (options.state_in_memory ? 1 : 0) << '\n'
             << "inline_cache_ops=" << (codegen_env_identity.empty() ? 0 : 1) << '\n'
             << "inline_fp=" << (inline_fp_identity.empty() ? "0" : inline_fp) << '\n'
             << "patches=" << patches.fingerprint << '\n';
    WriteActiveModuleMarkers(options.output, game.disc_id, options.backend, module);
    std::cout << "built module: " << module << '\n';
    return module;
  };
  if (fs::is_regular_file(built))
    return publish_module();

  fs::create_directories(artifact);
  fs::path recomp_dol = game.main_dol;
  if (!patches.entries.empty())
  {
    recomp_dol = artifact / "patched-main.dol";
    std::string patch_error;
    if (!PatchDol(game.main_dol, recomp_dol, patches, &patch_error))
    {
      std::cerr << patch_error << '\n';
      return std::nullopt;
    }
    std::cout << "applied " << patches.entries.size() << " default DOL patches\n";
  }
  const fs::path generated_parent = artifact / "dolrecomp-output";
  const unsigned build_jobs = BuildJobs();
  // DolRecomp reads the substitution set from the environment, so an explicit
  // --native-subst is published into it for the child process. Set here rather
  // than at parse time so it lands after the cache lookup above, which must see
  // the same list either way.
  if (!native_subst_list.empty())
  {
#if defined(_WIN32)
    _putenv_s("DOLRECOMP_NATIVE_SUBST", native_subst_list.c_str());
#else
    setenv("DOLRECOMP_NATIVE_SUBST", native_subst_list.c_str(), 1);
#endif
  }
  std::string generate = Quote(dolrecomp) + " -j" + std::to_string(build_jobs) +
                         " --backend=" + options.backend + " ";
  if (options.state_in_memory)
    generate += "--state-in-memory ";
  if (game.platform == moderngekko::GamePlatform::GameCube)
    generate += "--cpu gekko --gamecube " + Quote(recomp_dol) + " " + Quote(generated_parent);
  else
    generate += "--cpu broadway " + Quote(recomp_dol) + " " + game.disc_id + " " +
                Quote(generated_parent);
  if (!RunCommand(generate))
    return std::nullopt;

  fs::path generated = game.platform == moderngekko::GamePlatform::Wii ?
      generated_parent / (game.disc_id + "_generated") : generated_parent / "generated";
  std::string generated_stem =
      game.platform == moderngekko::GamePlatform::Wii ? game.disc_id : "generated";
  // DolRecomp's optional title database affects output naming only. An
  // explicit --cpu broadway keeps Wii semantics even when that database is absent.
  if (!fs::is_regular_file(generated / (generated_stem + ".h")) &&
      fs::is_regular_file(generated_parent / "generated" / "generated.h"))
  {
    generated = generated_parent / "generated";
    generated_stem = "generated";
  }
  const fs::path emitted_header = generated / (generated_stem + ".h");
  if (!fs::is_regular_file(emitted_header))
  {
    std::cerr << "DolRecomp did not produce " << emitted_header << '\n';
    return std::nullopt;
  }
  if (emitted_header.filename() != "generated.h")
    fs::copy_file(emitted_header, generated / "generated.h", fs::copy_options::overwrite_existing);
  fs::copy_file(recomp_dol, generated / "main.dol", fs::copy_options::overwrite_existing);
  const fs::path emitted_smc = generated / (generated_stem + "_smc.txt");
  const fs::path normalized_smc = generated / "generated_smc.txt";
  if (fs::is_regular_file(emitted_smc))
  {
    if (emitted_smc != normalized_smc)
      fs::copy_file(emitted_smc, normalized_smc, fs::copy_options::overwrite_existing);
  }
  else
    std::ofstream{normalized_smc};

  const std::string compiler_arg = IsClangCommand(compiler) || compiler.find('\\') != std::string::npos ||
                                           compiler.find('/') != std::string::npos
                                       ? Quote(fs::path(compiler))
                                       : compiler;
  std::string configure = "cmake -E env CMAKE_NINJA_FORCE_RESPONSE_FILE=1 cmake -S " +
      Quote(source_root / "vendor/dolphin/module-template") +
      " -B " + Quote(module_build) + " -G Ninja -DCMAKE_BUILD_TYPE=Release" +
      " -DCMAKE_C_COMPILER=" + compiler_arg + " -DGAME_ID=" + game.disc_id +
      " -DGENERATED_DIR=" + Quote(generated) +
      " -DGXRUNTIME_DIR=" + Quote(source_root / "vendor/dolphin/GXRuntime") +
      (options.opt_level.empty()
           ? std::string()
           : " -DRECOMPCORE_MODULE_OPT_LEVEL=" + options.opt_level) +
      (options.arch_flags.empty()
           ? std::string()
           : " \"-DRECOMPCORE_MODULE_ARCH_FLAGS=" + options.arch_flags + "\"") +
      " -DCHASSIS_ABI_DIR=" +
      Quote(source_root / "vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp");
  if (!options.pgo_generate.empty())
  {
    fs::path runtime = options.profile_runtime;
    if (runtime.empty())
      runtime = FindProfileRuntime(compiler);
    if (runtime.empty())
    {
      std::cerr << "cannot find clang's profiling runtime for an instrumented "
                   "build.\n       Pass --profile-runtime <path to "
                   "clang_rt.profile>, from the same clang as "
                << compiler << '\n';
      return std::nullopt;
    }
    std::error_code profile_dir_ec;
    fs::create_directories(options.pgo_generate, profile_dir_ec);
    configure += " \"-DCMAKE_C_FLAGS=-fprofile-generate=" +
                 CMakePathValue(options.pgo_generate) + "\"" +
                 " -DRECOMPCORE_MODULE_PROFILE_RUNTIME=\"" + CMakePathValue(runtime) + "\"";
    std::cout << "instrumented build: profiles will be written to " << options.pgo_generate
              << "\n  set LLVM_PROFILE_FILE to a path inside it when you play, then merge"
                 " with llvm-profdata\n";
  }
  else if (!options.pgo_use.empty())
  {
    configure += " \"-DCMAKE_C_FLAGS=-fprofile-use=" + CMakePathValue(options.pgo_use) + "\"";
    std::cout << "optimizing against profile: " << options.pgo_use << '\n';
  }
#if defined(_WIN32)
  if (IsClangCommand(compiler))
  {
    if (const auto rc = FindWindowsResourceCompiler())
      configure += " -DCMAKE_RC_COMPILER=" + Quote(*rc);
  }
#endif
  if (!RunCommand(configure) ||
      !RunCommand("cmake --build " + Quote(module_build) + " -j" +
                  std::to_string(build_jobs)))
    return std::nullopt;

  if (!fs::is_regular_file(built))
  {
    std::cerr << "module build completed but did not produce " << built << '\n';
    return std::nullopt;
  }
  return publish_module();
}

void Usage()
{
  std::cerr << "usage: moderngekko-port inspect <game-root>\n"
               "       moderngekko-port build <game-root> [--backend c|llvm] [--state-in-memory] [--toolchain auto|clang|gcc|msvc] [--opt-level 0-3] [--arch-flags \"flags\"] [--output path] [--dol-patch address=value] [--native-subst address:symbol]\n"
               "              [--pgo-generate dir | --pgo-use profile.profdata] [--profile-runtime path]\n"
               "       moderngekko-port run <game-root> [build options] [-- runner options]\n";
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    Usage();
    return 2;
  }
  const std::string command = argv[1];
  const fs::path root = argv[2];
  BuildOptions options;
  bool runner_args = false;
  for (int i = 3; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (runner_args)
      options.runner_arguments.push_back(arg);
    else if (arg == "--")
      runner_args = true;
    else if (arg == "--toolchain" && i + 1 < argc)
      options.toolchain = argv[++i];
    else if (arg == "--backend" && i + 1 < argc)
      options.backend = argv[++i];
    else if (arg == "--state-in-memory")
      options.state_in_memory = true;
    else if (arg == "--no-state-in-memory")
      options.state_in_memory = false;
    else if (arg == "--opt-level" && i + 1 < argc)
      options.opt_level = argv[++i];
    else if (arg == "--arch-flags" && i + 1 < argc)
      options.arch_flags = argv[++i];
    else if (arg == "--output" && i + 1 < argc)
      options.output = argv[++i];
    else if (arg == "--dol-patch" && i + 1 < argc)
      options.dol_patches.emplace_back(argv[++i]);
    else if (arg == "--native-subst" && i + 1 < argc)
      options.native_substitutions.emplace_back(argv[++i]);
    else if (arg == "--pgo-generate" && i + 1 < argc)
      options.pgo_generate = argv[++i];
    else if (arg == "--pgo-use" && i + 1 < argc)
      options.pgo_use = argv[++i];
    else if (arg == "--profile-runtime" && i + 1 < argc)
      options.profile_runtime = argv[++i];
    else if (command == "run")
      options.runner_arguments.push_back(arg);
    else
    {
      std::cerr << "unknown or incomplete option: " << arg << '\n';
      return 2;
    }
  }
  if (options.output.empty())
    options.output = DefaultOutput();
  if (options.backend != "c" && options.backend != "llvm")
  {
    std::cerr << "unknown backend: " << options.backend << '\n';
    return 2;
  }
  if (!options.opt_level.empty() &&
      (options.opt_level.size() != 1 || options.opt_level[0] < '0' || options.opt_level[0] > '3'))
  {
    std::cerr << "opt level must be 0, 1, 2, or 3: " << options.opt_level << '\n';
    return 2;
  }
  // -O0 is legitimate for bisecting a codegen bug, but it produces a module
  // that is roughly half the speed of -O2 and nothing downstream says so. A
  // Raspberry Pi 4 module was built this way to answer a boot question, kept,
  // and then benchmarked for a day: every guest memory access went through an
  // out-of-line read_be32/bswap32 call (314 of them survived in the binary,
  // versus none at -O2), which reads as "this hardware is slow" rather than as
  // a build setting. Say so at build time.
  if (options.opt_level == "0")
  {
    std::cerr << "warning: building the module at -O0. Expect roughly half the "
                 "speed of -O2:\n"
                 "         nothing inlines, so every guest memory access costs a "
                 "function call.\n"
                 "         Use this for debugging codegen, not for anything you "
                 "will measure.\n";
  }
  if (!options.pgo_generate.empty() && !options.pgo_use.empty())
  {
    std::cerr << "--pgo-generate and --pgo-use are the two phases of one "
                 "workflow and cannot be combined\n";
    return 2;
  }
  if (!options.pgo_use.empty() && !std::filesystem::is_regular_file(options.pgo_use))
  {
    std::cerr << "--pgo-use profile does not exist: " << options.pgo_use
              << "\n       run the training phase first, then merge the .profraw"
                 " files with llvm-profdata\n";
    return 2;
  }
#if !defined(MODERNGEKKO_DOLRECOMP_LLVM)
  if (options.backend == "llvm")
  {
    std::cerr << "LLVM backend is unavailable in this build\n";
    return 2;
  }
#endif
  if (command == "inspect")
    return Inspect(root, options.output);
  if (command != "build" && command != "run")
  {
    Usage();
    return 2;
  }
  const auto module = Build(argv[0], root, options);
  if (!module)
    return 1;
  if (command == "build")
    return 0;
  std::string run = Quote(SiblingExecutable(argv[0], "moderngekko-run")) + " --game " +
                    Quote(root) + " --module " + Quote(*module);
  for (const std::string& arg : options.runner_arguments)
    run += " " + Quote(arg);
  return RunCommand(run) ? 0 : 1;
}
