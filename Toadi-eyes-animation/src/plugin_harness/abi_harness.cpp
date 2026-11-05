// ABI harness for a Toadi VisionAlgo plug-in.
//
// Reproduces exactly what libPlugins.so::VisionAlgoPluginLoader does with a
// plug-in library, without needing the other application libraries:
//
//   1. dlopen(lib, RTLD_NOW)
//   2. dlsym("createVisionAlgoPlugin")
//   3. call the function pointer stored in that alias variable with Broker&
//   4. call plugin->makeVisionAlgos() through the vtable
//   5. delete the plug-in through the virtual destructor
//
// It also exercises the broker: the plug-in registers "example/visionplugin"
// and the harness looks it up.
//
// Build (aarch64, run under qemu):
//   make -C src/plugin_harness abi

#include "toadi/broker.h"
#include "toadi/vision_algo_plugin.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // The plug-in's broker-service registration is opt-in. Pass --no-service
    // to exercise the inert default that a plain copy to the robot uses.
    const bool want_service =
        !(argc > 2 && std::strcmp(argv[2], "--no-service") == 0);
    if (want_service) {
        setenv("TOADI_EXAMPLE_REGISTER_SERVICE", "1", 1);
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/libExampleVisionPlugin.so\n", argv[0]);
        return 2;
    }

    auto broker = toadi::makeBroker();
    check(broker != nullptr, "makeBroker()");
    if (!broker) {
        return 1;
    }

    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    check(true, "dlopen plug-in");

    // The exported symbol is an 8-byte alias variable containing the address
    // of toadi::createVisionAlgoPlugin(Broker&), exactly as Boost.DLL's
    // import_alias resolves it.
    using Factory = std::unique_ptr<toadi::VisionAlgoPlugin> (*)(toadi::Broker&);
    auto* slot = reinterpret_cast<Factory*>(dlsym(handle, "createVisionAlgoPlugin"));
    check(slot != nullptr, "dlsym createVisionAlgoPlugin");
    if (!slot) {
        dlclose(handle);
        return 1;
    }
    Factory factory = *slot;

    auto plugin = factory(*broker);
    check(plugin != nullptr, "factory returned a plugin");

    if (plugin) {
        auto algos = plugin->makeVisionAlgos();
        check(algos.empty(), "makeVisionAlgos() returns an empty list");
    }

    auto service = broker->queryInterface("example/visionplugin");
    if (want_service) {
        check(service.valid(), "broker queryInterface(example/visionplugin)");
    } else {
        check(!service.valid(), "inert mode: no broker service registered");
    }

    // Destroy everything created by the plug-in before unloading its code.
    service = toadi::QueryInterface<toadi::Unknown>{};
    plugin.reset();
    broker.reset();
    dlclose(handle);

    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
