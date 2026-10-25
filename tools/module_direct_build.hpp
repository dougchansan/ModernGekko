// SPDX-License-Identifier: GPL-2.0-or-later
//
// Builds a game module without CMake, Ninja or Python, so the launcher can build one on a
// player's machine from their own disc with nothing but a downloaded compiler (Zig's clang):
// module tables, the hook-aware rewrite, and a parallel compile and link.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace moderngekko::module_build
{
namespace fs = std::filesystem;

// The module-template's gen_module_tables.py: code, SMC and chunk ranges from DolRecomp's
// generated.h / generated_smc.txt, and a hash of each chunk's original text from the DOL.
bool WriteModuleTables(const fs::path& generated_h, const fs::path& smc_txt, const fs::path& dol,
                       const fs::path& out_inc, std::string* error);

struct Hook
{
  std::uint32_t address = 0;
  std::uint32_t size = 0;  // the hooked function's size in bytes
  bool has_return_hooks = false;
};

// Reads a hook list: one "ADDRESS SIZE [r]" per line (hex; r = the function has return hooks),
// '#' comments. Release packages ship one made from the bundled mods.
bool ReadHookList(const fs::path& path, std::vector<Hook>* hooks, std::string* error);

// scripts/make-hook-aware-module.py, in place on a DolRecomp output directory: calls and jumps
// to a hooked function go back through the chassis while that hook is live, and the module
// exports the hooked addresses. `idle_continuations` are return addresses of idle-loop calls
// that must go through the dispatcher (GC6E01: 0x800FEB9C). Constant branches into another
// chunk become tail calls of that chunk's function, except into `idle_loops`, the [start, end)
// ranges the runtime watches as idle loops (GC6E01: 0x800D31E0-0x800D321C).
bool MakeHookAware(const fs::path& generated_dir, const std::vector<Hook>& hooks,
                   const std::vector<std::uint32_t>& idle_continuations,
                   const std::vector<std::pair<std::uint32_t, std::uint32_t>>& idle_loops,
                   std::string* error);

struct CompileJob
{
  fs::path zig;            // zig executable; compiles with `zig cc`
  fs::path template_dir;   // vendor/dolphin/module-template
  fs::path gxruntime_dir;  // vendor/dolphin/GXRuntime
  fs::path chassis_abi_dir;
  fs::path generated_dir;  // DolRecomp output (generated.h, chunks/, main.dol, ...)
  fs::path work_dir;       // objects and module_tables.inc
  fs::path output;         // the module to write
  std::string game_id;
  std::string opt_level = "2";
  std::vector<std::string> extra_defines;  // e.g. "DOLRECOMP_C_MAX_CALL_DEPTH=24"
  std::vector<std::string> arch_flags;     // e.g. "-mfma" (module-template's ARCH_FLAGS)
  unsigned jobs = 0;
  // Called as work completes: (done, total, what). Compiles count one each; the link is the
  // last step.
  std::function<void(unsigned, unsigned, const std::string&)> progress;
};

// The zig target triple for this host's modules, e.g. "x86_64-windows-gnu".
std::string HostZigTarget();

bool CompileModule(const CompileJob& job, std::string* error);
}  // namespace moderngekko::module_build
