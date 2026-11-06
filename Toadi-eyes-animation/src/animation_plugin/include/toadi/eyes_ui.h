// Reconstructed from libEyesUI.so (Toadi app 6.7.0/6.8.0).
//
// The app registers an EyesUI service under the broker key "eyesui".  The
// public interface has only addLayer()/getLayer(); the layer interface has the
// drawing methods used by the stock EyesAnimator.
//
// vtable layout (aarch64, Itanium C++ ABI):
//   EyesUI:
//     +0x00  ~EyesUI()                                  (D1)
//     +0x08  ~EyesUI()                                  (D0)
//     +0x10  std::shared_ptr<EyesUILayer> addLayer(int priority)
//     +0x18  std::shared_ptr<EyesUILayer> getLayer(int priority)
//   EyesUILayer:
//     +0x00  ~EyesUILayer()                             (D1)
//     +0x08  ~EyesUILayer()                             (D0)
//     +0x10  void enable(bool)
//     +0x18  bool isEnabled() const
//     +0x20  void drawText(const std::string&)
//     +0x28  void drawText(const std::string&, DisplayMode)
//     +0x30  void drawImages(const std::vector<unsigned char>& left,
//                            const std::vector<unsigned char>& right,
//                            Display::WriteMode)
//
// Evidence: libEyesUI.so vtables at .data.rel.ro 0x3a0a8 / 0x3a2a0; the stock
// EyesAnimator calls drawImages at vtable +0x30 with WriteMode 0
// (libEmotions.so 0xcd618-0xcd630).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace toadi {

// Only the enum value used by the stock animator is documented; the underlying
// type is int and the call passes it in w3.
class Display {
public:
    enum class WriteMode : int {
        Normal = 0,
    };
};

// Declared with a fixed underlying type; drawText is not used by this plug-in.
enum class DisplayMode : int;

class EyesUILayer {
public:
    virtual ~EyesUILayer() = default;

    virtual void enable(bool enabled) = 0;
    virtual bool isEnabled() const = 0;
    virtual void drawText(const std::string& text) = 0;
    virtual void drawText(const std::string& text, DisplayMode mode) = 0;
    virtual void drawImages(const std::vector<unsigned char>& left,
                            const std::vector<unsigned char>& right,
                            Display::WriteMode mode) = 0;
};

class EyesUI {
public:
    virtual ~EyesUI() = default;

    // Creates (or returns the existing) layer for this priority/z-order.
    virtual std::shared_ptr<EyesUILayer> addLayer(int priority) = 0;
    virtual std::shared_ptr<EyesUILayer> getLayer(int priority) = 0;
};

class Broker;

// Runtime resolution of the app's exported
// queryInterfaceNoThrow<toadi::EyesUI>(name, broker) template instantiation.
// Returns null if the symbol is unavailable (e.g. isolated harness).
std::shared_ptr<EyesUI> queryEyesUI(Broker& broker);

}  // namespace toadi
