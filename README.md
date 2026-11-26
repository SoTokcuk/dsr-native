# dsr-native — Mali-G615 fork

**Dark Souls: Remastered** (Nintendo Switch) running natively on ARM64 Android. This is not an emulator: the
game's own ARM64 executable runs on the phone CPU, Switch OS services are implemented on the host (HLE), NVN
commands are executed through Vulkan, and Maxwell shaders are translated to SPIR-V at runtime.

A fork of [kvse/dsr-native](https://github.com/kvse/dsr-native) tuned for **ARM Mali**. Development target:
**Mali-G615 MC6** in the Xiaomi 14T (Dimensity 8300-Ultra, Android 16). Adreno is not tuned here, although
upstream's Adreno support is still present.

> [!IMPORTANT]
> No game files or keys are included. You need your own legal copy of the Switch version of Dark Souls:
> Remastered and must extract it yourself.

**Download:** [latest release](https://github.com/SoTokcuk/dsr-native/releases/latest) —
`dsr-native-mali-g615-release.apk`, arm64-v8a, Android 10+, signed with the repository's debug key.

## What this fork changes

All changes are in `src/hle/nvn/vk_backend.cpp`, `src/runtime/runtime.h`,
`src/platform/android/android_main.cpp` and the settings menu. Upstream features are unchanged.

### 1. AFBC stays on

Mali compresses framebuffer traffic with AFBC, but the driver refuses it for images created with
`VK_IMAGE_USAGE_STORAGE_BIT` and (before Valhall) `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`. The renderer used to
create every image with both. Mali now takes the lean-image path: those usages are added only when the game
actually needs them, and `VK_KHR_image_format_list` is enabled on Vulkan 1.1 drivers so sRGB/linear twins stay
compressed. `mali_afbc = 0` restores the old allocation.

### 2. Tile-GPU barriers

`mali_barriers = 1`: layout transitions wait only for the stages that use the image, and a colour target becomes
readable by fragment shaders when its pass ends. A tile GPU can then overlap one pass's fragment work with the
next pass's vertex work. `mali_tile_turbo` (experimental) narrows transitions further.

### 3. Cheaper textures and area loads

- BC1/BC3 textures are re-encoded as ETC2 RGBA8 instead of RGBA8: four times less texture memory and bandwidth
  at a small quality cost (`bc3_etc2 = 1`).
- The background-conversion threshold is 128×128 texels instead of 512×512. An area load converts ~2000 BC
  levels; they now get a quick preview and the full ETC2 encode runs on a worker thread instead of blocking the
  frame (previously a multi-second freeze during loading).

### 4. Depth, shadows, filtering

- NVN depth-stencil formats 51/54 use `D24_UNORM_S8_UINT` first: Arm's AFBC format list includes D24S8 but not
  D32F+S8, so depth and shadow maps stay compressed and move half the bytes.
- Defaults: `shadow_quality = 2` (1024×1024) and `max_anisotropy = 4`; both are bandwidth-heavy on a tile GPU
  and both remain adjustable.

### 5. The 180 MB tile-heap guard

Mali keeps the intermediate output of a render pass for the whole vertex range between a draw's lowest and
highest index in a fixed 180 MB region; overflowing it loses the Vulkan device. The renderer estimates that
range per pass and ends the pass before it crosses `mali_pass_vertices` (default 1 500 000, `0` disables).

The estimate is only as good as the index scan: small index buffers (up to 256 KB) are now hashed in full, so a
rewritten streaming buffer is noticed and the guard sees the real load. The 10-second stats report the largest
pass and how many were split.

### 6. Device faults are explained

`VK_EXT_device_fault` is enabled when available. On `VK_ERROR_DEVICE_LOST` the log contains the driver's own
description, fault addresses and vendor codes instead of just `-4`, which distinguishes a tile-heap overflow
from an invalid GPU address.

### 7. Recovery instead of a 0 fps freeze

A fault loses the logical device: every submit fails and fences never signal again. The renderer used to keep
waiting on those fences (60 s per frame, forever) — the "random freeze" this fork was chasing.

Now the device is rebuilt while the game keeps running: the render thread idles its workers, destroys device
objects, clears the caches and creates a fresh device; resources rebuild lazily from game memory over the next
frames. No restart, no lost progress. The log shows:

```
GPU device lost: rebuilding the Vulkan device (the game continues from the last frame)
Vulkan device rebuilt: rendering resumes
```

After repeated faults in a short window the backend gives up graphics cleanly instead of spinning.

## Status

- Target: **Mali-G615 MC6** (Xiaomi 14T), Android 16. Upstream reports Mali-G52/G57/G68 as playable; very old
  Mali (G51, driver r18) can still show a black image (upstream issue #78).
- Game versions: base 1.0.0 and update 1.0.3.
- Expect stutter and streaming textures while a new area loads. A 60 fps cap is recommended on Mali.
- The 180 MB guard is an estimate. If a fault still occurs, the device-fault block in the log says why; tune
  `mali_pass_vertices` (for example 1 000 000) or report the log.
- Adreno works as upstream but is not tuned in this fork.

## Settings

Open the in-game menu by holding **−** and **+** for one second. Settings live in
`Download/DSRNative/settings.txt`; values marked "next start" need a restart.

| Key | Default here | Meaning |
|---|---|---|
| `mali_afbc` | `1` | Lean images so AFBC stays on; `0` = old allocation |
| `mali_barriers` | `1` | Narrow tile-GPU barriers |
| `mali_tile_turbo` | `0` | Experimental: scope transitions to their stages/accesses |
| `mali_pass_vertices` | `1500000` | Split a pass at this vertex-range estimate; `0` = never |
| `bc3_etc2` | `1` | Re-encode BC3 as ETC2 RGBA8 |
| `shadow_quality` | `2` | 0 off, 1 = 512, 2 = 1024, 3 = the game's 2048 |
| `max_anisotropy` | `4` | Cap on the game's anisotropic filtering; `0` = game's own |
| `image_format_list` | `-1` | `-1` auto, `0` never, `1` force on Vulkan 1.1 |
| `render_pass_objects` | `0` | Use `VkRenderPass` objects even with dynamic rendering |
| `present_mode` | `fifo` | `fifo` (vsync) is safest on Mali; `mailbox` lowers latency |

Menu switches added by this fork: **Mali AFBC** and **Render pass objects** under *Graphics → Mali fixes*.
**Tile-GPU barriers** and **Mali Tile Turbo** are kept from upstream.

## Installing the game files

**NSP/XCI:** install the APK, grant "All files access", then pick the base `.nsp`/`.xci` and optionally the
v1.0.3 update. The app extracts what it needs into `Download/DSRNative/game/`.

**Extracted files:** put the romfs contents, `movww`, `movjp` and an `exefs` folder into one directory and choose
it in the app:

```
Download/DSRNative/game/
  exefs/main, main.npdm, ...
  dvdbnd0.bdt, dvdbnd0.bhd5, ...
  movww/, movjp/
```

## Building

Built with JDK 17, Android SDK platform 34, build-tools 34.0.0, NDK **26.1.10909125**, CMake **3.22.1**,
Gradle **8.7** and AGP 8.5.2.

```bash
cd android
gradle assembleDebug        # debug APK
gradle assembleRelease      # release APK (signed with the repository debug key)
```

Set `ANDROID_HOME` (or create `android/local.properties` with `sdk.dir=...`). Native code is compiled `-O2`
`RelWithDebInfo` in both variants. `GITHUB_RUN_NUMBER` becomes the version code: build date plus a same-day
build digit, for example `GITHUB_RUN_NUMBER=202611271 gradle assembleRelease`.

A Linux host build (`dsr_dryrun`) runs the HLE without a phone; on x86 it maps, relocates and binds imports and
prints the import coverage. CI builds the debug APK and runs the runtime tests on x86-64 and arm64.

## Diagnostics

Every launch writes `Download/DSRNative/logs/dsr_YYYYMMDD_HHMMSS.txt` and mirrors to logcat
(`adb logcat -s "DSR/*"`). The log contains device/GPU info, 10-second performance stats, the Mali pass
statistics and, on a fault, the driver's fault report and a backtrace with `main+0x...` offsets.

Look for:

```
Mali GPU: tile-based profile (compression-friendly images)
Mali: vertex-range estimate per render pass: largest N, M pass(es) split (limit 1500000)
device fault: <driver description>
GPU device lost: rebuilding the Vulkan device (the game continues from the last frame)
```

Hold **L3 + R3** to save a frame capture (`dsr_frame_*.bin`). If the picture flickers or effects disappear, try
one change at a time with a restart: **Render pass objects = On**, then **Mali AFBC = Off**, then
**Tile-GPU barriers = Off**, then **present_mode = fifo**.

## Credits

- [kvse/dsr-native](https://github.com/kvse/dsr-native) — upstream project and contributors (FSR 1, ASTC,
  shadow quality, skip_prepass/skip_hidden, Mali tile turbo and more).
- [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause) for custom Vulkan driver loading.
- Khronos for the Vulkan and SPIR-V headers.

The Mali profile, guard and recovery were developed with heavy AI assistance; every change is documented in the
commit messages. No game files, keys or copyrighted assets are distributed here.
