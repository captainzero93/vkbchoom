# smaa-dxvk-layer (vkbchoom) for MGSV

A Vulkan implicit layer that injects SMAA anti-aliasing into the swapchain
present path. Built for **MGSV: The Phantom Pain on Windows**, running through
**DXVK**.

Stripped-down fork of [vkBasalt](https://github.com/DadSchoorse/vkBasalt):
SMAA, an optional RCAS sharpen pass, a depth-based outline pass (on by
default), and two experimental colour stylisations (posterize,
black/white/red). No FXAA/LUT/deband, no ReShade FX support, no X11.

---

# Fixed: earlier versions broke other Vulkan applications

**If you installed a build before this one and your emulators, other Vulkan
games or `vulkaninfo` stopped working, that was this mod, and this version
fixes it.**

Immediate fix if you're on an old build:

```
install_vkbchoom.exe -uninstall
```

That removes the Vulkan implicit-layer registration and everything works
again. Deleting the mod files on their own does **not** do this — see
[Uninstalling properly](#uninstalling-properly).

### What went wrong

Vulkan has no way to register a layer for a single application. A global
implicit layer is the only mechanism the loader offers, so `smaa_layer.dll`
gets loaded into every process on the machine that creates a Vulkan instance.
That part is unavoidable and normal.

The layer was supposed to notice it wasn't in MGSV and get out of the way. It
didn't. The process check also disabled the code that hands out
`vkCreateInstance`, which is the only thing that populates the layer's
dispatch table — so outside MGSV the table was never populated, and every
function the loader or the application asked for came back `NULL`. The layer
stayed in the middle of the loader chain answering NULL to everything.
Whatever dereferenced the first null function pointer took the access
violation. That's the `0xC0000005` in `vulkaninfo` and the emulators failing
to start.

It also wrote its breadcrumb log and opened `smaa_layer.log` inside those
unrelated processes, which is why vkbchoom's own diagnostics listed
executables that had nothing to do with MGSV.

### What changed

- **The layer now declines at negotiation.**
  `vkNegotiateLoaderLayerInterfaceVersion` is the first thing a modern loader
  calls; outside the target executable it returns
  `VK_ERROR_INITIALIZATION_FAILED` and the loader drops the layer from the
  chain before an instance exists. Other applications see no layer at all.
- **The fallback path is a real passthrough.** If a loader skips negotiation,
  the proc-addr functions now forward to the next layer instead of returning
  NULL, and pass the create-info down unmodified.
- **No side effects outside the target.** No log file, no breadcrumb, no
  `OutputDebugString`, no config read. `VKBCHOOM_FORCE_LOG=1` overrides this
  if you're debugging the gate itself.
- **`-uninstall` cleans up properly**, including stale registrations left by
  earlier installs in other folders.
- **The installer explains what registering does** before you live with it,
  and no longer reports unrelated layers (ReShade, etc.) as vkbchoom problems.
  It has never modified, and will never modify, a layer registration that
  isn't its own.
- **Running the installer with no arguments is now a toggle** — first run
  installs, second run uninstalls. Use `-install` if you want it to only ever
  install.

Full technical detail in [Scope](#scope) and
[Notes for maintainers](#notes-for-maintainers).

### Verifying it on your own machine

After installing, run `vulkaninfo` and anything else Vulkan-based, then check:

```
%LOCALAPPDATA%\vkbchoom\vkbchoom_load.log
```

That file should **not exist** unless you've launched MGSV. If it does, open
it — any line whose executable column isn't `mgsvtpp.exe` means the process
gate isn't working, and that's worth reporting as a bug.

---

# Building

## What you need


| **Compiler** | MSVC (Visual Studio 2022 **Build Tools** is enough - no IDE needed). clang-cl and MinGW also work. |
| **Build system** | Meson + Ninja - `pip install meson ninja` |
| **Vulkan SDK** | [LunarG](https://vulkan.lunarg.com/sdk/home). Provides the headers (`vulkan/vulkan.h`, `vulkan/vk_layer.h`) *and* `glslangValidator.exe`, which the build shells out to for the shaders. |

`glslangValidator` must be on `PATH`. The SDK installer normally handles that;
check with `glslangValidator --version`.

## Build

From an **x64 Native Tools Command Prompt for VS 2022** (so `cl.exe` is on
`PATH`):

```
meson setup builddir --backend=ninja --buildtype=release
ninja -C builddir
```

If meson can't find the Vulkan headers:

```
meson setup builddir --backend=ninja --buildtype=release ^
    -Dcpp_args=-I"%VULKAN_SDK%\Include" -Dc_args=-I"%VULKAN_SDK%\Include"
```

```
builddir\src\smaa_layer.dll        the layer
builddir\tools\install_vkbchoom.exe  registers it, and self-diagnoses
```

---

# Installing

Copy four files into the MGS_TPP folder - the one with `mgsvtpp.exe` and your
DXVK `d3d11.dll` + dxgi.dll , I used DXVK 2.3.1:

```
builddir\src\smaa_layer.dll
builddir\tools\install_vkbchoom.exe
config\vkbchoom.json
config\vkbchoom.conf
```

Double-click `install_vkbchoom.exe`.

That's it. It writes the layer manifest to `HKCU` (no admin needed), verifies
the write by reading it back, then runs a full set of checks and reports
anything that would stop the layer loading.

**No Steam launch options. No environment variables.** Launch the game normally.

```
install_vkbchoom.exe                TOGGLE: installs if off, uninstalls if on
install_vkbchoom.exe -install       install only - never toggles off
install_vkbchoom.exe -uninstall     remove the registration (and stale ones)
install_vkbchoom.exe -status        report registration state only
install_vkbchoom.exe -check         run all checks, change nothing
install_vkbchoom.exe -debug on|off  diagnostic logging (restart Steam after)
```

Running it with no arguments is a **toggle**. Double-click once to install,
again to uninstall - it says which of the two just happened. If you want to
check the install without risking turning it off, use `-status` or `-check`;
if you want "install and stay installed", use `-install`.

### Uninstalling properly

Installing writes a global Vulkan implicit-layer registration under
`HKCU\Software\Khronos\Vulkan\ImplicitLayers`. **Deleting the mod files does
not remove it** - the value is keyed by the absolute path of
`vkbchoom.json`, so a deleted or moved install leaves the loader with a
dangling implicit layer forever.

Always uninstall with:

```
install_vkbchoom.exe -uninstall
```

which removes our registration and sweeps HKCU for any stale `vkbchoom`
entries left by earlier installs in other folders. It reports, but will not
touch, anything in HKLM or any layer that isn't ours.

### What `-check` looks for

All of these fail silently at runtime, which is why they need explicit checks:

- a Vulkan **loader settings file** (`HKLM\SOFTWARE\Khronos\Vulkan\LoaderSettings`),
  written by Vulkan Configurator and some SDK installs. When one exists it takes
  complete control of layer selection and ignores the `ImplicitLayers` keys
- competing or stale layer registrations in HKCU and HKLM
- `enable_environment` present in the manifest
- whether `library_path` resolves
- DLL architecture, and Mark-of-the-Web (which it clears for you)
- **DXVK actually being present** - without `d3d11.dll` the game uses native
  D3D11, never creates a Vulkan instance, and no layer can load
- a `vulkan-1.dll` in the game folder shadowing the system loader. The
  application directory beats System32 in the DLL search order and
  `vulkan-1.dll` isn't a KnownDLL, so the game can end up on a different loader
  than `vulkaninfo` uses
- `VK_LOADER_LAYERS_DISABLE` and friends, across process/user/machine scope
- the breadcrumb log from the last launch

---

# Scope

Vulkan's implicit-layer registration has no concept of "only for this one
game" - once `vkbchoom.json` is registered, any process on the machine that
creates a Vulkan instance gets `smaa_layer.dll` loaded and asked to
participate. That's not hypothetical: `vkbchoom_load.log` has shown this
layer loading into chaiNNer's bundled `python.exe`, and into unrelated
emulators.

**This was previously handled badly, and it broke things.** The layer checked
the executable name before intercepting anything, which the README described
as "a complete passthrough everywhere except a configured target". It was
not. Skipping the interception also skipped handing out `vkCreateInstance`,
so the dispatch maps were never populated, so every subsequent function query
fell through to a `return nullptr`. The layer stayed in the chain and
answered NULL to everything asked of it. Unrelated Vulkan applications
crashed downstream on the first null function pointer they dereferenced.

How it works now, in order:

1. **Negotiation declines.** `vkNegotiateLoaderLayerInterfaceVersion` is the
   first thing a modern loader calls. Outside the target process it returns
   `VK_ERROR_INITIALIZATION_FAILED` and the loader drops the layer from the
   chain before an instance exists. Nothing else in the DLL runs.
2. **Passthrough as a fallback.** If a loader skips negotiation and calls the
   manifest's named exports directly, `vkGetInstanceProcAddr` and
   `vkGetDeviceProcAddr` now forward properly instead of returning NULL -
   minimal `vkCreateInstance`/`vkCreateDevice` shims advance the chain link,
   record the next layer's proc-addrs, and pass `pCreateInfo` down
   unmodified. No dispatch tables, no swapchain hooks, no effects.
3. **No side effects either.** The breadcrumb log and the `Logger`
   constructor are both gated too, so the layer no longer creates files or
   writes `OutputDebugString` inside other people's applications. The earlier
   note about `vkbchoom.conf` being read in every process is also resolved:
   the gate reads the one key it needs with raw `CreateFile`/`ReadFile` and
   builds no `Config` object at all outside the target.

The decision depends on nothing but `kernel32` and the executable name, so it
is safe to answer from `DllMain` and from negotiation - earlier than any
config, logger or CRT state can be relied on. Default target is
`mgsvtpp.exe`; override with `targetExecutable` in `vkbchoom.conf`
(case-insensitive), or `%VKBCHOOM_TARGET_EXE%` for a temporary override.

**The registration itself is still global.** There is no way around that -
it's the only mechanism Vulkan offers. The layer is inert everywhere else,
but the registry value exists machine-wide until you remove it, and deleting
the mod files does not remove it. `install_vkbchoom.exe -uninstall` does.

---

# Using it

SMAA is **always on**. There is no hotkey and nothing to press - install it and
play.

There was an F9 A/B toggle in earlier builds. It worked, but SMAA at sane
settings is subtle enough that you can't tell by eye which state you're in,
which made it a way to silently disable your anti-aliasing for a whole session
by brushing one key. Removed.

To A/B compare, set `renderMode = bypass` in `vkbchoom.conf` and relaunch: the
layer stays loaded and registered but passes frames through untouched.

## Screenshots

`screenshotKey` (default `F11`) captures the current frame - the real, final
swapchain image, after every active effect (SMAA, RCAS, outline, posterize,
black/white/red, whatever's on) has already run - to a timestamped BMP in
`screenshots\` next to `smaa_layer.dll`. See `vkbchoom.conf` for the full
list of keys this accepts.

This exists because external capture tools (NVIDIA's overlay, Steam) can miss
this layer's own work specifically in exclusive fullscreen: their capture
path in that mode reads from a different point in the present chain than
what's actually on screen, so a screenshot taken that way can show the
pre-sharpened frame even though the display doesn't. This isn't a bug in
those tools or in this layer individually, just friction from having a
Vulkan layer inserted into a chain those tools weren't written expecting


## Tuning

Edit `vkbchoom.conf` next to the DLL. Read at launch - no rebuild needed.

Shipped defaults: threshold 0.03, 32/16 search, corner rounding 25, colour
edge detection. The threshold sits deliberately between SMAA's reference
"Ultra" (0.05) and the aggressive 0.02 - more coverage than Ultra, without the
texture softening 0.02 introduces.

`smaaThreshold` is the only dial that trades sharpness for coverage:

| value | effect |
|---|---|
| `0.08` | very sharp, only strong edges touched |
| `0.05` | SMAA reference Ultra |
| `0.03` | **default.** More coverage than Ultra, still crisp |
| `0.02` | aggressive, noticeably softer |
| `0.001` | diagnostic only - flags nearly every pixel, visibly smears detail |

### Sharpen (RCAS)

Off by default. `rcasSharpen = on` adds a single extra pass after SMAA: AMD
FSR1's RCAS (Robust Contrast Adaptive Sharpening), reimplemented directly from
AMD's published algorithm. This is the sharpen half of FSR1 only - there's no
lower-than-native internal render target in this pipeline for the upscaling
half (EASU) to scale up from, so it isn't in scope here.

`rcasSharpness` follows AMD's own convention: `0.0` is maximum sharpening,
and each `+1.0` is one stop (halving) less. RCAS solves for the most
sharpening it can apply before clipping highlights or shadows, so it's
naturally conservative near strong edges - `0.2`-`0.4` is a reasonable
mild-to-moderate range; you don't need to go near `0.0` for it to be visible.

The layer looks for `vkbchoom.conf` next to `smaa_layer.dll` first, then next
to the game exe, then the working directory - all as absolute paths.

### Depth outline

Off by default, and needs `depthCapture = on` as well as `depthOutline = on`
- two separate switches, since depth capture is a real cost (extra image
tracking, an extra pass reading it) worth paying only when something is
actually using it.

Draws a dark line along depth discontinuities instead of colour ones, so it
traces silhouettes - where an object actually ends against whatever's behind
it - rather than colour-edge or texture-detail artifacts. This is what gives
outline-based "toon" looks their clean, shape-following lines instead of a
messy tangle along every bit of surface detail.

**Still worth verifying, even with the resolution match.** Set
`depthOutlineDebugView = on` before turning the outline itself on: it
replaces the outline computation with a direct grayscale view of whatever's
being captured. A sane result should look like a rough grayscale render of
the scene itself - near/far gradient, character and object shapes visible as
distinct depth "shapes" - not a tiny corner of the screen, a flat unchanging
colour, or a single frame that never updates while the game keeps running.
Confirm that first; only then does `depthOutlineThreshold` tuning mean
anything.

**How this picks a depth image.** Fox Engine creates a lot of depth-format
resources - shadow map atlases, downsample cascades, and the real per-frame
scene buffer all show up as the same kind of Vulkan object. The original
vkBasalt-inherited tracking locked onto whichever one was created *first*
and never let go, which in testing turned out to be an early loading-screen
resource, not the real depth buffer - the outline pass just showed a frozen
single frame while the game kept running underneath it. It now instead
searches every depth image that's actually been bound for one whose
resolution matches the current swapchain's, preferring the most recently
bound match if more than one exists at that resolution. This isn't a
perfect disambiguator - if the engine ever has two genuinely different
depth-format resources at the exact same resolution alive at once, this
can't tell them apart - but it reliably rules out the shadow maps and
downsample chains, which was the actual problem. `smaa_layer.log` now logs
every depth image detected (index, format, resolution) and whether each one
matched an active swapchain and got switched to, so a wrong pick is visible
directly in the log rather than only showing up as a wrong-looking debug
view.

`depthOutlineThreshold` was originally checked against raw device-space
depth, which is *not* linear - perspective projection compresses precision
hard toward the far plane. In testing that meant the outline only ever
appeared on nearby geometry and never on anything at a distance. It's now
computed on *linearised* depth (via reciprocal - `1/depth` - which undoes
that compression for a conventional, non-reversed-Z perspective buffer) and
compared as a relative rather than absolute difference, so the threshold
means roughly "N% closer/farther" regardless of distance from camera. This
assumes Fox Engine uses a conventional depth convention rather than
reversed-Z; if distant geometry still doesn't outline after this change,
that assumption is the first thing to question - the debug view is how to
check.

### Posterize

Off by default. `posterize = on` flattens colour into `posterizeLevels`
discrete steps per channel instead of a smooth gradient - the actual
flat-shaded half of an "anime" look, as distinct from the outline above
(which draws the ink lines) or black/white/red below (which discards colour
entirely). Meant to be layered with the outline: outline gives the lines,
posterize gives the flat fills.

New and untested against real gameplay.

### Black/white/red

Off by default. `blackWhiteRed = on` adds a selective-colour pass, in the
Schindler's List sense specifically: a full grayscale image, with colour
kept only where a pixel is genuinely, strongly red - not a flat black/white/
red poster.

The first version of this was closer to the poster: a hard 2-tone black/
white split plus anything where red was merely the largest of three
similar channels got flattened to solid red. In testing that meant skin
tones and warm lighting turned solid red (not what "genuinely red" should
mean), and the black/white split turned any noisy or high-frequency source
texture into visual static rather than a clean tone. Both are fixed now:
red requires real saturation on top of channel dominance
(`blackWhiteRedSaturation`, not just `blackWhiteRedSensitivity`), which
skin tones mostly fail; kept pixels keep their own original colour rather
than being flattened to a flat swatch; and the non-red side is a genuine
full grayscale conversion (every luminance level, not two) computed from a
small averaged neighbourhood rather than a single noisy pixel.

New and untested against real gameplay.

## Logging

**`smaa_layer.log`**, next to `smaa_layer.dll`. ~30 lines at the default `info`
level. A healthy run:

```
BUILD = RELEASE 1.0
config file: ...\MGS_TPP\vkbchoom.conf
CreateSwapchainKHR in: format=43 usage=0x13 flags=0x0 extent=1920x1080
CreateSwapchainKHR (transfer) result: 0
SMAA layer initialized, effect pipeline created
first vkQueuePresentKHR reached -- present chain is hooked
submitting SMAA command buffer 00000000089550D0 -- effect is active

```
If the layer isn't loading and `-check` comes back clean: `-debug on`, restart
Steam fully, and run Sysinternals DebugView **as administrator** with *Capture
Win32* and *Capture Global Win32* on. The Vulkan loader states its reasoning
there - on Windows it writes to stderr and `OutputDebugString`, and
`mgsvtpp.exe` is a windowed process with no console, which is why layer
problems here are so consistently silent.

---

# Notes for maintainers

Things that cost real time on this codebase and are easy to reintroduce.

### The export table is not optional

`vkNegotiateLoaderLayerInterfaceVersion` used to be exported via
`#pragma comment(linker, "/export:...")` - MSVC-only. Under GCC/MinGW that
pragma is ignored without a warning, and `gnu_symbol_visibility: 'hidden'` in
`src/meson.build` then buries the symbol. Verified empirically:

```
without smaa_layer.def       with smaa_layer.def
--------------------------   --------------------------------------
vkbChoom_GetDeviceProcAddr   vkNegotiateLoaderLayerInterfaceVersion
vkbChoom_GetInstanceProcAddr vkbChoom_GetDeviceProcAddr
                             vkbChoom_GetInstanceProcAddr
```


### Never log to a relative path

The old logger opened `"smaa_layer.log"` - relative, resolved against the
process working directory - and never checked `is_open()`. A failed open left a
non-null stream with failbit set, so every write vanished. "DLL never loaded"
and "DLL loaded but the working directory moved" produced byte-identical
evidence: no file. `src/config.cpp` had the same bug for `vkbchoom.conf`, which
meant `renderMode` silently fell back to defaults.

All paths are absolute now, resolved from the DLL's own module handle via
`GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)`, tried in
order, each checked, with `OutputDebugStringA` as the last-resort channel.


### `stripFseInfo`

Strips `VK_EXT_full_screen_exclusive` structs from the swapchain create. It was
a workaround for swapchain creation failing on NVIDIA under a layered setup,
but it made DXVK believe it had exclusive fullscreen while the driver never got
the request - which broke keyboard input at scene transitions. Default is now
`off`. Only turn it on if `CreateSwapchainKHR ... result:` stops being `0`.

### The layer is global by construction - gate on the process, not the registration

`vkbchoom.json` has to register as `GLOBAL` with no `enable_environment` (see
above) for the layer to load at all under Steam. That means every
Vulkan-creating process on the machine gets this DLL loaded into it - this is
exactly how it turned up running inside chaiNNer's bundled `python.exe`.
There's no registration-level fix for this; the Vulkan loader has no "only
for this one executable" concept.

The gate lives entirely in code, in three places, and all three matter:

1. `layerShouldRunHere()` in `platform_win32.cpp` - the decision itself.
   Executable name only, `kernel32` only, cached in a plain `int` with a
   constant initialiser so there's no magic-static guard to take under the
   loader lock. It has to be answerable from `DllMain` and from negotiation,
   which is why it can't touch `Config`, `Logger`, iostreams or anything
   that needs static init to have finished.
2. `vkNegotiateLoaderLayerInterfaceVersion` in `basalt.cpp` - returns
   `VK_ERROR_INITIALIZATION_FAILED` outside the target, and the loader drops
   the layer from the chain entirely. This is the one that does the real
   work; everything else is a safety net under it.
3. The passthrough path in `vkbChoom_GetInstanceProcAddr` /
   `vkbChoom_GetDeviceProcAddr`, for loaders that skip negotiation.

### What the previous gate got wrong, and don't do it again

The old note here claimed that outside the target process, asking the layer
for its own `GetInstanceProcAddr`/`GetDeviceProcAddr` "hands back whatever's
next in the chain instead". **It did not.** It handed back `nullptr`.

Gating `INTERCEPT_CALLS` on `isTargetProcess()` also gated the handout of
`vkCreateInstance`. `vkCreateInstance` is the only thing that populates
`instanceDispatchMap`. So in a non-target process the map stayed empty
forever, and the fallthrough at the bottom of both functions - which does
`map.find(...)`, misses, and returns `nullptr` - was the answer to *every*
query, for every function. The layer was still sitting in the chain,
answering NULL to everything. Unrelated Vulkan applications crashed
downstream on the first null function pointer they dereferenced: emulators
stopped launching and `vulkaninfo` died with `0xC0000005`.

The lesson generalises: **an implicit layer cannot opt out by declining to
answer.** Once the loader has put you in the chain you are obliged to give
correct answers, and "no answer" is not a correct answer. There are exactly
two honest options - get removed from the chain (decline at negotiation), or
forward faithfully (the `passthrough_*` functions). Anything that looks like
"just don't hook anything and it'll be fine" is this bug again.

Two corollaries:

- The `passthrough_*` functions keep their own maps, separate from
  `instanceDispatchMap`/`deviceMap`, so no effect, swapchain or screenshot
  code can ever observe a non-target process. Don't merge them.
- `passthrough_CreateInstance` passes `pCreateInfo` down **unmodified**. The
  target path bumps `apiVersion` to 1.1 and can add extensions; doing either
  inside somebody else's application is interference, even if it happens not
  to break anything today.

### Side effects count as interference too

`breadcrumb()` and the `Logger` constructor both used to run unconditionally,
at `DLL_PROCESS_ATTACH` and static init respectively - in every Vulkan
process on the machine. That's how vkbchoom's own diagnostics ended up
listing `citron.exe` and `pcsx2-qt.exe` as having loaded the layer. Both are
gated now. If you add anything that writes a file, touches the registry or
calls `OutputDebugString` from static init or `DllMain`, gate it.

`VKBCHOOM_FORCE_LOG=1` turns the breadcrumb back on everywhere, for when the
thing you're debugging *is* the gate.


# License

See `LICENSE`. Upstream vkBasalt is zlib-licensed; SMAA (Jorge Jimenez et al.)
carries its own MIT-style license, reproduced in `src/shader/smaa.h`.
