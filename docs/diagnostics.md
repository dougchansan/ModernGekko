# Performance diagnostics

ModernGekko can record what the runtime was actually doing while a game ran and
write it to a single shareable file. The point is to answer the question "why is
this recomp fast on one machine and slow on another" with evidence instead of a
frame-rate number.

A report distinguishes, among other things, a guest/StaticRecomp CPU
bottleneck from a GPU bottleneck, a CPU-to-GPU synchronization stall, renderer
submission cost, GX processing, vertex conversion, texture decoding, EFB
traffic, shader and pipeline compilation hitches, interpreter fallbacks, MMIO
slow paths, audio and scheduler waits, netplay waits, and a single saturated
thread on an otherwise idle machine.

## Enabling diagnostics

```
moderngekko-run --game <extracted-root> --module <module> --diagnostics
```

Diagnostics are off unless asked for. With them off the instrumentation costs a
relaxed load of one global and a predictable branch, and nothing is written to
disk.

### Command line options

| Option | Meaning |
| --- | --- |
| `--diagnostics` | Enable diagnostics at the default `basic` level. |
| `--diagnostics-level <basic\|detailed\|trace>` | Choose how much is recorded. |
| `--diagnostics-overlay` | Enable diagnostics and show the on-screen overlay. |
| `--diagnostics-output <directory>` | Where reports are written. Defaults to `<user-dir>/Diagnostics`. |
| `--diagnostics-capture <seconds>` | Start capturing at boot and stop automatically after this many seconds. |
| `--diagnostics-history <seconds>` | Size of the rolling history buffer. Default 30 s. |
| `--diagnostics-sample-hz <hz>` | Guest PC sampling rate at `trace` level. Default 500 Hz, clamped to 50-2000. |
| `--diagnostics-symbols <file>` | A `recomp_symbols.json` sidecar used to name guest hotspots. |
| `--diagnostics-no-anonymize` | Keep paths and adapter strings verbatim. Off by default; see [Privacy](#privacy). |

### Capture controls

While diagnostics are enabled the runner reads single-letter commands from
standard input:

| Key | Effect |
| --- | --- |
| `c` + Enter | Start a capture, or stop the running one and write a report. |
| `h` + Enter | Write the rolling history buffer (the last ~30 s) without disturbing a running capture. |
| `o` + Enter | Toggle the on-screen overlay. |

The console loop works on every platform, including headless hosts with no game
window. In-window F9/F10/F3 hotkeys are not wired up yet; see
[Limitations](#limitations).

`--diagnostics-capture <seconds>` needs no interaction at all: it starts at boot
and writes the report when the timer expires.

## Levels

| Level | What is recorded | Overhead |
| --- | --- | --- |
| `basic` | Per-frame timing, emulation speed, and every counter (dispatches, fallbacks, draw calls, MMIO, EFB, texture and shader activity). No scope timers. | About 2 ns per instrumented call; not measurable at frame scale. |
| `detailed` | Everything in `basic`, plus per-subsystem scope timing for guest execution, GX, vertex conversion, texture decoding and shader generation. | Roughly 60 ns per timed scope; these scopes are coarse (once per FIFO burst, draw, texture or shader), so the cost stays well under a percent of a frame. |
| `trace` | Everything in `detailed`, plus scope timing on the hot paths (MMIO, interpreter fallback, scheduler, mod host calls) and periodic guest PC sampling for a hotspot profile. | Noticeably higher. Use it to find a hotspot, not to measure a frame rate. |

Measured on the development host with an unrolled loop around the macros:
counters cost 2.1 ns enabled and 2.2 ns disabled; a scope timer costs 2.5 ns
when its level is inactive and 59 ns when active.

Compare like with like: a `trace` capture is not a fair performance sample
against a `basic` one, and `moderngekko-diag compare` flags the mismatch.

## The report file

A capture writes one `.mgdiag` file. It is a ZIP container with stored
(uncompressed) entries, so any unzip tool opens it.

| File | Contents |
| --- | --- |
| `report.json` | Schema version, build and game identity, frame statistics, analyzer verdict. |
| `summary.json` | Frame statistics, milliseconds per frame by zone, verdict. |
| `system.json` | Host CPU model, cores, instruction sets, memory, OS. |
| `build.json` | ModernGekko commit, working-tree state, configuration, compiler, LTO. |
| `game.json` | Disc ID, platform, DOL/REL/asset SHA-256, entry point, recomp module hash. |
| `config.json` | Graphics backend and settings, diagnostics settings. |
| `frames.csv` | One row per presented frame: time, frame time, FPS, VPS, speed, every zone, every counter. |
| `threads.csv` | Per-thread CPU seconds, utilization and zone attribution. |
| `events.jsonl` | Timestamped events: capture start/stop, shader and pipeline compilations, long frames. |
| `counters.json` | Counter totals for the capture. |
| `hotspots.csv` | `guest_pc,symbol,samples,percent` from guest PC sampling (`trace` only). |
| `runtime.log` | Captured runtime log lines. |
| `README.txt` | A plain-text description of the above. |

The layout is versioned: `report.json` carries `"schema_version": 1`. A reader
refuses a report whose schema is newer than it understands rather than
misreading it.

### Frame statistics

Averages hide stutter, so the report keeps per-frame data and derives FPS,
average/median frame time, P90, P95, P99, P99.9 (at 1000 samples or more),
maximum frame time, 1% low and 0.1% low (at 1000 samples or more), and the
percentage of frames over the target budget.

Emulation speed is recorded alongside the frame rate, because a GameCube title
that renders at 30 FPS by design and one that renders at 30 FPS because the
emulator is running at half speed are different problems. The analyzer says
which one it is.

## The analyzer

Every report carries a verdict produced by a deterministic, offline rule set.
There are no remote services and no model in the loop; the same telemetry always
produces the same verdict. Each verdict names its evidence and its confidence:

```
LIKELY BOTTLENECK:
  CPU <-> GPU synchronization bound
CONFIDENCE:
  HIGH
EVIDENCE:
  - Average guest CPU: 7.80 ms
  - Average GPU execution: 5.10 ms
  - Average GPU wait: 8.90 ms
  - Waits consumed 37.1% of frame time
  - Interpreter fallbacks: 0
  - Shader compilations during capture: 0
```

When the evidence does not support a verdict the analyzer says
`Insufficient evidence` rather than guessing. It also refuses to attribute a
subsystem at `basic` level, where no scope timings exist.

## Reading and comparing reports

`moderngekko-diag` reads reports without launching a game:

```
moderngekko-diag info      report.mgdiag     # identity: game, build, host
moderngekko-diag summarize report.mgdiag     # statistics, counters, verdict
moderngekko-diag compare   a.mgdiag b.mgdiag # side-by-side
```

`compare` first checks that the two captures describe the same thing: disc ID,
DOL/REL/asset hashes, recomp module hash, ModernGekko commit, build
configuration, graphics backend, internal resolution and diagnostics level. Any
difference is printed as a warning before the numbers, and the tool exits with
status 3 so scripts do not silently compare unrelated captures.

```
                        SYSTEM A                  SYSTEM B
CPU                     AMD Ryzen 9 9950X3D       AMD Ryzen 5 3400G
GPU                     Radeon RX 9700 XT         Radeon Vega 11

Average speed           100.0 %                   71.0 %
Average FPS             60.00                     42.80
P99 frame               17.20 ms                  31.90 ms
Guest CPU               2.80 ms                   7.60 ms
GX                      0.90 ms                   2.30 ms
Renderer CPU            0.70 ms                   2.10 ms
GPU execution           1.80 ms                   5.20 ms
Synchronization         0.30 ms                   9.10 ms
Interpreter fallbacks   0                         0

DOMINANT DIFFERENCE:
  Synchronization +8.80 ms/frame
```

## Privacy

Reports are meant to be attached to a public issue, so by default they contain
no game content and no personal information. A report never contains DOL or REL
bytes, disc image content, textures, save files, memory dumps, screenshots,
controller serial numbers or authentication material.

With anonymization on (the default):

* Home directory prefixes become `<USER>`, so `C:\Users\Doug\Games\...` is
  reported as `<USER>/Games/...`.
* The host user name is stripped wherever it appears in text.
* IPv4 addresses are replaced with `<IP>`, so netplay captures carry no peer
  addresses.

Games are identified by SHA-256 alone: the disc ID plus the hashes of
`sys/main.dol`, `sys/main.rel` and the extracted `files` tree, and the SHA-256 of
the loaded recomp module when it is a file on disk. That is enough to prove two
reports tested identical content without shipping any of it.

`--diagnostics-no-anonymize` turns the redaction off. Only use it for a report
you are keeping to yourself.

## Reporting a performance problem

1. Use the same ModernGekko and recomp module build you want investigated.
2. Launch with diagnostics enabled:
   `moderngekko-run --game <root> --module <module> --diagnostics-level detailed`
3. Play up to the section that runs badly.
4. Press `c` + Enter to start the capture.
5. Play for 20-60 seconds.
6. Press `c` + Enter again. The runner prints the path of the written report.
7. Attach the `.mgdiag` file to the issue.
8. Say what performance you expected and what you observed.

If the problem is a sudden hitch rather than a steady slowdown, press `h` +
Enter right after it happens instead: that writes the last 30 seconds from the
rolling history buffer.

## Limitations

* GPU execution time, fence waits, present time, EFB and texture upload counters
  are reported through the diagnostics API but are only populated where the
  graphics backend feeds them. Backends that do not support timestamp queries
  report no GPU execution time, and the analyzer treats it as unavailable rather
  than guessing.
* Per-thread CPU time is published by each thread for itself, because reading
  another thread's CPU time is not portable. Threads that never call into
  diagnostics are reported as `unavailable` instead of being omitted or faked.
* In-window F9/F10/F3 hotkeys are not implemented; capture control goes through
  the runner's console commands described above.
* The overlay is drawn through Dolphin's on-screen message system and refreshes
  about twice a second. There is no scrolling frame-time graph yet.
* Guest PC sampling covers the ModernGekko dispatch loop, which publishes the
  guest PC on every dispatch. Hotspots inside a single recompiled block are not
  resolved.
