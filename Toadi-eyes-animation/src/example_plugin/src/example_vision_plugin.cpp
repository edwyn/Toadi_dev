// Example Toadi Broker plug-in.
//
// Builds as an ordinary aarch64 shared library placed in /home/toadi/plugins.
// libPlugins.so discovers every *.so / *.dylib in that directory and offers
// each one to the four built-in loaders. This library answers the VisionAlgo
// loader by exporting the Boost.DLL alias symbol "createVisionAlgoPlugin".
//
// The plug-in:
//   * receives the live toadi::Broker& from the loader,
//   * can register a small service under the name "example/visionplugin"
//     (enabled with TOADI_EXAMPLE_REGISTER_SERVICE=1; inert by default),
//   * returns a VisionAlgoPlugin whose makeVisionAlgos() list is empty,
//     which is safe and leaves the stock SLAM plug-in untouched.
//
// Extend it by returning DetectionProcess-derived objects from
// makeVisionAlgos(); see include/toadi/vision_algo_plugin.h and spec 100.

#include "toadi/vision_algo_plugin.h"
#include "toadi/example_hearts.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

void log(const char* fmt, ...) {
    std::fputs("[example-vision-plugin] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

// A broker service. BrokerImpl stores it under every name returned by
// GetInterfaces(); typed clients would negotiate through QueryInterface().
class ExampleService final : public toadi::Unknown {
public:
    bool QueryInterface(toadi::Unknown& /*query*/) override {
        return false;  // no typed interface is exported by this example
    }

    std::vector<std::string> GetInterfaces() const override {
        return {"example/visionplugin"};
    }
};

class ExampleVisionPlugin final : public toadi::VisionAlgoPlugin {
public:
    explicit ExampleVisionPlugin(toadi::Broker& broker) : broker_(broker) {}

    std::vector<std::unique_ptr<toadi::DetectionProcess>> makeVisionAlgos() override {
        log("makeVisionAlgos() called (broker=%p)", static_cast<void*>(&broker_));
        // Add DetectionProcess-derived objects here to contribute vision
        // algorithms. The loader will call getName() on each object and hand
        // ownership to DetectionFramework.
        return {};
    }

private:
    toadi::Broker& broker_;
};

}  // namespace

namespace toadi {

std::unique_ptr<VisionAlgoPlugin> createVisionAlgoPlugin(Broker& broker) {
    try {
        log("factory invoked (broker=%p)", static_cast<void*>(&broker));

        // Optional hearts animation controller + REST trigger.  Off unless
        // the wrapper explicitly exports TOADI_EXAMPLE_HEARTS=1.
        const char* hearts_flag = std::getenv("TOADI_EXAMPLE_HEARTS");
        if (hearts_flag != nullptr && *hearts_flag != '\0') {
            example_hearts_start(broker);
        }

        // Registering a service that outlives the factory is only safe if the
        // host keeps this .so mapped. That is true in the stock application
        // (see spec 100, section 7.3), but make the side effect opt-in so the
        // library is inert when simply dropped into /home/toadi/plugins.
        const char* register_flag = std::getenv("TOADI_EXAMPLE_REGISTER_SERVICE");
        if (register_flag != nullptr && *register_flag != '\0') {
            if (broker.registerInterface(std::make_unique<ExampleService>())) {
                log("registered broker service 'example/visionplugin'");
            } else {
                log("broker registration failed (already registered?)");
            }

            // Demonstrate a lookup through the live registry. These names
            // exist in the stock app after initialisation; absence is okay.
            for (const char* name : {"watchdogmonitor", "activityscheduler", "configuration"}) {
                auto iface = broker.queryInterface(name);
                log("queryInterface(\"%s\") -> %s", name,
                    iface.valid() ? "present" : "absent");
            }
        } else {
            log("broker service registration disabled "
                "(set TOADI_EXAMPLE_REGISTER_SERVICE=1 to enable)");
        }

        return std::make_unique<ExampleVisionPlugin>(broker);
    } catch (const std::exception& e) {
        log("factory failed: %s", e.what());
    } catch (...) {
        log("factory failed: unknown exception");
    }
    return nullptr;
}

}  // namespace toadi

// Boost.DLL alias. libPlugins.so does:
//   dlsym(handle, "createVisionAlgoPlugin");
// and calls the function pointer stored in this variable.
extern "C" {
using CreateVisionAlgoPluginFn =
    std::unique_ptr<toadi::VisionAlgoPlugin> (*)(toadi::Broker&);

__attribute__((visibility("default"), used))
CreateVisionAlgoPluginFn createVisionAlgoPlugin = &toadi::createVisionAlgoPlugin;
}
