# dsr-native — Mali-G615 fork

Runs **Dark Souls: Remastered** (Nintendo Switch) natively on an ARM64 Android phone.

This is not an emulator: the game's own ARM64 executable (`exefs/main`) is loaded and executed directly on
the phone's CPU, and the Switch operating system services it calls are implemented on the host (high-level
emulation). Graphics go through the game's NVN API, which is recorded and executed on Vulkan; the game's
Maxwell shaders are translated to SPIR-V at runtime.

> [!IMPORTANT]
> No game files are included. You must own a legal copy of the Nintendo Switch version of Dark Souls:
> Remastered and extract it yourself. This project cannot and will not help with obtaining the game.

> [!NOTE]
> This is a **fork** of [kvse/dsr-native](https://github.com/kvse/dsr-native), tuned for ARM Mali GPUs
> (development target: Mali-G615 MC6 in the Xiaomi 14T, Dimensity 8300-Ultra, Android 16). Adreno is not a
> target of this fork, although upstream's Adreno support is still present. See
> [What this fork changes](#what-this-fork-changes).

---

## What this project is

Dark Souls: Remastered on the Switch is a normal ARM64 program. On an ARM64 Android phone the instruction
set is the same, so instead of emulating a CPU this project:

1. **Loads the game** — maps `exefs/main` (NSO), relocates it, binds its imports to host implementations and
   runs the game's own startup code (`nninitStartup`, static constructors, `nnMain`).
2. **Implements the Switch OS (HLE)** — nnSdk services used by the game: memory, threads, synchronisation,
   filesystem, HID, audio, VI, applet/oe, crypto, and NVN.
3. **Translates graphics** — NVN command lists are recorded into host command lists and executed on Vulkan.
   Maxwell shaders (the Switch GPU's ISA) are decoded and emitted as SPIR-V; descriptors, render targets,
   textures and samplers are rebuilt from the game's state.
4. **Converts textures** — the game ships BC1/BC3/BC4/BC5 textures. Phones without BC support get them
   re-encoded once on the CPU (ETC2, ASTC, or uncompressed) and uploaded to the GPU.
5. **Plays audio and video** — AAudio for the game's 6-channel output, MediaCodec for the pre-rendered
   movies, FMOD calls passed through.
6. **Presents** — the game's frame texture is copied to an Android swapchain each frame; with
   `compositor_scaling` the swapchain is the game-frame size and the display compositor scales it, which
   saves GPU time and power.

No CPU emulation, no JIT, no game-code patches beyond the documented engine hooks (frame rate, resolution,
skip intro, effect quality). Everything else is the original game.

### How it runs

- The game renders at its native 1280x720 (or a chosen scale/aspect ratio).
- The renderer is a Vulkan backend (`src/hle/nvn/vk_backend.cpp`). It records, submits and presents;
  NVN syncs wait on the render queue.
- `android/` is the Android app (NativeActivity + a Java settings menu, touch controls, file picker).
- `src/platform/host/` builds the same loader as a Linux command-line dry run (`dsr_dryrun`) for HLE
  development without a phone.

### Requirements

- ARM64 Android device, Android 10 (API 29) or newer.
- Vulkan 1.1+ (Vulkan 1.3 preferred: dynamic rendering; without it the renderer uses `VkRenderPass`
  objects).
- 6 GB RAM or more recommended (the game's guest heap wants ~3 GB; textures and the geometry cache need
  another ~1–2 GB).
- Switch Dark Souls: Remastered base 1.0.0 or update 1.0.3, extracted as described below.

---

## What this fork changes

The upstream project is Adreno-first (Snapdragon). This fork keeps all upstream behaviour and adds a
Mali-first performance profile plus crash protection. All changes are in `src/hle/nvn/vk_backend.cpp`,
`src/runtime/runtime.h`, `src/platform/android/android_main.cpp` and the settings menu.

### 1. Arm Frame Buffer Compression (AFBC)

Mali compresses framebuffer traffic with AFBC. The driver refuses to do so for images created with
`VK_IMAGE_USAGE_STORAGE_BIT` (all GPUs) and, before Valhall, for images with
`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`. The renderer used to create **every** image with both usages, so
almost nothing was compressed.

- Mali now uses the same lean-image path as older Adreno GPUs: the storage and mutable-format usages are
  added only when the game actually binds an image as a storage image or views it in its sRGB/linear twin
  format (the image is re-created once, with the usage added).
- Vulkan 1.1 Mali drivers enable `VK_KHR_image_format_list` when advertised, so images viewed in both
  sRGB and linear form keep their compression on Valhall.
- `mali_afbc = 0` (or the **Mali AFBC** switch in the menu) restores the old allocation behaviour.

### 2. Tile-GPU barriers

`mali_barriers` is on by default. A layout transition waits only for the stages that use the image, and a
colour target becomes readable by fragment shaders when its pass ends, so a tile-based GPU can overlap one
pass's fragment work with the next pass's vertex work. The setting stays in the menu for A/B testing.

### 3. BC3 textures as ETC2

On GPUs without BC, BC1 and BC3 textures are re-encoded as ETC2 instead of being decoded to RGBA8. ETC2
RGBA8 is 8 bytes per 4×4 block, four times less texture memory and bandwidth than RGBA8, at a small
quality cost. `bc3_etc2 = 0` disables it.

### 4. 4-byte depth-stencil and 1024 shadow maps

- NVN depth-stencil formats 51/54 use `D24_UNORM_S8_UINT` first on Mali. Arm's AFBC format list includes
  D24S8 (4 bytes) but not D32F+S8 (8 bytes), so the main depth buffer and shadow maps stay compressed and
  move half the bytes. 24 bits are plenty for the game's 720p depth range.
- `shadow_quality = 2` (1024×1024 shadow maps) and `max_anisotropy = 4` are the defaults: both are large
  bandwidth consumers on a tile GPU. Both remain adjustable in the menu.

### 5. Protection against the 180 MB Mali render-pass limit

Mali keeps the intermediate output of a whole render pass — varying data for **every vertex between a
draw's lowest and highest index**, not only the indices used — in a fixed 180 MB region. Exceeding it
loses the Vulkan device (Arm calls the fault "Tile heap overflow"), which in practice looks like a
freeze at 0 fps.

The renderer now estimates that vertex range per render pass (indexed draws: `max index - min index + 1`,
times instances) and ends the pass before it crosses `mali_pass_vertices` (default **1 500 000**, about
96 MB at 64 bytes of varyings; `0` disables the split). The next draw starts a new pass with `LOAD`, the
incremental rendering Arm recommends. The 10-second statistics report the largest pass estimate and how
many passes were split.

A single draw that by itself references more vertices than the limit cannot be fixed by splitting the
pass; the log names that case once.

### 6. Device-fault diagnostics

`VK_EXT_device_fault` is enabled when the driver offers it. On `VK_ERROR_DEVICE_LOST` the backend logs the
driver's own description, the fault addresses and vendor fault codes instead of only `-4`. On Mali the
description distinguishes a tile-heap overflow from an invalid GPU address, which is exactly what a bug
report needs.

### Kept from upstream

FSR 1 / FXAA, sharp HUD and text at reduced render scale, `skip_hidden`, `skip_prepass`, the geometry
cache, pipeline cache, ASTC/ETC2 texture options, cheats, languages, touch controls and custom Vulkan
drivers (Turnip/PanVK via libadrenotools) all work as upstream.

---

## Status

- Development target: **Mali-G615 MC6** (Xiaomi 14T, Dimensity 8300-Ultra), Android 16. Upstream reports
  Mali-G52/G57/G68 devices as playable; very old Mali (G51, r18 driver) still reports a black screen
  upstream.
- Base v1.0.0 and update v1.0.3 are supported.
- Expect stutter while new areas and shaders load; textures stream in. A 60 fps cap is recommended on
  Mali.
- The 180 MB limit protection is an estimate. If a crash still happens, the device-fault block in the log
  says why; tune `mali_pass_vertices` (for example 1 000 000) or split the offending draw in a future
  change.

Tested areas upstream: Undead Burg, Firelink Shrine, Undead Asylum.

---

## Settings

Open the in-game menu by holding **−** and **+** for one second. Settings live in
`Download/DSRNative/settings.txt` and are also editable there; "at the next start" settings need a
restart.

Useful for Mali:

| Key | Default in this fork | Meaning |
|---|---|---|
| `mali_afbc` | `1` | Lean images so AFBC stays on; `0` = old behaviour (no compression) |
| `mali_barriers` | `1` | Narrow tile-GPU barriers / render targets readable at pass end |
| `mali_tile_turbo` | `0` | Experimental: scope layout barriers to their stages/accesses |
| `mali_pass_vertices` | `1500000` | Split a render pass at this vertex-range estimate; `0` = never |
| `bc3_etc2` | `1` | Re-encode BC3 as ETC2 RGBA8 (4× less memory than RGBA8) |
| `shadow_quality` | `2` | 0 off, 1 = 512, 2 = 1024, 3 = the game's 2048 |
| `max_anisotropy` | `4` | Cap on the game's anisotropic filtering (0 = the game's) |
| `image_format_list` | `-1` | `-1` auto, `0` never, `1` also on other Vulkan 1.1 drivers |
| `render_pass_objects` | `0` | Use `VkRenderPass` objects even with dynamic rendering (flicker tests) |
| `present_mode` | `fifo` | `fifo` (vsync) is the safest on Mali; `mailbox` for lower latency |

Menu switches added by this fork: **Mali AFBC** and **Render pass objects** under *Graphics → Mali fixes*;
the existing **Tile-GPU barriers** and **Mali Tile Turbo** are kept.

---

## Installing the game files

> [!IMPORTANT]
> You need your own `prod.keys` for method 1. Never share game files or keys.

**Method 1 — NSP/XCI:** install the APK, launch it, grant "All files access", then pick the base
`.nsp`/`.xci` and optionally the v1.0.3 update. The app extracts the needed files into
`/storage/emulated/0/Download/DSRNative/game/` and boots from there.

**Method 2 — extracted files:** put the extracted **romfs** contents, the folders **movww**, **movjp** and
an **exefs** folder into your game directory, then choose that directory in the app.

```
Download/DSRNative/game/
  exefs/main, main.npdm, ...
  dvdbnd0.bdt, dvdbnd0.bhd5, ...
  movww/, movjp/
```

---

## Building

This fork was built with:

- JDK 17
- Android SDK platform 34, build-tools 34.0.0
- Android NDK **26.1.10909125**
- CMake **3.22.1**
- Gradle **8.7**, Android Gradle Plugin 8.5.2

Set `ANDROID_HOME` (or create `android/local.properties` with `sdk.dir=...`), then:

```bash
cd android
gradle assembleDebug        # debug APK
gradle assembleRelease      # release APK (signed with the repository's debug key)
```

The APK is under `android/app/build/outputs/apk/<variant>/`. Native code is always compiled with `-O2`
and `RelWithDebInfo`, even for the debug variant.

A release build with the current version scheme:

```bash
GITHUB_RUN_NUMBER=20261126 gradle assembleRelease
```

### Host dry run (Linux)

```bash
cmake -S . -B build-host && cmake --build build-host -j
./build-host/dsr_dryrun /path/to/DarkSoulsR --list --verbose
```

On x86 it maps, relocates and binds imports and prints the import coverage report; on an ARM64 Linux host,
`--run` executes the game like the APK does.

CI (`.github/workflows/build.yml`) builds the debug APK and runs the runtime tests on x86-64 and arm64.

---

## Diagnostics

Every launch writes a log to the phone's shared storage:

```
Download/DSRNative/logs/dsr_YYYYMMDD_HHMMSS.txt
```

and to logcat (`adb logcat -s "DSR/*"`). The log contains device/GPU information, every runtime message,
10-second performance statistics and, on a crash, a report with registers and a backtrace translated to
`main+0x...` offsets (look them up in Ghidra using the ELF from `tools/mkelf.py`).

Hold **L3 + R3** while playing to save a frame capture (`dsr_frame_*.bin`) with the render targets of that
frame. Use **Detailed diagnostic files** in the settings menu for the logcat mirror and shader dumps.

For this fork, look for:

```
Mali GPU: tile-based profile (compression-friendly images)
Mali GPU: storage / mutable-format usage only on the images that need it (AFBC kept)
device fault reporting: VK_EXT_device_fault
Mali: vertex-range estimate per render pass: largest N, M pass(es) split (limit 1500000)
```

If the GPU is lost, the log now contains:

```
device fault: <driver description>
  fault address 0: type ..., address 0x..., precision 0x...
  vendor fault 0: <description> (code 0x...)
```

Include that block and the log in any bug report.

If the picture flickers or effects disappear, try (one at a time, restart between):

1. **Graphics → Mali fixes → Render pass objects** = On (uses `VkRenderPass` instead of dynamic rendering).
2. **Mali AFBC** = Off.
3. **Tile-GPU barriers** = Off.
4. **Present mode** = FIFO.

---

## Known issues

- Mali flicker reports still exist upstream; the switches above isolate the renderer path.
- Very old Mali GPUs (Mali-G51, driver r18) may show a black game image despite a running game; see
  upstream issue #78.
- A single draw referencing more than the 180 MB limit can still lose the device; the log names it.
- The game's own autosave is written when resting at a bonfire; a device loss before the next rest loses
  progress since the last save.
- Adreno is not tuned in this fork; upstream settings still work.
- 1.0.0 and 1.0.3 game versions only.

---

## Credits

- [kvse/dsr-native](https://github.com/kvse/dsr-native) — the upstream project this fork is based on.
- Upstream contributors whose work is included here: fernanfez00 (FSR 1, ASTC, shadow quality,
  skip_prepass/skip_hidden), Dann-os3d (Mali tile turbo, render-pass stencil key), and everyone else in
  the upstream git history.
- [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause) for custom Vulkan driver
  loading.
- Khronos for the Vulkan and SPIR-V headers.

The upstream project notes that AI assistance was used heavily during development; the Mali profile and
crash protection in this fork were developed the same way. The changes are documented in the commit
messages and can be reviewed file by file.

No game files, keys or copyrighted assets are distributed with this repository.
