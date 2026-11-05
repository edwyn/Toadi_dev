// Hearts animation + REST trigger for the example Toadi plug-in.
//
// Enabled when the wrapper exports TOADI_EXAMPLE_HEARTS=1.  The controller:
//   * resolves the app's "eyesui" broker service lazily,
//   * adds/enables a high-priority EyesUILayer,
//   * draws the 12 embedded 1024-byte heart frames at 8 fps,
//   * listens on 0.0.0.0:8081 (override with TOADI_EXAMPLE_HEARTS_PORT).
//
// REST:
//   GET|POST|PUT /hearts[?loops=N]   play (N default 1, clamp 1..100)
//   GET|POST|PUT /hearts/off         stop and disable the layer
//   GET           /status            JSON state
#pragma once

namespace toadi {

class Broker;
class EyesUI;

// Starts the controller once.  Safe to call more than once; later calls are
// ignored.  Does nothing unless called by the plug-in factory.
void example_hearts_start(Broker& broker);

}  // namespace toadi

// Test hooks (unmangled).  The harness injects a fake EyesUI so the drawing
// path can be verified without the real app.
extern "C" {
__attribute__((visibility("default")))
void toadi_example_hearts_set_eyesui(toadi::EyesUI* ui);
__attribute__((visibility("default")))
void toadi_example_hearts_trigger(int loops);
__attribute__((visibility("default")))
void toadi_example_hearts_stop(void);
__attribute__((visibility("default")))
unsigned long toadi_example_hearts_frames_drawn(void);
__attribute__((visibility("default")))
int toadi_example_animations_trigger(const char* name, int loops);
__attribute__((visibility("default")))
int toadi_example_animations_count(void);
__attribute__((visibility("default")))
const char* toadi_example_animations_name(int index);
}
