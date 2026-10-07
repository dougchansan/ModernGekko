#include "moderngekko/mod_loader.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int entry_hooks = 0;
int return_hooks = 0;
int event_callbacks = 0;
int runtime_callbacks = 0;
int exports_called = 0;
ModernGekkoModFunction imported_function = nullptr;

void BaseExport(CPUState *) { ++exports_called; }

void BasePatch(CPUState *state) { moderngekko_mod_return_u32(state, 77u); }

void EntryHook(CPUState *state) {
  ++entry_hooks;
  state->gpr[3] = 0xDEADBEEFu;
}

void ReturnHook(CPUState *state) {
  ++return_hooks;
  state->gpr[3] = 0xCAFEBABEu;
}

void EventCallback(CPUState *state) {
  ++event_callbacks;
  state->gpr[3] = 0x11111111u;
}

void RuntimeCallback(CPUState *state) {
  ++runtime_callbacks;
  state->gpr[3] = 0x22222222u;
}

constexpr ModernGekkoModPatch base_patches[] = {
    RECOMP_PATCH(0x80002000u, BasePatch),
};

constexpr ModernGekkoModHook base_hooks[] = {
    RECOMP_HOOK(0x80003000u, EntryHook),
    RECOMP_HOOK_RETURN(0x80003000u, ReturnHook),
};

constexpr ModernGekkoModExportEntry base_exports[] = {
    RECOMP_EXPORT("call", BaseExport),
};

constexpr ModernGekkoModEvent base_events[] = {
    RECOMP_DECLARE_EVENT("tick"),
};

const ModernGekkoModDesc base_descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    "base_mod",
    "1.2.0",
    "Base mod",
    nullptr,
    0u,
    base_patches,
    1u,
    base_hooks,
    2u,
    base_exports,
    1u,
    nullptr,
    0u,
    base_events,
    1u,
    nullptr,
    0u,
    nullptr,
    nullptr,
};

constexpr ModernGekkoModDependency dependent_dependencies[] = {
    {"base_mod", "1.1.0", 0u},
};

ModernGekkoModImportEntry dependent_imports[] = {
    RECOMP_IMPORT("base_mod", "call", &imported_function),
};

constexpr ModernGekkoModCallback dependent_callbacks[] = {
    RECOMP_CALLBACK("base_mod", "tick", EventCallback),
    RECOMP_CALLBACK("*", "runtime_start", RuntimeCallback),
};

const ModernGekkoModDesc dependent_descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    "dependent_mod",
    "2.0.0",
    "Dependent mod",
    dependent_dependencies,
    1u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    dependent_imports,
    1u,
    nullptr,
    0u,
    dependent_callbacks,
    2u,
    nullptr,
    nullptr,
};

constexpr ModernGekkoModDependency missing_dependencies[] = {
    {"not_installed", "1.0.0", 0u},
};

const ModernGekkoModDesc missing_descriptor = {
    MODERNGEKKO_MOD_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    "missing_dep_mod",
    "1.0.0",
    "Missing dependency mod",
    missing_dependencies,
    1u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    0u,
    nullptr,
    nullptr,
};
}

int main() {
  moderngekko::ModManager manager;
  if (manager.InterceptionGeneration() != 1 ||
      moderngekko::ModManager::HostCallGeneration(nullptr) != 0)
    return 21;
  const auto initial_generation = manager.InterceptionGeneration();
  const std::vector<moderngekko::ModSource> sources = {
      moderngekko::ModSource::AttachedDescriptor(&dependent_descriptor,
                                                 "dependent"),
      moderngekko::ModSource::AttachedDescriptor(&base_descriptor, "base"),
  };
  const auto loaded = manager.Load(sources, "TEST01");
  if (!loaded || loaded.loaded.size() != 2u)
    return 1;
  const auto loaded_generation = manager.InterceptionGeneration();
  if (loaded_generation != initial_generation + 4 ||
      moderngekko::ModManager::HostCallGeneration(&manager) != loaded_generation)
    return 22;
  if (loaded.loaded[0].id != "base_mod" ||
      loaded.loaded[1].id != "dependent_mod")
    return 2;
  if (imported_function != BaseExport)
    return 3;
  if (!manager.HandlesAddress(0x80002000u) ||
      !manager.HandlesAddress(0x80003000u) ||
      manager.HandlesAddress(0x80004000u))
    return 17;
  if (!manager.HandlesRange(0x80001000u, 0x80002800u) ||
      !manager.HandlesRange(0x80003000u, 0x80003004u) ||
      manager.HandlesRange(0x80004000u, 0x80005000u) ||
      manager.HandlesRange(0x80002000u, 0x80002000u))
    return 20;
  // These addresses collide with the registered patch and hook filter bits.
  if (manager.HandlesAddress(0x80006004u) ||
      manager.HandlesAddress(0x80007004u))
    return 32;
  imported_function(nullptr);
  if (exports_called != 1)
    return 4;

  CPUState state{};
  state.lr = 0x80004000u;
  state.gpr[3] = 5u;
  if (!manager.Dispatch(&state, 0x80002000u))
    return 5;
  if (state.gpr[3] != 77u || state.pc != state.lr || runtime_callbacks != 1)
    return 6;

  if (manager.InterceptionGeneration() != loaded_generation)
    return 23;
  state.gpr[3] = 9u;
  if (manager.Dispatch(&state, 0x80003000u))
    return 7;
  if (entry_hooks != 1 || state.gpr[3] != 9u)
    return 8;
  if (!manager.HandlesAddress(state.lr))
    return 18;
  const auto pushed_generation = manager.InterceptionGeneration();
  if (pushed_generation != loaded_generation + 1)
    return 24;
  // A second return at the same address must still invalidate cached results.
  manager.Dispatch(&state, 0x80003000u);
  if (manager.InterceptionGeneration() != pushed_generation + 1)
    return 25;
  manager.Dispatch(&state, state.lr);
  if (!manager.HandlesAddress(state.lr) ||
      manager.InterceptionGeneration() != pushed_generation + 2)
    return 26;
  if (manager.Dispatch(&state, state.lr))
    return 9;
  if (return_hooks != 2 || state.gpr[3] != 9u)
    return 10;
  if (manager.HandlesAddress(state.lr))
    return 19;
  if (manager.InterceptionGeneration() != pushed_generation + 3)
    return 27;
  if (manager.HandlesAddress(0x8000800cu))
    return 33;
  const auto settled_generation = manager.InterceptionGeneration();
  manager.Dispatch(&state, state.lr);
  manager.HandlesRange(0x80004000u, 0x80005000u);
  if (manager.InterceptionGeneration() != settled_generation)
    return 28;
  // A pending return sharing a patch bit remains visible, then becomes stale.
  state.lr = 0x80006004u;
  manager.Dispatch(&state, 0x80003000u);
  if (!manager.HandlesAddress(state.lr) ||
      !manager.HandlesAddress(0x80002000u))
    return 34;
  manager.Dispatch(&state, state.lr);
  if (manager.HandlesAddress(state.lr) ||
      !manager.HandlesAddress(0x80002000u))
    return 35;
  // Overflow evicts a distinct first return without clearing any filter bits.
  state.lr = 0x80009000u;
  manager.Dispatch(&state, 0x80003000u);
  if (!manager.HandlesAddress(state.lr))
    return 36;
  state.lr = 0x80004000u;
  for (unsigned i = 0; i < 4095; ++i)
    manager.Dispatch(&state, 0x80003000u);
  const auto full_generation = manager.InterceptionGeneration();
  manager.Dispatch(&state, 0x80003000u);
  if (manager.HandlesAddress(0x80009000u) ||
      !manager.HandlesAddress(state.lr))
    return 37;
  if (manager.InterceptionGeneration() != full_generation + 2)
    return 29;

  if (!manager.TriggerEvent("base_mod", "tick", &state))
    return 11;
  if (event_callbacks != 1 || state.gpr[3] != 9u)
    return 12;

  const auto before_unload = manager.InterceptionGeneration();
  manager.Unload();
  if (manager.InterceptionGeneration() <= before_unload ||
      manager.HandlesAddress(state.lr) || manager.HandlesAddress(0x80002000u) ||
      manager.HandlesAddress(0x80006004u))
    return 30;
  if (imported_function != nullptr)
    return 13;
  const auto rejected =
      manager.Load({moderngekko::ModSource::AttachedDescriptor(
                       &missing_descriptor, "missing")},
                   "TEST01");
  if (rejected || rejected.issues.empty() || !manager.Empty())
    return 14;

  const auto before_dynamic = manager.InterceptionGeneration();
  const auto dynamic =
      manager.LoadDirectories({MODERNGEKKO_TEST_MOD_DIR}, "TEST01");
  if (!dynamic || dynamic.loaded.size() != 1u ||
      dynamic.loaded[0].id != "dynamic_fixture")
    return 15;
  if (manager.InterceptionGeneration() != before_dynamic + 2 ||
      !manager.HandlesAddress(0x80001000u))
    return 31;
  if (manager.HandlesAddress(0x80002000u) ||
      manager.HandlesAddress(0x80003000u) ||
      manager.HandlesAddress(0x80004000u))
    return 38;
  state.lr = 0x80005000u;
  if (!manager.Dispatch(&state, 0x80001000u) || state.gpr[3] != 999u ||
      state.pc != state.lr)
    return 16;

  return 0;
}
