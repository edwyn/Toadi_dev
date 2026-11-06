// Reconstructed from libPlugins.so and plugins/libSlamPluginApp.so
// (Toadi app 6.8.0-202607011333+3b3ec6e).
//
// VisionAlgoPlugin is the simplest additive plug-in interface:
//
//   vtable layout (aarch64):
//     +0x00  ~VisionAlgoPlugin()  (D1)
//     +0x08  ~VisionAlgoPlugin()  (D0)
//     +0x10  std::vector<std::unique_ptr<DetectionProcess>> makeVisionAlgos()
//
// Factory symbol exported by a plug-in library (a Boost.DLL alias variable,
// not a function):  createVisionAlgoPlugin
// C++ factory signature:  std::unique_ptr<VisionAlgoPlugin> createVisionAlgoPlugin(Broker&)
//
// libPlugins.so::VisionAlgoPluginLoader::loadPlugin does, per .so file:
//   dlsym(handle, "createVisionAlgoPlugin")
//   fn = *(unique_ptr<VisionAlgoPlugin> (*)(Broker&)*)symbol
//   auto plugin = fn(broker);
//   for (auto& algo : plugin->makeVisionAlgos()) {
//       auto name = algo->getName();                  // DetectionProcess vtable +0x28
//       detectionFramework.add(std::move(algo));       // DetectionFramework vtable +0x48
//   }
//
// Plug-ins are additive and keyed by library path, so a new plug-in can live
// in /home/toadi/plugins next to libSlamPluginApp.so without replacing it.
//
// DetectionProcess is only forward-declared here. Returning an empty vector
// needs nothing else. The recovered full vtable (from toadi::EeveSlamAlgo) is:
//   +0x00 ~DetectionProcess()
//   +0x08 ~DetectionProcess()
//   +0x10 update(std::shared_ptr<MultiSourceImageFrame> const&)
//   +0x18 configure(Broker&, Configuration const&)
//   +0x20 run()
//   +0x28 std::string getName() const
//   +0x30 bool hasWork() const
//   +0x38 setPaused(bool)
//   +0x40 rePaint()
//   +0x48 getMaxCycleTime() const
//   +0x50 getConstraints() const
// Only slots +0x10..+0x30 are confirmed by name; the remaining return types
// are not encoded in the dynamic symbols. See spec 100 for details.
#pragma once

#include <memory>
#include <vector>

#include "toadi/broker.h"

namespace toadi {

class DetectionProcess {
public:
    // Kept virtual so unique_ptr<DetectionProcess> deletes real processes
    // through their own vtable. The real class has ten virtual methods; only
    // the destructor is needed for an empty plug-in.
    virtual ~DetectionProcess() = default;
};

class VisionAlgoPlugin {
public:
    virtual ~VisionAlgoPlugin() = default;
    virtual std::vector<std::unique_ptr<DetectionProcess>> makeVisionAlgos() = 0;
};

// Implemented by the plug-in and exported through the createVisionAlgoPlugin
// alias variable. The Broker is the application's live registry; it may be
// used for registerInterface()/queryInterface().
std::unique_ptr<VisionAlgoPlugin> createVisionAlgoPlugin(Broker& broker);

} // namespace toadi
