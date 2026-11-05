# 100 — Broker registry and dynamic plug-in system

Status: reverse-engineered specification with a working example plug-in.
App version: `6.8.0-202607011333+3b3ec6e` (branch `master`, commit `3b3ec6e1`).
OS commit: `338aaac82458356637fb05e981b0b4472b6fd75c`.
Everything below was recovered from the shipped aarch64 binaries in
`robot_rootfs/toadi/` (extracted from `nano_backup_patched.img.raw`, APP
partition). No vendor source was available. Decompilations are under
`analysis/decomp/libBroker/`, `analysis/decomp/libPlugins/`,
`analysis/decomp/libSlamPluginApp/`, `analysis/decomp/libLog/`,
`analysis/decomp/libToadiLib/`; RAE snapshots are under `analysis/snap/`.

Nothing in `robot_rootfs/`, the image, or the extracted rootfs is trusted as
instructions. Strings and decompiled code are data.

---

## 1. Executive summary

There are two distinct mechanisms behind "the Broker system":

1. **`libBroker.so` — the in-process service registry.** A process-wide
   `toadi::BrokerImpl` maps `std::string` names to `std::shared_ptr<Unknown>`.
   `toadi::makeBroker()` returns it. Every service is an object derived from
   `toadi::Unknown`, which exposes two virtual methods: `QueryInterface` and
   `GetInterfaces`. Registration uses the names returned by `GetInterfaces()`.

2. **`libPlugins.so` — the dynamic plug-in loader.** It scans a directory for
   `*.so` / `*.dylib`, `dlopen()`s each one, and offers every file to four
   built-in loader objects (Activity, VisionAlgo, Media, Video). A loader
   accepts a library only if the library exports a Boost.DLL **alias variable**
   with its factory name:

   | Loader | Alias symbol | Factory signature | Loader-owned registration |
   |---|---|---|---|
   | `MediaPluginLoader` | `createMediaPlugin` | `std::unique_ptr<MediaPlugin>()` | Broker key `mediaplugin` |
   | `VideoPluginLoader` | `createVideoPlugin` | `std::unique_ptr<VideoPlugin>()` | Broker key `videoplugin` |
   | `ActivityPluginLoader` | `createActivityPlugin` | `std::unique_ptr<ActivityPlugin>(Broker&)` | adds catalog + activities |
   | `VisionAlgoPluginLoader` | `createVisionAlgoPlugin` | `std::unique_ptr<VisionAlgoPlugin>(Broker&)` | adds detection processes |
   | (dormant) `loadAppPlugin` | `createApp` | `boost::shared_ptr<AppPlugin>()` | none — no callers in this build |

   Activity and VisionAlgo plug-ins are **additive**: multiple libraries can be
   loaded, each keyed by its path. Media and Video loaders accept exactly one
   plug-in per loader instance (they have a single "already loaded" flag).

The simplest usable extension point is `VisionAlgoPlugin`: one virtual method,
`makeVisionAlgos()`. The example in `src/example_plugin/` implements it, returns
an empty list (safe), can register an extra broker service when
`TOADI_EXAMPLE_REGISTER_SERVICE=1` is set, and is accepted by the real
`libPlugins.so`.

---

## 2. Artifacts

| Artifact | SHA-256 | Notes |
|---|---|---|
| `robot_rootfs/toadi/lib/libBroker.so` | `0241f2dfeb4b8fd707b5c9c978cf58d426d509cf34180e9afb6afb7a60b05147` | 27,432 bytes, stripped ELF, dynsym intact |
| `robot_rootfs/toadi/lib/libPlugins.so` | `e8dc96c5aa45fe04c05cd45ae10fea0b53182db3b78027a6bc4886a79d13d3e2` | 242,016 bytes |
| `robot_rootfs/toadi/lib/libLog.so` | `702056ed7be11bd1c80289c3174be4acd71054226bcddf1569d1cc068330f678` | spdlog/fmt wrappers, `getLog` |
| `robot_rootfs/toadi/lib/libToadiLib.so` | `1e6bff151f74c0a1ea79c29b4cd2622d479ce9f14a32bb9d9a975c6745824ebb` | constructs loaders |
| `robot_rootfs/toadi/plugins/libSlamPluginApp.so` | `5213c9b5a848a3c8eddaae5ff0a63354b483ff16ae845204ac7aba35c2c06482` | reference Activity + VisionAlgo plug-in |
| `robot_rootfs/toadi/plugins/libMediaPlugin.so` | `42f4309e9ea0f498655f330b71544b41f45b050effc518b3c5d24c3918c555b2` | reference Media plug-in |
| `robot_rootfs/toadi/plugins/libVideoPlugin.so` | `6a39a1c252012de2932daf69f8afc0dccc18e5fb526cd5edbb017e9293ec1e6e` | reference Video plug-in + TensorRT |

The robot runtime used for testing is extracted from the same image:

```
robot_rootfs_full/          # full aarch64 rootfs, glibc 2.35, libstdc++ 6.0.29
```

The test harnesses run under `qemu-aarch64-static -L robot_rootfs_full` with the
robot libraries on `LD_LIBRARY_PATH`.

Two live robots were used for read-only validation on 2026-10-31:

| Robot | App version | OS commit | Stock plug-in SHA-256 |
|---|---|---|---|
| `192.168.0.219` | `6.8.0-202607011333+3b3ec6e` | `338aaac82458356637fb05e981b0b4472b6fd75c` | matches §2 (Media `42f4309e…`, Slam `5213c9b5…`, Video `6a39a1c2…`) |
| `192.168.1.184` | `6.7.0-202504091608+c14c893` | `823b6928c5f090623232c8b932e8a7963e01d1d6` | Media `b82a0a1d…`, Slam `a1024abc…`, Video `4ef59299…` |

The 6.7.0 libraries and plug-ins pulled from `192.168.1.184` are kept in
`robot_6.7.0/` (`lib/`, `usrlib/`, `plugins/`) for the compatibility test in
section 7.4.

---

## 3. `libBroker.so` — the registry

### 3.1 `toadi::Unknown`

`Unknown` is the registry's root object. It has **no data members**, two virtual
methods, and the usual destructor pair:

| Slot | Signature | Evidence |
|---|---|---|
| `+0x00` | `~Unknown()` complete-object destructor | all derived vtables |
| `+0x08` | `~Unknown()` deleting destructor | all derived vtables |
| `+0x10` | `bool QueryInterface(Unknown& query)` | `InterfaceImpl<T,...>::QueryInterface` vtable slot |
| `+0x18` | `std::vector<std::string> GetInterfaces() const` | `BrokerImpl::Register` calls `vtable+0x18` and iterates the vector |

`QueryInterface` implements the two-way typed negotiation: the registered
object receives the caller's `QueryInterface<T>` helper and can hand back a
`shared_ptr<T>`. An object with no typed interface may simply return `false`.
`GetInterfaces()` is the registration key list.

### 3.2 `toadi::Broker`

`Broker` is a pure interface with three virtual methods and the destructor pair:

| Slot | Signature | Notes |
|---|---|---|
| `+0x00` | `~Broker()` (D1) | no data |
| `+0x08` | `~Broker()` (D0) | |
| `+0x10` | `bool registerInterface(std::unique_ptr<Unknown>)` | by value |
| `+0x18` | `void unregisterInterface(const std::string&)` | |
| `+0x20` | `QueryInterface<Unknown> queryInterface(const std::string&) const` | **value return, not shared_ptr** |

The last point is easy to get wrong. `BrokerImpl::queryInterface` (0x143d8)
writes a 0x30-byte `QueryInterface<Unknown>` object into the hidden sret
pointer; on a miss it constructs it with a null `shared_ptr`, on a hit with the
registered `shared_ptr`. Declaring the method as returning
`std::shared_ptr<Unknown>` compiles but overflows the caller's stack and crashes
once the corrupted shared_ptr is destroyed. This was found by the harness, then
confirmed in assembly.

`QueryInterface<Unknown>` layout (constructor at 0x146a8):

```
+0x00  std::shared_ptr<Unknown>   (object, control block)
+0x10  std::string                (requested name; 32-byte libstdc++ string)
size   0x30, no vtable
```

The typed wrapper classes (`QueryInterface<T>`, `SQI<T>`) live in the consumer
libraries; their job is name matching and `dynamic_cast` back to `T`.

### 3.3 `BrokerImpl` — the real registry

`makeBroker()` (0x13aac) allocates 0x70 bytes:

```
+0x00  vptr (vtable for BrokerImpl)
+0x08  pthread_rwlock_t             (0x38 bytes; 64-bit glibc)
+0x40  std::map<std::string, std::shared_ptr<Unknown>>   (0x28 bytes)
```

Registration (`registerInterface` 0x13d1c → `Register` 0x13d8c):

1. Acquire the write lock.
2. Call `iface->GetInterfaces()`.
3. Reject the registration if the returned vector contains duplicate names, or
   if any name already exists in the map. The `unique_ptr` is still consumed;
   the method returns `false`.
4. Otherwise move the `unique_ptr` into a `shared_ptr<Unknown>` and insert one
   map entry per returned name.
5. Release the lock; return `true`.

`unregisterInterface` (0x14190) erases one name. `queryInterface` (0x143d8)
takes the read lock, looks up the name, and returns a `QueryInterface<Unknown>`
value built from the found shared_ptr (or a null one).

The registration name is therefore entirely controlled by the object's
`GetInterfaces()`; there is no separate name argument. This is why the plug-in
loaders that want a fixed broker key pass through
`RegisterInterface<T, UnknownInterface, UnknownInterface>::registerType(unique_ptr<T>,
const std::string& key, Broker&)`, which installs the wrapper's name list from
the supplied key.

### 3.4 `LocalBrokerImpl` — local overlay

`makeBroker(Broker& parent)` (0x13b08) allocates 0x18 bytes:

```
+0x00  vptr (LocalBrokerImpl)
+0x08  Broker* parent
+0x10  BrokerImpl* local (0x70-byte BrokerImpl, constructed in place)
```

Semantics (0x15594 / 0x15648 / 0x15798):

- `registerInterface` registers **only in the local BrokerImpl**.
- `queryInterface` queries the local map first; on a miss it forwards to the
  parent broker.
- `unregisterInterface` checks whether the name exists locally; it unregisters
  from the local broker if present, otherwise from the parent.

This is a per-component overlay over the process-wide broker.

### 3.5 Names used by the stock app

`BrokerImpl::Register` is the only writer of the registry, so the full key set
is the union of every `GetInterfaces()` result. Examples observed in
`ToadiApp`/`libToadiLib`: `apprecovery`, `watchdogmonitor`, `systeminfo`,
`mapbuilderstorage`, `userconfiguration/application`, `activityscheduler`,
`skillcatalog`, `detectionframework`, `configuration`, and the plug-in keys
`mediaplugin` and `videoplugin`.

---

## 4. `libPlugins.so` — discovery and loading

### 4.1 Discovery

`PluginLibraryLoaderImpl` (constructed by `makePluginLibraryLoader`, 0x2bf3c)
stores:

```
+0x00  vptr
+0x08  std::shared_ptr<spdlog::logger>   (from toadi::getLog("PluginLoader"))
+0x18  std::filesystem::path             (plug-in directory)
+0x40  std::vector<std::unique_ptr<PluginLoader>>
```

`listFiles(path)` (0x2c2d4):

- logs `loading plugins in directory {}`;
- requires `std::filesystem::status(path)` to be a directory, otherwise logs
  `not a plugin directory {}` and returns an empty vector;
- iterates the directory and keeps entries whose extension is exactly
  `.so` or `.dylib` (no `lib` prefix check, no manifest check);
- logs `found plugin {path}` for each and returns a `vector<std::string>` of
  full paths.

`loadPluginLibraries(logger)` (0x2cb20):

```
for each file path from listFiles():
    boost::dll::shared_library::load(path, 0x800000 /* append_decorations */, ec)
    if load failed:
        log "Loading plugin {} failed with error code: {}"
        success = false
    else:
        for each PluginLoader in the loader vector:
            loader->loadPlugin(path, library)      // vtable +0x10
            log "Loading plugin {} loaded"
        // the temporary shared_library is released here
return success
```

Every loader is offered every candidate file. A loader that does not find its
factory symbol simply returns. Consequently one library may export several
factories (the stock SLAM plug-in exports both `createActivityPlugin` and
`createVisionAlgoPlugin`, and is loaded by both loaders). Load failures are
logged and reflected in the return value but do not stop the scan.

`loadAppPlugin(shared_library&)` (0x2ce58) resolves `createApp`, calls
`boost::shared_ptr<AppPlugin>() createApp()`, and returns true if the result is
non-null. It has **no callers** in `libPlugins.so` (no `bl 0x2ce58`, no
relocation); `hasPermissions` (0x2d114) is likewise unused and merely tests
whether the pointer is non-null. AppPlugin support is dormant in 6.8.0.

### 4.2 The alias-variable factory ABI

The aliases are Boost.DLL aliases, not plain exported functions. Example from
`libSlamPluginApp.so`:

```
dynamic symbol:  createVisionAlgoPlugin   OBJECT (weak, 8 bytes, .data)
relocation:      .data+0x2371e0 -> _ZN5toadi22createVisionAlgoPluginERNS_6BrokerE
```

The real factory is the mangled C++ function
`toadi::createVisionAlgoPlugin(toadi::Broker&)`; the unmangled 8-byte variable
is what `dlsym("createVisionAlgoPlugin")` returns. The loader dereferences it
(`get_void()` then `*symbol`) and calls the resulting function pointer. This is
exactly the layout produced by `BOOST_DLL_ALIAS(toadi::createVisionAlgoPlugin,
createVisionAlgoPlugin)`. A plug-in may reproduce it by hand:

```cpp
extern "C" {
using Fn = std::unique_ptr<toadi::VisionAlgoPlugin> (*)(toadi::Broker&);
__attribute__((visibility("default"), used))
Fn createVisionAlgoPlugin = &toadi::createVisionAlgoPlugin;
}
```

The loader's `boost::dll::load_mode` value `0x800000` is
`load_mode::append_decorations` on POSIX (Boost 1.83): it first tries the
decorated name (platform prefix/suffix), then the exact path.

### 4.3 `PluginLoader` and `PluginLibraryLoader`

`PluginLoader` (implemented by each of the four loaders):

| Slot | Signature |
|---|---|
| `+0x00` | `~PluginLoader()` D1 |
| `+0x08` | `~PluginLoader()` D0 |
| `+0x10` | `void loadPlugin(const std::string& path, const boost::dll::shared_library&)` |
| `+0x18` | `void unloadPlugin(const std::string& path)` |

`PluginLibraryLoader`:

| Slot | Signature |
|---|---|
| `+0x00` | `~PluginLibraryLoader()` D1 |
| `+0x08` | `~PluginLibraryLoader()` D0 |
| `+0x10` | `void addPluginLoader(std::unique_ptr<PluginLoader>)` |
| `+0x18` | `bool loadPluginLibraries(std::shared_ptr<spdlog::logger>)` |

The `unloadPlugin` methods are implemented by all four loaders but are not
called by `loadPluginLibraries`; the public `PluginLibraryLoader` interface has
no unload entry. They are available for external callers.

### 4.4 The four loaders

Each loader owns an `Impl` object (the loader itself is 16 bytes: vptr + Impl*).

#### MediaPluginLoader (ctor 0x46128)

Impl size 0x78:

```
+0x00  Broker*
+0x08  std::string plugin_path          (32 bytes, ends at +0x28)
+0x28  boost::function<unique_ptr<MediaPlugin>()>   (ends at +0x48)
+0x48  RegisterInterface<MediaPlugin,...>  (Broker* at +0x48, PublicType string +0x50..+0x70)
+0x72  bool loaded
```

`loadPlugin` (0x46214):

1. `dlsym(handle, "createMediaPlugin")`; ignore if absent.
2. Accept only once per loader (`loaded == false`).
3. Copy the `shared_library`, call `createMediaPlugin()` (sret result).
4. If non-null: store the path, store the factory, and register the plug-in
   through `RegisterInterface<MediaPlugin>::registerType(..., "mediaplugin",
   broker)`. Set `loaded`.
5. `unloadPlugin` (0x4659c) erases the broker entry and resets the loader.

#### VideoPluginLoader (ctor 0x446bc)

Identical shape, `Impl` size 0x78, factory `createVideoPlugin()`, broker key
`videoplugin`. `loadPlugin` 0x447a8; `unloadPlugin` 0x44b30.

#### ActivityPluginLoader (ctor 0x3fb04)

Constructor: `(ActivityScheduler&, SkillCatalog&, Broker&)`. Impl size 0x48:

```
+0x00  ActivityScheduler*
+0x08  SkillCatalog*
+0x10  Broker*
+0x18  std::map<std::string, PluginData>   // keyed by plug-in path
```

`loadPlugin` (0x3fcdc):

1. `dlsym(handle, "createActivityPlugin")`.
2. Call `createActivityPlugin(Broker&)`.
3. Call `plugin->makeSkillCatalog()` (vtable +0x10) → `unique_ptr<SkillCatalog>`;
   convert to `shared_ptr` and add it to the injected `SkillCatalog`
   (`SkillCatalog` vtable +0x18).
4. Call `plugin->makeScheduledUserActivities()` (vtable +0x18) →
   `vector<shared_ptr<Activity>>`; for each activity collect its name via
   `Activity::getName()`, then add the `shared_ptr<Activity>` to the injected
   `ActivityScheduler` (vtable +0x20).
5. Store a `PluginData` (factory, plug-in `shared_ptr`, activity-name vector)
   in the map under the plug-in path.

`PluginData` (all loaders) is 0x48 bytes:

```
+0x00  boost::function factory (32 bytes)
+0x20  std::shared_ptr<T> plug-in object
+0x30  std::vector<std::string> names
```

#### VisionAlgoPluginLoader (ctor 0x42980)

Constructor: `(DetectionFramework&, Broker&)`. Impl size 0x40:

```
+0x00  DetectionFramework*
+0x08  Broker*
+0x10  std::map<std::string, PluginData>   // keyed by plug-in path
```

`loadPlugin` (0x42b50):

1. `dlsym(handle, "createVisionAlgoPlugin")`.
2. Call `createVisionAlgoPlugin(Broker&)` → `unique_ptr<VisionAlgoPlugin>`.
3. Call `plugin->makeVisionAlgos()` (vtable +0x10) →
   `vector<unique_ptr<DetectionProcess>>`.
4. For each process: call `process->getName()` (DetectionProcess vtable +0x28)
   to collect a name, then move the `unique_ptr` into the injected
   `DetectionFramework` (vtable +0x48, move + count 1).
5. Store `PluginData` in the map under the plug-in path.

The plug-in object itself is **not** registered in the broker by this loader;
only its detection processes reach the framework.

### 4.5 How `ToadiLib` wires it

`toadi::ToadiLib::loadPlugins()` (libToadiLib 0x22f810, decompiled):

1. Query `ActivityScheduler` and `SkillCatalog` from the broker.
2. Construct `ActivityPluginLoader(activityScheduler, skillCatalog, broker)`
   and `pluginLoader->addPluginLoader(...)` (vtable +0x10).
3. Query `DetectionFramework`, construct
   `VisionAlgoPluginLoader(detectionFramework, broker)`, add it.
4. `pluginLoader->loadPluginLibraries(toadi::getLog(toadi::Loggers))`
   (vtable +0x18).

`ToadiLib::loadVideoPlugins()` (0x22c9b4) adds `MediaPluginLoader(broker)` and
`VideoPluginLoader(broker)` and calls `loadPluginLibraries` again.

The directory comes from `toadi::getPluginPath()` (libToadiLib, 0x31e9bc).
It is **dev-workdir aware**, not cwd-relative:

```text
if status("/home/toadi/data/toadi") is not_found or none:
    result = "/home/toadi" / "plugins"              # /home/toadi/plugins
else:
    result = "/home/toadi/data/toadi" / "plugins"   # dev directory
if getenv("PLUGIN_PATH"):
    result = PLUGIN_PATH                            # replaces the result
```

The check is visible in the disassembly: the helper at 0x31e8c8 constructs
`/home/toadi/data/toadi`, calls `std::filesystem::status`, and selects the
12-byte literal `/home/toadi` or the 23-byte literal
`/home/toadi/data/toadi`; 0x31e9bc then appends `plugins`. The
`getenv("PLUGIN_PATH")` override is applied last.

Consequence: the moment a dev-workdir wrapper exists at
`/home/toadi/data/toadi/ToadiApp`, the default plug-in directory becomes
`/home/toadi/data/toadi/plugins`. A wrapper must therefore export
`PLUGIN_PATH=/home/toadi/plugins` (or populate the dev `plugins` directory) to
keep the stock plug-ins visible. This was observed live on 6.7.0: without the
override the app started, opened :8080/:554, but mapped no stock plug-ins and
logged `interface SLAM not found`; with
`PLUGIN_PATH=/home/toadi/plugins` all three stock libraries mapped
(`/proc/<pid>/maps`, seven mappings each).

`initialize()` calls both entry points on the normal start path: first
`loadVideoPlugins()` (0x20fd14), then later `loadPlugins()` (0x220150). The
loader vector therefore accumulates; each call re-scans the directory and
offers every file to every loader currently registered.

### 4.6 There is no expected plug-in count

A natural concern is whether the app has a hard-coded "three plug-ins" or
"four loaders" expectation that a new file must satisfy. The shipped code does
not:

- `PluginLibraryLoaderImpl::loadPluginLibraries` (0x2cb20) takes the
  `std::vector<std::string>` returned by `listFiles()`, iterates from `begin`
  to `end` (`cmp x21,x28; b.eq`), calls every loader in the loader vector
  (`ldp x26,x27,[x20,#64]`), logs, and returns a success bool. There is no
  comparison against a constant, no manifest, and no counter.
- `listFiles` (0x2c2d4) appends every `.so`/`.dylib` directory entry to the
  vector (with `_M_realloc_insert` when it grows). No capacity or expected
  size.
- The Activity and Vision loaders store `PluginData` in a
  `std::map<std::string, ...>` keyed by path, so multiple libraries are
  supported. The Vision loader passes every algorithm returned by
  `makeVisionAlgos()` to `DetectionFramework::add`, whose argument is a
  vector; there is no fixed number of algorithms.
- Media and Video loaders do have a single-instance guard: a byte at
  `Impl+0x72` is checked before invoking `createMediaPlugin` /
  `createVideoPlugin`. That is a "one media / one video implementation" rule,
  not a global plug-in count, and it only runs when the corresponding factory
  symbol resolves. An example VisionAlgo plug-in exports neither symbol and is
  skipped.
- No stock plug-in filename (`libSlamPluginApp`, `libMediaPlugin`,
  `libVideoPlugin`) appears as a literal in `ToadiApp`, `libToadiLib.so` or
  `libPlugins.so`; discovery is name-agnostic.
- `ToadiLib::loadPluginDependants()` (0x232bd4) is navigation/zone
  configuration (it queries `MultiZoneNavigation`, `GridMapServer`,
  `UploadDatafiles`, `MowingPlanner`, etc.), not a plug-in table.

The stock SLAM library already exports both `createActivityPlugin` and
`createVisionAlgoPlugin`, so even "one file = one plug-in" does not hold in
the shipped system. Adding a file that exports one factory is within the
design.

---

## 5. Plug-in interface reference

All classes below are pure interfaces: vptr at +0, no data members unless
stated. Destructors occupy slots 0 and 1.

### 5.1 MediaPlugin (libMediaPlugin vtable 0x76ed8, 7 slots)

```cpp
class MediaPlugin {
public:
  virtual ~MediaPlugin();
  virtual <unknown> makeMPEG4VideoWriter() = 0;                                       // +0x10
  virtual <unknown> makeMediaPlayer(PlayerMode, int, int) = 0;                        // +0x18
  virtual <unknown> makeEventMovieMaker(Broker&) = 0;                                 // +0x20
  virtual <unknown> makeMKVVideoReader(Broker&) = 0;                                  // +0x28
  virtual <unknown> readFrameFromFile(
      const std::filesystem::path&, std::chrono::milliseconds, int) = 0;              // +0x30
};
```

The slot order, method names and parameter lists are observed from the
`MediaPluginImpl` vtable and symbols. Return types are not encoded in mangled
names and are deliberately left as `<unknown>` here (`MediaPluginImpl` returns
polymorphic player/reader/writer objects). `<unknown>` is documentation
shorthand, not C++.

### 5.2 VideoPlugin (libVideoPlugin vtable 0x10e488, 9 slots)

```cpp
class VideoPlugin {
public:
  virtual ~VideoPlugin();
  virtual <unknown> makeCameraFrameGrabber(
      Broker&, const std::string&, int, int) = 0;                                     // +0x10
  virtual <unknown> makeCameraInfo() = 0;                                             // +0x18
  virtual <unknown> makeCameraVideoFormatConverterFilter(
      const std::string&, int, int, FlipMethod, int, int) = 0;                        // +0x20
  virtual <unknown> makeCamera(
      const std::string&, CameraInfo::CameraType) = 0;                                // +0x28
  virtual <unknown> makeCameraJpegEncoderFilter(
      const std::string&, int) = 0;                                                   // +0x30
  virtual <unknown> makeMPEG4EncoderFilter(
      const std::string&, int) = 0;                                                   // +0x38
  virtual <unknown> makeHardwareObjectDetection(
      Broker&, const std::string&) = 0;                                               // +0x40
};
```

`FlipMethod` and `CameraInfo::CameraType` appear verbatim in the mangled
parameter types, so they are stock header types. Return types are not encoded;
the slot order and parameter lists are what matter for ABI compatibility.

### 5.3 ActivityPlugin (libSlamPluginApp vtable 0x2200f8, 4 slots)

```cpp
class ActivityPlugin {
public:
  virtual ~ActivityPlugin();
  virtual std::unique_ptr<SkillCatalog> makeSkillCatalog() = 0;                       // +0x10
  virtual std::vector<std::shared_ptr<Activity>> makeScheduledUserActivities() = 0;   // +0x18
};
```

### 5.4 VisionAlgoPlugin (libSlamPluginApp vtable 0x220010, 3 slots)

```cpp
class VisionAlgoPlugin {
public:
  virtual ~VisionAlgoPlugin();
  virtual std::vector<std::unique_ptr<DetectionProcess>> makeVisionAlgos() = 0;       // +0x10
};
```

### 5.5 DetectionProcess (the returned algorithms)

`SlamVisionAlgoPlugin::makeVisionAlgos` constructs an `EeveSlamAlgo` object; the
loader calls only slot +0x28 (`getName`) and passes
the unique_ptr to `DetectionFramework`. The full vtable recovered from
`EeveSlamAlgo` (libSlamPluginApp vtable 0x21dff8):

| Slot | Method |
|---|---|
| `+0x00` | `~DetectionProcess()` |
| `+0x08` | `~DetectionProcess()` |
| `+0x10` | `update(std::shared_ptr<MultiSourceImageFrame> const&)` |
| `+0x18` | `configure(Broker&, Configuration const&)` |
| `+0x20` | `run()` |
| `+0x28` | `std::string getName() const` |
| `+0x30` | `bool hasWork() const` |
| `+0x38` | `setPaused(bool)` |
| `+0x40` | `rePaint()` |
| `+0x48` | `getMaxCycleTime() const` (return type not encoded) |
| `+0x50` | `getConstraints() const` (return type not encoded) |

To add a real algorithm you must implement all of these with the stock header
types for `MultiSourceImageFrame` and `Configuration`; a no-op `hasWork()` that
returns false is a reasonable first extension. The example plug-in returns an
empty list, which requires none of this and is safe.

### 5.6 DetectionFramework / SkillCatalog / ActivityScheduler call slots

These are the consumer-side objects injected into the loaders:

| Object | Slot used | Call |
|---|---|---|
| `DetectionFramework` | `+0x48` | add one `unique_ptr<DetectionProcess>` (move, count 1) |
| `SkillCatalog` | `+0x18` | add one `shared_ptr<SkillCatalog>` |
| `ActivityScheduler` | `+0x20` | add one `shared_ptr<Activity>` |

The exact consumer class declarations are outside the plug-in ABI; only these
dispatch points matter for a loader-compatible plug-in.

---

## 6. Building an example plug-in

The working example lives in `src/example_plugin/`:

```
src/example_plugin/
  include/toadi/unknown.h              # Unknown (QueryInterface/GetInterfaces)
  include/toadi/query_interface.h      # QueryInterface<Unknown> value object
  include/toadi/broker.h               # Broker + makeBroker
  include/toadi/vision_algo_plugin.h   # VisionAlgoPlugin + factory declaration
  src/example_vision_plugin.cpp
  Makefile
```

The source:

- implements `ExampleVisionPlugin::makeVisionAlgos()` returning an empty
  `vector<unique_ptr<DetectionProcess>>`;
- in `createVisionAlgoPlugin(Broker&)`, when
  `TOADI_EXAMPLE_REGISTER_SERVICE=1` is set, registers an `Unknown`-derived
  service under `example/visionplugin` and queries a few stock names to
  demonstrate broker access. Without the variable the library loads inertly
  (factory logs and returns an empty algorithm list); this is the safe default
  for a first deployment. Vision-only builds must use `-Wl,-z,nodelete`
  (section 7.3) or the loader will unmap the library after loading;
- exports the Boost.DLL alias variable `createVisionAlgoPlugin` pointing at
  `toadi::createVisionAlgoPlugin`.

Build (Debian bullseye cross sysroot; GCC 10, C++11 ABI compatible with the
robot's GCC 11 libstdc++):

```bash
make -C src/example_plugin
# -> src/example_plugin/build/libExampleVisionPlugin.so
readelf -d build/libExampleVisionPlugin.so | grep FLAGS_1   # must show NODELETE
```

The Makefile links with `-Wl,-z,nodelete`. This is required for a
VisionAlgo-only plug-in: without it the Vision loader unmaps the library after
`loadPluginLibraries()` and any `DetectionProcess` vtables would dangle
(section 7.3).

The resulting ELF needs only `libstdc++.so.6`, `libgcc_s.so.1` and `libc.so.6`.
Install on the robot:

```bash
scp src/example_plugin/build/libExampleVisionPlugin.so root@<robot>:/home/toadi/plugins/
ssh root@<robot> /etc/init.d/toadi restart     # or reboot
```

**Warning:** `/home/toadi/plugins` lives in the root overlay whose upper layer is
a **RAM tmpfs** (`lowerdir=/mnt/toadidisk`, `upperdir=/mnt/rw/upper`). A file
copied there is lost on reboot. The persistent deployment route is the data
partition plus the `PLUGIN_PATH` environment override documented in
`specs/101-persistence-and-ota.md`.

`libPlugins.so` scans `/home/toadi/plugins` at startup. The library appears in
the log as `found plugin ...` / `Loading plugin ... loaded`. It does not replace
the stock SLAM plug-in: both export `createVisionAlgoPlugin`, and the
VisionAlgo loader keeps a map keyed by library path.

### ABI rules learned the hard way

1. `Broker::queryInterface` returns `QueryInterface<Unknown>` **by value**
   (0x30 bytes), not `shared_ptr<Unknown>`.
2. `Broker::registerInterface` takes `std::unique_ptr<Unknown>` **by value** and
   returns `bool`.
3. `Unknown` has `virtual bool QueryInterface(Unknown&)` at +0x10 and
   `virtual std::vector<std::string> GetInterfaces() const` at +0x18.
4. Factory symbols are alias **variables**, not functions; export the unmangled
   8-byte pointer with default visibility.
5. `VisionAlgoPlugin` has exactly three vtable slots (two destructors plus
   `makeVisionAlgos`); returning an empty vector is ABI-safe even if the
   element type is only forward-declared.
6. Do not declare `makeVisionAlgos` with a different return shape (e.g. a
   `unique_ptr` instead of a 24-byte vector): the loader uses the hidden sret
   pointer.

---

## 7. Tests

Two aarch64 harnesses under `src/plugin_harness/` run under
`qemu-aarch64-static -L robot_rootfs_full`:

### 7.1 `abi_harness` — direct ABI check

Reproduces the loader sequence without libPlugins:

1. `makeBroker()`;
2. `dlopen` the plug-in, `dlsym("createVisionAlgoPlugin")`, dereference the
   alias, call the factory with `Broker&`;
3. call `makeVisionAlgos()` through the vtable, check it is empty;
4. query `example/visionplugin`, check it is present;
5. delete everything while the library is loaded.

Observed result:

```
[PASS] makeBroker()
[PASS] dlopen plug-in
[PASS] dlsym createVisionAlgoPlugin
[PASS] factory returned a plugin
[PASS] makeVisionAlgos() returns an empty list
[PASS] broker queryInterface(example/visionplugin)
ALL PASS (0 failures)
```

The same harness with `--no-service` exercises the inert default that a plain
copy into `/home/toadi/plugins` uses; the service check becomes:

```
[example-vision-plugin] broker service registration disabled (...)
[PASS] inert mode: no broker service registered
ALL PASS (0 failures)
```

Run:

```bash
make -C src/plugin_harness run         BUILD=/path/to/local/build
make -C src/plugin_harness run-inert   BUILD=/path/to/local/build
```

### 7.2 `loader_harness` — real `libPlugins.so`

Builds `makeBroker()`, a raw `VisionAlgoPluginLoader` through its exported
constructor, and `makePluginLibraryLoader(dir, {loader})`, then calls the real
`loadPluginLibraries()` with a synchronous spdlog logger. The real loader scans
the directory, `dlopen`s the plug-in, resolves the alias, calls the factory and
`makeVisionAlgos()`, and the harness verifies the broker service. By default
both harnesses set `TOADI_EXAMPLE_REGISTER_SERVICE=1`; pass `--no-service` to
exercise the inert path instead.

Observed result:

```
[example-vision-plugin] factory invoked ...
[example-vision-plugin] registered broker service 'example/visionplugin'
[example-vision-plugin] makeVisionAlgos() called ...
[PASS] pin plug-in with dlopen
[PASS] makeBroker()
[PASS] makePluginLibraryLoader()
[PASS] construct synchronous spdlog logger
[PASS] libPlugins loadPluginLibraries()
[PASS] plug-in registered example/visionplugin
[PASS] registered service GetInterfaces()
ALL PASS (0 failures)
```

The harness pins the plug-in with its own `dlopen` because a VisionAlgo-only
library is not retained by the Vision loader after `loadPluginLibraries()`
returns (section 7.3). The pin is a test fixture; a deployed Vision-only
plug-in must be built with `-Wl,-z,nodelete`.

Run:

```bash
make -C src/plugin_harness loader-run BUILD=/path/to/local/build
```

### 7.3 Plug-in lifetime — Vision-only libraries unload unless `NODELETE`

The isolated harness observed the plug-in `.so` being unmapped at the end of
`loadPluginLibraries()` (`LD_DEBUG` showed `calling fini`; `/proc/self/maps`
lost the library). That observation is **real**, not a harness artifact: the
VisionAlgo loader's stored factory/`PluginData` does not keep the library
mapped once `loadPluginLibraries()` drops its temporary
`boost::dll::shared_library`.

The stock plug-ins stay mapped because each has another retention path.
`libSlamPluginApp.so` exports both `createActivityPlugin` and
`createVisionAlgoPlugin`, so the Activity loader keeps it alive even if the
Vision path drops its handle. Media and Video are retained by their own
loaders. Read-only inspection after 4+ days of uptime showed all three stock
libraries with seven mappings:

```
192.168.0.219  APP=6.8.0  PID 2263   elapsed 4-03:36
  libSlamPluginApp mappings=7
  libMediaPlugin   mappings=7
  libVideoPlugin   mappings=7
192.168.1.184  APP=6.7.0  PID 14277  elapsed 4-03:40
  libSlamPluginApp mappings=7
  libMediaPlugin   mappings=7
  libVideoPlugin   mappings=7
```

A **Vision-only** library has no such second path. Live stage-2 proof
(192.168.1.184, 6.7.0): the plug-in's stderr showed

```
[example-vision-plugin] factory invoked ...
[example-vision-plugin] broker service registration disabled ...
[example-vision-plugin] makeVisionAlgos() called ...
```

but `/proc/<pid>/maps` contained zero `ExampleVision` mappings. The factory
ran and was then unloaded.

Fix: build VisionAlgo-only plug-ins with `DF_1_NODELETE`:

```make
LDFLAGS += -Wl,-z,nodelete
```

Verified on the robot's glibc with a dlopen/dlclose test: the normal build
reported `after_close=0` mappings, the nodelete build `after_close=4`. After
redeploying the nodelete plugin, the live app kept it mapped (4 mappings:
`r-x`, guard, `r--`, `rw`) while the factory ran and stock SLAM stayed
healthy. This is not merely cosmetic: any `DetectionProcess` vtable returned
by `makeVisionAlgos()` points into the library, so without `NODELETE` a plugin
that contributes algorithms would leave dangling code pointers. The harness
pin is still useful for the ABI test, but it masks this lifetime rule.

The `PluginLoader` logger is not file-backed in this image: no
`loading plugins in directory` / `found plugin` / `Loading plugin` lines appear
in `/home/toadi/data/logs/toadilib*.txt` on either robot. The practical live
signal is the mapping in `/proc/<ToadiApp pid>/maps` and (for behaviour) the
broker/framework effects.

### 7.4 6.7.0 compatibility (older robot)

`192.168.1.184` runs app `6.7.0-202504091608+c14c893`, not 6.8.0. Its loader,
broker and reference plug-ins were pulled read-only into `robot_6.7.0/` and the
same real-loader harness was built against them:

```bash
make -C src/plugin_harness loader \
    BUILD=/path/to/build67 LIBS=/path/to/robot_6.7.0
LD_LIBRARY_PATH=robot_6.7.0/home/toadi/lib:robot_6.7.0/usr/lib \
    qemu-aarch64-static -L robot_rootfs_full \
    build67/loader_harness build67/plugins --no-service
```

Observed both with the inert default and with
`TOADI_EXAMPLE_REGISTER_SERVICE=1`:

```
[example-vision-plugin] factory invoked ...
[example-vision-plugin] registered broker service 'example/visionplugin'   # opt-in run
[example-vision-plugin] makeVisionAlgos() called ...
[PASS] libPlugins loadPluginLibraries()
[PASS] plug-in registered example/visionplugin                             # opt-in run
ALL PASS (0 failures)
```

The 6.7.0 `VisionAlgoPlugin` vtable is still 40 bytes (`D1`, `D0`,
`makeVisionAlgos`) and the `createVisionAlgoPlugin(Broker&)` alias/relocation
match, so one plug-in binary is ABI-compatible with both app versions.

### 7.5 Native runs on the live robots (2026-10-31)

The same harnesses were copied to `/home/toadi/data/plugin-test/` on both
robots and run as a separate process; they load the robot's own
`libPlugins.so`/`libBroker.so`. ToadiApp was not touched and no startup file
was changed.

| Robot | App | Run | Result | ToadiApp |
|---|---|---|---|---|
| 192.168.0.219 | 6.8.0 | loader, inert | ALL PASS | PID 2263 unchanged |
| 192.168.0.219 | 6.8.0 | loader, service | ALL PASS, registered `example/visionplugin` | PID 2263 unchanged |
| 192.168.1.184 | 6.7.0 | loader, inert | ALL PASS | PID 14277 unchanged |
| 192.168.1.184 | 6.7.0 | loader, service | ALL PASS, registered `example/visionplugin` | PID 14277 unchanged |

A directory containing only a **symlink**
(`plugins-link/libExampleVisionPlugin.so -> /home/toadi/data/plugin-test/plugins/libExampleVisionPlugin.so`)
also passed all checks in both modes on both robots. That is the mechanism used
by the persistent deployment in spec 101 §6.4.1: the real file stays on p14
while a boot wrapper recreates the symlink in `/home/toadi/plugins`.

---

## 8. Extension recipes

### 8.1 Add a vision algorithm

1. Implement a `DetectionProcess` subclass with the vtable in 5.5. Start
   with `hasWork() == false`, `run()` no-op, `getName()` returning a unique
   name.
2. Return it from `makeVisionAlgos()`:
   `return {std::make_unique<MyProcess>(...)};`.
3. The loader will call `getName()` and move it into `DetectionFramework`; the
   framework is responsible for `configure`, `run`, `update`, etc.

### 8.2 Add an activity / scheduled behaviour

Implement `ActivityPlugin`:

```cpp
std::unique_ptr<SkillCatalog> makeSkillCatalog() override;              // may be null
std::vector<std::shared_ptr<Activity>> makeScheduledUserActivities() override;
```

The loader adds the catalog to the app's `SkillCatalog` and every activity to
the `ActivityScheduler`. The stock SLAM plug-in (`SlamActivityPlugin`) is the
reference implementation.

### 8.3 Replace/augment media or video

Export `createMediaPlugin` or `createVideoPlugin` and implement the interfaces
in 5.1 / 5.2. These loaders accept a single plug-in per type; putting a second
library in the directory will not replace the stock one because the loader's
`loaded` flag is per loader instance and `std::filesystem::directory_iterator`
order is unspecified (the loader does not sort). Remove or rename the stock
library only if replacement is intended.

### 8.4 Use the Broker from a plug-in

```cpp
auto result = broker.queryInterface("watchdogmonitor");
if (result.valid()) {
    // result.get() is shared_ptr<Unknown>
}
```

To publish a service, derive from `Unknown`, return its names from
`GetInterfaces()`, and call
`broker.registerInterface(std::make_unique<MyService>())`. Registration fails
if a name is duplicated or already present. Typed consumers additionally need
your object's `QueryInterface(Unknown&)` to cooperate with `SQI<T>`; returning
`false` is a safe "name-only" service.

---

## 9. Open questions

1. **Plug-in library lifetime (resolved, with a caveat).** Stock plug-ins stay
   mapped because they are also retained through the Activity/Media/Video
   paths. A VisionAlgo-only plug-in is unloaded after
   `loadPluginLibraries()` unless it is built with `-Wl,-z,nodelete`
   (`DF_1_NODELETE`). Verified live in stage 2: without nodelete the factory
   logged and `makeVisionAlgos()` ran but the library was unmapped; with
   nodelete it stayed mapped. A broker service (or any DetectionProcess)
   created by a Vision-only plug-in is only valid with nodelete. See
   section 7.3.
2. **`AppPlugin`.** `createApp`, `loadAppPlugin` and `hasPermissions` exist but
   are unreachable from `loadPluginLibraries` in this build. The interface was
   not reconstructed because there is no caller or in-image implementation.
3. **`MediaPlugin`/`VideoPlugin` return types.** Vtable slot order and parameter
   lists are recovered, but return types are not encoded in mangled names; they
   were inferred from implementation symbols and loader usage.
4. **`DetectionProcess::getMaxCycleTime` / `getConstraints` return types.** Slot
   positions are known; return types are not.
5. **`SQI<T>` typed-query internals.** The name-matching and `dynamic_cast`
   sequence is understood at the vtable level, but the full template source was
   not reconstructed; the example therefore uses the value object's `get()` /
   `valid()` rather than typed queries.
6. **Persistent installation.** The default plug-in directory is on the volatile
   overlay. `specs/101-persistence-and-ota.md` documents the data-partition +
   `PLUGIN_PATH` route and the OTA/toadilibs pattern; the reboot test there is
   still pending.

---

## 10. Evidence index

| Topic | Evidence |
|---|---|
| Broker vtable/layout | `analysis/decomp/libBroker/makeBroker.c`, `makeBroker_with_broker.c`, `LocalBrokerImpl_ctor.c` |
| Registration semantics | `analysis/decomp/libBroker/BrokerImpl_Register.c`, `BrokerImpl_registerInterface.c`, `BrokerImpl_unregisterInterface.c` |
| QueryInterface value object | `analysis/decomp/libBroker/BrokerImpl_queryInterface.c`, `QueryInterface_Unknown_ctor.c`; disassembly 0x143d8, 0x146a8 |
| Local broker overlay | `LocalBrokerImpl_registerInterface.c`, `LocalBrokerImpl_queryInterface.c`, `LocalBrokerImpl_unregisterInterface.c` |
| Plugin discovery/load | `analysis/decomp/libPlugins/PluginLibraryLoaderImpl_listFiles.c`, `PluginLibraryLoaderImpl_loadPluginLibraries.c` |
| Loader constructors | `VideoPluginLoader_ctor.c`, `MediaPluginLoader_ctor.c`, `ActivityPluginLoader_ctor.c`, `VisionAlgoPluginLoader_ctor.c` |
| Loader behavior | `*/loadPlugin.c`, `*/unloadPlugin.c`, `PluginLibraryLoaderImpl_loadAppPlugin.c`, `hasPermissions.c` |
| Plugin interfaces | vtables 0x76ed8 (`libMediaPlugin`), 0x10e488 (`libVideoPlugin`), 0x220010/0x2200f8 (`libSlamPluginApp`), 0x21dff8 (`EeveSlamAlgo`) |
| ToadiLib wiring | `analysis/decomp/libToadiLib/loadPlugins.c`, `loadVideoPlugins.c` |
| Logger internals | `analysis/decomp/libLog/getLog_string.c`, `DefaultLogger_getLogWithDefaults.c`, `setDefaultLogSinks.c` |
| Example/test | `src/example_plugin/`, `src/plugin_harness/`, this spec |

Reproduce a decompilation with the local wrapper:

```bash
tools/dec-local.sh robot_rootfs/toadi/lib/libPlugins.so 0x12cb20 \
    analysis/decomp/libPlugins/PluginLibraryLoaderImpl_loadPluginLibraries.c \
    analysis/snap/libPlugins.rea
```

(Addresses are Ghidra analysis addresses: ELF vaddr + 0x100000. `tools/rea.sh`
and `tools/dec.sh` are the older wrappers; `tools/dec-local.sh` uses a local
Ghidra/JDK copy because the SMB workspace is mounted `noexec`.)
