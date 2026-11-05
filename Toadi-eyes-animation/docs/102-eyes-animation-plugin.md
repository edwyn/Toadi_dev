# 102 — Eyes animation from a plug-in, with a REST trigger

Status: implemented and visually verified on `192.168.1.184` (app 6.7.0,
2026-10-31). Eight animations are accepted: `hearts`, `spidey`, `spidey2`,
`spider`, `pumpkin`, `badge`, `walle` and `fireworks`. The stock app remains
the owner of the SPI panels; the plug-in draws through the app's public EyesUI
layer API. Final accepted plug-in SHA-256:
`dbbd883a96c4d9bc0df47c11610d710ba4a17f09af6674bf17f67c4c657ebbe8`.

## 1. Why not reuse the standalone SPI driver

`toadi_eyes` can drive `/dev/spidev0.0/.1` directly, but only after stopping
the stock stack: ToadiApp holds both spidevs and the panel GPIOs for its whole
lifetime. Doing that from inside a plug-in (which runs inside ToadiApp) is
self-defeating.

The supported path is the app's **EyesUI** broker service. The stock
`EyesAnimator` uses exactly this path: it obtains an `EyesUILayer` and calls
`drawImages(left, right, WriteMode)` at 8 fps.

## 2. Broker interface and ABI

Service key: **`eyesui`** (lowercase). Evidence:

- 6.8.0 `libToadiLib.so` rodata at 0x164ed4: `...\0zonemotionplanning\0eyesui\0...`
- 6.7.0 `libToadiLib.so` at 0x13bf4b: same string.
- `queryInterfaceNoThrow<toadi::EyesUI>` is exported by `libWebServer.so`
  (6.8.0 0x1947fc, 6.7.0 0x18e478). The plug-in resolves it with `dlsym`
  rather than adding a libToadiLib/libWebServer dependency.

Public interface (aarch64 Itanium ABI; offsets are from the vptr):

```
EyesUI:
  +0x00  ~EyesUI()                                    D1
  +0x08  ~EyesUI()                                    D0
  +0x10  shared_ptr<EyesUILayer> addLayer(int priority)
  +0x18  shared_ptr<EyesUILayer> getLayer(int priority)

EyesUILayer:
  +0x00  ~EyesUILayer()                               D1
  +0x08  ~EyesUILayer()                               D0
  +0x10  void enable(bool)
  +0x18  bool isEnabled() const
  +0x20  void drawText(const std::string&)
  +0x28  void drawText(const std::string&, DisplayMode)
  +0x30  void drawImages(const vector<unsigned char>& left,
                         const vector<unsigned char>& right,
                         Display::WriteMode)
```

Evidence: `libEyesUI.so` vtables at `.data.rel.ro` 0x3a0a8/0x3a2a0 (6.8.0)
and 0x39f68/0x3a128 (6.7.0); `DisplayImpl::update` call from the stock
`EyesAnimator` at `libEmotions.so` 0xcd618-0xcd630 passes `WriteMode = 0`.

`drawImages` is a no-op unless the layer is the visible/enabled layer:
`EyesUILayerImpl::drawImages` calls `EyesUIImpl::isVisible(layer)` first.
`addLayer(priority)` creates the layer for that z-order; the implementation
keeps them in a `std::map<int, shared_ptr<EyesUILayer>>`.

## 3. Frame format and the bit-order correction

The heart frames are the 12 × 1024-byte 128×64 1-bpp frames from
`toadi_src/hearts.h` / `hearts/*.bin`. They are packed **MSB-first**, matching
the standalone `toadi_eyes` driver.

The EyesUI path differs. `DisplayImpl::update` (libPlatforms.so 0x5a6ac)
walks source index `y*width + x` and tests **bit `(x & 7)` — LSB-first** —
before writing the page/column/bit transform:

```
source bit  = (frame[y*width + x] >> (x & 7)) & 1
dest byte   = (7 - (y >> 3)) * width + (127 - x)
dest bit    = 1 << (7 - (y & 7))
```

Passing MSB-packed frames directly leaves a recognisable but ragged/speckled
heart: each byte's 8 horizontal pixels are reversed. Reversing each byte
before `drawImages` makes the plug-in output byte-identical to the
hardware-verified standalone `stored_to_wire()` transform. Offline check on
frame 11:

| Path | Differing wire bytes vs intended |
|---|---|
| MSB frame passed directly | 156 / 1024 |
| byte-reversed frame | 0 / 1024 |

The plug-in therefore stores/embeds `hearts.h` unchanged and calls
`reverseBits()` on each byte in `drawFrame()`.

## 4. Lifecycle and layer behaviour

- Plug-in library is linked with `-Wl,-z,nodelete` (spec 100 §7.3). The
  controller runs detached worker/REST threads; nodelete keeps their code and
  the frame data mapped after the Vision loader drops its temporary handle.
- `TOADI_EXAMPLE_HEARTS=1` gates the controller. It is set by the dev-workdir
  wrapper; without it the plug-in remains the inert VisionAlgo example.
- The controller queries `eyesui` lazily on the first trigger, calls
  `addLayer(2000)` (override: `TOADI_EXAMPLE_HEARTS_PRIORITY`), calls
  `enable(true)`, draws frames at 8 fps, then calls `enable(false)`. Disabling
  the plug-in layer lets the stock layer become visible again.
- The frame loop checks an atomic stop flag every 10 ms, so `/hearts/off`
  responds promptly.

### 4.1 Who owns layer priority

Layer priority is **core ToadiApp functionality**, not part of the
Broker/plug-in loader (spec 100 has no priority concept). It is implemented in
`libEyesUI.so`:

- `EyesUIImpl::addLayer(int zOrder)` (6.7.0 symbol 0x1b7b4) rejects negative
  values and stores one layer per integer in a
  `std::map<int, shared_ptr<EyesUILayer>>`. Adding an existing z-order does not
  replace it (the log string is `Layer with priority {} already existed`).
- `EyesUIImpl::getVisibleLayer()` (0x1bf9c) walks the map in ascending
  z-order and returns the **first enabled** layer.
  `EyesUILayerImpl::drawImages` calls `EyesUIImpl::isVisible(*this)` (0x1be9c),
  which compares identity with that layer, so only the selected layer draws.
- Numerically **lower z-order is in front** in this build. Stock emotions pass
  `Emotion::getPriority()` as the layer z-order:
  `EmotionsImpl::prepareAnimation` calls `getLayer(priority)` then
  `addLayer(priority)` (6.7.0 libEmotions.so 0x5d738/0x5d818), and the default
  `Emotion` priority is 0 (constructor `str xzr,[x0,#32]`). Most stock activity
  animations — including `RemoteControl` → `Logo` — therefore use layer 0.
  The plug-in's 2000 is behind them, which is why the stock motion-control
  animation still takes over the eyes.
- `TOADI_EXAMPLE_HEARTS_PRIORITY` changes the plug-in's z-order
  (non-negative). Values are layer keys, not relative weights; 0 is normally
  occupied by the stock emotion layer while it animates, and negative values
  are rejected by `addLayer`.

## 5. REST trigger

The stock web server runs on :8080, but a plug-in cannot add a route to it:

- `WebRequestHandlersContext` exposes no public handler-registration virtual
  (the `InterfaceImpl` vtable only has the Unknown methods;
  `addWebRequestHandler` is a non-virtual method on
  `WebRequestHandlersContextImpl`).
- The existing `/maintenance/showEmotion` endpoint is not usable here: on
  6.7.0 every tested stock emotion (`Docked`, `QrScanning`, `BatteryLow`,
  `WifiConnected`, `Docking`) returns HTTP 500, while
  `/maintenance/hideEmotion` returns 200. The decompiled handler
  (`libWebServer.so` 0x1be380) queries the `emotions` service, asks for the
  emotion-description JSON and calls `makeEyesAnimation`; the animation lookup
  fails on this build.

The plug-in therefore runs a small HTTP listener on :8081 (free on the robot):

```
GET|POST|PUT /hearts[?loops=N]    filled heart, 12 frames
GET|POST|PUT /spidey[?loops=N]    filled Spidey lenses, 12 frames
GET|POST|PUT /spidey2[?loops=N]   contour Spidey lenses, 12 frames
GET|POST|PUT /spider[?loops=N]    crawling spider, 16 frames
GET|POST|PUT /pumpkin[?loops=N]   pulsing pumpkin, 12 frames
GET|POST|PUT /badge[?loops=N]     pulsing Paw Patrol badge, 8 frames
GET|POST|PUT /pawpatrol[?loops=N] alias for /badge
GET|POST|PUT /walle[?loops=N]     Wall-E eyes, 16 frames (alias /wall-e)
GET|POST|PUT /fireworks[?loops=N&screen=left|right|both]
                                  procedural firework, 28 frames
GET|POST|PUT /animation?name=X&loops=N[&screen=...]
GET|POST|PUT /hearts/off          stop and disable the layer
GET           /animations         JSON list of names
GET           /status             JSON state
OPTIONS       *                   CORS preflight
```

Loops default to 1 and are clamped to 1..100. `screen` currently selects the
active panel for `fireworks` only (default `both`; the inactive panel receives
zeroed frames). The other animations always draw both panels. `/status` fields:
`ok`, `eyesui`, `layer`, `animating`, `animation`, `animations`, `frames`,
`port`, `priority`. `frames` is a cumulative per-boot counter, not a per-trigger
count. Responses carry `Access-Control-Allow-Origin: *`. The endpoint is
unauthenticated; treat the robot LAN as trusted or add a token/allowlist if
that is not acceptable.

The listener is a plain POSIX socket loop (no extra library): bind
`0.0.0.0`, accept sequentially, parse the request line, send a fixed
JSON response, close. Port override: `TOADI_EXAMPLE_HEARTS_PORT`.

## 6. Build and harness

`src/example_plugin/` now contains:

```
include/toadi/eyes_ui.h          reconstructed EyesUI/EyesUILayer ABI
include/toadi/example_hearts.h   start + test hooks
src/hearts_controller.cpp        animation table, worker, REST server
src/hearts.h                     embedded heart frames (toadi_src/hearts.h)
src/spidey_frames.h              filled lenses (left/right PNGs)
src/spidey2_frames.h             contour lenses, mirrored from left.png
src/spider_frames.h              procedural crawling spider
src/pumpkin_frames.h             pulsing pumpkin logo
src/badge_frames.h               pulsing blue Paw Patrol badge
src/walle_frames.h               Wall-E eyes, 16 frames (left/right sets)
```

Generated animations come from
`tools/generate_animation_frames.py`, which reads
`reference/spidey_ref/{left.png,right.png,pumping.png,
paw-patrol-badge-diy-1440x768.png,walle_left.png,walle_right.png}`. The Spidey
lenses are filled from the PNG interiors; spidey2 uses the contours with a
mirrored left mask for symmetry. The pumpkin uses the logo's orange/green
silhouette plus four procedural crease lines; a gentle 12-frame pulse is
applied. The badge is extracted from the blue shield (bottom centre of the
sheet) as its white border+paw components, with an 8-frame pulse and a
`/pawpatrol` alias.

The Wall-E references are line drawings with a solid lens disc nested inside a
thin housing outline. The generator identifies the two connected components by
bounding-box containment (not by area — the filled lens is the larger area),
then renders the housing as a white outline and the lens as a rim plus glint
on black. The 16-frame animation tilts the housing around the lens (eased
0° → 4° → 8° and back, 120 ms/frame) and closes the iris by scaling the rim
uniformly; the whole eye is never translated. The `walle_left.png` /
`walle_right.png` files map directly to the left/right panels (the spidey
references, by contrast, needed the swap).

`fireworks` is generated at trigger time in C++ (`buildFireworks()`), not
embedded: a 5-frame rocket launch, a flash ring, then 22 frames of 20 radial
particles with drag, gravity and late twinkle. The burst is zoomed 1.45× and
drawn with 2 px blocks (3 px flash head) so it reads on the 128×64 panel; the
`screen` query selects which panel receives the frames. Regenerate with:

```bash
python3 tools/generate_animation_frames.py
```

Previews are written to `animations_preview/`.

Build (still `-Wl,-z,nodelete`, now also `-pthread -ldl`):

```bash
make -C src/example_plugin
readelf -d build/libExampleVisionPlugin.so | grep FLAGS_1   # NODELETE
```

`src/plugin_harness/hearts_harness.cpp` loads the plug-in directly and injects
a fake `eyesui` through the exported test hook
`toadi_example_hearts_set_eyesui()`. It checks:

- 12 `drawImages` calls per loop, each buffer 1024 bytes
- `addLayer(2000)`, enable true, disable after stop
- first heart frame is the **LSB-repacked** `heart_frames[0]`
- spidey, spidey2 and walle use different left/right buffers
- fireworks draws 28 frames per loop, and `screen=left/right/both` activates
  exactly the expected panel(s) while the other panel stays zeroed
- `/status`, `/hearts`, `/spidey`, `/spidey2`, `/spider`, `/pumpkin`,
  `/badge`, `/walle`, `/fireworks`, `/animations`, `/hearts/off` on :8081

Run under qemu with the robot rootfs:

```bash
make -C src/plugin_harness hearts-run BUILD=/path/to/build \
     LIBS=/path/to/robot_rootfs_full ROOTFS=/path/to/robot_rootfs_full
```

Result: `ALL PASS (0 failures)`.

## 7. Live result

Deployed on `192.168.1.184` only as a trial rebuild of the stage-2/3 plug-in:

- final accepted plug-in hash
  `dbbd883a96c4d9bc0df47c11610d710ba4a17f09af6674bf17f67c4c657ebbe8`
- wrapper exports `TOADI_EXAMPLE_HEARTS=1`, keeps `PLUGIN_PATH=/home/toadi/plugins`
  and the enable/trial/OK gating
- live `/status` after trigger: `eyesui:true`, `layer:true`, frames advancing,
  `priority:2000`; `/animations` returns all eight names
- hearts ×10 (120 frames), spidey ×5 (60), spidey2 ×5 (60), spider ×5 (80),
  pumpkin ×6 (72), badge ×6 (48), walle (16 frames/loop, 120 ms) and fireworks
  (28 frames/loop, 110 ms) all completed with the app and stock plug-ins
  healthy
- the first six animations were visually confirmed on the robot. The spidey
  left/right panel buffers had to be swapped relative to the PNG labels; the
  spidey2 contour uses a single mirrored mask and a larger 112×62 centred fit
  to avoid the skew of the two differently-proportioned source PNGs.
- fireworks was confirmed and accepted; the zoomed/thicker tuning (1.45× burst,
  2 px trails) was requested after the first version looked too small.
- Wall-E was iterated three times: the first procedural version was rejected;
  the reference-based version initially assigned the eyes the spidey way
  (swapped) and was flipped after a visual check; the first animation
  translated the whole eye, which read as the contour sliding, and was
  replaced by the lens-pivot tilt plus iris-contraction blink. The result was
  accepted as a good stopping point, with further polish explicitly left for
  later.
- previous accepted binaries are kept on the robot under
  `/home/toadi/data/plugin_backups/` (`…-5024a4f2.so`, `…-57447065.so`,
  `…-58d2c530.so`) for quick rollback.

## 8. Caveats

- Only one layer draws at a time: the first enabled layer in ascending
  z-order. Stock emotions default to z-order 0, so they sit in front of the
  plug-in's 2000 and cover its animation while active; that is why activity
  animations still work. The earlier note about a max-int web-handler layer was
  not verified and is superseded by §4.1.
- Drawing is synchronous SPI work at ~8 ms/frame/panel; the 8 fps worker is
  well within the stock animator's 30 fps ceiling.
- The REST listener has no TLS/authentication; bind/allowlist accordingly.
- The stock `showEmotion` 500 behaviour may differ on 6.8.0; it was not used.
- `screen` is honoured only by the procedural fireworks path; static frame sets
  always draw both panels.
- Wall-E is accepted but not final: the lens could be bolder on the small
  panel, and the tilt/blink timing can be tuned further.
