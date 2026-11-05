# Toadi eyes animation plug-in

A loadable Toadi Broker `VisionAlgoPlugin` for the Toadi / EEVE robot that
draws eye-panel animations through the app's public **EyesUI** layer API and
triggers them over a small REST listener on **:8081**.

This folder is the published subset of a larger reverse-engineering workspace.
It contains the documentation, the source code, the accepted binary, a setup
howto and a one-command deployment script.

- Status: **implemented and visually verified** on robot `192.168.1.184`
  (app 6.7.0, 2026-10-31).
- Accepted plug-in SHA-256: `dbbd883a96c4d9bc0df47c11610d710ba4a17f09af6674bf17f67c4c657ebbe8`
  (see [`SHA256SUMS`](SHA256SUMS)).
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
src/example_plugin/        plug-in source (C++17, aarch64)
src/plugin_harness/        ABI + real-libPlugins harnesses (qemu), incl. hearts
tools/generate_animation_frames.py  regenerates the embedded frame sets
reference/spidey_ref/      reference images the generator reads
hearts/                    raw 128×64 heart frames (provenance of hearts.h)
animations_preview/        contact-sheet previews of the generated frames
plugins/libExampleVisionPlugin.so   the accepted binary (SHA-256 above)
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

```bash
make -C src/example_plugin           # -> build/libExampleVisionPlugin.so
readelf -d build/libExampleVisionPlugin.so | grep FLAGS_1   # expect NODELETE
```

Requires an aarch64 GCC 10/11 cross toolchain (Debian bullseye sysroot
recommended, C++11 ABI). The harness in `src/plugin_harness/` runs under
`qemu-aarch64-static` against an extracted robot rootfs and needs it plus
`libBroker.so` from the robot; use `make abi loader hearts` / `hearts-run`
(the `follow` target needs `src/follow_plugin`, which is part of the full
workspace, not this publish). See the spec §6 for the exact invocation.

## Safety

The plug-in only draws on the eye panels and answers HTTP on :8081; it does
**not** move the robot or touch the ODrive. The REST endpoint is
unauthenticated — treat the robot LAN as trusted or add a token/allowlist
first. Deployment restarts the stock app; keep the physical e-stop within
reach while working on the robot.

## License

No license is attached to this publish yet; treat the contents as all rights
reserved unless the owner says otherwise.
