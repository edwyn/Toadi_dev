// Real-loader harness: drives libPlugins.so's own discovery and loading path.
//
// It builds the same objects ToadiLib builds:
//   - toadi::makeBroker() (libBroker.so)
//   - a VisionAlgoPluginLoader (constructed through its exported constructor)
//   - toadi::makePluginLibraryLoader(plugin_dir, {loader})
//   - loadPluginLibraries(logger)
//
// libPlugins.so then scans the directory for *.so / *.dylib, dlopen()s each
// file, dlsym()s "createVisionAlgoPlugin", calls the alias factory with the
// Broker&, and calls makeVisionAlgos() on the returned plug-in.
//
// The harness pins the plug-in with its own dlopen() before loading. In this
// isolated process libPlugins releases its temporary shared_library once
// loadPluginLibraries() returns (see spec 100), so without the pin the
// plug-in's code would disappear while its broker service is still alive.
// The stock application keeps plug-in-owned objects alive through its
// DetectionFramework/loader lifetime; the pin makes the same guarantee here.
//
// This translation unit deliberately re-declares the few ABI types it needs
// instead of including Boost headers, mirroring the exact vtable slots
// recovered from libPlugins.so.

#include "toadi/broker.h"

#include <dlfcn.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace spdlog {
class logger;
namespace sinks {
class sink;
}
}  // namespace spdlog

namespace boost {
namespace dll {
class shared_library;
}
}  // namespace boost

namespace toadi {

class DetectionFramework;

// vtable: +0x00 D1, +0x08 D0, +0x10 loadPlugin, +0x18 unloadPlugin.
class PluginLoader {
public:
    virtual ~PluginLoader() = default;
    virtual void loadPlugin(const std::string& name,
                            const boost::dll::shared_library& library);
    virtual void unloadPlugin(const std::string& name);
};

// vtable: +0x00 D1, +0x08 D0, +0x10 addPluginLoader, +0x18 loadPluginLibraries.
class PluginLibraryLoader {
public:
    virtual ~PluginLibraryLoader();
    virtual void addPluginLoader(std::unique_ptr<PluginLoader> loader);
    virtual bool loadPluginLibraries(std::shared_ptr<spdlog::logger> logger);
};

std::unique_ptr<PluginLibraryLoader> makePluginLibraryLoader(
    const std::filesystem::path& directory,
    std::vector<std::unique_ptr<PluginLoader>> loaders);

}  // namespace toadi

// Exported constructor of the concrete loader class:
//   toadi::VisionAlgoPluginLoader::VisionAlgoPluginLoader(
//       toadi::DetectionFramework&, toadi::Broker&)
// The object is 16 bytes (vptr + Impl*). We allocate it and let the real
// constructor install the real vtable.
extern "C" void vision_algo_plugin_loader_ctor(
    void* self, void* detection_framework, toadi::Broker* broker)
    asm("_ZN5toadi22VisionAlgoPluginLoaderC1ERNS_18DetectionFrameworkERNS_6BrokerE");

// A synchronous spdlog logger, constructed through libLog.so's exported
// constructor. toadi::getLog() returns an *async* logger that needs the
// application's global spdlog thread pool; a bare harness has none.
extern "C" void spdlog_logger_ctor(
    void* self, std::string name, std::shared_ptr<spdlog::sinks::sink> sink)
    asm("_ZN6spdlog6loggerC2ENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt10shared_ptrINS_5sinks4sinkEE");

namespace {

// Minimal sink that discards everything. spdlog::sinks::sink's vtable is
//   +0x00 D1, +0x08 D0, +0x10 log(log_msg const&), +0x18 flush(),
//   +0x20 set_pattern(string const&), +0x28 set_formatter(unique_ptr<formatter>)
// The logger only ever calls log()/flush(), so no-ops are sufficient.
struct FakeSink {
    void** vptr;
};

void fake_sink_noop(void*) {}
void fake_sink_log(void*, const void*) {}
void fake_sink_set_pattern(void*, const void*) {}
void fake_sink_set_formatter(void*, void*) {}

struct FakeSinkVtable {
    long offset_to_top;
    void* typeinfo;
    void (*d1)(void*);
    void (*d0)(void*);
    void (*log)(void*, const void*);
    void (*flush)(void*);
    void (*set_pattern)(void*, const void*);
    void (*set_formatter)(void*, void*);
};

FakeSinkVtable fake_sink_vtable = {
    0, nullptr,
    fake_sink_noop, fake_sink_noop,
    fake_sink_log, fake_sink_noop,
    fake_sink_set_pattern, fake_sink_set_formatter,
};

std::shared_ptr<spdlog::logger> make_sync_logger(const std::string& name) {
    static FakeSink fake_sink;
    fake_sink.vptr = reinterpret_cast<void**>(&fake_sink_vtable.d1);
    std::shared_ptr<spdlog::sinks::sink> sink(
        reinterpret_cast<spdlog::sinks::sink*>(&fake_sink),
        [](spdlog::sinks::sink*) {});

    void* logger_mem = ::operator new(4096);
    spdlog_logger_ctor(logger_mem, name, sink);
    return std::shared_ptr<spdlog::logger>(
        reinterpret_cast<spdlog::logger*>(logger_mem),
        [](spdlog::logger*) {});
}

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/plugin-directory\n", argv[0]);
        return 2;
    }
    const bool want_service =
        !(argc > 2 && std::strcmp(argv[2], "--no-service") == 0);
    if (want_service) {
        setenv("TOADI_EXAMPLE_REGISTER_SERVICE", "1", 1);
    }
    const std::filesystem::path plugin_dir = argv[1];
    const std::filesystem::path plugin_path =
        plugin_dir / "libExampleVisionPlugin.so";

    // Pin the plug-in for the duration of the test (see header comment).
    void* pinned = dlopen(plugin_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    check(pinned != nullptr, "pin plug-in with dlopen");
    if (!pinned) {
        std::fprintf(stderr, "dlopen(%s): %s\n", plugin_path.c_str(), dlerror());
        return 1;
    }

    auto broker = toadi::makeBroker();
    check(broker != nullptr, "makeBroker()");

    // DetectionFramework is not touched when makeVisionAlgos() is empty.
    void* detection_framework = ::operator new(64);
    std::memset(detection_framework, 0, 64);

    void* loader_mem = ::operator new(16);
    vision_algo_plugin_loader_ctor(loader_mem, detection_framework, broker.get());

    std::vector<std::unique_ptr<toadi::PluginLoader>> loaders;
    loaders.emplace_back(reinterpret_cast<toadi::PluginLoader*>(loader_mem));

    auto library_loader =
        toadi::makePluginLibraryLoader(plugin_dir, std::move(loaders));
    check(library_loader != nullptr, "makePluginLibraryLoader()");

    auto logger = make_sync_logger("plugin-harness");
    check(logger != nullptr, "construct synchronous spdlog logger");

    const bool loaded = library_loader->loadPluginLibraries(logger);
    check(loaded, "libPlugins loadPluginLibraries()");

    auto service = broker->queryInterface("example/visionplugin");
    if (want_service) {
        check(service.valid(), "plug-in registered example/visionplugin");
        if (service.valid()) {
            auto names = service.get()->GetInterfaces();
            check(!names.empty() && names[0] == "example/visionplugin",
                  "registered service GetInterfaces()");
        }
    } else {
        check(!service.valid(), "inert mode: no broker service registered");
    }

    // Destroy plug-in-owned objects before the pin is dropped.
    service = toadi::QueryInterface<toadi::Unknown>{};
    broker.reset();
    library_loader.reset();
    logger.reset();
    dlclose(pinned);
    ::operator delete(detection_framework);

    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
