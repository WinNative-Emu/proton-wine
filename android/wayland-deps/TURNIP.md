# Wayland Turnip

Eight drivers, all OUR build: `libvulkan_freedreno.so` from the Banners-Turnip `wayland` branch
(`build_wayland.sh`, workflow "Build Wayland variant"), NDK r29, API 29: Turnip with the KGSL
backend and the Wayland WSI, built Linux-style on bionic like Termux's. The containers' own Vulkan
drivers (the wrapper, adrenotools builds) have no Wayland WSI, so winewayland points
`VK_ICD_FILENAMES` at one of these when it runs on the Bannerlator compositor.
`usr/lib/libdrm.so` (Termux 2.4.134) is what they were linked against and is bundled next to them.

All eight carry Bannerlator's zero-copy WSI (`patches/wayland/banner_ahb_wsi.py`): with the
compositor's `banner_ahb_v1` global they can allocate swapchain images as gralloc
`AHardwareBuffer`s and hand them over for the display layer. Since versionCode 6 they follow the
**live** switch — `banner_ahb_v1` version 2's `mode` event, sent on bind and on every flip of the
in-game "Zero-copy presentation" toggle — and retire a swapchain built for the other mode
(`VK_ERROR_OUT_OF_DATE_KHR`, which DXVK, vkd3d-proton and Zink all rebuild on). The registry bind
clamps to `MIN(advertised, 2)`, and against a compositor that only advertises version 1 (any
Bannerlator up to 3.1.2-wayland-pre3) the behaviour is exactly versionCode 5's: gralloc images iff
`BANNER_WSI_AHB=1`, decided per swapchain, nothing ever retired. `BANNER_WSI_AHB=0` forces the
feature off everywhere.

**Since versionCode 12 those gralloc buffers can be UBWC-compressed.** QTI gralloc compresses only
when the producer sets its vendor bit `GRALLOC_USAGE_PRIVATE_ALLOC_UBWC` (`AHARDWAREBUFFER_USAGE_VENDOR_0`,
bit 28) with a GPU usage and no CPU bit; Mesa never set it, so every zero-copy swapchain up to
versionCode 11 was linear (the Pocket FIT and the Fold both logged `on gralloc buffers: linear`).
The eight drivers (Banners-Turnip `wayland` `0121416`, workflow run 34999563098) now ask gralloc in
turn for UBWC (usage `0x10000b00`), then the old plain request, then a CPU-bit (linear) one. UBWC is
asked for only when the chain's modifier list holds `QCOM_COMPRESSED` - the compositor offers it
for the format (the app's `BANNER_WAYLAND_UBWC=0` takes it away) and the driver can create the
swapchain's usage as UBWC - and never for storage swapchains or with `BANNER_WSI_AHB_LINEAR=1`. A
UBWC buffer is kept only when its QTI private handle (`'gmsm'`) says UBWC, `vkCreateImage` accepts
gralloc's pitch with `QCOM_COMPRESSED`, and gralloc's buffer holds the driver's whole UBWC image;
anything else falls to the next request. A handle without `'gmsm'` (newer grallocs, e.g. the Fold's
2 fds / 34 ints) ends on the CPU-bit request exactly as before, and its ints are printed once per
request for a future reader. `wine_debug.log` names the result once per swapchain:
`banner-ahb: WxH swapchain (N images) on gralloc buffers: UBWC (QCOM_COMPRESSED), stride S px`, or
`… linear, stride S px (no UBWC: <why>)`. The compositor needs no change: the zero-copy layer
hands the buffer to SurfaceControl as is, and its log already says `AHB swapchain … UBWC
(QCOM_COMPRESSED)` when that is what arrives.

## Variants

Each driver is its own Mesa checkout: the plain one is the Banners-Turnip release commit, the
others are pinned to the commit the community release they reproduce was built from (the recipe
files are vendored verbatim under Banners-Turnip `patches/upstream/<release>/`, with a `SOURCE.md`
recording tag, commit and how the Mesa pin was established). All four get the same flags and the
same Wayland changes first (`-Dfreedreno-kmds=msm,kgsl` + libdrm, `no_pthread_cancel.py`, the KGSL
timestamp assert turned into a warning, Android detection off), then the recipe on top. The build
refuses a recipe script that reports a missing anchor or leaves the tree unchanged, checks that
the eight share one SONAME and NEEDED set, that `FD710` is only in a7xx + a8xx_white (the fork
carries 710/720/722 too), `Adreno (TM) 825` only in the WN/gen8/white a8xx builds, the PWR_MAX log
string only in a8xx_perf, `deck_emu` only in a8xx_gen8 + a8xx_white, `Adreno (TM) 812`,
"whitebelyash branch" (driverInfo) and ` (v31)` only in a8xx_white, that a8xx_upstream carries
upstream's `Adreno (TM) 840` entry and is neither byte-identical to plain nor carries plain's
embedded Mesa git string, that a8xx_smxz carries
`has_early_preamble = False` on a7xx_gen1 at source level and differs from every other binary
(its recipe has no textual marker), and that the WN a8xx builds differ.

| variant | `usr/lib/` | ICD manifest (`usr/share/vulkan/icd.d/`) | recipe | Mesa | Adreno |
| --- | --- | --- | --- | --- | --- |
| plain | `libvulkan_freedreno_wayland.so` | `banner_wayland_turnip.json` | none (also provides EGL + Zink) | `7cda7850edd103ace21aac37d416d2fdf7a282e1` (26.3.0-devel, 2026-09-11) | 6xx, 730/740/750 (upstream also lists 722) |
| a7xx | `libvulkan_freedreno_wayland_a7xx.so` | `banner_wayland_turnip_a7xx.json` | Vauzi-17/710 release 3.6 (tag commit `5db89bde`): `add_710_720_722.py`, FD710/FD720/FD722 entries with per-GPU magic regs, `num_ccu` 1/2/2 (replaces upstream's 722) | `7631b5254f1a0a4371f5594e630ce2f2b8394e73` (26.3.0-devel, 2026-08-27 05:57Z) | 710/720/722 |
| a8xx | `libvulkan_freedreno_wayland_a8xx.so` | `banner_wayland_turnip_a8xx.json` | WinNative-Emu/Drivers v1.15 (WN-Turnip 1.15, tag commit `8407c801`): `fix_gralloc_flushall`, `fix_a8xx_dev_info` (A810/A829 `disable_gmem`, the check really lands here), `apply_a8xx_gpus` (A825 entry, A810 speedbin id, A829 KGSL ids), `apply_a7xx_gen1_quirks`, `apply_a7xx_gen2_ubwc_hint`, `add_aimapper_gralloc`, `add_ubwc_swapchain_usage`, then `apply_balance_variant` (**Balanced**: GMEM autotuner bandwidth multiplier 11 -> 10) | `12b7b819edb4ddd3580e7e5ffe384610ae726c90` (26.3.0-devel, 2026-09-10, from their release notes) | 830/840 (8 Elite: also 810/825/829); the a7xx_gen1/gen2 quirks also touch 720/725/730/740/X1-85 on this driver |
| a8xx-perf | `libvulkan_freedreno_wayland_a8xx_perf.so` | `banner_wayland_turnip_a8xx_perf.json` | the same set, then `apply_perf_variant` with `BUILD_VARIANT=p` (**Performance**: `KGSL_CONTEXT_PWR_CONSTRAINT` + `KGSL_PROP_PWR_CONSTRAINT = PWR_MAX` at queue creation, re-asserted every 1000 submissions, `KGSL_CMDBATCH_PWR_CONSTRAINT` on submits; higher clocks, higher power draw) | same as a8xx | same as a8xx |
| a8xx-smxz | `libvulkan_freedreno_wayland_a8xx_smxz.so` | `banner_wayland_turnip_a8xx_smxz.json` | [StevenMXZ/Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers) release [v36](https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v36) "Turnip Gen8 V36" (tag commit `50cbd613`): its `build_turnip.sh` seds (`has_early_preamble = False` on a7xx_gen1; the ` (%s)` strip matches nothing upstream). The job's clone target (mesa-tu8 `gen8`, April 2026) is not what shipped: the released binary embeds upstream `git-c501e1d16e`, see `patches/upstream/smxz-v36/SOURCE.md` | `c501e1d16e11c256610cd5922b1afa5660f2f5ea` (mesa/mesa main, 2026-09-08 05:54Z) | 8xx as upstream at that commit (810/829/830/840/X2; no 825) |
| a8xx-white | `libvulkan_freedreno_wayland_a8xx_white.so` | `banner_wayland_turnip_a8xx_white.json` | [whitebelyash/freedreno_turnip-CI](https://github.com/whitebelyash/freedreno_turnip-CI) release [tu_v31](https://github.com/whitebelyash/freedreno_turnip-CI/releases/tag/tu_v31) "Mainline Turnip v31" (tag commit `258fc219`), primary asset: the [mesa-unified](https://github.com/whitebelyash/mesa-unified) `turnip/gen8` branch as is + `tu_version.h` = `"v31"`; `patches/39751.diff` (the `-sync` asset's KGSL binary/timeline sync rework) not applied | `9c475fc367a7283a7eee58501fb48149780f2c1e` (fork `turnip/gen8` head 2026-08-07: upstream ~08-07 + ~28 hack commits) | 840/830/829/825/812/810 + 710/720/722 (sysmem only) + upstream 6xx/7xx |
| a8xx-upstream | `libvulkan_freedreno_wayland_a8xx_upstream.so` | `banner_wayland_turnip_a8xx_upstream.json` | none: pure [mesa/mesa](https://gitlab.freedesktop.org/mesa/mesa) `main`, only the Wayland changes | `bbc7792f717f27b17b4c12e6a4503d703a362aac` (main head when pinned, 2026-09-13T15:39:30Z, "Revert \"nvk: Expose BAR as host cached\""; bump deliberately in `build_wayland.sh`) | 8xx as upstream lists (810/829/830/840/X2) + everything else upstream |
| a8xx-gen8 | `libvulkan_freedreno_wayland_a8xx_gen8.so` | `banner_wayland_turnip_a8xx_gen8.json` | Banners-Turnip's own Android a8xx recipe (`turnip_build_combined.yml` a8xx job, mirrored from `build_turnip.sh`): `patches/a8xx_gen8.patch` = whitebelyash tu8 series (13 commits: DECK_EMU `TU_DEBUG=deck_emu`, gralloc UBWC hack, `disable_gmem`, drm-shim ids, A825, VK1.3 without multiview, a8xx family configs, A810 fixes/feature cuts, forced `nocb`, A825/829 offsets, A810 custom resolve, ir3 UBO coalescing) + `patches/a8xx_shared_mem.py` (`cs_shared_mem_size` 32K -> 64K, x31) | `12b7b819edb4ddd3580e7e5ffe384610ae726c90` (that job clones Mesa `main` unpinned; pinned here to the WN-Turnip commit so the three 8xx builds share one Mesa) | 8xx (built for the Galaxy Fold / Adreno 840 case where WN Balanced performs poorly) |

Notes:
- The Vauzi 3.6 release names no Mesa commit; its binary embeds `git-25219437df`, a commit that is
  public nowhere (a local commit on top of main), so the pin is the newest mesa/mesa `main` commit
  before the zip's build time (2026-08-27 07:01 builder-local, read as UTC). If that clock was
  UTC+7 the head would have been `d45779b3` (2026-08-26); nothing in Turnip changed in between.
- Their README recommends **`TU_DEBUG=sysmem`** on 710/720/722 for stability (GMEM is more prone to
  rendering artifacts there). It is NOT baked into the driver: set it in the container's
  environment for a7xx users.
- WinNative's Android-side scripts (gralloc `gmsm` bypass, the AIMapper gralloc backend, the UBWC
  swapchain usage bit) are applied as their build applies them; the files they change are either
  not compiled in a Wayland build (u_gralloc, vk_android.c) or inert without Android's loader (the
  `ahb_vendor_usage_compressed` field). Their Android build's NDK `sed` fixes and the
  `-Werror=gnu-empty-initializer` strip are build-environment steps, not part of the driver, and are
  not done.
- Sizes: the artifact ships the drivers unstripped; the wcp's own strip step strips every
  `lib/*.so`, so inside the wcp they are ~14 MB each.

## How winewayland picks one (`dlls/winewayland.drv/waylanddrv_main.c`, `use_bundled_drivers`)

Evaluated once per process on the Bannerlator compositor, first match wins:

1. `BANNER_WAYLAND_VK_ICD=<absolute path>` — the ICD manifest of a driver the app manages (an
   imported one). Taken if it is readable; logged at ERR level as
   `winewayland: Vulkan driver <path> (app-selected)`. Not absolute / not readable: ERR, fall
   through.
2. `BANNER_WAYLAND_VK_VARIANT=a7xx`, `a8xx`, `a8xx-perf`, `a8xx-gen8`, `a8xx-smxz`, `a8xx-white`
   or `a8xx-upstream` — the bundled manifest above (`a8xx-perf` -> `banner_wayland_turnip_a8xx_perf.json`,
   `a8xx-gen8` -> `_a8xx_gen8.json`, `a8xx-smxz` -> `_a8xx_smxz.json`, `a8xx-white` -> `_a8xx_white.json`,
   `a8xx-upstream` -> `_a8xx_upstream.json`; `a8xx` is the Balanced one, the default an "Auto" choice
   should make on 8xx). If that file is missing from the wcp: ERR ("… is
   missing (path), using the plain one"), fall through. Any other value: ERR ("unknown
   BANNER_WAYLAND_VK_VARIANT=…"), fall through (`plain` is accepted silently).
3. The plain bundled manifest — today's behaviour.

The chosen manifest becomes `VK_ICD_FILENAMES` and is logged as
`winewayland: Vulkan driver <manifest>` (the app greps for that prefix); its `library_path`,
resolved against the manifest's directory, is `dlopen`ed `RTLD_NODELETE` so the loader can never
unload the driver mid-process (winevulkan pins itself alongside, see its loader.c). Zink/OpenGL is
independent of this choice: it always goes through the bundled EGL (`BANNER_WAYLAND_GL=0` turns
it off) and reaches the same driver through the imagefs Vulkan loader.

## Why our build

Why ours and not Termux's `mesa-vulkan-icd-freedreno` 26.0.6-3, which the plain file used to be: with
Termux's driver, any program that destroys a Vulkan device and creates another (the AIO Graphics
Test switching backends; DiRT Rally 2.0's probe device before its real one) progressively starves
and then hangs; and OpenGL through Zink died after a few seconds. That does not happen with this
build: all eight AIO backends switch fluidly in one launch. (The "OpenGL holds at ~230 fps" that
used to stand here was not a measurement of this path at all: the AIO Graphics Test presents every
one of its backends, its OpenGL one included, through a Vulkan swapchain - its queues are all
`{mesa vk ...}`, never a `{mesa egl ...}` - so it never went through EGL. The EGL path is measured
with a real GL title instead.)

What the recipe needed (all in build_wayland.sh):
- `-Dfreedreno-kmds=msm,kgsl`. With `kgsl` alone Mesa's meson decides the system has no KMS/DRM,
  drops libdrm and never compiles `wsi_common_drm.c`; the Wayland WSI still asks for DRM images, so
  `vkCreateSwapchainKHR` walks into a compiled-out branch and the guest dies with an access
  violation. That was the 2026-09-12 "crashes in vkCreateSwapchainKHR" result.
- With libdrm present Mesa also builds the VK_KHR_display WSI, which stops threads with
  `pthread_cancel`; bionic has none. `patches/wayland/no_pthread_cancel.py` does what Termux's
  0006 does (a SIGUSR2 handler that `pthread_exit`s), written against the source text.
- Termux 0014: the KGSL timestamp wait no longer asserts on an unexpected errno.
- Built unstripped (`-Dstrip=false`, resolvable crash addresses) with `-Db_ndebug=true`.
Termux's other patches were checked: 0000/0002/0018 are applied inline; 0008/0015 only act when
`__TERMUX__` is defined; 0003 only affects the wl_shm path.

# OpenGL (EGL + Zink)

`usr/lib/libEGL.so.1`, `libGLESv2.so.2` and `libgallium-26.3.0-devel.so` are Mesa 26.3.0-devel at
7cda7850edd103ace21aac37d416d2fdf7a282e1 (the Banners-Turnip release commit), from the same
`build_wayland.sh` tree as the plain Turnip: EGL on the Wayland platform with Zink, no LLVM,
no GLX, as a Linux-style build on bionic like Termux's Mesa. Since versionCode 9, `libEGL.so.1`
alone comes from a later run (Banners-Turnip `wayland` `644f1a5c`, workflow run 34877806759, the
no-render-node fix below) while every other library is still versionCode 7's run 34804055227: the
fix changes nothing but EGL's Wayland platform code, the two runs build the same Mesa commit with
the same flags, the new libEGL imports exactly the symbols the old one did and the old
libgallium exports exactly what the new run's does - so the Vulkan drivers and Zink stay the
bytes already proven, and only EGL changed. Since versionCode 12 the eight Turnips come from
Banners-Turnip `wayland` `0121416`, run 34999563098 (the UBWC request above); that run's libEGL,
libGLESv2 and libdrm are byte-identical to the ones here and its libgallium differs only in the
tree hash it embeds, so those stay. winewayland points Mesa at Zink
(`MESA_LOADER_DRIVER_OVERRIDE=zink`, `WINE_USE_EGL=1`; NOT `LIBGL_ALWAYS_SOFTWARE`, which makes
Zink demand a CPU Vulkan device) when these are present.

**Since versionCode 7 this EGL goes through Mesa's Wayland _DRM_ path, and that is what makes
native OpenGL work at all.** Up to versionCode 6 `build_wayland.sh` forced every Zink display onto
`dri2_initialize_wayland_swrast()` - a path with no `zwp_linux_dmabuf_v1` branch at any version,
which therefore never sees the compositor's dma-buf feedback, never gets a render node, and leaves
`fd_render_gpu` at -1, which `dri2_setup_device()` refuses unless `ForceSoftware` is set (and
`ForceSoftware` makes Zink demand a CPU Vulkan device). So the kopper screen that patch was written
for could never be built: EGL retried in software, this gallium build has no rasteriser, and native
GL windows committed never-written buffers - solid black, no GPU frame, from the first Wayland
build until 2026-09-13. Stock Mesa instead takes `dri2_initialize_wayland_drm()`, which binds
dma-buf at `MIN(version, 4)`, takes `main_device` out of the default feedback (the compositor has
advertised version 4 with a real format table since Bannerlator's 2026-09-13 build), opens that
render node and lands on `driver_name = zink`, `kopper = true`. GL then renders on the Turnip above
and presents through its Vulkan WSI - the same swapchain code, zero-copy `banner_ahb_v1` included,
that every Vulkan game uses. Note what the render node is here: `/dev/dri/renderD128` is the
display controller (`msm_drm`), not the Adreno, which is reached with KGSL; nothing renders on it.
Zink matches the physical device by DRM major/minor and falls back to "the only Vulkan device
there is" when no match exists - which, with one ICD in `VK_ICD_FILENAMES`, is our Turnip.
`-Dllvm=disabled` stays: on this path nothing is rasterised on the CPU, and llvmpipe would only add
a large CPU renderer nothing would ever pick.

**Since versionCode 9 the render node is optional.** Retail phones (Adreno 830/840 reports,
2026-09-14) give apps no `/dev/dri` node at all, so the compositor's feedback names `main_device`
0:0; stock Mesa then has `fd_render_gpu = -1`, still builds the kopper screen (fd -1 means "no DRM"
in `kopper_init_screen`), but `dri2_setup_device(disp, false)` fails the display
(`loader_is_device_render_capable(-1)`, no render-only fallback for -1) and `eglInitialize` retries
in software: black window, sound playing - on every such phone, since versionCode 7. Banners-Turnip
`patches/wayland/egl_wayland_no_drm_node.py` (applied by `build_wayland.sh`) changes
`dri2_initialize_wayland_drm()` only: when the display is kopper and there is no usable node (none
in the feedback, or one `loader_is_device_render_capable()`/`_eglFindDevice()` cannot place under
an enforcing SELinux policy - exactly when the stock call would fail), it drops the fd, builds the
kopper screen without one (`pipe_loader_vk_probe_dri` -> `zink_create_screen` -> the one Vulkan
device, as X11's `LIBGL_KOPPER_DRI2` path does) and leaves the display without an EGLDevice (as
Android's pure-swrast path does; the software EGLDevice would make win32u report the display
unaccelerated). A node that works keeps the stock path. `wine_debug.log` shows which ran:
`MESA-EGL: warning: wayland-egl: the compositor names no DRM render node this process can open;
running zink on the Vulkan device without one` (this path), `… OpenGL is on Mesa's wl_shm software
path … the window will stay black` (the software fallback, which still means black), nothing new
on the DRM path.
Zink opens `libvulkan.so.1`, the imagefs Vulkan loader, which follows `VK_ICD_FILENAMES` to the
Wayland Turnip above.

`libwayland-client.so` and `libwayland-egl.so` are Termux's libwayland 1.25.0-1, the version these
libraries were linked against.

# Keyboard layout names (xkeyboard-config)

`usr/share/X11/xkb/` is xkeyboard-config 2.48's data (Termux x11 package, see `XKB-SOURCE.md`),
bundled into the wcp as `share/X11/xkb`. The bundled libxkbregistry/libxkbcommon default to
Termux's private root, which the app cannot read, so `rxkb_context_parse_default_ruleset()` used
to fail in every container and winewayland named every layout "us". `use_bundled_xkb()`
(waylanddrv_main.c) now sets `XKB_CONFIG_ROOT=<wcp>/share/X11/xkb` before the compositor is
contacted, when the variable is unset and `rules/evdev.xml` is readable there, and logs
`winewayland: Xkb config root <path>`. If parsing still fails the keyboard keeps working with the
"Xkb registry unavailable, layout names default to us" WARN as before.
