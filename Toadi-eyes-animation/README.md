# Toadi eyes animation plug-in

A loadable Toadi Broker `VisionAlgoPlugin` for the Toadi / EEVE robot that
draws eye-panel animations through the app's public **EyesUI** layer API and
triggers them over a small REST listener on **:8081**. It builds one dedicated
deployable library, `libEyesAnimationPlugin.so` — the library does **only**
eye animations: it contributes no vision algorithms, registers no broker
services and never touches the motors.

This folder is the published subset of a larger reverse-engineering workspace.
It contains the documentation, the source code, the binary, a setup howto and
a one-command deployment script. The source is split into two clear sets:
[`src/animation_plugin/`](src/animation_plugin/) (the deployable animation
plug-in) and [`src/example_plugin/`](src/example_plugin/) (the inert spec-100
loader example, kept only to prove the SDK builds and loads).

- Status: **implemented and visually verified** on robot `192.168.1.184`
  (app 6.7.0, 2026-10-31). The verification was done with the pre-split
  combined library that spec 102 §7 refers to as `libExampleVisionPlugin.so`
  (SHA-256 `dbbd883a…`); the split-out animation library is the same code
  built as a dedicated plug-in (see [`BUILD.md`](BUILD.md) §8).
- Shipped binary: `plugins/libEyesAnimationPlugin.so` — SHA-256 in
  [`SHA256SUMS`](SHA256SUMS).
- The stock app remains the owner of the SPI panels; the plug-in draws through
  the `eyesui` broker service, so stock emotion/motion-control animations keep
  working and take over the eyes when they are active.

## Animations

Eight animations, triggered via HTTP:

| Route | Animation |
|---|---|
| `/hearts` | filled heart, 12 frames |
| `/spidey` | filled Spidey lenses, 12 frames |
| `/spidey2` | contour Spidey lenses, 12 frames |
| `/spider` | crawling spider, 16 frames |
| `/pumpkin` | pulsing pumpkin, 12 frames |
| `/badge` (`/pawpatrol`) | pulsing Paw Patrol badge, 8 frames |
| `/walle` (`/wall-e`) | Wall-E eyes, 16 frames |
| `/fireworks?screen=left\|right\|both` | procedural firework, 28 frames |

Plus `/animations` (JSON list), `/status` (JSON state), `/hearts/off` (stop +
disable the layer) and CORS preflight. See
[`docs/102-eyes-animation-plugin.md`](docs/102-eyes-animation-plugin.md) §5 for
the full route table.

## Folder layout

```
docs/                      the three relevant specs (see below)
src/animation_plugin/      the deployable eyes animation plug-in (C++17, aarch64)
src/example_plugin/        the inert spec-100 loader/broker example
src/plugin_harness/        ABI + real-libPlugins harnesses (qemu), incl. hearts
tools/generate_animation_frames.py  regenerates the embedded frame sets
reference/spidey_ref/      reference images the generator reads
hearts/                    raw 128×64 heart frames (provenance of hearts.h)
animations_preview/        contact-sheet previews of the generated frames
plugins/libEyesAnimationPlugin.so    the shipped binary (SHA-256 in SHA256SUMS)
BUILD.md                   how each library is built and which source files it needs
setup.sh                   one-command persistent deployment to the robot
SETUP.md                   howto: deploy, approve, verify, rollback, build
SHA256SUMS                 checksums
```

The full workspace (movement stack, follow plug-in, all specs, analysis
evidence) is not part of this publish.

## Documentation

- [`docs/102-eyes-animation-plugin.md`](docs/102-eyes-animation-plugin.md) —
  **the topic spec**: EyesUI broker ABI, frame bit order, lifecycle, REST
  trigger, build/harness, live results, caveats.
- [`docs/100-plugin-system.md`](docs/100-plugin-system.md) — the Broker /
  plug-in loader system this plug-in rides on.
- [`docs/101-persistence-and-ota.md`](docs/101-persistence-and-ota.md) —
  `PLUGIN_PATH` override, persistent p14 deployment, crash-loop-safe wrapper
  (what `setup.sh` installs).

## Setup on the robot

```bash
./setup.sh root@192.168.1.184            # trial boot (recommended first time)
./setup.sh root@192.168.1.184 --approve  # approve the plug-in permanently
./setup.sh root@192.168.1.184 --uninstall
```

Full instructions: [`SETUP.md`](SETUP.md).

## Build from source

The complete build description — the exact source files needed, the toolchain,
the flags and how the shipped binary was produced — is in
[`BUILD.md`](BUILD.md). In short:

```bash
make -C src/animation_plugin          # -> build/libEyesAnimationPlugin.so (the deployable)
readelf -d build/libEyesAnimationPlugin.so | grep FLAGS_1   # expect NODELETE
make -C src/example_plugin            # -> build/libExampleVisionPlugin.so (inert example)
```

Requires an aarch64 GCC 10/11 cross toolchain (Debian bullseye sysroot
recommended, C++11 ABI). The harness in `src/plugin_harness/` runs under
`qemu-aarch64-static` against an extracted robot rootfs and needs it plus
`libBroker.so` from the robot; use `make abi loader hearts` / `hearts-run`
(the `follow` target needs `src/follow_plugin`, which is part of the full
workspace, not this publish). See [`BUILD.md`](BUILD.md) §9 and spec 102 §6.

## Safety

The plug-in only draws on the eye panels and answers HTTP on :8081; it does
**not** move the robot or touch the ODrive. The REST endpoint is
unauthenticated — treat the robot LAN as trusted or add a token/allowlist
first. Deployment restarts the stock app; keep the physical e-stop within
reach while working on the robot.

## License

[MIT](../LICENSE). Note: the reference images under `reference/` were
user-supplied and may carry their own rights; the license applies to the
code and documentation in this repository.
