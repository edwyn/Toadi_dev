# 101 — Root filesystem overlay, persistent storage, and the OTA pattern

Status: verified from the shipped boot chain and two live robots (2026-10-31).
Companion to `specs/100-plugin-system.md`; this document explains why a plain
copy into `/home/toadi/plugins` does not survive a reboot and how to deploy a
plug-in persistently.

Artifacts and evidence:

| Item | Value |
|---|---|
| Initramfs (`/boot/initrd`) | SHA-256 `9459a9e0c6e4c9a052d0ee20664e9db22443a45ab2ce84f28e1eef015019a1a0` |
| Initramfs `/init` | SHA-256 `85792904adc9cb124c3b01e94a9249266caa05c13f5a8f697d71b87d47dd5cf1` |
| extlinux.conf | SHA-256 `476c383ad165e32e24c53f845d1379027789eb071c6e0d8dc1a6e4d0bd7115b0` |
| 6.7.0 vs 6.8.0 initramfs | **byte-identical** (`192.168.1.184` and `192.168.0.219`) |
| Evidence copies | `analysis/boot/{initrd,init,extlinux.conf,SHA256SUMS}` |

Live robots used for read-only verification:

| Robot | App | OS commit | Notes |
|---|---|---|---|
| `192.168.0.219` | 6.8.0-202607011333+3b3ec6e | 338aaac8… | no gyro (second unit) |
| `192.168.1.184` | 6.7.0-202504091608+c14c893 | 823b6928… | ICM-20689 gyro (reference unit) |

---

## 1. Answer: yes, `/home/toadi/plugins` is volatile

The normal boot path is:

1. The kernel/initramfs mounts the APP partition **read-only** at
   `/mnt/toadidisk`.
   `analysis/boot/init` lines 44–67: `disk_root=/mnt/toadidisk`,
   `mount -t ext4 -o ro ${x} ${disk_root}`.
2. It mounts a **tmpfs** at `/mnt/rw`, creates `upper/` and `work/`, and mounts
   the root overlay (lines 80–88):
   ```
   mount -t tmpfs toadi-ramdisk /mnt/rw
   mount -t overlay -o lowerdir=/mnt/toadidisk,upperdir=/mnt/rw/upper,workdir=/mnt/rw/work \
         overlayfs-root /mnt/newroot
   ```
3. It `switch_root`s into that overlay.

Consequences:

- `/` is an overlay whose **upper is RAM**. `/home/toadi/plugins` is not a
  separate mount; it is part of that overlay.
- `df -hT /` reports **1.9 GiB** on both robots, which matches the default
  tmpfs size (half of 3.8 GiB RAM), not p1 (638 MiB), p13 (256 MiB) or p14
  (13 GiB).
- A file written to `/home/toadi/plugins` exists only in the tmpfs upper and is
  gone after a reboot.
- The initramfs has a debug `norootoverlay` cmdline option that mounts p1
  read-write directly (`analysis/boot/init` lines 26 and 90–95); this is not the
  production path.

Empirical reboot test: not yet run (it requires a reboot). The boot source code
above is definitive; the test is listed as a verification step in section 6.

---

## 2. Partition layout

From `/proc/partitions` and `/dev/disk/by-partlabel` on both robots:

| Device | Label | Size | Role |
|---|---|---|---|
| `mmcblk0p1` | `APP` | 652,808 KiB | application rootfs; overlay **lower** (read-only) |
| `mmcblk0p2` | `DTB` | 1 MiB | device tree |
| `mmcblk0p3` | `TOS` | 1 MiB | boot firmware |
| `mmcblk0p4` | `EKS` | 1 MiB | boot keys |
| `mmcblk0p5` | `LNX` | 1 MiB | bootloader/kernel slot |
| `mmcblk0p6`–`p9` | `DTB-1`, `TOS-1`, `EKS-1`, `LNX-1` | 1 MiB each | A/B copies |
| `mmcblk0p10` | `BMP` | 1 MiB | boot image |
| `mmcblk0p11` | `RP4` | 1 MiB | Raspberry Pi 4 boot files |
| `mmcblk0p13` | `APC` | 262,144 KiB | `/home/toadi/conf`, ext4, persistent |
| `mmcblk0p14` | (data) | 13,664,096 KiB | `/home/toadi/data`, ext4, persistent |

Only p13 and p14 are mounted read-write by the running system:

```
/dev/mmcblk0p13 /home/toadi/conf ext4 rw,relatime,errors=panic,data=ordered
/dev/mmcblk0p14 /home/toadi/data ext4 rw,relatime,errors=panic
```

`/mnt/rw` and `/mnt/toadidisk` are not visible in the final mount namespace;
the overlay superblock holds the references. The initramfs is the authoritative
definition.

---

## 3. Boot hooks and runtime reconstruction

The overlay is deliberately volatile. Persistent state is on p13/p14 and is
projected into the RAM overlay **at every boot** by scripts. Three hooks matter:

### 3.1 `/usr/bin/toadi-setup-dev`

Called at the top of `/etc/init.d/toadi start()`. It:

1. sources `/etc/toadi.conf` (p1 lower; defaults for the launcher);
2. checks/formats p13 and p14, fsck, mounts them at `/home/toadi/conf` and
   `/home/toadi/data`;
3. **sources `/home/toadi/data/toadi.conf` if present**:
   ```sh
   if [ -f $datadir/toadi.conf ]; then
       . $datadir/toadi.conf
   fi
   ```
   This is a persistent, shell-sourced hook on p14. It runs after the data
   partition is mounted and before CommandExecutor/NetworkManager/ToadiApp are
   started, so any `export` there reaches the app.

`/etc/toadi.conf` (p1) currently contains only launcher defaults:

```sh
toadi_start_app="YES"
toadi_core_dump="NO"
toadi_int_app="ToadiApp"
toadi_int_workdir="/home/toadi"
toadi_dev_app="ToadiApp"
toadi_dev_workdir="/home/toadi/data/toadi"
```

The `toadi_dev_workdir` default is another p14 override: if
`/home/toadi/data/toadi/ToadiApp` exists, the launcher runs that binary instead
of the stock one.

### 3.2 `unsquashToadiLibs()` in `/etc/init.d/toadi`

```sh
unsquashToadiLibs() {
    mkdir /tmp/toadilibs
    mount /home/toadi/data/toadilibs.squ /tmp/toadilibs
    cp -sR /tmp/toadilibs/* /
}
```

- `/home/toadi/data/toadilibs.squ` is a symlink on p14 to the current versioned
  squashfs (`toadilibs.squ-20240315_0906-2.0.2` on the reference robot).
- The squashfs contains CUDA 10.2, TensorRT 8.2.1, cuDNN, PCL and FFmpeg
  libraries under `usr/lib` and `usr/local/cuda-10.2/lib`; it does **not**
  contain app libs or plug-ins.
- `cp -sR /tmp/toadilibs/* /` recreates the directory tree and symlinks in the
  RAM overlay at every boot. For example:
  ```
  /usr/local/cuda-10.2/lib/libcublas.so.10.2.3.300
      -> /tmp/toadilibs/usr/local/cuda-10.2/lib/libcublas.so.10.2.3.300
  ```
- This is exactly the OTA pattern: the authoritative payload lives on p14; the
  volatile overlay only receives symlinks at boot.

### 3.3 App/library loading

`/etc/init.d/toadi start()` then:

- exports `LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib`, appends
  `${toadi_workdir}/lib`, and starts CommandExecutor/NetworkManager with the
  same library path;
- starts `ToadiApp` with `cd ${toadi_workdir} && ./ToadiApp ${toadi_flags} &`;
- `toadi_workdir` is `/home/toadi` unless a dev app exists in
  `/home/toadi/data/toadi/`.

The app's own libraries and plug-ins (`/home/toadi/lib`, `/home/toadi/plugins`)
come from the p1 lower; they are not reconstructed from p14.

---

## 4. Plug-in path override: `PLUGIN_PATH`

`toadi::getPluginPath()` (libToadiLib) is the key that makes a persistent
plug-in deployment possible without touching p1:

- 6.8.0: `analysis/decomp/libToadiLib/getPluginPath.c`, function at 0x31e9bc.
- 6.7.0: function at 0x2f70a4; disassembly calls `getenv@plt` at 0x2f7278 and
  `std::filesystem::path::operator=(char const*)` at 0x2f728c, with the
  `PLUGIN_PATH` string at file offset 0x13a094.

Observed logic (verified against 6.8.0 disassembly, and live on 6.7.0):

```
if status("/home/toadi/data/toadi") is not_found or none:
    result = "/home/toadi/plugins";
else:
    result = "/home/toadi/data/toadi/plugins";   // dev workdir exists
const char* p = getenv("PLUGIN_PATH");
if (p != nullptr)
    result = p;                                  // replace, not append
return result;
```

The dev-directory branch is the 23-byte literal `/home/toadi/data/toadi`
checked with `std::filesystem::status`; only if that path is absent does the
code use `/home/toadi`. `PLUGIN_PATH` is applied last and **replaces** the
result.

This has two consequences:

- A dev-workdir wrapper at `/home/toadi/data/toadi/ToadiApp` makes the
  directory itself exist, so the stock default becomes
  `/home/toadi/data/toadi/plugins`. The wrapper **must** export
  `PLUGIN_PATH=/home/toadi/plugins` (or populate the dev plugin directory) or
  the app loads no stock plug-ins at all. This was observed live: the first
  stage-1 boot started ToadiApp and opened :8080/:554 but mapped no stock
  plug-ins and logged `interface SLAM not found`; with the override, all three
  stock libraries mapped seven times each.
- Because the override replaces the directory, an alternate directory must
  contain the stock plug-ins too (symlinks are sufficient; the real loader
  follows them, verified with the loader harness).

### 4.1 The same rule moves the stock web root

`toadi::getWebServerRootPath()` (libToadiLib, 6.7.0 symbol at 0x2f6e9c) applies
the identical dev-workdir check, then appends the literal `web`:

```
if status("/home/toadi/data/toadi") is not_found or none:
    result = "/home/toadi/web";
else:
    result = "/home/toadi/data/toadi/web";   // dev workdir exists
return result;
```

The function is two nested calls: a helper at 0x2f6fb0 returns
`/home/toadi/data/toadi` when it exists and `/home/toadi` otherwise (23-byte
and 11-byte literals at `.rodata` 0x13b616 / 0x139c31), and the entry point
appends the 3-byte literal `web` (0x139ec9) with
`std::filesystem::path::operator/=`.

Consequence: merely creating `/home/toadi/data/toadi/ToadiApp` changes the
document root to `/home/toadi/data/toadi/web`, which normally does not exist.
The :8080 HTTP server still starts and the API works, but every static request
— including `/` — returns the JSON 404 `{"error":"Not Found","status":404}`.
The UI is not served. This was observed live on 2026-11-01 after the wrappers
had been in place since stage 1.

Fix (persistent, p14):

```sh
ln -s /home/toadi/web /home/toadi/data/toadi/web
```

The running server resolves the root path per request, so the UI returned 200
immediately without an app restart. Any deployment that creates the dev
workdir must create this symlink as well; otherwise the stock web UI appears
broken even though the app and API are healthy.

---

## 5. How the OTA bypasses the overlay

The OTA/update design uses the same split as the boot hooks:

| Persistent thing | Location | Runtime projection |
|---|---|---|
| CUDA/TensorRT/PCL bundle | `toadilibs.squ-<date>-<ver>` + `toadilibs.squ` symlink on p14 | mounted at `/tmp/toadilibs`, `cp -sR` symlinks into `/` at boot |
| Component/firmware files, model data | p14 `/home/toadi/data` (versioned files + symlinks) | app/boot scripts use the symlink paths |
| Component manifests | p14 `/home/toadi/data/app_manifest-<version>/manifest.json` | `FirmwareUpgrade` reads/updates them |
| App/OS root filesystem | p1 `APP` (read-only lower) | updated by replacing/flashing the APP partition; not by writing through the overlay |

Supporting observations:

- `/home/toadi/data/toadilibs.squ` is a symlink to the current versioned
  squashfs; `losetup`/sysfs show the loop device backing file.
- `libBackendCommunication.so` implements `FirmwareUpgrade`
  (`prepareUpdateFile`, `initPendingUpdatesPath`, `parseNewManifestFile`,
  `writeNewManifestFile`, `applyNewSymlinksToDataFilesAndCompleteUpdate`) and
  exports `toadi::completeUpgradeAfterBoot(...)` at 0x2b1d7c. Decompilation
  shows it works on manifests, pending update files and **symlinks under the
  data path**, then completes the update after reboot.
- The OS update checker is separately registered as component `"os"`; the
  APP partition is the read-only lower, so an OS update must replace p1 or use
  the A/B boot slots, not write `/home/toadi` through the overlay.
- The initramfs is identical on both 6.7.0 and 6.8.0, so this is the stable
  platform design, not an app-version quirk.

This is the answer to "how does OTA store anything": it does not write through
the RAM overlay for persistent payloads. It writes the payload/manifest to
p13/p14 and lets the boot scripts rebuild the runtime view. The APP partition
itself is updated separately.

---

## 6. Persistent plug-in deployment recipe

Goal: keep the example plug-in across reboots without modifying p1, while
preserving the three stock plug-ins.

### 6.1 Layout on the data partition

```
/home/toadi/data/
├── toadi.conf                    # sourced by toadi-setup-dev at boot
└── plugins/
    ├── libMediaPlugin.so         -> /home/toadi/plugins/libMediaPlugin.so
    ├── libSlamPluginApp.so       -> /home/toadi/plugins/libSlamPluginApp.so
    ├── libVideoPlugin.so         -> /home/toadi/plugins/libVideoPlugin.so
    └── libExampleVisionPlugin.so # real file (copy of dist/)
```

`toadi.conf`:

```sh
# Persistent plug-in directory on the data partition (p14).
export PLUGIN_PATH=/home/toadi/data/plugins
```

Why symlinks for the stock plug-ins: `PLUGIN_PATH` replaces the default
directory. The symlinks track the p1 files (including any future OTA update of
those files) and cost nothing. The example plug-in itself must be a real file on
p14, because a symlink to `/home/toadi/plugins/...` would point into the
volatile overlay.

### 6.2 Commands (per robot)

```sh
mkdir -p /home/toadi/data/plugins
ln -sf /home/toadi/plugins/libMediaPlugin.so /home/toadi/data/plugins/libMediaPlugin.so
ln -sf /home/toadi/plugins/libSlamPluginApp.so /home/toadi/data/plugins/libSlamPluginApp.so
ln -sf /home/toadi/plugins/libVideoPlugin.so  /home/toadi/data/plugins/libVideoPlugin.so
cp libExampleVisionPlugin.so /home/toadi/data/plugins/
cat > /home/toadi/data/toadi.conf <<'EOF'
# Persistent plug-in directory on the data partition (p14).
export PLUGIN_PATH=/home/toadi/data/plugins
EOF
/etc/init.d/toadi restart
```

If `/home/toadi/data/toadi.conf` already exists, merge the export instead of
overwriting it.

### 6.3 Verify

```sh
pid=$(pidof ToadiApp)
grep -E '/home/toadi/data/plugins/(libSlamPluginApp|libMediaPlugin|libVideoPlugin|libExampleVisionPlugin)' \
     /proc/$pid/maps
```

Expected: stock libraries mapped seven times each; a Vision-only example with
four mappings (`r-x`, guard, `r--`, `rw`) because it is built with
`-Wl,-z,nodelete`. Without nodelete the library is loaded, its factory runs,
and it is unmapped again (spec 100 §7.3). In the alternate-directory design,
all four libraries must be present there (the three stock ones via symlinks);
`/home/toadi/plugins` itself then shows only the three stock files.

### 6.4 Crash-loop-safe deployment via the dev-workdir hook (recommended)

`/etc/toadi.conf` already points `toadi_dev_workdir` at
`/home/toadi/data/toadi`. The launcher uses `${toadi_dev_workdir}/ToadiApp`
instead of the stock binary when that file exists. This is a persistent,
SSH-editable hook on p14 and can carry a **fail-safe wrapper**:

```sh
#!/bin/sh
# /home/toadi/data/toadi/ToadiApp  (mode 0755)
# Loads the persistent plug-in only after an explicit trial/approval, and
# disarms itself before starting so a bad plug-in cannot cause a boot loop.
cd /home/toadi || exit 1
export LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib
# Mandatory: the dev workdir exists, so the default would be
# /home/toadi/data/toadi/plugins and no stock plug-ins would load.
export PLUGIN_PATH=/home/toadi/plugins

# The dev workdir also moves the stock web document root (section 4.1).
# Keep it pointing at the stock UI or :8080 404s every static request.
WEBROOT=/home/toadi/data/toadi/web
[ -e "$WEBROOT" ] || ln -sfn /home/toadi/web "$WEBROOT"

ENABLE=/home/toadi/data/plugin-enable   # operator intent
OK=/home/toadi/data/plugin-ok           # proven stable
TRIAL=/home/toadi/data/plugin-trial     # one-shot request

if [ -f "$ENABLE" ] && { [ -f "$OK" ] || [ -f "$TRIAL" ]; }; then
    # Consume a trial BEFORE the plug-in is loaded. If the app dies, the next
    # boot has ENABLE but neither OK nor TRIAL, so it starts the stock app.
    [ -f "$TRIAL" ] && rm -f "$TRIAL"
    # Alternate directory must also contain the three stock plug-ins
    # (symlinks are enough); otherwise use the in-place symlink design in 6.4.1.
    export PLUGIN_PATH=/home/toadi/data/plugins
    if [ ! -f "$OK" ]; then
        ( sleep 600; pidof ToadiApp >/dev/null 2>&1 && touch "$OK" ) &
    fi
fi

exec /home/toadi/ToadiApp "$@"
```

State machine:

| Files on p14 | Behaviour |
|---|---|
| `plugin-enable` + `plugin-trial`, no `plugin-ok` | trial boot: plug-in loads once; if the app survives 10 min, `plugin-ok` is created |
| `plugin-enable` + `plugin-ok` | normal persistent load |
| `plugin-enable` only (trial consumed, no OK) | **stock app, no plug-in** — automatic recovery after a crash |
| no `plugin-enable` (or no wrapper) | stock app with stock plug-ins; the wrapper's `PLUGIN_PATH=/home/toadi/plugins` preserves them |

Critical details:

- The wrapper **must** set `LD_LIBRARY_PATH` to include `/home/toadi/lib`.
  When the launcher uses the dev workdir it appends
  `${toadi_workdir}/lib` (= `/home/toadi/data/toadi/lib`), which does not
  contain the stock app libraries; without the explicit path the app cannot
  resolve `libToadiLib.so` and will not start.
- The wrapper **must** also export `PLUGIN_PATH=/home/toadi/plugins` on every
  boot, including the disarmed/recovery path. The existence of
  `/home/toadi/data/toadi` makes `getPluginPath()` select
  `/home/toadi/data/toadi/plugins`; without the override the app starts with
  zero stock plug-ins (observed live, section 8.6).
- The dev workdir must contain `web -> /home/toadi/web` (symlink). The same
  dev-workdir existence check in `getWebServerRootPath()` otherwise moves the
  stock document root to `/home/toadi/data/toadi/web`, and :8080 serves the API
  with a 404 for every UI/static request (section 4.1).
- It must `cd /home/toadi` (the stock launcher runs the app from there).
- It `exec`s the stock binary, so `pidof ToadiApp` and signal handling behave
  normally.
- Delete `/home/toadi/data/toadi/ToadiApp` over SSH to remove the wrapper; the
  launcher falls back to the stock binary on the next start.
- The wrapper itself is shell on p14; if it is malformed, no app starts, but
  network/sshd are already up (section 8) and the wrapper can be deleted via
  SSH.

### 6.4.1 The original plug-in directory without changing `toadi.conf`

`PLUGIN_PATH` moves the loader to a different directory; it does not put the
file in `/home/toadi/plugins`. To make the plug-in appear **in the original
directory** alongside the three stock plug-ins, persist the real `.so` on p14
and have the dev-workdir wrapper recreate a symlink in the volatile overlay
before the app starts:

```sh
# /home/toadi/data/toadi/ToadiApp (same wrapper as 6.4, in-place link step)
cd /home/toadi || exit 1
export LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib
export PLUGIN_PATH=/home/toadi/plugins        # mandatory; dev workdir exists
WEBROOT=/home/toadi/data/toadi/web             # stock UI document root (4.1)
[ -e "$WEBROOT" ] || ln -sfn /home/toadi/web "$WEBROOT"

ENABLE=/home/toadi/data/plugin-enable
OK=/home/toadi/data/plugin-ok
TRIAL=/home/toadi/data/plugin-trial
REAL=/home/toadi/data/plugins/libExampleVisionPlugin.so
LINK=/home/toadi/plugins/libExampleVisionPlugin.so

if [ -f "$ENABLE" ] && { [ -f "$OK" ] || [ -f "$TRIAL" ]; } && [ -f "$REAL" ]; then
    [ -f "$TRIAL" ] && rm -f "$TRIAL"
    ln -sfn "$REAL" "$LINK"
    [ -f "$OK" ] || ( sleep 600; pidof ToadiApp >/dev/null 2>&1 && touch "$OK" ) &
else
    rm -f "$LINK"                              # no stale link on app restart
fi
exec /home/toadi/ToadiApp "$@"
```

Properties:

- The `.so` survives reboot because it lives on p14; only the symlink is
  volatile, and the wrapper recreates it on every boot.
- The stock plug-ins are untouched; the loader scans the real directory and
  loads the stock plug-ins plus the symlinked one.
- `PLUGIN_PATH=/home/toadi/plugins` is **required** because the dev workdir
  exists. Without it `getPluginPath()` selects
  `/home/toadi/data/toadi/plugins` and the stock plug-ins (and the symlink)
  are never seen. This was observed live on the first stage-1 boot.
- The deployed `.so` must be built with `-Wl,-z,nodelete` (`DF_1_NODELETE`).
  The Vision loader does not retain a Vision-only library after
  `loadPluginLibraries()`; without nodelete it is unloaded even though its
  factory ran. Verified live in stage 2 (spec 100 §7.3, §8.7 below).
- Both the `libPlugins.so` directory scan and `dlopen` follow symlinks. The
  real-loader harness passed with a symlink-only directory on both robots
  (inert and service modes), see spec 100 §7.5.
- No `toadi.conf` edit; the only startup condition is the existing
  `${toadi_dev_workdir}/${toadi_dev_app}` check in `/etc/init.d/toadi:80`,
  whose default already points at `/home/toadi/data/toadi/ToadiApp`.
- Keep the trial/OK gating from 6.4 around the `ln`: the symlink is created
  only while the plug-in is armed. After a crash the next boot finds no
  `plugin-trial`, so it starts the stock app and — because the overlay is
  fresh, and the `else` branch removes any stale link — there is no symlink
  either.
- Uninstall is `rm /home/toadi/data/toadi/ToadiApp` (plus optionally the
  `.so`); the next start uses the stock binary and the fresh overlay has no
  symlink.

Alternatives that also place the file in `/home/toadi/plugins` without a
wrapper: add it to the `toadilibs.squ` squashfs so
`cp -sR /tmp/toadilibs/* /` creates the symlink at boot, or patch the p1 lower.
Both modify vendor payloads or the read-only system image and are riskier;
the dev-workdir wrapper is the only method that is removable over SSH and
leaves `toadi.conf` and p1 untouched.

### 6.5 Reboot test (definitive overlay proof)

Reboot one robot, wait for ToadiApp, and repeat the `/proc/$pid/maps` check and
the `/home/toadi/data/plugins` listing. The plug-in should still load, proving
the data-partition deployment survives the RAM-overlay reset.

A complementary test that proves the overlay itself is volatile: write a marker
file to `/home/toadi/plugins/.marker`, reboot, and observe it is gone.

Executed on `192.168.1.184` (stage 3, 2026-10-31): `.stage3-overlay-marker` was
gone after reboot, the wrapper had recreated
`libExampleVisionPlugin.so -> /home/toadi/data/plugins/...`, the plug-in was
mapped 4×, `plugin-enable`/`plugin-ok` retained their pre-reboot mtimes, and
`plugin-trial` remained absent. This proves the symlink is recreated at boot,
not preserved by the overlay.

### 6.6 Rollback

For the dev-workdir wrapper deployment:

```sh
rm -rf /home/toadi/data/toadi           # wrapper, backups, and the directory
rm -f /home/toadi/data/plugin-enable /home/toadi/data/plugin-ok \
      /home/toadi/data/plugin-trial
rm -f /home/toadi/plugins/libExampleVisionPlugin.so   # overlay; reboot clears it
reboot
```

The **directory** must be removed, not just `ToadiApp`: `getPluginPath()` checks
the existence of `/home/toadi/data/toadi` and would otherwise still select
`/home/toadi/data/toadi/plugins`, leaving the stock app with no stock plug-ins.
(Alternatively, populate that dev plugin directory with the stock plug-ins.)

For the `toadi.conf` + `PLUGIN_PATH` route:

```sh
rm /home/toadi/data/toadi.conf          # or remove only the PLUGIN_PATH line
/etc/init.d/toadi restart
```

The loader returns to `/home/toadi/plugins`; the data directory can stay or be
removed.

### 6.7 Caveats

- `PLUGIN_PATH` replaces, not appends, the default directory.
- `std::filesystem::directory_iterator` order is unspecified; Media and Video
  loaders accept one plug-in per type, Vision/Activity accept multiple.
- The app and loader run as root; a plug-in is fully privileged code. Treat
  `data/plugins` as a trusted-code location.
- If p14 is reformatted (`toadi-setup-dev` does `mkfs.ext4` when it sees no
  ext4 signature), `toadi.conf` and the plug-in are lost.
- An OTA that rewrites p1 updates the stock plug-in symlink targets
  automatically; an OTA that resets or replaces the data partition would drop
  the persistent override. Re-apply after major updates.
- `toadi.conf` is sourced as shell; keep it valid POSIX sh and root-owned
  (0644).

---

## 7. Alternatives considered

### 7.1 Add the plug-in to `toadilibs.squ`

`unsquashToadiLibs()` runs `cp -sR /tmp/toadilibs/* /`. If the squashfs
contained `home/toadi/plugins/libExampleVisionPlugin.so`, the boot script would
create that path as a symlink into `/tmp/toadilibs/...` every boot — literally
placing the plug-in in `/home/toadi/plugins` without an env var.

Trade-offs: the squashfs is a 527 MB OTA-managed component; rebuilding and
transferring it is heavy, and the next `libs` update may replace it. Not
recommended for iterative work; useful if a no-env deployment is required.

### 7.2 Patch the APP partition (p1)

The stock plug-ins live in p1's `/home/toadi/plugins`, the overlay lower. Adding
a file there is persistent and needs no boot hook, but requires offline image
patching or writing the mounted block device while it is the active lower, and
is lost when an OS update replaces p1. Not recommended for an experimental
plug-in.

### 7.3 Use the dev workdir override

`/etc/toadi.conf` already points `toadi_dev_workdir` at
`/home/toadi/data/toadi`. A wrapper script there could `export PLUGIN_PATH` and
`exec /home/toadi/ToadiApp`, making the launcher use it. This works but changes
the app's working directory/binary selection; the `toadi.conf` hook is simpler
and less invasive.

---

## 8. Recovery surface and boot-loop analysis

If a plug-in ever crashes ToadiApp, the platform still gives several recovery
paths.

### 8.1 SSH is up before the app

Runlevel 5 boot order on both robots:

```
S01networking      network interface up
S09sshd            sshd listener
S50toadi           ToadiApp / plug-in loading
S99rmnologin.sh
```

Observed live: `sshd: /usr/sbin/sshd [listener]` is running independently of
ToadiApp. A plug-in crash therefore does not remove SSH access. (ToadiApp
itself starts after `configureNetwork` and `startGpsd` in the launcher.)

### 8.2 Watchdog gives a recovery window

`/usr/bin/toadi-ping-wdog` is a shell script. Started as
`/usr/bin/toadi-ping-wdog 60 &` before ToadiApp, it toggles GPIO216 and writes
to `/dev/watchdog0` and `/dev/watchdog1` for 60 seconds, then exits. If
ToadiApp crashes before taking over the watchdog, the hardware resets after the
remaining timeout. Each boot therefore provides a fresh network/SSH window of
roughly a minute or more. A host-side rescue loop can exploit it:

```sh
for i in $(seq 1 120); do
    ssh -i toadi_root_key root@<robot>         'rm -f /home/toadi/data/plugin-trial /home/toadi/data/plugin-ok                /home/toadi/data/toadi/ToadiApp; /etc/init.d/toadi restart' && break
    sleep 5
done
```

Removing the wrapper (or the trial/OK flags) restores the stock launcher; no
p1 reflash is required.

### 8.3 The one-shot wrapper prevents the loop in the first place

With the section 6.4 wrapper, the trial flag is consumed **before** the plug-in
is loaded. A plug-in crash on the first trial boot means the next boot has no
trial and no OK, so the stock app starts without `PLUGIN_PATH`. The crash
cannot repeat unless an operator re-creates `plugin-trial`.

### 8.4 Last-resort recovery

The persistent state is entirely on p14 (`/dev/mmcblk0p14`, ext4). Even if the
network/SSH were unavailable, removing or editing
`/home/toadi/data/toadi/ToadiApp`, `toadi.conf` or the plugin on that partition
from another machine restores the stock path. A full APP-partition reflash is
not needed for a plug-in mistake. This is strictly better than transporting the
board back to a dev fixture for reflashing.

### 8.5 Pre-deployment native test (no startup change)

Before touching the startup path, the real-loader harness can be run **natively
on the robot** in a separate process (for example `/home/toadi/data/plugin-test/`):

```sh
LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib \
    ./loader_harness ./plugins --no-service
```

This exercises the exact `.so`, the robot's `libPlugins.so`/`libBroker.so`,
and the kernel/CPU without loading anything into ToadiApp. If it crashes, only
the test process dies; ToadiApp and the watchdog are unaffected. Use the
6.8.0-built harness on `192.168.0.219` and the 6.7.0-built harness on
`192.168.1.184`.

Result (2026-10-31, both robots, no startup changes):

| Robot | App | Inert | Service | ToadiApp |
|---|---|---|---|---|
| `192.168.0.219` | 6.8.0 | ALL PASS (RC 0) | ALL PASS, `example/visionplugin` registered (RC 0) | PID 2263 unchanged |
| `192.168.1.184` | 6.7.0 | ALL PASS (RC 0) | ALL PASS, `example/visionplugin` registered (RC 0) | PID 14277 unchanged |

The run was repeated against a directory containing only a symlink to the
persistent `.so`; both modes passed on both robots. This validates the
mechanism in §6.4.1 before any boot-path change.

The wrapper state machine was also dry-run on both robots: the exact script
from §6.4.1, with the final `exec /home/toadi/ToadiApp` replaced by a fake app,
executed under `env -i` through its real shebang. Four cases were checked:

| State | Symlink created? | Trial consumed? |
|---|---|---|
| none | no | no |
| `plugin-enable` only | no | no |
| `plugin-enable` + `plugin-trial` | yes | yes; `plugin-ok` created after the timer (`pidof ToadiApp` sees the exec'd app) |
| `plugin-enable` + `plugin-ok` | yes | n/a |

`sh -n` passed, and the fake app observed cwd `/home/toadi`, the exact
`LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib`, and verbatim
argument passthrough. On these images `/bin/sh` is a symlink to zsh, so this
also exercised the real interpreter. The only wrapper behaviour not exercised
is the final `exec` of the stock binary itself.

### 8.6 Live stage 1: pass-through wrapper (2026-10-31)

Stage 1 was deployed on `192.168.1.184` only (6.7.0): a wrapper at
`/home/toadi/data/toadi/ToadiApp` that only sets `LD_LIBRARY_PATH`, `cd`s to
`/home/toadi`, and `exec`s the stock binary. No plug-in, flags or symlink were
added.

The first boot exposed the dev-aware path rule in §4:

| Boot | Wrapper contents | Result |
|---|---|---|
| 1 | `LD_LIBRARY_PATH` + cwd only | ToadiApp up, :8080/:554 listening, **zero** plug-ins mapped, `interface SLAM not found` every ~5 s |
| 2 | plus `PLUGIN_PATH=/home/toadi/plugins` | all three stock plug-ins mapped, 7 mappings each at ~28 s uptime; `AiGraphSelector: AI started` |

Stage-1 pass criteria on boot 2:

- `pidof ToadiApp` = 2279, exe `/home/toadi/ToadiApp`, cmdline
  `/home/toadi/ToadiApp`, cwd `/home/toadi`
- `/proc/<pid>/environ`: `LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib`,
  `PLUGIN_PATH=/home/toadi/plugins`
- `/proc/<pid>/maps`: `libSlamPluginApp.so`, `libMediaPlugin.so`,
  `libVideoPlugin.so` at 7 mappings each
- `/home/toadi/plugins` contains only the three stock files
- no `plugin-enable`, `plugin-ok` or `plugin-trial` files

Stage-1 did not test the web UI itself. On 2026-11-01 the same wrapper was found
to have moved the stock document root to `/home/toadi/data/toadi/web` (§4.1),
so :8080 returned the API's JSON 404 for `/` and all static files. The listener
had been healthy throughout; only the document root was wrong. Adding
`/home/toadi/data/toadi/web -> /home/toadi/web` restored the UI.

The first boot's failure was **not** a wrapper or launcher failure: the app was
running and serving; it simply could not see the stock plug-ins because the
dev directory existed. Boot 2 proves the wrapper path itself works and gives
the mandatory `PLUGIN_PATH` for stage 2.

### 8.7 Live stage 2: trial plug-in load (2026-10-31)

Stage 2 was deployed on `192.168.1.184` only:

- persistent `.so` at `/home/toadi/data/plugins/libExampleVisionPlugin.so`
- wrapper exports `PLUGIN_PATH=/home/toadi/plugins` and creates the symlink
  `/home/toadi/plugins/libExampleVisionPlugin.so` only while armed
- `plugin-enable` + `plugin-trial` present, `plugin-ok` absent

Attempt 1 used the original build (no `DF_1_NODELETE`). The app's stderr,
redirected to `/home/toadi/data/toadi-app.err` for the test, proved the factory
ran:

```
[example-vision-plugin] factory invoked (broker=0x...)
[example-vision-plugin] broker service registration disabled ...
[example-vision-plugin] makeVisionAlgos() called (broker=0x...)
```

but `/proc/<pid>/maps` had **zero** `ExampleVision` mappings after startup:
the Vision loader dropped its temporary handle and nothing else retained a
Vision-only library.

The plugin was then rebuilt with `-Wl,-z,nodelete` and redeployed. A
dlopen/dlclose test on the robot confirmed the mechanism:

| Build | `after_open` | `after_close` |
|---|---|---|
| normal | 4 | 0 |
| nodelete | 4 | 4 |

Attempt 2 (nodelete build) passed the load checks:

- symlink present: `/home/toadi/plugins/libExampleVisionPlugin.so ->
  /home/toadi/data/plugins/libExampleVisionPlugin.so`
- `/proc/<pid>/maps`: `libExampleVisionPlugin.so` at 4 mappings
  (`r-x`, guard, `r--`, `rw`), plus the three stock plug-ins at 7 mappings each
- `plugin-trial` consumed; `plugin-enable` and `plugin-ok` present after the
  600 s timer (checked at 11:44 uptime)
- factory and `makeVisionAlgos()` logged; app healthy on :8080/:554,
  `PLUGIN_PATH=/home/toadi/plugins`, cwd `/home/toadi`
- library still mapped when `plugin-ok` armed; stock plug-ins unchanged

### 8.8 Live stage 3: persistence reboot (2026-10-31)

Pre-reboot state: `plugin-enable` (mtime 20:52) + `plugin-ok` (mtime 21:14),
no `plugin-trial`. A marker `/home/toadi/plugins/.stage3-overlay-marker` was
written in the volatile overlay.

After reboot (new ToadiApp PID 2264, 29 s uptime at verification):

- `.stage3-overlay-marker` was **gone** — the overlay reset as expected
- `/home/toadi/plugins/libExampleVisionPlugin.so` was **recreated** (mtime
  21:18) pointing at `/home/toadi/data/plugins/libExampleVisionPlugin.so`
- `libExampleVisionPlugin.so` mapped 4× (`r-x`, guard, `r--`, `rw`)
- stock SLAM/Media/Video mapped 7× each
- `plugin-enable` and `plugin-ok` retained their pre-reboot mtimes; no
  `plugin-trial`
- wrapper mtime 20:52 (p14, persistent); app cmdline `/home/toadi/ToadiApp`,
  cwd `/home/toadi`, `PLUGIN_PATH=/home/toadi/plugins`
- `AiGraphSelector: AI started`; :8080 listening; logs healthy

Stage 3 passes: the plug-in survives a full reboot, and the symlink is
demonstrably recreated by the wrapper rather than preserved by the overlay.

### 8.9 Live stage 4: animation plug-in (2026-10-31)

Rebuilt the plug-in with the eyes animation controller and REST trigger (spec
102), deployed on `192.168.1.184` as a fresh trial:

- wrapper adds `TOADI_EXAMPLE_HEARTS=1`; `PLUGIN_PATH` and trial/OK gating are
  unchanged
- final plug-in SHA-256
  `fd41e75b6af25cd990eddce8ed6386634a439687ba7ea6bc320fecbc5cc73249`
- `/status` after trigger: `eyesui:true`, `layer:true`, `priority:2000`,
  frame counter advancing; `/animations` lists `hearts`, `spidey`, `spidey2`,
  `spider`, `pumpkin`, `badge`
- hearts ×10, spidey ×5, spidey2 ×5, spider ×5, pumpkin ×6 and badge ×6 completed with
  the app and all stock plug-ins healthy
- the first visual attempt exposed an MSB/LSB packing mismatch inside
  `DisplayImpl::update`; byte-reversing each frame fixed it (156/1024 wire
  bytes differ without the reversal, 0 with it)
- Spidey left/right buffers were swapped relative to the PNG labels; spidey2
  uses a mirrored contour for symmetry. All six animations were visually
  confirmed on the robot.

Details, ABI and endpoints: `specs/102-eyes-animation-plugin.md`.

### 8.10 Live stage 4b: fireworks and Wall-E (2026-10-31)

Two more animations were added to the same controller and deployed on
`192.168.1.184` as fresh trials (each trial boot consumes `plugin-trial` and
re-arms `plugin-ok` after 600 s):

- final accepted plug-in SHA-256
  `dbbd883a96c4d9bc0df47c11610d710ba4a17f09af6674bf17f67c4c657ebbe8`
- **fireworks**: procedural, generated per trigger in C++ — 5-frame rocket,
  flash ring, 22 frames of 20 radial particles with drag/gravity/twinkle.
  Enlarged to a 1.45× burst with 2 px trails (3 px flash) after the first
  version looked too small; `/fireworks` honours `screen=left|right|both` and
  zeroes the inactive panel. Visually accepted.
- **Wall-E**: reference-based 16-frame set (120 ms/frame) from the user's
  `walle_left.png` / `walle_right.png` line art. The generator identifies the
  housing outline and solid lens by bounding-box containment, draws a white
  housing + lens rim + glint, tilts the housing around the lens (eased
  0° → 4° → 8° and back) and closes the iris by shrinking the rim uniformly.
  Initial trials exposed two issues: the eyes were assigned opposite to the
  spidey swap, and the first animation translated the whole eye, which read as
  the contour sliding. Both were corrected; the direct left/right mapping and
  the lens-pivot animation were accepted as a stopping point, with further
  polish left for later.
- previous accepted binaries retained under
  `/home/toadi/data/plugin_backups/` (`…-5024a4f2.so`, `…-57447065.so`,
  `…-58d2c530.so`).
- the accepted build survived its trial window: `plugin-ok` armed at 23:57
  (10 min after the 23:47 boot), `libExampleVisionPlugin.so` still mapped,
  app healthy on :8080/:554 and :8081.
- 2026-11-01: the stock :8080 UI was found returning JSON 404s while the API
  stayed up. The wrapper's dev workdir had moved the document root to
  `/home/toadi/data/toadi/web` (§4.1); the missing symlink was added and the
  UI returned 200 without a restart. The wrapper now also creates the symlink
  idempotently on every boot (`[ -e "$WEBROOT" ] || ln -sfn /home/toadi/web
  "$WEBROOT"`); its SHA-256 is
  `02a7573b4600005a2d02f3e086605d859d81833658b5e4eb7fb722af9b9d4ed2`, with the
  previous version kept as `ToadiApp.pre-webfix`. The change takes effect on
  the next boot (the running app already has the symlink).

### 8.11 Live follow plug-in and boot/e-stop behaviour (2026-11-01)

The follow plug-in was deployed on `192.168.1.184` next to the eyes plug-in:

- current 6.8-branch follow build SHA-256
  `45b3a23ec33f74f2d249e826ab0d61dedbefc5400d230419b05f046b3e8b94a3`
  (`/home/toadi/data/plugins/libFollowPlugin.so`, deployed and live-validated
  2026-11-02). It adds the 6.8 depth consumer, masked guard, UI-overlay person
  source, the stock-transformer primer (`GET /prime`; spec 105 §5.2) and the
  smooth range-based acceleration/deceleration controller (spec 106).
  Previous builds `e6fbb5a9…` and `06d11632…` are retained as
  `libFollowPlugin.e6fbb5a9` and `libFollowPlugin.06d116`. Earlier builds:
  adaptive `fceacb6e…`, visual follow `60161bf2…`, camera-mode `796b270a…`.
- wrapper exports `TOADI_EXAMPLE_FOLLOW=1` and
  `TOADI_FOLLOW_ALLOW_MOTION=1`; `/home/toadi/data/follow-enable` links the
  follow `.so`; the wrapper recreates the volatile `/home/toadi/plugins`
  symlink on boot
- motion remains gated by the per-test flag
  `/home/toadi/data/follow-allow-motion` plus `confirm=1`

A full reboot loaded the new build and `:8082/status` returned the visual
fields, so the deployment persists. Off-dock boot latched:

```json
GET /system/emergencyStop
{"description":"Sensors:[\"slam\"]"}
```

`/navigation/releaseEmergencyStop` was refused while that sensor alarm was
active. The app's `EmergencyStopAlarmsHandler` retried by itself; the latch
cleared when navigation/map initialization completed (about 5–6 min after
boot), without a force-clear. A transient
`Sensors:["slam","surfaceDetection"]` also appeared during an earlier boot and
cleared once surface detection initialized. Do not start motion while
`/system/emergencyStop` is non-empty.

Live follow: with the camera frame at mean ~152, `/follow?mode=visual` locked
a person at 62–95%, ran one bounded 12 cm step, then stopped at the safe-ROI
width (266 px) with `arrived at safe distance`; no e-stop. A prior 20 s run
ran three 12 cm steps (36 cm). Cold-start from the dark NightVision frame
(mean ~85) did not produce person alarms within the built-in 20 s lock
window; a 25 cm wake sent afterward brightened the frame immediately. Treat
the cold start as lighting/AE-sensitive (spec 103 §6/§8).

Adaptive build (2026-11-02): the `fceacb6e…` plug-in loaded after reboot and
`:8082/follow?mode=visual&dry_run=1` returned `speed=0.20`, `max_speed=0.20`,
`adaptive=true`, near `0.10/0.12`, mid `0.15/0.18`, far `0.20/0.24`, and
stop ROI `160×280`; the host harness passed all adaptive/profile/search tests.
A supervised live run in TwilightVision then followed ~15.96 m across six
sessions, exercising the near/mid/far profiles, 3–4 s search/reacquire,
safe-ROI arrivals and one 8 s `person lost` stop, with no e-stop. The host
gate's close-range start/arrive churn was fixed with a margin and 5 s cooldown
after the run (not yet retested off-dock). To keep the stock scheduler from
taking over while the follow work continues,
`/settings/mowingPlanner/mowingFrequency` was set to `"STOP"` and
`/settings/docking/autoDockingEnabled` to `false`; both persisted across the
reboot. These must be restored (`mowingFrequency` normal value, auto-docking
`true`) before returning the robot to stock behaviour.

## 9. Verification status and open questions

Verified:

- Overlay lower = p1 read-only, upper = tmpfs, from the initramfs source; both
  robot versions use the identical initramfs.
- `/home/toadi/data` and `/home/toadi/conf` are persistent ext4 partitions.
- `/home/toadi/data/toadi.conf` is sourced at boot by `toadi-setup-dev` on both
  robots; the file is currently absent.
- `getPluginPath()` honors `PLUGIN_PATH` on both 6.7.0 and 6.8.0, and is
  dev-workdir aware: if `/home/toadi/data/toadi` exists the default becomes
  `/home/toadi/data/toadi/plugins`; otherwise `/home/toadi/plugins`.
  Verified statically on 6.8.0 and live on 6.7.0 (stage 1, §8.6).
- `getWebServerRootPath()` uses the same dev-workdir check and appends `web`;
  with the wrapper directory present but no `web` symlink, the :8080 API stays
  up while `/` and all static UI files 404. Confirmed live on 6.7.0 and fixed
  with `/home/toadi/data/toadi/web -> /home/toadi/web`; the running server
  resolved it per request (root/index/CSS all 200 without a restart) (§4.1).
- The stage-1 pass-through wrapper was validated live on 6.7.0: with
  `PLUGIN_PATH=/home/toadi/plugins` the app is healthy and maps all three
  stock plug-ins seven times each; no custom plug-in or flags present.
- The real `libPlugins.so` accepts a symlinked plug-in (loader harness, ALL
  PASS natively on both robots in inert and service modes), so a
  boot-recreated symlink in `/home/toadi/plugins` is a valid deployment
  mechanism.
- VisionAlgo-only plug-in lifetime: the Vision loader does not retain the
  library after `loadPluginLibraries()`; without `DF_1_NODELETE` the factory
  runs but the `.so` is unmapped. With `-Wl,-z,nodelete` the live app kept the
  example mapped (4 mappings) and stock SLAM healthy (stage 2, §8.7).
- Stage 2 trial load on 6.7.0: symlink recreated, custom `.so` mapped alongside
  the three stock plug-ins, trial consumed, factory and `makeVisionAlgos()`
  logged, app healthy, and `plugin-ok` armed after 600 s.
- Stage 3 persistence reboot on 6.7.0: overlay marker gone, symlink recreated,
  custom `.so` mapped 4× again, stock plug-ins 7× each, enable/OK retained,
  trial absent, app healthy (§8.8).
- Stage 4 animation plug-in on 6.7.0: the plug-in resolved the live `eyesui`
  service, created a layer, and drew six animations (hearts, spidey, spidey2,
  spider, pumpkin, badge); `/status` reported frames advancing and all were visually
  confirmed after the bit-order fix (§8.9, spec 102).
- Stage 4b on 6.7.0: fireworks (procedural, screen-scoped) and Wall-E (16-frame
  reference set) were added and visually accepted; the final build
  `dbbd883a…` survived its trial window and armed `plugin-ok` at 23:57 on
  2026-10-31 (§8.10).
- Stage-5 persistence on 6.7.0 (2026-11-01): full reboots loaded both
  `libExampleVisionPlugin.so` (`dbbd883a…`) and `libFollowPlugin.so`
  (`60161bf2…`); each was mapped 4×, `:8081/status` listed all eight
  animations, and `:8082/status` returned visual-follow fields (§8.11).
- Adaptive follow build `fceacb6e…` (2026-11-02) loaded after reboot, was
  mapped 4× alongside the eyes plug-in, and reported the adaptive dry-run
  profile; mowing `STOP` and auto-docking `false` also persisted (§8.11).
- 6.8-branch overlay build `06d11632…` (2026-11-02) loaded under the
  side-by-side 6.8 app, produced live depth/overlay status, and moved only
  while the per-test flag was present; `plugin68-ok` was recreated afterward to
  pin the validated plug-in boot (spec 105 §5.2).
- Primer build `e6fbb5a9…` (2026-11-02) loaded after a reboot with
  `plugin68-ok` removed for fallback, mapped 4×, and returned
  `overlay_enabled:true`. `GET /prime` returned `overlay_primed:true` with
  motion disabled; the overlay detected the person without the UI button. The
  guarded 30 s session ran 14 × 12 cm = 168 cm with no e-stop. `plugin68-ok`
  was recreated and the previous build retained as `libFollowPlugin.06d116`.
- Smooth build `45b3a23e…` (2026-11-02) loaded after a reboot with fallback
  reset, reported `smooth_enabled:true`, and ran a supervised 30 s session at
  ~388 cm with `smooth_speed` 0.139–0.150 m/s, continuous turn-radius
  steering and no e-stop. A 60 s house run followed ~220 cm through a
  search/reacquire and safe arrival; a third run validated
  `arrived at smooth stand-off` at ratio 0.95. `plugin68-ok` was recreated; the
  previous build is retained as `libFollowPlugin.e6fbb5a9` (spec 106).
- The EyesUI path reads frame bytes LSB-first. Repacking the MSB-first heart
  frames makes the draw output byte-identical to the standalone-verified
  transform (0/1024 differing wire bytes; 156/1024 without the fix).

Not yet done / open:

1. Stage-5 persistence is now verified (2026-11-01, re-verified 2026-11-02
   with adaptive build `fceacb6e…`): after full reboots with `plugin-ok`
   present, both `libExampleVisionPlugin.so` (`dbbd883a…`) and
   `libFollowPlugin.so` (`fceacb6e…`) were mapped 4× each. `:8081/status`
   listed all eight animations and `:8082/status` returned visual mode; see
   §8.11. No open stage-5 item.
2. The exact OS/APP update flasher path and whether a full OS OTA preserves
   `/home/toadi/data/toadi.conf` and `data/plugins`.
3. Whether `PLUGIN_PATH` is honored anywhere else in the app (only
   `getPluginPath()` was found to read it).
4. The complete `FirmwareUpgrade` state machine (manifest/pending-update paths)
   was not fully decompiled; section 5 describes the observed data/symlink
   pattern, not every branch.
5. The deployment has been exercised on 6.7.0 (`192.168.1.184`) only; 6.8.0
   (`192.168.0.219`) is still stock.

---

## 10. Evidence index

| Topic | Evidence |
|---|---|
| Initramfs overlay setup | `analysis/boot/init` lines 26, 32, 44–67, 80–95 |
| Initramfs identity | `analysis/boot/SHA256SUMS`; byte-identical on 6.7.0 and 6.8.0 |
| Live mounts/sizes | `/proc/mounts`, `/proc/self/mountinfo`, `df -hT` on both robots |
| Partition labels | `/proc/partitions`, `/dev/disk/by-partlabel` |
| Persistent config hook | `/usr/bin/toadi-setup-dev` (sources `$datadir/toadi.conf`) |
| Launcher defaults | `/etc/toadi.conf` on both robots |
| toadilibs projection | `/etc/init.d/toadi` `unsquashToadiLibs()`; live symlinks under `/usr/local/cuda-10.2/lib` |
| PLUGIN_PATH override | `analysis/decomp/libToadiLib/getPluginPath.c`; 6.7.0 disassembly 0x2f70a4–0x2f728c |
| OTA apply logic | `analysis/decomp/libBackendCommunication/completeUpgradeAfterBoot.c`; `FirmwareUpgrade` symbols in `libBackendCommunication.so` |
| Symlink plug-in test | `src/plugin_harness` `loader_harness` against a symlinked plug-in (ALL PASS) |
| Boot order / SSH independence | `/etc/rc5.d` on both robots; live `sshd` listener |
| Watchdog behaviour | `/usr/bin/toadi-ping-wdog` source (60 s loop, GPIO216 + `/dev/watchdog0/1`) |
| Recovery window | runlevel `S09sshd` before `S50toadi`; watchdog reset loop |
