# smaa-dxvk-layer (vkbchoom)

A Vulkan implicit layer that injects SMAA anti-aliasing into the swapchain
present path. Built for **MGSV: The Phantom Pain on Windows**, running through
**DXVK**.

MGSV's anti-tamper blocks conventional D3D11 injectors (ReShade's normal hook,
3Dmigoto, RenderDoc). Running the game through DXVK turns its D3D11 calls into
Vulkan calls, and a Vulkan implicit layer can then intercept the present path
without the application's cooperation. This layer hooks three calls —
`vkCreateSwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkQueuePresentKHR` — and
runs SMAA's three passes between the game rendering a frame and that frame
reaching the screen.

Stripped-down fork of [vkBasalt](https://github.com/DadSchoorse/vkBasalt):
SMAA, an optional RCAS sharpen pass, a depth-based outline pass (on by
default), and two experimental colour stylisations (posterize,
black/white/red). No FXAA/LUT/deband, no ReShade FX support, no X11.

---

# Building

## What you need

| | |
|---|---|
| **Compiler** | MSVC (Visual Studio 2022 **Build Tools** is enough — no IDE needed). clang-cl and MinGW also work. |
| **Build system** | Meson + Ninja — `pip install meson ninja` |
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

Two artifacts come out:

```
builddir\src\smaa_layer.dll        the layer
builddir\tools\install_vkbchoom.exe  registers it, and self-diagnoses
```

## Verify the build before installing it

```
dumpbin /exports builddir\src\smaa_layer.dll
```

**Three** names must appear:

```
vkNegotiateLoaderLayerInterfaceVersion
vkbChoom_GetDeviceProcAddr
vkbChoom_GetInstanceProcAddr
```

If `vkNegotiateLoaderLayerInterfaceVersion` is missing, a Vulkan loader that
requires interface-v2 negotiation will skip the layer **silently** — game runs
fine, nothing logs, nothing errors. `src/smaa_layer.def` exists to prevent
exactly that; see *Notes for maintainers* below.

MinGW equivalent: `objdump -p smaa_layer.dll`.

---

# Installing

Copy four files into the MGS_TPP folder — the one with `mgsvtpp.exe` and your
DXVK `d3d11.dll`:

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
install_vkbchoom.exe                register, verify, and check everything
install_vkbchoom.exe -uninstall     remove the registration
install_vkbchoom.exe -status        report registration state only
install_vkbchoom.exe -check         run all checks, change nothing
install_vkbchoom.exe -debug on|off  diagnostic logging (restart Steam after)
```

Registering is idempotent — running it twice is safe.

### What `-check` looks for

All of these fail silently at runtime, which is why they need explicit checks:

- a Vulkan **loader settings file** (`HKLM\SOFTWARE\Khronos\Vulkan\LoaderSettings`),
  written by Vulkan Configurator and some SDK installs. When one exists it takes
  complete control of layer selection and ignores the `ImplicitLayers` keys
- competing or stale layer registrations in HKCU and HKLM
- `enable_environment` present in the manifest
- whether `library_path` resolves
- DLL architecture, and Mark-of-the-Web (which it clears for you)
- **DXVK actually being present** — without `d3d11.dll` the game uses native
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
game" — once `vkbchoom.json` is registered, any process on the machine that
creates a Vulkan instance gets `smaa_layer.dll` loaded and asked to
participate. That's not hypothetical: `vkbchoom_load.log` has shown this
layer loading into chaiNNer's bundled `python.exe`, because chaiNNer's Vulkan
backend does the same thing MGSV does — create an instance — and the loader
doesn't discriminate.

The layer checks the host process's own executable name before any real
interception happens, and is a complete passthrough everywhere except a
configured target: no fake swapchain, no SMAA, nothing — every function
resolves straight to whatever's underneath it, the same as if the layer
weren't installed. Default target is `mgsvtpp.exe`; override with
`targetExecutable` in `vkbchoom.conf` (case-insensitive) to point this at a
different game's executable instead.

One thing this doesn't (yet) avoid: `vkbchoom.conf` itself is still read in
every process the layer loads into, since the check needs a config value
before it can know whether to skip everything else. So `smaa_layer.log` may
still appear with a config dump somewhere harmless like chaiNNer, even though
nothing past that point runs. Cosmetic, not functional — flag it if it's
worth tightening further.

---

# Using it

SMAA is **always on**. There is no hotkey and nothing to press — install it and
play.

There was an F9 A/B toggle in earlier builds. It worked, but SMAA at sane
settings is subtle enough that you can't tell by eye which state you're in,
which made it a way to silently disable your anti-aliasing for a whole session
by brushing one key. Removed.

To A/B compare, set `renderMode = bypass` in `vkbchoom.conf` and relaunch: the
layer stays loaded and registered but passes frames through untouched.

## Screenshots

`screenshotKey` (default `F11`) captures the current frame — the real, final
swapchain image, after every active effect (SMAA, RCAS, outline, posterize,
black/white/red, whatever's on) has already run — to a timestamped BMP in
`screenshots\` next to `smaa_layer.dll`. See `vkbchoom.conf` for the full
list of keys this accepts.

This exists because external capture tools (NVIDIA's overlay, Steam) can miss
this layer's own work specifically in exclusive fullscreen: their capture
path in that mode reads from a different point in the present chain than
what's actually on screen, so a screenshot taken that way can show the
pre-sharpened frame even though the display doesn't. This isn't a bug in
those tools or in this layer individually, just friction from having a
Vulkan layer inserted into a chain those tools weren't written expecting —
capturing from inside the layer itself sidesteps the ambiguity entirely,
since it reads the exact image about to be handed to the real present call.

The hotkey is a read-only key-state poll (`GetAsyncKeyState`), not a hook —
it can't intercept, consume, or delay the keypress, so the game (and
anything else watching that key) sees input exactly as it always would. This
is a different situation from the F9 toggle removed above: that one silently
changed behaviour with no visible confirmation either way, which is what
made it risky. This either produces a file or it doesn't, and
`smaa_layer.log` logs both the attempt and the result either way.

Output is BMP, not PNG — uncompressed and larger on disk, but written with
nothing beyond what this project already links against. A PNG path was
tried and produced invalid output in testing; rather than debug an image
codec under release time pressure, this reverted to the simpler format that
was already confirmed working.

The capture briefly blocks on the GPU to guarantee the frame it grabs is
actually complete — expect a small, one-frame hitch exactly when you press
it, and only then.

## Tuning

Edit `vkbchoom.conf` next to the DLL. Read at launch — no rebuild needed.

Shipped defaults: threshold 0.03, 32/16 search, corner rounding 25, colour
edge detection. The threshold sits deliberately between SMAA's reference
"Ultra" (0.05) and the aggressive 0.02 — more coverage than Ultra, without the
texture softening 0.02 introduces.

`smaaThreshold` is the only dial that trades sharpness for coverage:

| value | effect |
|---|---|
| `0.08` | very sharp, only strong edges touched |
| `0.05` | SMAA reference Ultra |
| `0.03` | **default.** More coverage than Ultra, still crisp |
| `0.02` | aggressive, noticeably softer |
| `0.001` | diagnostic only — flags nearly every pixel, visibly smears detail |

### Sharpen (RCAS)

Off by default. `rcasSharpen = on` adds a single extra pass after SMAA: AMD
FSR1's RCAS (Robust Contrast Adaptive Sharpening), reimplemented directly from
AMD's published algorithm. This is the sharpen half of FSR1 only — there's no
lower-than-native internal render target in this pipeline for the upscaling
half (EASU) to scale up from, so it isn't in scope here.

`rcasSharpness` follows AMD's own convention: `0.0` is maximum sharpening,
and each `+1.0` is one stop (halving) less. RCAS solves for the most
sharpening it can apply before clipping highlights or shadows, so it's
naturally conservative near strong edges — `0.2`-`0.4` is a reasonable
mild-to-moderate range; you don't need to go near `0.0` for it to be visible.

The layer looks for `vkbchoom.conf` next to `smaa_layer.dll` first, then next
to the game exe, then the working directory — all as absolute paths.

### Depth outline

Off by default, and needs `depthCapture = on` as well as `depthOutline = on`
— two separate switches, since depth capture is a real cost (extra image
tracking, an extra pass reading it) worth paying only when something is
actually using it.

Draws a dark line along depth discontinuities instead of colour ones, so it
traces silhouettes — where an object actually ends against whatever's behind
it — rather than colour-edge or texture-detail artifacts. This is what gives
outline-based "toon" looks their clean, shape-following lines instead of a
messy tangle along every bit of surface detail.

**Still worth verifying, even with the resolution match.** Set
`depthOutlineDebugView = on` before turning the outline itself on: it
replaces the outline computation with a direct grayscale view of whatever's
being captured. A sane result should look like a rough grayscale render of
the scene itself — near/far gradient, character and object shapes visible as
distinct depth "shapes" — not a tiny corner of the screen, a flat unchanging
colour, or a single frame that never updates while the game keeps running.
Confirm that first; only then does `depthOutlineThreshold` tuning mean
anything.

**How this picks a depth image.** Fox Engine creates a lot of depth-format
resources — shadow map atlases, downsample cascades, and the real per-frame
scene buffer all show up as the same kind of Vulkan object. The original
vkBasalt-inherited tracking locked onto whichever one was created *first*
and never let go, which in testing turned out to be an early loading-screen
resource, not the real depth buffer — the outline pass just showed a frozen
single frame while the game kept running underneath it. It now instead
searches every depth image that's actually been bound for one whose
resolution matches the current swapchain's, preferring the most recently
bound match if more than one exists at that resolution. This isn't a
perfect disambiguator — if the engine ever has two genuinely different
depth-format resources at the exact same resolution alive at once, this
can't tell them apart — but it reliably rules out the shadow maps and
downsample chains, which was the actual problem. `smaa_layer.log` now logs
every depth image detected (index, format, resolution) and whether each one
matched an active swapchain and got switched to, so a wrong pick is visible
directly in the log rather than only showing up as a wrong-looking debug
view.

`depthOutlineThreshold` was originally checked against raw device-space
depth, which is *not* linear — perspective projection compresses precision
hard toward the far plane. In testing that meant the outline only ever
appeared on nearby geometry and never on anything at a distance. It's now
computed on *linearised* depth (via reciprocal — `1/depth` — which undoes
that compression for a conventional, non-reversed-Z perspective buffer) and
compared as a relative rather than absolute difference, so the threshold
means roughly "N% closer/farther" regardless of distance from camera. This
assumes Fox Engine uses a conventional depth convention rather than
reversed-Z; if distant geometry still doesn't outline after this change,
that assumption is the first thing to question — the debug view is how to
check.

### Posterize

Off by default. `posterize = on` flattens colour into `posterizeLevels`
discrete steps per channel instead of a smooth gradient — the actual
flat-shaded half of an "anime" look, as distinct from the outline above
(which draws the ink lines) or black/white/red below (which discards colour
entirely). Meant to be layered with the outline: outline gives the lines,
posterize gives the flat fills.

New and untested against real gameplay.

### Black/white/red

Off by default. `blackWhiteRed = on` adds a selective-colour pass, in the
Schindler's List sense specifically: a full grayscale image, with colour
kept only where a pixel is genuinely, strongly red — not a flat black/white/
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

The `BUILD` line is stamped in at compile time. **Check it first** — if it
doesn't match the build you think you copied, the game folder has a stale DLL,
and that has burned more time on this project than any actual bug.

`result: 0` is `VK_SUCCESS`. The `submitting` line confirms the SMAA command
buffer — not the passthrough one — is what reaches the GPU.

**`%LOCALAPPDATA%\vkbchoom\vkbchoom_load.log`** — written from `DllMain` with
raw Win32 calls only, so it appears whenever the DLL is in the process at all,
regardless of working directory, CRT state, or folder permissions. If
`mgsvtpp.exe` isn't in here, the layer isn't loading and nothing else matters.

`VKBCHOOM_LOG_LEVEL=trace` adds per-call entry markers — several thousand
lines, mostly MGSV's repeated DXGI adapter enumeration at startup.

If the layer isn't loading and `-check` comes back clean: `-debug on`, restart
Steam fully, and run Sysinternals DebugView **as administrator** with *Capture
Win32* and *Capture Global Win32* on. The Vulkan loader states its reasoning
there — on Windows it writes to stderr and `OutputDebugString`, and
`mgsvtpp.exe` is a windowed process with no console, which is why layer
problems here are so consistently silent.

---

# Notes for maintainers

Things that cost real time on this codebase and are easy to reintroduce.

### The export table is not optional

`vkNegotiateLoaderLayerInterfaceVersion` used to be exported via
`#pragma comment(linker, "/export:...")` — MSVC-only. Under GCC/MinGW that
pragma is ignored without a warning, and `gnu_symbol_visibility: 'hidden'` in
`src/meson.build` then buries the symbol. Verified empirically:

```
without smaa_layer.def       with smaa_layer.def
--------------------------   --------------------------------------
vkbChoom_GetDeviceProcAddr   vkNegotiateLoaderLayerInterfaceVersion
vkbChoom_GetInstanceProcAddr vkbChoom_GetDeviceProcAddr
                             vkbChoom_GetInstanceProcAddr
```

Keep the `.def`, and keep checking `dumpbin /exports` after building.

### Never log to a relative path

The old logger opened `"smaa_layer.log"` — relative, resolved against the
process working directory — and never checked `is_open()`. A failed open left a
non-null stream with failbit set, so every write vanished. "DLL never loaded"
and "DLL loaded but the working directory moved" produced byte-identical
evidence: no file. `src/config.cpp` had the same bug for `vkbchoom.conf`, which
meant `renderMode` silently fell back to defaults.

All paths are absolute now, resolved from the DLL's own module handle via
`GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)`, tried in
order, each checked, with `OutputDebugStringA` as the last-resort channel.

### `enable_environment` is a trap for Steam titles

The manifest used to carry `enable_environment: { ENABLE_VKBCHOOM: "1" }`,
which means the loader won't even `LoadLibrary` the DLL unless that variable is
in the game's environment. It was supplied via Steam launch options — and Steam
stores those in `Steam\userdata\<id>\config\localconfig.vdf`, which is local to
the machine and **not restored by a clean Windows + Steam reinstall**. It's
gone. Don't put it back.

### Installers must not toggle

Both the old `install.ps1` and the old `install_vkbchoom.exe` toggled: running
one twice "to be sure" silently unregistered the layer. Registration is
idempotent now, with an explicit `-uninstall`. The PowerShell scripts are gone
entirely — the exe isn't subject to execution policy.

### `stripFseInfo`

Strips `VK_EXT_full_screen_exclusive` structs from the swapchain create. It was
a workaround for swapchain creation failing on NVIDIA under a layered setup,
but it made DXVK believe it had exclusive fullscreen while the driver never got
the request — which broke keyboard input at scene transitions. Default is now
`off`. Only turn it on if `CreateSwapchainKHR ... result:` stops being `0`.

### The layer is global by construction — gate on the process, not the registration

`vkbchoom.json` has to register as `GLOBAL` with no `enable_environment` (see
above) for the layer to load at all under Steam. That means every
Vulkan-creating process on the machine gets this DLL loaded into it — this is
exactly how it turned up running inside chaiNNer's bundled `python.exe`.
There's no registration-level fix for this; the Vulkan loader has no "only
for this one executable" concept.

The gate lives entirely in code: `isTargetProcess()` in `basalt.cpp`, checked
once at the top of `INTERCEPT_CALLS`, wrapping every `GETPROCADDR` entry —
including the self-referential `vkGetInstanceProcAddr` /
`vkGetDeviceProcAddr` ones, not just the "real" functions further down. That
last part matters: outside the target process, asking this layer for its own
`GetInstanceProcAddr`/`GetDeviceProcAddr` hands back whatever's *next* in the
chain instead, not a pointer to an inert version of our own function. A
non-target process that resolves either of those once gets a direct line
past us for everything after — our own hooks are never reached again for
that instance/device, not just declined when reached. Don't special-case the
self-referential entries back out of that `if` block; that would put the
layer back in every process's chain, just quietly no-opping deeper down,
which is a smaller version of the exact problem this exists to fix.

### Debug visualiser

To see SMAA's intermediate stages directly, replace the body of
`src/shader/smaa_neighbor.frag.glsl` with:

```glsl
layout(set = 0, binding = 0) uniform sampler2D colorImg;
layout(set = 0, binding = 1) uniform sampler2D edgesTex;
layout(set = 0, binding = 4) uniform sampler2D blendTex;

layout(location = 0) out vec4 fragColor;
layout(location = 0) in vec2 textureCoord;
layout(location = 1) in vec4 offset;

void main()
{
    vec2 edges   = texture(edgesTex, textureCoord).rg;
    vec4 weights = texture(blendTex, textureCoord);
    fragColor = vec4(edges.r, edges.g,
                     clamp(dot(weights, vec4(1.0)) * 4.0, 0.0, 1.0), 1.0);
}
```

(drop the `#include` lines and the `#version`/`#extension` header stays.)

Rebuild and set `smaaThreshold = 0.001`. Red/green = detected
edges (pass 1). Blue = blend weights (pass 2). Reading binding 1 here is legal —
the descriptor set carries all five bindings for every pass.

- coloured fringes on outlines → both stages working
- red/green but no blue → blend weight pass produces nothing
- fully black → edge detection produces nothing
- normal game image → stale DLL

This matters because `SMAANeighborhoodBlendingPS` with zero blend weights
returns the input pixel *bit-identically*. "No visible difference" is the exact
fingerprint of zero blend weights, not vague evidence.

---

# License

See `LICENSE`. Upstream vkBasalt is zlib-licensed; SMAA (Jorge Jimenez et al.)
carries its own MIT-style license, reproduced in `src/shader/smaa.h`.
