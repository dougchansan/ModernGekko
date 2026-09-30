# Linux Vulkan NGX / DLSS proof of concept

This branch is the first checkpoint for adding NVIDIA DLSS to ModernGekko's
Dolphin-derived Vulkan renderer. It intentionally does **not** evaluate DLSS
Super Resolution yet. The purpose of this checkpoint is to prove that:

1. normal ModernGekko builds remain unchanged when NGX is disabled;
2. an NGX-enabled Linux x86-64 build can link against NVIDIA's public DLSS SDK;
3. ModernGekko can initialize and shut down NGX against the same Vulkan
   instance/device used by the renderer; and
4. failure to initialize NGX never prevents the normal Vulkan renderer from
   starting.

The integration is compiled out by default and is runtime opt-in even in an
NGX-enabled build.

## Hardware scope

An RTX 30-series card such as the RTX 3090 is suitable for this checkpoint and
for the next DLSS Super Resolution / DLAA stages. DLSS 5 3D-Guided Neural
Rendering is a later provider/path and is not part of this checkpoint.

## Prerequisites

Use a recent NVIDIA Linux driver with working Vulkan support and obtain the
public NVIDIA DLSS SDK checkout. The SDK root passed to CMake must contain:

- `include/nvsdk_ngx_vk.h`
- `lib/Linux_x86_64/libnvsdk_ngx.a`

A convenient local layout is:

```text
~/src/ModernGekko
~/src/NVIDIA-DLSS
```

Before building, confirm that the host sees the expected GPU:

```bash
nvidia-smi
vulkaninfo --summary
```

## Clone the feature branch

```bash
git clone --recurse-submodules \
  --branch feature/dlss-ngx-vulkan-poc \
  https://github.com/dougchansan/ModernGekko.git

cd ModernGekko
git submodule status vendor/dolphin
```

The `vendor/dolphin` submodule should resolve to the matching
`feature/dlss-ngx-vulkan-poc` commit from
`dougchansan/RecompCore-ModernGekko`.

## Baseline build: NGX disabled

Always establish the regression baseline first:

```bash
cmake -S . -B build-baseline -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build build-baseline --target moderngekko-run -j"$(nproc)"
```

Run Pokémon Colosseum using the same command/game/module paths already used on
the host and force the Vulkan backend. There should be no NGX dependency or NGX
log output.

## NGX-enabled build

Assuming the NVIDIA DLSS SDK is checked out at `~/src/NVIDIA-DLSS`:

```bash
cmake -S . -B build-ngx -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMODERNGEKKO_ENABLE_NGX=ON \
  -DMODERNGEKKO_NGX_SDK_ROOT="$HOME/src/NVIDIA-DLSS"

cmake --build build-ngx --target moderngekko-run -j"$(nproc)"
```

An NGX-enabled binary still behaves exactly like baseline unless the environment
variable below is set.

## Runtime handshake test

Prefix the host's normal Colosseum launch command with:

```bash
MODERNGEKKO_DLSS=1 <normal-moderngekko-run-command> --graphics Vulkan
```

Expected successful log text:

```text
ModernGekko NGX: Vulkan SDK initialized on NVIDIA GeForce RTX 3090.
Phase 1 only validates the SDK/driver handshake; DLSS Super Resolution
evaluation is not enabled yet.
```

If initialization fails, ModernGekko logs the NGX result and continues through
the ordinary Vulkan path. NGX writes its own diagnostic data under:

```text
$XDG_CACHE_HOME/moderngekko/ngx
```

or, when `XDG_CACHE_HOME` is unset:

```text
$HOME/.cache/moderngekko/ngx
```

## Evidence to retain from the 3090 host

For the first validation pass, keep:

- `nvidia-smi` output;
- `vulkaninfo --summary` output;
- the exact CMake configure command;
- the final build result;
- NGX initialization line or failure result;
- a normal Vulkan boot with `MODERNGEKKO_DLSS` unset;
- a Vulkan boot with `MODERNGEKKO_DLSS=1`; and
- any NGX log generated under the cache directory.

No visual improvement is expected at this checkpoint.

## Next checkpoint

After the handshake is verified, the renderer work should proceed in this
order:

1. query NGX/DLSS Super Resolution support and required Vulkan extensions
   before final device creation;
2. expose the EFB color and depth images to the DLSS pass;
3. add an output-resolution render target;
4. generate camera motion vectors and temporal jitter;
5. create/evaluate DLSS Super Resolution in Quality mode;
6. compare baseline vs DLSS image quality, GPU time, and frame time;
7. replace camera-only vectors with GX-derived per-object motion vectors;
8. add DLAA and other quality modes; and
9. keep DLSS 5 Neural Rendering behind a separate future provider so RTX
   30-series support does not complicate the initial path.

Do not distribute NVIDIA SDK binaries from this repository. Keep the SDK as an
external build dependency while the integration is experimental.
