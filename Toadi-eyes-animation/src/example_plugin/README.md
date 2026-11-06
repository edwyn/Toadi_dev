# Example Toadi Broker plug-in

A minimal, loadable `VisionAlgoPlugin` for the Toadi / EEVE robot. It is the
runnable companion to `specs/100-plugin-system.md`.

## What it does

- `createVisionAlgoPlugin(Broker&)` is exported through the Boost.DLL alias
  variable the real `libPlugins.so` looks for.
- With `TOADI_EXAMPLE_REGISTER_SERVICE=1`, the factory registers a name-only
  broker service, `example/visionplugin`, using `Broker::registerInterface`,
  and queries a few stock names to show broker access. Without the variable the
  library loads inertly, which is the safe default on a live robot (see the
  lifetime note in spec 100 section 7.3).
- `makeVisionAlgos()` returns an empty list, so the stock SLAM plug-in and
  detection framework are untouched. This is deliberate: it makes the example
  safe to load on a live robot while still proving the complete loader path.

This set is the **inert broker/loader example only** (the companion to
`docs/100-plugin-system.md`). The eyes animation plug-in is a separate source
set in `../animation_plugin/` and builds `libEyesAnimationPlugin.so`.

## Build

Requires the Debian bullseye aarch64 cross sysroot used by this workspace
(`tools/cross-bullseye/root`), or any aarch64 GCC 10/11 toolchain with the
robot's C++11 ABI (`_GLIBCXX_USE_CXX11_ABI=1`, the default).

```bash
make                       # -> build/libExampleVisionPlugin.so
make clean
```

The output depends only on `libstdc++.so.6`, `libgcc_s.so.1` and `libc.so.6`.
The complete build description — every source file needed and how the shipped
binary was produced — is in `../../BUILD.md`.

## Install on the robot

```bash
scp build/libExampleVisionPlugin.so root@<robot>:/home/toadi/plugins/
ssh root@<robot> /etc/init.d/toadi restart
```

`libPlugins.so` scans `/home/toadi/plugins` when `ToadiLib::loadPlugins()` and
`loadVideoPlugins()` run. Look for the plug-in lines in the app log:

```
loading plugins in directory /home/toadi/plugins
found plugin /home/toadi/plugins/libExampleVisionPlugin.so
Loading plugin /home/toadi/plugins/libExampleVisionPlugin.so loaded
[example-vision-plugin] factory invoked ...
[example-vision-plugin] registered broker service 'example/visionplugin'
```

The plug-in is additive. The stock `libSlamPluginApp.so` also exports
`createVisionAlgoPlugin`; the VisionAlgo loader keeps one entry per library
path, so both are loaded.

## Files

```
Makefile                             build driver (cross aarch64, -z,nodelete)
include/toadi/unknown.h              toadi::Unknown — the registry root object
include/toadi/query_interface.h      QueryInterface<Unknown> — value returned by Broker::queryInterface
include/toadi/broker.h               Broker + makeBroker()
include/toadi/vision_algo_plugin.h   VisionAlgoPlugin, factory declaration, DetectionProcess notes
src/example_vision_plugin.cpp        the plug-in
```

The SDK headers are duplicated in `../animation_plugin/include/toadi/` so each
source set builds standalone. See `../../BUILD.md` for the full build
description.

## Extending it

### Add a vision algorithm

`makeVisionAlgos()` returns
`std::vector<std::unique_ptr<toadi::DetectionProcess>>`. The loader calls
`getName()` on each process and moves it into the app's `DetectionFramework`.
The full ten-slot `DetectionProcess` interface is documented in spec 100
(section 5.5). Start with a no-op process whose `hasWork()` returns false, then
implement `configure`/`run`/`update` against the stock types.

### Add an activity plug-in instead

Export `createActivityPlugin(Broker&)` and implement the two-slot
`ActivityPlugin` interface (`makeSkillCatalog`,
`makeScheduledUserActivities`). The stock `SlamActivityPlugin` in
`libSlamPluginApp.so` is the reference implementation.

### Media or video plug-in

Export `createMediaPlugin` / `createVideoPlugin` and implement the interfaces in
spec 100 sections 5.1 / 5.2. Those two loaders accept one plug-in per type, so
do not expect an extra library to replace the stock one.

## ABI rules that matter

1. `Broker::queryInterface(name)` returns a **value** of
   `QueryInterface<Unknown>` (0x30 bytes: `shared_ptr` + `std::string`), not a
   `shared_ptr<Unknown>`. Using the wrong return type smashes the stack.
2. `Broker::registerInterface` takes `std::unique_ptr<Unknown>` **by value** and
   returns `bool`.
3. Factories are exported as **alias variables** (an 8-byte pointer in `.data`
   under the unmangled name), not as unmangled functions.
4. `VisionAlgoPlugin` has exactly three vtable slots: D1, D0, `makeVisionAlgos`.
5. A plug-in's `GetInterfaces()` names must be unique and not already
   registered, or `registerInterface` returns false.

## Test without the robot

See `../plugin_harness/`. The harnesses run the aarch64 plug-in under
`qemu-aarch64-static` with the extracted robot rootfs:

```bash
make -C ../plugin_harness run         BUILD=/path/to/local/build   # direct ABI test
make -C ../plugin_harness run-inert   BUILD=/path/to/local/build   # inert default
make -C ../plugin_harness loader-run  BUILD=/path/to/local/build   # real libPlugins.so
```

The hearts/animation harness (`hearts-run`) belongs to the animation source
set; see `../animation_plugin/README.md`.

Both pass with the current build. The loader harness pins the plug-in with its
own `dlopen`; see spec 100 section 7.3 for the lifetime observation behind
that.
