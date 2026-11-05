# Eyes animation plug-in — setup on the Toadi

How to deploy the eyes animation plug-in to a Toadi / EEVE robot, approve it,
verify it, use it, and roll it back. The deployment is **persistent across
reboots** and **crash-loop-safe**: a plug-in that kills the app falls back to
the stock app automatically.

Verified on `192.168.1.184` (app 6.7.0, 2026-10-31). Read
[`docs/101-persistence-and-ota.md`](docs/101-persistence-and-ota.md) §4.1,
§6.4–6.6 and [`docs/102-eyes-animation-plugin.md`](docs/102-eyes-animation-plugin.md)
§4, §7 for the full background.

## 1. Prerequisites

- SSH access to the robot as root (`ssh root@<robot>`).
- The accepted binary `plugins/libExampleVisionPlugin.so`
  (SHA-256 `dbbd883a96c4d9bc0df47c11610d710ba4a17f09af6674bf17f67c4c657ebbe8`).
  `setup.sh` verifies this against [`SHA256SUMS`](SHA256SUMS) before copying.
- The robot on a LAN you trust. The REST trigger is **unauthenticated**.

## 2. Quick start (trial deployment)

```bash
cd Toadi-eyes-animation
./setup.sh root@192.168.1.184
```

What happens:

1. The binary is verified against `SHA256SUMS` and copied to the persistent
   data partition: `/home/toadi/data/plugins/libExampleVisionPlugin.so`.
2. A dev-workdir wrapper `/home/toadi/data/toadi/ToadiApp` is installed. It
   sets the mandatory `LD_LIBRARY_PATH`, `PLUGIN_PATH=/home/toadi/plugins`,
   `TOADI_EXAMPLE_HEARTS=1`, keeps the stock web UI working
   (`web -> /home/toadi/web`), and gates the plug-in behind
   `plugin-enable` / `plugin-trial` / `plugin-ok` files.
3. The trial is armed (`plugin-enable` + `plugin-trial`) and the app is
   restarted.
4. The wrapper consumes the trial **before** loading the plug-in. If
   `ToadiApp` survives 10 minutes, the wrapper creates `plugin-ok` — the
   approval.

State machine:

| Files on p14 (`/home/toadi/data/`) | Behaviour |
|---|---|
| `plugin-enable` + `plugin-trial`, no `plugin-ok` | trial boot: plug-in loads once; if the app survives 10 min, `plugin-ok` is created |
| `plugin-enable` + `plugin-ok` | normal persistent load |
| `plugin-enable` only (trial consumed, no OK) | **stock app, no plug-in** — automatic recovery after a crash |
| none (or no wrapper) | stock app with stock plug-ins |

## 3. Verify the deployment

```bash
# the REST listener and its state
curl -s http://192.168.1.184:8081/status
curl -s http://192.168.1.184:8081/animations

# trigger an animation (loops default 1, clamp 1..100)
curl -s -X POST "http://192.168.1.184:8081/hearts?loops=3"

# the plug-in must be mapped into ToadiApp (nodelete -> 4 mappings)
ssh root@192.168.1.184 \
  'grep libExampleVisionPlugin /proc/$(pidof ToadiApp)/maps | head'
```

Expected `/status`: `ok:true`, `eyesui:true`, `layer:true`, `animating:false`
(while idle), `frames` counting up per trigger, `port:8081`, `priority:2000`.

## 4. Approve (after a good trial)

The wrapper approves automatically after 10 minutes of uptime. To approve
immediately:

```bash
./setup.sh root@192.168.1.184 --approve
```

or manually: `ssh root@192.168.1.184 'touch /home/toadi/data/plugin-ok'`.

## 5. Use the animations

`GET`, `POST` and `PUT` all trigger. `?loops=N` defaults to 1 (clamped 1..100).

```bash
ROBOT=http://192.168.1.184:8081
curl -s -X POST "$ROBOT/hearts?loops=2"
curl -s -X POST "$ROBOT/spidey"
curl -s -X POST "$ROBOT/spidey2"
curl -s -X POST "$ROBOT/spider"
curl -s -X POST "$ROBOT/pumpkin"
curl -s -X POST "$ROBOT/badge"
curl -s -X POST "$ROBOT/walle"
curl -s -X POST "$ROBOT/fireworks?screen=right"
curl -s -X POST "$ROBOT/animation?name=hearts&loops=5"
curl -s "$ROBOT/hearts/off"        # stop + hand the panels back to the stock layer
```

Notes:

- Only one layer draws at a time; the stock emotion layer (z-order 0) sits in
  front of the plug-in layer (z-order 2000) and takes over while a stock
  animation is active.
- `/status` `frames` is a cumulative per-boot counter, not per trigger.
- Responses carry `Access-Control-Allow-Origin: *`.

## 6. Configuration

`setup.sh` options:

```bash
./setup.sh root@<robot>                # trial deploy (default)
./setup.sh root@<robot> --approve      # deploy + approve immediately
./setup.sh root@<robot> --inert        # deploy without TOADI_EXAMPLE_HEARTS:
                                       # plug-in loads as the inert VisionAlgo example
./setup.sh root@<robot> --port 8082    # set TOADI_EXAMPLE_HEARTS_PORT
./setup.sh root@<robot> --priority 500 # set TOADI_EXAMPLE_HEARTS_PRIORITY
./setup.sh root@<robot> --key ~/.ssh/toadi_root_key   # SSH key
./setup.sh root@<robot> --plugin path/to/libExampleVisionPlugin.so
./setup.sh root@<robot> --uninstall    # remove wrapper + plug-in, restore stock app
```

Environment variables honoured by the plug-in itself
(see [`docs/102-eyes-animation-plugin.md`](docs/102-eyes-animation-plugin.md) §4–5):

| Variable | Default | Meaning |
|---|---|---|
| `TOADI_EXAMPLE_HEARTS` | unset | `1` enables the eyes controller; unset = inert VisionAlgo example |
| `TOADI_EXAMPLE_HEARTS_PORT` | `8081` | REST listener port |
| `TOADI_EXAMPLE_HEARTS_PRIORITY` | `2000` | EyesUI layer z-order (lower is in front; negative rejected) |

## 7. Redeploy after a rebuild

```bash
make -C src/example_plugin
./setup.sh root@192.168.1.184 --approve --plugin src/example_plugin/build/libExampleVisionPlugin.so
```

`setup.sh` in trial mode removes `plugin-ok` first, so a fresh trial is run;
`--approve` keeps approval state. If you swap in a binary whose hash is not in
`SHA256SUMS`, update the file or use `--force`.

## 8. Uninstall / rollback

```bash
./setup.sh root@192.168.1.184 --uninstall
```

removes the wrapper, the gating flags, the `.so` on p14 and the overlay
symlink, then restarts the stock app. For the full documented rollback (also
removes the dev-workdir directory), see spec 101 §6.6:

```bash
ssh root@192.168.1.184 'rm -rf /home/toadi/data/toadi; \
  rm -f /home/toadi/data/plugin-enable /home/toadi/data/plugin-ok /home/toadi/data/plugin-trial; \
  rm -f /home/toadi/plugins/libExampleVisionPlugin.so; reboot'
```

## 9. Build from source

```bash
make -C src/example_plugin
```

Requires an aarch64 GCC 10/11 cross toolchain (`make CROSS=/path/to/sysroot`
if it is not the workspace default). The link line must keep
`-Wl,-z,nodelete` — without it the Vision loader unloads the library after
its factory returns. Verify with:

```bash
readelf -d src/example_plugin/build/libExampleVisionPlugin.so | grep FLAGS_1
```

The harness (`src/plugin_harness/`, `hearts-run` target) additionally needs an
extracted robot rootfs and `libBroker.so`; it runs under
`qemu-aarch64-static` and does not need the robot.

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `:8081` unreachable, `/status` empty | plug-in not loaded: check `/proc/$(pidof ToadiApp)/maps`; if the trial was consumed by a crash, re-run `setup.sh` (trial mode) |
| :8080 serves the API but 404s the UI | missing `web` symlink in the dev workdir; `setup.sh` creates it, or `ln -s /home/toadi/web /home/toadi/data/toadi/web` |
| no stock plug-ins mapped at all | wrapper must export `PLUGIN_PATH=/home/toadi/plugins` whenever `/home/toadi/data/toadi` exists |
| app does not start after deploying the wrapper | wrapper must export `LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib` and `cd /home/toadi`; delete `/home/toadi/data/toadi/ToadiApp` over SSH to fall back |
| plug-in loads, animation is ragged/speckled | frame bit order — the embedded frames are MSB-first and the plug-in byte-reverses them for the LSB-first EyesUI path; do not "fix" the reversal |
| stock animations cover the plug-in's | expected: stock layers use z-order 0, in front of 2000; see `--priority` |

## 11. References

- [`docs/102-eyes-animation-plugin.md`](docs/102-eyes-animation-plugin.md) — topic spec
- [`docs/100-plugin-system.md`](docs/100-plugin-system.md) — Broker / plug-in loader
- [`docs/101-persistence-and-ota.md`](docs/101-persistence-and-ota.md) — persistence, wrapper, trial gating
