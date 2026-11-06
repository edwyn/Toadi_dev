// Toadi eyes animation plug-in.
//
// Builds as an aarch64 shared library (libEyesAnimationPlugin.so) placed in
// /home/toadi/plugins. libPlugins.so discovers every *.so / *.dylib in that
// directory and offers each one to the four built-in loaders. This library
// answers the VisionAlgo loader by exporting the Boost.DLL alias symbol
// "createVisionAlgoPlugin".
//
// The plug-in:
//   * receives the live toadi::Broker& from the loader,
//   * starts the eyes animation controller + REST trigger when the wrapper
//     exports TOADI_EXAMPLE_HEARTS=1 (spec 102, section 4); without it the
//     library loads inertly, which is the safe default,
//   * returns a VisionAlgoPlugin whose makeVisionAlgos() list is empty, so
//     the stock SLAM plug-in and detection framework are untouched.
//
// The controller itself lives in hearts_controller.cpp.

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
    std::fputs("[eyes-animation-plugin] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

class EyesAnimationPlugin final : public toadi::VisionAlgoPlugin {
public:
    explicit EyesAnimationPlugin(toadi::Broker& broker) : broker_(broker) {}

    std::vector<std::unique_ptr<toadi::DetectionProcess>> makeVisionAlgos() override {
        log("makeVisionAlgos() called (broker=%p)", static_cast<void*>(&broker_));
        // Deliberately empty: this plug-in contributes no vision algorithms.
        // It only draws eye-panel animations through the eyesui service.
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

        // The eyes animation controller + REST trigger. Off unless the
        // wrapper explicitly exports TOADI_EXAMPLE_HEARTS=1.
        const char* hearts_flag = std::getenv("TOADI_EXAMPLE_HEARTS");
        if (hearts_flag != nullptr && *hearts_flag != '\0') {
            example_hearts_start(broker);
        }

        return std::make_unique<EyesAnimationPlugin>(broker);
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
