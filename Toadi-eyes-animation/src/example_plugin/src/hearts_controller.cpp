// Hearts animation controller and REST trigger for the example plug-in.
//
// The frame data is the 12 x 1024-byte 128x64 1-bpp set from the standalone
// eyes work (toadi_src/hearts.h).  The controller draws through the app's
// public EyesUI layer API, so ToadiApp remains the only owner of the SPI
// panels.  It is gated by TOADI_EXAMPLE_HEARTS=1.

#include "toadi/example_hearts.h"
#include "toadi/eyes_ui.h"
#include "toadi/broker.h"

#include "hearts.h"
#include "spidey_frames.h"
#include "spidey2_frames.h"
#include "spider_frames.h"
#include "pumpkin_frames.h"
#include "badge_frames.h"
#include "walle_frames.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace toadi {
namespace {

constexpr int kDefaultPort = 8081;
constexpr int kDefaultPriority = 2000;

std::atomic<EyesUI*> g_injected_eyesui{nullptr};

// hearts.h is packed MSB-first, matching the standalone driver.  The stock
// EyesUI path differs: DisplayImpl::update reads source bit (x & 7), i.e.
// LSB-first, before applying the page/column/bit transform.  Repacking each
// byte makes the plugin output byte-identical to the hardware-verified
// standalone stored_to_wire() transform (checked offline: 156/1024 wire bytes
// differ without it, 0 with it).
unsigned char reverseBits(unsigned char v) {
    v = static_cast<unsigned char>((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = static_cast<unsigned char>((v & 0xCC) >> 2 | (v & 0x33) << 2);
    v = static_cast<unsigned char>((v & 0xAA) >> 1 | (v & 0x55) << 1);
    return v;
}

void logf(const char* fmt, ...) {
    std::fputs("[example-hearts] ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

struct AnimationDef {
    const char* name;
    int frame_count;
    int delay_ms;
    const unsigned char (*left)[1024];
    const unsigned char (*right)[1024];
};

const AnimationDef kAnimations[] = {
    {"hearts", HEART_FRAMES, 125, heart_frames, heart_frames},
    // The reference files are labelled from the viewer's side; the panel
    // buffers are ordered from the robot's side, so swap left/right here.
    {"spidey", SPIDEY_LEFT_FRAMES, 125,
     spidey_right_frames, spidey_left_frames},
    {"spidey2", SPIDEY2_LEFT_FRAMES, 125,
     spidey2_right_frames, spidey2_left_frames},
    {"spider", SPIDER_FRAMES, 110, spider_frames, spider_frames},
    {"pumpkin", PUMPKIN_FRAMES, 140, pumpkin_frames, pumpkin_frames},
    {"badge", BADGE_FRAMES, 180, badge_frames, badge_frames},
    // Visual check: the viewer-labelled files map directly to the panels here
    // (unlike the spidey references, which needed the swap).
    {"walle", WALLE_LEFT_FRAMES, 120,
     walle_left_frames, walle_right_frames},
    // frames generated per trigger by buildFireworks()
    {"fireworks", 0, 110, nullptr, nullptr},
};
constexpr int kAnimationCount =
    static_cast<int>(sizeof(kAnimations) / sizeof(kAnimations[0]));

int animationIndexByName(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < kAnimationCount; ++i) {
        if (std::strcmp(name, kAnimations[i].name) == 0) return i;
    }
    return -1;
}

using Frame = std::array<unsigned char, 1024>;

void putPixel(Frame& frame, int x, int y) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    const int i = y * 128 + x;
    frame[i >> 3] |= static_cast<unsigned char>(0x80 >> (i & 7));
}

void putBlock(Frame& frame, int x, int y, int size) {
    for (int dy = 0; dy < size; ++dy) {
        for (int dx = 0; dx < size; ++dx) {
            putPixel(frame, x + dx, y + dy);
        }
    }
}

void drawLine(Frame& frame, int x0, int y0, int x1, int y1, int size = 1) {
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) {
        putBlock(frame, x1, y1, size);
        return;
    }
    for (int i = 0; i <= steps; ++i) {
        const int x = x0 + dx * i / steps;
        const int y = y0 + dy * i / steps;
        putBlock(frame, x, y, size);
    }
}

// Single-screen procedural peony: 5 rocket frames, flash, 22 burst frames.
// screen: 0 = left, 1 = right, 2 = both.  The inactive panel gets zeros.
void buildFireworks(int screen, std::vector<Frame>& left,
                    std::vector<Frame>& right) {
    constexpr int kFrames = 28;
    constexpr int kBurstX = 64;
    constexpr int kBurstY = 20;
    constexpr int kParticles = 20;
    constexpr float kZoom = 1.45f;
    constexpr int kThick = 2;

    std::vector<Frame> base(kFrames);
    for (auto& f : base) f.fill(0);

    std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= reinterpret_cast<std::uintptr_t>(&base);
    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    std::uniform_real_distribution<float> jitter(-0.18f, 0.18f);
    std::uniform_real_distribution<float> speed(2.8f, 4.8f);

    struct Particle {
        float x, y, vx, vy;
    };
    std::vector<Particle> particles;
    particles.reserve(kParticles);
    for (int i = 0; i < kParticles; ++i) {
        const float angle = (2.0f * 3.14159265358979f * i) / kParticles +
                            jitter(rng);
        const float s = speed(rng) * kZoom;
        particles.push_back({static_cast<float>(kBurstX),
                             static_cast<float>(kBurstY),
                             std::cos(angle) * s, std::sin(angle) * s});
    }

    for (int f = 0; f < kFrames; ++f) {
        Frame& frame = base[f];
        if (f <= 4) {
            const int y = 60 - f * 10;
            putBlock(frame, kBurstX - 1, y, kThick);
            for (int t = 2; t <= 8; t += 2) {
                if ((f + t) % 3 != 0)
                    putBlock(frame, kBurstX - 1, y + t, kThick);
            }
            if (f % 2 == 0) {
                putBlock(frame, kBurstX - 2, y + 1, kThick);
                putBlock(frame, kBurstX + 1, y + 1, kThick);
            }
        } else if (f == 5) {
            putBlock(frame, kBurstX - 1, kBurstY - 1, 3);
            for (int a = 0; a < 32; ++a) {
                const float ang = a * (2.0f * 3.14159265358979f / 32.0f);
                if (a % 2 == 0)
                    putBlock(frame,
                             kBurstX + static_cast<int>(std::cos(ang) * 6 * kZoom),
                             kBurstY + static_cast<int>(std::sin(ang) * 6 * kZoom),
                             kThick);
                putBlock(frame,
                         kBurstX + static_cast<int>(std::cos(ang) * 9 * kZoom),
                         kBurstY + static_cast<int>(std::sin(ang) * 9 * kZoom),
                         kThick);
            }
        } else {
            for (int i = 0; i < kParticles; ++i) {
                Particle& p = particles[i];
                const int px = static_cast<int>(p.x);
                const int py = static_cast<int>(p.y);
                p.x += p.vx;
                p.y += p.vy;
                p.vy += 0.28f * kZoom;
                p.vx *= 0.985f;
                p.vy *= 0.985f;
                const int x = static_cast<int>(p.x);
                const int y = static_cast<int>(p.y);
                // late twinkle: some particles disappear for a frame
                const bool twinkle = (f > 20 && ((f + i) % 3 == 0));
                if (!twinkle) {
                    drawLine(frame, px, py, x, y, kThick);
                    putBlock(frame, x, y, kThick);
                }
            }
        }
    }

    left.assign(kFrames, Frame{});
    right.assign(kFrames, Frame{});
    for (auto& f : left) f.fill(0);
    for (auto& f : right) f.fill(0);
    for (int i = 0; i < kFrames; ++i) {
        if (screen != 1) left[i] = base[i];
        if (screen != 0) right[i] = base[i];
    }
}

}  // namespace

// The app exports this template instantiation from libWebServer.so (weak).
// We resolve it at run time so the plug-in keeps only libc/libstdc++
// dependencies and still works in the isolated harness.
std::shared_ptr<EyesUI> queryEyesUI(Broker& broker) {
    using QueryFn = std::shared_ptr<EyesUI> (*)(const std::string&,
                                               const Broker&);
    static QueryFn fn = [] {
        return reinterpret_cast<QueryFn>(dlsym(
            RTLD_DEFAULT,
            "_ZN5toadi21queryInterfaceNoThrowINS_6EyesUIEEESt10shared_ptrIT_"
            "ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKNS_6"
            "BrokerE"));
    }();
    if (fn == nullptr) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            logf("queryInterfaceNoThrow<EyesUI> not found");
        }
        return {};
    }
    try {
        return fn(std::string("eyesui"), broker);
    } catch (...) {
        logf("eyesui query threw");
        return {};
    }
}

namespace {

class HeartsController {
public:
    void start(Broker& broker) {
        std::call_once(start_once_, [this, &broker] {
            broker_ = &broker;
            if (const char* p = std::getenv("TOADI_EXAMPLE_HEARTS_PORT")) {
                int v = std::atoi(p);
                if (v > 0 && v < 65536) port_ = v;
            }
            if (const char* p = std::getenv("TOADI_EXAMPLE_HEARTS_PRIORITY")) {
                int v = std::atoi(p);
                if (v >= 0) priority_ = v;
            }
            logf("controller started (port=%d priority=%d)", port_, priority_);
            std::thread(&HeartsController::workerLoop, this).detach();
            std::thread(&HeartsController::serverLoop, this).detach();
        });
    }

    bool triggerAnimation(int index, int loops, int screen = 2) {
        if (index < 0 || index >= kAnimationCount) return false;
        if (screen < 0 || screen > 2) screen = 2;
        if (loops < 1) loops = 1;
        if (loops > 100) loops = 100;
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_anim_ = index;
            pending_loops_ = loops;
            pending_screen_ = screen;
        }
        current_anim_.store(index);
        current_screen_.store(screen);
        stop_.store(false);
        cv_.notify_one();
        return true;
    }

    bool triggerByName(const char* name, int loops, int screen = 2) {
        return triggerAnimation(animationIndexByName(name), loops, screen);
    }

    void trigger(int loops) { (void)triggerAnimation(0, loops, 2); }

    static const char* screenName(int screen) {
        return screen == 0 ? "left" : screen == 1 ? "right" : "both";
    }

    void stopAnimation() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_loops_ = 0;
        }
        stop_.store(true);
        cv_.notify_all();
    }

    unsigned long framesDrawn() const { return frames_.load(); }

    const char* animationName() const {
        return kAnimations[current_anim_.load()].name;
    }

private:
    void workerLoop() {
        for (;;) {
            int loops = 0;
            int anim_index = 0;
            int screen = 2;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return pending_loops_ > 0; });
                loops = pending_loops_;
                anim_index = pending_anim_;
                screen = pending_screen_;
                pending_loops_ = 0;
            }

            const AnimationDef& anim = kAnimations[anim_index];
            std::vector<Frame> dynamic_left;
            std::vector<Frame> dynamic_right;
            int frame_count = anim.frame_count;
            if (anim.left == nullptr) {
                buildFireworks(screen, dynamic_left, dynamic_right);
                frame_count = static_cast<int>(dynamic_left.size());
            }

            animating_.store(true);
            if (!ensureLayer()) {
                logf("eyesui/layer unavailable; trigger ignored");
                animating_.store(false);
                continue;
            }

            try {
                layer_->enable(true);
            } catch (...) {
                logf("layer enable threw");
            }

            bool aborted = false;
            for (int l = 0; l < loops && !aborted; ++l) {
                for (int f = 0; f < frame_count; ++f) {
                    if (stop_.load()) {
                        aborted = true;
                        break;
                    }
                    const unsigned char* lsrc =
                        anim.left != nullptr ? anim.left[f]
                                             : dynamic_left[f].data();
                    const unsigned char* rsrc =
                        anim.right != nullptr ? anim.right[f]
                                              : dynamic_right[f].data();
                    if (!drawFrame(lsrc, rsrc)) {
                        aborted = true;
                        break;
                    }
                    ++frames_;
                    sleepWithStop(anim.delay_ms);
                }
            }

            try {
                if (layer_) layer_->enable(false);
            } catch (...) {
                logf("layer disable threw");
            }
            stop_.store(false);
            animating_.store(false);
        }
    }

    bool ensureLayer() {
        if (!ui_) {
            EyesUI* injected = g_injected_eyesui.load();
            if (injected != nullptr) {
                ui_ = std::shared_ptr<EyesUI>(injected, [](EyesUI*) {});
            } else if (broker_ != nullptr) {
                ui_ = queryEyesUI(*broker_);
            }
            eyesui_ready_.store(static_cast<bool>(ui_));
        }
        if (!ui_) return false;

        if (!layer_) {
            try {
                layer_ = ui_->addLayer(priority_);
            } catch (...) {
                logf("addLayer(%d) threw", priority_);
                layer_.reset();
            }
            layer_ready_.store(static_cast<bool>(layer_));
        }
        return static_cast<bool>(layer_);
    }

    bool drawFrame(const unsigned char* lsrc, const unsigned char* rsrc) {
        if (!layer_) return false;
        std::vector<unsigned char> left(1024);
        std::vector<unsigned char> right(1024);
        for (int i = 0; i < 1024; ++i) {
            left[i] = reverseBits(lsrc[i]);
            right[i] = reverseBits(rsrc[i]);
        }
        try {
            layer_->drawImages(left, right, Display::WriteMode::Normal);
            return true;
        } catch (...) {
            logf("drawImages threw");
            return false;
        }
    }

    void sleepWithStop(int total_ms) {
        const int step = 10;
        for (int elapsed = 0; elapsed < total_ms; elapsed += step) {
            if (stop_.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
        }
    }

    void serverLoop() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            logf("socket() failed: %s", std::strerror(errno));
            return;
        }

        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            logf("bind(:%d) failed: %s", port_, std::strerror(errno));
            ::close(fd);
            return;
        }
        if (::listen(fd, 16) != 0) {
            logf("listen(:%d) failed: %s", port_, std::strerror(errno));
            ::close(fd);
            return;
        }
        logf("REST listening on :%d", port_);

        for (;;) {
            int client = ::accept(fd, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) continue;
                logf("accept failed: %s", std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            handleClient(client);
            ::close(client);
        }
    }

    void handleClient(int fd) {
        timeval tv{};
        tv.tv_sec = 3;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return;
        buf[n] = '\0';

        char method[16] = {0};
        char target[1024] = {0};
        if (std::sscanf(buf, "%15s %1023s", method, target) != 2) {
            sendResponse(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}");
            return;
        }

        std::string target_s(target);
        std::string path = target_s;
        std::string query;
        const auto qpos = target_s.find('?');
        if (qpos != std::string::npos) {
            path = target_s.substr(0, qpos);
            query = target_s.substr(qpos + 1);
        }

        if (std::strcmp(method, "OPTIONS") == 0) {
            sendResponse(fd, 204, "");
            return;
        }

        int anim_index = -1;
        if (path == "/hearts") anim_index = 0;
        else if (path == "/spidey") anim_index = 1;
        else if (path == "/spidey2") anim_index = 2;
        else if (path == "/spider") anim_index = 3;
        else if (path == "/pumpkin") anim_index = 4;
        else if (path == "/badge" || path == "/pawpatrol") anim_index = 5;
        else if (path == "/walle" || path == "/wall-e") anim_index = 6;
        else if (path == "/fireworks") anim_index = 7;
        else if (path == "/animation") {
            anim_index = animationIndexByName(
                parseQueryValue(query, "name").c_str());
        }

        if (anim_index >= 0) {
            const int loops = parseLoops(query);
            const int screen = parseScreen(query);
            if (!triggerAnimation(anim_index, loops, screen)) {
                sendResponse(fd, 400,
                             "{\"ok\":false,\"error\":\"unknown animation\"}");
                return;
            }
            char out[224];
            std::snprintf(out, sizeof(out),
                          "{\"ok\":true,\"animation\":\"%s\",\"loops\":%d,"
                          "\"screen\":\"%s\",\"animating\":true}",
                          kAnimations[anim_index].name, loops,
                          screenName(screen));
            sendResponse(fd, 200, out);
            return;
        }

        if (path == "/animations") {
            sendResponse(fd, 200,
                         "{\"ok\":true,\"animations\":[\"hearts\","
                         "\"spidey\",\"spidey2\",\"spider\",\"pumpkin\","
                         "\"badge\",\"walle\",\"fireworks\"]}");
            return;
        }

        if (path == "/hearts/off" || path == "/off") {
            stopAnimation();
            sendResponse(fd, 200, "{\"ok\":true,\"animating\":false}");
            return;
        }

        if (path == "/status") {
            char out[512];
            std::snprintf(out, sizeof(out),
                          "{\"ok\":true,\"eyesui\":%s,\"layer\":%s,"
                          "\"animating\":%s,\"animation\":\"%s\","
                          "\"animations\":[\"hearts\",\"spidey\","
                          "\"spidey2\",\"spider\",\"pumpkin\",\"badge\","
                          "\"walle\",\"fireworks\"],\"screen\":\"%s\","
                          "\"frames\":%lu,\"port\":%d,\"priority\":%d}",
                          eyesui_ready_.load() ? "true" : "false",
                          layer_ready_.load() ? "true" : "false",
                          animating_.load() ? "true" : "false",
                          animationName(), screenName(current_screen_.load()),
                          frames_.load(), port_, priority_);
            sendResponse(fd, 200, out);
            return;
        }

        sendResponse(fd, 404, "{\"ok\":false,\"error\":\"not found\"}");
    }

    static std::string parseQueryValue(const std::string& query,
                                       const std::string& key) {
        const std::string prefix = key + "=";
        const auto pos = query.find(prefix);
        if (pos == std::string::npos) return {};
        const auto start = pos + prefix.size();
        const auto end = query.find('&', start);
        return query.substr(start, end == std::string::npos
                                       ? std::string::npos
                                       : end - start);
    }

    static int parseScreen(const std::string& query) {
        const std::string value = parseQueryValue(query, "screen");
        if (value == "left") return 0;
        if (value == "right") return 1;
        return 2;  // both / default
    }

    static int parseLoops(const std::string& query) {
        int loops = 1;
        const std::string key = "loops=";
        const auto pos = query.find(key);
        if (pos != std::string::npos) {
            loops = std::atoi(query.c_str() + pos + key.size());
        }
        if (loops < 1) loops = 1;
        if (loops > 100) loops = 100;
        return loops;
    }

    static void sendResponse(int fd, int status, const std::string& body) {
        const char* reason = "OK";
        if (status == 204) reason = "No Content";
        if (status == 400) reason = "Bad Request";
        if (status == 404) reason = "Not Found";

        char header[512];
        std::snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                      "Access-Control-Allow-Headers: Content-Type\r\n"
                      "Connection: close\r\n"
                      "Content-Length: %zu\r\n"
                      "\r\n",
                      status, reason, body.size());
        (void)::send(fd, header, std::strlen(header), MSG_NOSIGNAL);
        if (!body.empty()) {
            (void)::send(fd, body.data(), body.size(), MSG_NOSIGNAL);
        }
    }

    std::once_flag start_once_;
    Broker* broker_ = nullptr;
    std::shared_ptr<EyesUI> ui_;
    std::shared_ptr<EyesUILayer> layer_;
    std::mutex mu_;
    std::condition_variable cv_;
    int pending_loops_ = 0;
    int pending_anim_ = 0;
    int pending_screen_ = 2;
    std::atomic<int> current_anim_{0};
    std::atomic<int> current_screen_{2};
    std::atomic<bool> stop_{false};
    std::atomic<bool> animating_{false};
    std::atomic<bool> eyesui_ready_{false};
    std::atomic<bool> layer_ready_{false};
    std::atomic<unsigned long> frames_{0};
    int port_ = kDefaultPort;
    int priority_ = kDefaultPriority;
};

HeartsController& controller() {
    static HeartsController* instance = new HeartsController();
    return *instance;
}

}  // namespace

void example_hearts_start(Broker& broker) { controller().start(broker); }

}  // namespace toadi

extern "C" {

void toadi_example_hearts_set_eyesui(toadi::EyesUI* ui) {
    toadi::g_injected_eyesui.store(ui);
}

void toadi_example_hearts_trigger(int loops) {
    toadi::controller().trigger(loops);
}

int toadi_example_animations_trigger(const char* name, int loops) {
    return toadi::controller().triggerByName(name, loops) ? 1 : 0;
}

int toadi_example_animations_count(void) {
    return toadi::kAnimationCount;
}

const char* toadi_example_animations_name(int index) {
    if (index < 0 || index >= toadi::kAnimationCount) return nullptr;
    return toadi::kAnimations[index].name;
}

void toadi_example_hearts_stop(void) {
    toadi::controller().stopAnimation();
}

unsigned long toadi_example_hearts_frames_drawn(void) {
    return toadi::controller().framesDrawn();
}

}  // extern "C"
