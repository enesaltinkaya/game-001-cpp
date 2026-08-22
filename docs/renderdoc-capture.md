# RenderDoc — Frame Capture & Inspection

How to capture a game frame with RenderDoc and inspect what's in the capture,
both interactively (GUI) and headlessly (Python replay API).

## What's installed

| Component                                                     | Location                                                                                        |
| ------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| RenderDoc source + build (v1.46-dev, `ENABLE_PYRENDERDOC=ON`) | `/home/enes/Apps/renderdoc`                                                                     |
| Core library (hooks + API)                                    | `/home/enes/Apps/renderdoc/build/lib/librenderdoc.so`                                           |
| GUI / CLI tools                                               | `/home/enes/Apps/renderdoc/build/bin/{qrenderdoc,renderdoccmd}` (symlinked into `~/.local/bin`) |
| Python replay module                                          | `/home/enes/Apps/renderdoc/build/lib/renderdoc.so` (use `PYTHONPATH=.../build/lib`)             |
| Implicit Vulkan layer registration                            | `/etc/vulkan/implicit_layer.d/renderdoc_capture.json`                                           |
| Game-side header (`renderdoc_app.h`)                          | `/home/enes/Apps/renderdoc/renderdoc/api/app/` (wired in `c-engine/CMakeLists.txt`)             |

The Arch `renderdoc` package was removed; everything above is the single source
of truth. If you rebuild or move the RenderDoc tree, update the layer json,
the `~/.local/bin` symlinks, `c-engine/CMakeLists.txt`, and the `LD_PRELOAD`
lines in `scripts/*.sh`.

### Why layer + preload (not just LD_PRELOAD)

The engine loads Vulkan entry points through **volk** (`dlopen` + `dlsym`),
which bypasses the PLT — plain `LD_PRELOAD` symbol interposition never
intercepts those calls. The **implicit Vulkan layer** wraps the driver inside
the loader itself, so it works regardless of how the app fetches entry points.
Both `LD_PRELOAD` and the layer map the _same_ library file (deduped by
inode), so there is exactly one RenderDoc instance: the layer installs the
Vulkan hooks, the preload gives us `RENDERDOC_GetAPI` for triggering.

(`ENABLE_DLSYM_HOOKING` in the RenderDoc build is not an option here: it
requires glibc's internal `_dl_sym`, which glibc ≥ 2.34 no longer exports, so
it fails to link.)

## Capturing a frame

### 1. Programmatic (headless)

The game arms an in-process trigger behind env vars
(`c-engine/renderer/vulkan/Vulkan.cpp`, `VulkanUtils.cpp`; debug builds, Linux):

```bash
ENGINE_RENDERDOC_CAPTURE=1 \
ENGINE_RENDERDOC_CAPTURE_DELAY_MS=6000 \
ENGINE_SKIP_MAIN_MENU=1 \
ENGINE_LOG_TIMEOUT=12000 \
./scripts/run.sh renderdoc
```

- `scripts/run.sh renderdoc` sets `LD_PRELOAD` (the lib above) and
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` (activates the implicit layer).
- `ENGINE_RENDERDOC_CAPTURE=1` schedules `TriggerCapture()` after
  `ENGINE_RENDERDOC_CAPTURE_DELAY_MS` (default 1000).
- Keep the delay well below `ENGINE_LOG_TIMEOUT` so the process is still
  running when the capture fires; use a long enough timeout that asset
  loading finishes first (≥ 10000 recommended, see AGENTS.md).

On success you'll see in the console:

```
RenderDoc: using loaded /home/enes/Apps/renderdoc/build/lib/librenderdoc.so
RenderDoc API initialized!
RenderDoc: TriggerCapture called (look for the .rdc in /tmp/RenderDoc)
```

The capture file lands in `/tmp/RenderDoc/`:

```
/tmp/RenderDoc/c-game_2026.08.21_16.30_frame320.rdc   (~1 GB)
```

(`RENDERDOC_TEMP=/some/dir` overrides that directory. Old captures pile up —
delete them.)

A low-delay capture (e.g. the default 1000 ms) hits the _loading_ phase and
contains no draw calls — raise the delay to land in gameplay.

### 2. Interactive (GUI / target control)

```bash
./scripts/run.sh renderdoc     # no timeout: runs until you kill it
qrenderdoc                     # pick the running target, capture from the UI
```

RenderDoc's default in-app hotkey (F12, while the game window has focus) also
triggers a capture.

## Inspecting a capture

### GUI

```bash
qrenderdoc /tmp/RenderDoc/c-game_....rdc
```

Draw calls, pipeline state, textures, buffers, shader sources, GPU counters.

### Headless (Python replay API)

The module is built with `ENABLE_PYRENDERDOC=ON`; no extra install needed:

```bash
PYTHONPATH=/home/enes/Apps/renderdoc/build/lib python3 inspect.py
```

Verified working recipe (action census + dumping a render target to PNG):

```python
import sys
sys.path.insert(0, "/home/enes/Apps/renderdoc/build/lib")
import renderdoc as rd
from collections import Counter

CAP = "/tmp/RenderDoc/c-game_2026.08.21_16.30_frame320.rdc"

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
print("open:", cap.OpenFile(CAP, "", None), "| driver:", cap.DriverName())
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
print("replay:", res)

F = rd.ActionFlags

def walk(a):                       # draws are nested under pass/cmd-buffer
    yield a                        # boundary actions -> recurse children
    for c in a.children:
        yield from walk(c)

acts = [a for r in ctrl.GetRootActions() for a in walk(r)]
cnt = Counter()
for a in acts:
    for f in a.flags:
        cnt[f] += 1
names = {v: k for k, v in vars(F).items() if isinstance(v, int)}
print({names.get(k, str(k)): v for k, v in cnt.most_common(6)})

draws = [a for a in acts if F.Drawcall in a.flags]
print("draw calls:", len(draws))
for a in draws[:5]:
    print("  eid=%-5d %s" % (a.eventId, (a.customName or "?")[:60]))

# dump the last draw's output target to PNG
last = draws[-1]
ctrl.SetFrameEvent(last.eventId, True)
out = ctrl.GetPipelineState().GetOutputTargets()[0]
ts = rd.TextureSave()
ts.resourceId = out.resource
ts.mip = 0
sm = rd.TextureSliceMapping(); sm.first = 0; sm.count = 1
ss = rd.TextureSampleMapping(); ss.first = 0; ss.count = 1
ts.slice, ts.sample, ts.destType = sm, ss, rd.FileType.PNG
print("save:", ctrl.SaveTexture(ts, "/tmp/rdc_lastout.png"))

ctrl.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
```

Useful controller methods beyond the above: `GetBuffers()` /
`GetBufferData()`, `GetShader()` + `DisassembleShader()`, `FetchCounters()` /
`EnumerateCounters()`, `GetDebugMessages()`.

## scripts/rdc.py (capture inspector)

A CLI around the replay API for the everyday workflow — extract pass output
images without opening the GUI:

```bash
scripts/rdc.py list                          # passes (debug-label groups) + their output targets
scripts/rdc.py dump heightmap_terrain --out 2   # save that pass's result as PNG -> /tmp/rdc-dump/
scripts/rdc.py dump oit_accumulate --frames 3   # same pass on 3 consecutive frames (flicker debugging)
scripts/rdc.py dump --last                    # output of the frame's last draw call
scripts/rdc.py dump --all                     # every pass's color output
scripts/rdc.py dump 241                       # raw event id
scripts/rdc.py clean --keep 1 --dry-run       # .rdc files are ~1 GB each
```

It defaults to the newest `.rdc` in `/tmp/RenderDoc`. Passes are the vk
debug-label **PushMarker groups** the engine emits (`scene_depth_prepass`,
`heightmap_terrain`, `azgaar_props`, …); a pass's *result* is read at its
last draw (the `vkCmdBeginRendering` event is post-clear/pre-draw, and
dispatch/EndPass events report no color outputs — compute-only groups like
`oit_composite` are marked `(compute)` in `list`).

## API gotchas (this v1.46-dev module)

- **Recurse the action tree.** `GetRootActions()` returns only top-level
  entries; draw/dispatch actions live in `children` under
  `BeginPass`/`CommandBufferBoundary`/`PushMarker` nodes.
- **Result objects, not bools.** `CaptureFile.OpenFile()` and
  `OpenCapture()` return `ResultDetails` — check `r.OK()` / `r.Message()`.
  `SetFrameEvent()` returns void (None) in this build.
- **Names:** `ActionDescription.GetName()` takes an `SDFile` out-param in this
  version — just use `.customName` (the Vulkan debug label, e.g.
  `vkCmdDrawIndexedIndirectCount(<0>)`).
- **`SetFrameEvent(eid, force)`** then **`GetPipelineState()`** (no args) —
  the state call is relative to the _current_ event.
- **`GetTextures()` is on the controller** (no args; returns every texture in
  the capture). Fields are lowercase (`arraysize`, `mips`, `cubemap`); the
  format name comes from `t.format.Name()`. `ResourceId` exposes no field
  accessors — key on `str(rid)` (`ResourceId::157`).
- **`SaveTexture(saveStruct, path)`** — two args; `slice`/`sample` need
  `TextureSliceMapping`/`TextureSampleMapping` objects, not ints.
- **Pass outputs:** render targets are only reported in _draw_ pipeline
  state — not at `vkCmdBeginRendering` (post-clear), dispatch, or EndPass
  events. Compute passes expose their write targets through no reliable API
  here (`GetReadWriteResources` came back empty).
- **Float outputs** (R16F G-buffer, HDR) are converted 0–1 → 0–255 on save,
  so HDR images look dark; that's expected, not a bug.
- Shutdown order matters: `controller` → `capture file` → `ShutdownReplay()`
  (skipping it aborts with a double-free).

## Environment variables

| Variable                                     | Read by             | Effect                                                   |
| -------------------------------------------- | ------------------- | -------------------------------------------------------- |
| `ENGINE_RENDERDOC_CAPTURE=1`                 | game (debug, Linux) | arm the in-process `TriggerCapture()`                    |
| `ENGINE_RENDERDOC_CAPTURE_DELAY_MS=N`        | game                | delay before the trigger fires (default 1000)            |
| `ENGINE_RENDERDOC_LIB=/path/librenderdoc.so` | game                | override which lib to `dlopen` for the API               |
| `LD_PRELOAD=.../build/lib/librenderdoc.so`   | dynamic loader      | map RenderDoc early (set by `run.sh renderdoc`)          |
| `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`          | Vulkan loader       | load the implicit capture layer (**required for hooks**) |
| `DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46=1`    | Vulkan loader       | force-disable the layer                                  |
| `RENDERDOC_TEMP=/dir`                        | RenderDoc           | override the default `/tmp/RenderDoc` dir                |

## Troubleshooting

| Symptom                                                            | Cause / fix                                                                                                                                                                  |
| ------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `RenderDoc not present.`                                           | Lib not mapped — run via `./scripts/run.sh renderdoc` (or set `LD_PRELOAD` yourself)                                                                                         |
| `RenderDoc API initialized!` but no `.rdc` appears                 | Layer not active: `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` missing; or trigger fired after process exit (delay ≥ timeout); check `/tmp/RenderDoc` and `RenderDoc_app_*.log` there |
| Capture has 0 draw calls                                           | Triggered during asset loading — raise `ENGINE_RENDERDOC_CAPTURE_DELAY_MS`                                                                                                   |
| Intermittent `material "..." is already initialized!` CRIT on exit | Pre-existing engine flake, unrelated to RenderDoc — just rerun                                                                                                               |
| `qrenderdoc` not found                                             | `~/.local/bin` symlink missing — recreate from `build/bin/`                                                                                                                  |
