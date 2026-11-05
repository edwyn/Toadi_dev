// Hearts plug-in harness: fake eyesui service + REST round-trip.
//
// Loads libExampleVisionPlugin.so directly, invokes the factory with a real
// Broker, injects a fake EyesUI through the plug-in's test hook, then:
//   1. triggers one animation and verifies 12 drawImages calls of 1024 bytes,
//   2. checks layer priority/enable/disable,
//   3. exercises GET /status, GET /hearts?loops=1, GET /hearts/off on :8081.
//
// No ToadiApp, SPI or real EyesUI is involved.

#include "toadi/broker.h"
#include "toadi/example_hearts.h"
#include "toadi/eyes_ui.h"
#include "toadi/vision_algo_plugin.h"

#include "hearts.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

unsigned char reverseBits(unsigned char v) {
    v = static_cast<unsigned char>((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = static_cast<unsigned char>((v & 0xCC) >> 2 | (v & 0x33) << 2);
    v = static_cast<unsigned char>((v & 0xAA) >> 1 | (v & 0x55) << 1);
    return v;
}

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

bool anyNonZero(const std::vector<unsigned char>& v) {
    for (unsigned char b : v) {
        if (b != 0) return true;
    }
    return false;
}

class FakeLayer final : public toadi::EyesUILayer {
public:
    void enable(bool enabled) override {
        std::lock_guard<std::mutex> lock(mu_);
        enable_true_ = enable_true_ || enabled;
        enable_false_ = enable_false_ || !enabled;
        enabled_ = enabled;
    }
    bool isEnabled() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return enabled_;
    }
    void drawText(const std::string&) override {}
    void drawText(const std::string&, toadi::DisplayMode) override {}

    void drawImages(const std::vector<unsigned char>& left,
                    const std::vector<unsigned char>& right,
                    toadi::Display::WriteMode) override {
        std::lock_guard<std::mutex> lock(mu_);
        ++draws_;
        last_left_size_ = left.size();
        last_right_size_ = right.size();
        if (first_left_.empty()) first_left_ = left;
        if (first_right_.empty()) first_right_ = right;
        if (frames_seen_ < HEART_FRAMES) {
            // The controller draws frames in order; capture the first cycle.
            frames_seen_++;
        }
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mu_);
        draws_ = 0;
        frames_seen_ = 0;
        first_left_.clear();
        first_right_.clear();
        enable_true_ = false;
        enable_false_ = false;
        enabled_ = false;
    }

    int draws() const { std::lock_guard<std::mutex> lock(mu_); return draws_; }
    size_t leftSize() const { std::lock_guard<std::mutex> lock(mu_); return last_left_size_; }
    size_t rightSize() const { std::lock_guard<std::mutex> lock(mu_); return last_right_size_; }
    bool sawEnableTrue() const { std::lock_guard<std::mutex> lock(mu_); return enable_true_; }
    bool sawEnableFalse() const { std::lock_guard<std::mutex> lock(mu_); return enable_false_; }
    std::vector<unsigned char> firstLeft() const {
        std::lock_guard<std::mutex> lock(mu_);
        return first_left_;
    }

    std::vector<unsigned char> firstRight() const {
        std::lock_guard<std::mutex> lock(mu_);
        return first_right_;
    }

private:
    mutable std::mutex mu_;
    int draws_ = 0;
    int frames_seen_ = 0;
    size_t last_left_size_ = 0;
    size_t last_right_size_ = 0;
    bool enable_true_ = false;
    bool enable_false_ = false;
    bool enabled_ = false;
    std::vector<unsigned char> first_left_;
    std::vector<unsigned char> first_right_;
};

class FakeEyesUI final : public toadi::EyesUI {
public:
    explicit FakeEyesUI(std::shared_ptr<FakeLayer> layer)
        : layer_(std::move(layer)) {}

    std::shared_ptr<toadi::EyesUILayer> addLayer(int priority) override {
        priority_ = priority;
        return layer_;
    }
    std::shared_ptr<toadi::EyesUILayer> getLayer(int) override { return layer_; }

    int priority() const { return priority_; }

private:
    std::shared_ptr<FakeLayer> layer_;
    int priority_ = -1;
};

bool waitFor(const std::function<bool()>& pred, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

int connectLocal(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string httpGet(int port, const std::string& path) {
    int fd = -1;
    for (int i = 0; i < 300; ++i) {
        fd = connectLocal(port);
        if (fd >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (fd < 0) return {};

    std::string request = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    (void)::send(fd, request.data(), request.size(), 0);

    std::string response;
    char buf[1024];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return response;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/libExampleVisionPlugin.so\n",
                     argv[0]);
        return 2;
    }

    setenv("TOADI_EXAMPLE_HEARTS", "1", 1);
    setenv("TOADI_EXAMPLE_HEARTS_PRIORITY", "2000", 1);
    setenv("TOADI_EXAMPLE_HEARTS_PORT", "8081", 1);

    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    auto* factory_slot = static_cast<std::unique_ptr<toadi::VisionAlgoPlugin> (**)(
        toadi::Broker&)>(dlsym(handle, "createVisionAlgoPlugin"));
    check(factory_slot != nullptr && *factory_slot != nullptr,
          "createVisionAlgoPlugin alias");

    auto set_eyesui = reinterpret_cast<void (*)(toadi::EyesUI*)>(
        dlsym(handle, "toadi_example_hearts_set_eyesui"));
    auto trigger = reinterpret_cast<void (*)(int)>(
        dlsym(handle, "toadi_example_hearts_trigger"));
    auto stop = reinterpret_cast<void (*)(void)>(
        dlsym(handle, "toadi_example_hearts_stop"));
    auto frames_drawn = reinterpret_cast<unsigned long (*)(void)>(
        dlsym(handle, "toadi_example_hearts_frames_drawn"));
    check(set_eyesui && trigger && stop && frames_drawn,
          "hearts test hooks exported");
    if (!factory_slot || !*factory_slot || !set_eyesui || !trigger || !stop ||
        !frames_drawn) {
        return 1;
    }

    auto layer = std::make_shared<FakeLayer>();
    auto fake_ui = std::make_shared<FakeEyesUI>(layer);
    set_eyesui(fake_ui.get());

    auto broker = toadi::makeBroker();
    check(broker != nullptr, "makeBroker()");

    auto plugin = (*factory_slot)(*broker);
    check(plugin != nullptr, "factory returned a plug-in");
    if (plugin) plugin->makeVisionAlgos();

    layer->reset();
    trigger(1);
    check(waitFor([&] { return frames_drawn() >= HEART_FRAMES; }, 5000),
          "direct trigger drew 12 frames");
    check(layer->draws() == HEART_FRAMES, "12 drawImages calls");
    check(layer->leftSize() == 1024 && layer->rightSize() == 1024,
          "left/right frame buffers are 1024 bytes");
    check(fake_ui->priority() == 2000, "addLayer priority 2000");
    check(layer->sawEnableTrue(), "layer enabled on start");
    check(waitFor([&] { return layer->sawEnableFalse(); }, 2000),
          "layer disabled on stop");
    const auto first = layer->firstLeft();
    std::vector<unsigned char> expected_first(1024);
    for (int i = 0; i < 1024; ++i) {
        expected_first[i] = reverseBits(heart_frames[0][i]);
    }
    check(first == expected_first,
          "first frame is the LSB-repacked heart_frames[0]");

    const std::string status = httpGet(8081, "/status");
    check(status.find("200 OK") != std::string::npos &&
              status.find("\"eyesui\":true") != std::string::npos &&
              status.find("\"layer\":true") != std::string::npos,
          "GET /status reports eyesui + layer");

    layer->reset();
    const unsigned long before = frames_drawn();
    const std::string play = httpGet(8081, "/hearts?loops=1");
    check(play.find("200 OK") != std::string::npos, "GET /hearts returns 200");
    check(waitFor([&] { return frames_drawn() >= before + HEART_FRAMES; }, 5000),
          "REST trigger drew 12 frames");

    layer->reset();
    const unsigned long before_spidey = frames_drawn();
    const std::string spidey = httpGet(8081, "/spidey?loops=1");
    check(spidey.find("200 OK") != std::string::npos,
          "GET /spidey returns 200");
    check(waitFor([&] { return frames_drawn() >= before_spidey + 12; }, 5000),
          "spidey drew 12 frames");
    check(layer->firstLeft() != layer->firstRight(),
          "spidey uses different left/right lenses");
    check(httpGet(8081, "/status").find("\"animation\":\"spidey\"") !=
              std::string::npos,
          "status reports spidey");

    layer->reset();
    const unsigned long before_spidey2 = frames_drawn();
    const std::string spidey2 = httpGet(8081, "/spidey2?loops=1");
    check(spidey2.find("200 OK") != std::string::npos,
          "GET /spidey2 returns 200");
    check(waitFor([&] { return frames_drawn() >= before_spidey2 + 12; }, 5000),
          "spidey2 drew 12 frames");
    check(layer->firstLeft() != layer->firstRight(),
          "spidey2 uses different left/right contours");
    check(httpGet(8081, "/status").find("\"animation\":\"spidey2\"") !=
              std::string::npos,
          "status reports spidey2");

    layer->reset();
    const unsigned long before_spider = frames_drawn();
    const std::string spider = httpGet(8081, "/spider?loops=1");
    check(spider.find("200 OK") != std::string::npos,
          "GET /spider returns 200");
    check(waitFor([&] { return frames_drawn() >= before_spider + 16; }, 5000),
          "spider drew 16 frames");
    check(httpGet(8081, "/status").find("\"animation\":\"spider\"") !=
              std::string::npos,
          "status reports spider");
    check(httpGet(8081, "/animations").find("\"spider\"") != std::string::npos,
          "GET /animations lists spider");

    layer->reset();
    const unsigned long before_pumpkin = frames_drawn();
    const std::string pumpkin = httpGet(8081, "/pumpkin?loops=1");
    check(pumpkin.find("200 OK") != std::string::npos,
          "GET /pumpkin returns 200");
    check(waitFor([&] { return frames_drawn() >= before_pumpkin + 12; }, 5000),
          "pumpkin drew 12 frames");
    check(httpGet(8081, "/status").find("\"animation\":\"pumpkin\"") !=
              std::string::npos,
          "status reports pumpkin");
    check(httpGet(8081, "/animations").find("\"pumpkin\"") !=
              std::string::npos,
          "GET /animations lists pumpkin");

    layer->reset();
    const unsigned long before_badge = frames_drawn();
    const std::string badge = httpGet(8081, "/badge?loops=1");
    check(badge.find("200 OK") != std::string::npos,
          "GET /badge returns 200");
    check(waitFor([&] { return frames_drawn() >= before_badge + 8; }, 5000),
          "badge drew 8 frames");
    check(httpGet(8081, "/status").find("\"animation\":\"badge\"") !=
              std::string::npos,
          "status reports badge");
    check(httpGet(8081, "/animations").find("\"badge\"") !=
              std::string::npos,
          "GET /animations lists badge");

    layer->reset();
    const unsigned long before_walle = frames_drawn();
    const std::string walle = httpGet(8081, "/walle?loops=1");
    check(walle.find("200 OK") != std::string::npos,
          "GET /walle returns 200");
    check(waitFor([&] { return frames_drawn() >= before_walle + 16; }, 5000),
          "walle drew 16 frames");
    check(layer->firstLeft() != layer->firstRight(),
          "walle uses different left/right eyes");
    check(httpGet(8081, "/status").find("\"animation\":\"walle\"") !=
              std::string::npos,
          "status reports walle");
    check(httpGet(8081, "/animations").find("\"walle\"") !=
              std::string::npos,
          "GET /animations lists walle");

    layer->reset();
    const unsigned long before_fw_left = frames_drawn();
    const std::string fw_left =
        httpGet(8081, "/fireworks?loops=1&screen=left");
    check(fw_left.find("200 OK") != std::string::npos,
          "GET /fireworks?screen=left returns 200");
    check(waitFor([&] { return frames_drawn() >= before_fw_left + 28; }, 6000),
          "fireworks left drew 28 frames");
    check(anyNonZero(layer->firstLeft()), "fireworks left panel is active");
    check(!anyNonZero(layer->firstRight()), "fireworks right panel is blank");

    layer->reset();
    const unsigned long before_fw_right = frames_drawn();
    check(httpGet(8081, "/fireworks?loops=1&screen=right")
              .find("200 OK") != std::string::npos,
          "GET /fireworks?screen=right returns 200");
    check(waitFor([&] { return frames_drawn() >= before_fw_right + 28; }, 6000),
          "fireworks right drew 28 frames");
    check(!anyNonZero(layer->firstLeft()), "fireworks left stays blank");
    check(anyNonZero(layer->firstRight()), "fireworks right panel is active");

    layer->reset();
    const unsigned long before_fw_both = frames_drawn();
    check(httpGet(8081, "/fireworks?loops=1&screen=both")
              .find("200 OK") != std::string::npos,
          "GET /fireworks?screen=both returns 200");
    check(waitFor([&] { return frames_drawn() >= before_fw_both + 28; }, 6000),
          "fireworks both drew 28 frames");
    check(anyNonZero(layer->firstLeft()) && anyNonZero(layer->firstRight()),
          "fireworks both panels are active");
    check(httpGet(8081, "/status").find("\"animation\":\"fireworks\"") !=
              std::string::npos,
          "status reports fireworks");
    check(httpGet(8081, "/animations").find("\"fireworks\"") !=
              std::string::npos,
          "GET /animations lists fireworks");

    const std::string off = httpGet(8081, "/hearts/off");
    check(off.find("200 OK") != std::string::npos, "GET /hearts/off returns 200");
    check(waitFor([&] { return !layer->isEnabled(); }, 2000),
          "layer disabled after /hearts/off");

    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
