// Follow plug-in harness: REST trigger + safety interlocks + stock forwarding.
//
// Loads libFollowPlugin.so, starts a mock stock web server, then exercises:
//   - /status motion interlock reporting
//   - dry-run /follow (no stock request)
//   - confirmed /follow with speed clamping (stock request is recorded)
//   - /stop
//   - auto-stop timer
//   - refusal when the allow-motion flag is removed
//
// No ToadiApp, camera, TensorRT or motors are involved.

#include "toadi/broker.h"
#include "toadi/follow_plugin.h"
#include "toadi/vision_algo_plugin.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

std::string httpGet(int port, const std::string& path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    const std::string request =
        "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    (void)::send(fd, request.data(), request.size(), 0);
    std::string response;
    char buf[1024];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return response;
}

bool waitForServer(int port, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool ok =
            ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        ::close(fd);
        if (ok) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool sendWsTextFrame(int fd, const std::string& payload);

class MockStock {
public:
    bool start(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd_, 4) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        thread_ = std::thread(&MockStock::loop, this);
        return true;
    }

    void stop() {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    int count() const { return count_.load(); }

    std::vector<std::string> paths() const {
        std::lock_guard<std::mutex> lock(mu_);
        return paths_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        paths_.clear();
        count_.store(0);
    }

    void setAutopilotDelay(int ms) { autopilot_delay_ms_.store(ms); }

    void setActivityJson(const std::string& json) {
        std::lock_guard<std::mutex> lock(mu_);
        activity_json_ = json;
    }

    void setWsEnabled(bool enabled) { ws_enabled_.store(enabled); }

private:
    void loop() {
        for (;;) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) return;
            char buf[2048];
            const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
            std::string target;
            std::string body = "{\"ok\":true,\"mock\":true}";
            if (n > 0) {
                buf[n] = '\0';
                char method[16] = {0};
                char target_buf[1024] = {0};
                bool delay = false;
                if (std::sscanf(buf, "%15s %1023s", method, target_buf) == 2) {
                    target = target_buf;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        paths_.push_back(target);
                        ++count_;
                        if (target.rfind("/api/activities/info", 0) == 0) {
                            body = activity_json_;
                        }
                    }
                    delay =
                        std::strncmp(target_buf, "/navigation/autopilot", 21) == 0 &&
                        autopilot_delay_ms_.load() > 0;
                }
                if (ws_enabled_.load() &&
                    target.rfind("/observable/Observations", 0) == 0) {
                    std::fprintf(stderr, "[mock] ws upgrade %s\n",
                                 target.c_str());
                    const std::string handshake =
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: "
                        "dG9hZGktZm9sbG93LXBsdWdpbg==\r\n\r\n";
                    ::send(client, handshake.data(), handshake.size(),
                           MSG_NOSIGNAL);
                    std::thread([client] {
                        const std::string alarm =
                            R"({"@type":"toadi:Alarm",)"
                            R"("category":"toadi:Object","objects":[)"
                            R"({"label":"person","prob":95,)"
                            R"("roi":[250,0,120,220]}]})";
                        for (int i = 0; i < 80; ++i) {
                            if (!sendWsTextFrame(client, alarm)) break;
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(200));
                        }
                        ::close(client);
                    }).detach();
                    continue;
                }
                if (delay) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        autopilot_delay_ms_.load()));
                }
            }
            char header[256];
            std::snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                          body.size());
            ::send(client, header, std::strlen(header), 0);
            ::send(client, body.data(), body.size(), 0);
            ::close(client);
        }
    }

    int fd_ = -1;
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<std::string> paths_;
    std::string activity_json_{
        "{\"scheduledActivity\":\"\",\"userActivity\":\"\"}"};
    std::atomic<int> count_{0};
    std::atomic<int> autopilot_delay_ms_{0};
    std::atomic<bool> ws_enabled_{false};
};

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool hasPath(const std::vector<std::string>& paths, const char* prefix) {
    for (const auto& path : paths) {
        if (path.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// Server-to-client WebSocket text frame (unmasked, single fragment).
bool sendWsTextFrame(int fd, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + text
    const size_t n = payload.size();
    if (n <= 125) {
        frame.push_back(static_cast<char>(n));
    } else if (n <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((n >> 8) & 0xff));
        frame.push_back(static_cast<char>(n & 0xff));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((n >> shift) & 0xff));
        }
    }
    frame += payload;
    const ssize_t sent = ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(frame.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/libFollowPlugin.so\n", argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);

    const int stock_port = 18080;
    const int rest_port = 18082;
    const char* flag_path = "/tmp/follow-harness-allow";
    std::remove(flag_path);
    std::FILE* flag = std::fopen(flag_path, "w");
    if (flag != nullptr) std::fclose(flag);

    setenv("TOADI_EXAMPLE_FOLLOW", "1", 1);
    setenv("TOADI_FOLLOW_PORT", "18082", 1);
    setenv("TOADI_FOLLOW_STOCK_HOST", "127.0.0.1", 1);
    setenv("TOADI_FOLLOW_STOCK_PORT", "18080", 1);
    setenv("TOADI_FOLLOW_FLAG_FILE", flag_path, 1);
    setenv("TOADI_FOLLOW_ALLOW_MOTION", "1", 1);
    setenv("TOADI_FOLLOW_TEST_SELECT_OK", "1", 1);
    unsetenv("TOADI_FOLLOW_START_TIMEOUT_MS");
    const bool smooth_mode =
        std::getenv("TOADI_FOLLOW_SMOOTH") != nullptr &&
        std::strcmp(std::getenv("TOADI_FOLLOW_SMOOTH"), "1") == 0;

    MockStock mock;
    check(mock.start(stock_port), "mock stock server started");

    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        mock.stop();
        return 1;
    }
    auto* factory_slot = static_cast<std::unique_ptr<toadi::VisionAlgoPlugin> (**)(
        toadi::Broker&)>(dlsym(handle, "createVisionAlgoPlugin"));
    check(factory_slot != nullptr && *factory_slot != nullptr,
          "createVisionAlgoPlugin alias");

    auto broker = toadi::makeBroker();
    check(broker != nullptr, "makeBroker()");
    auto plugin = factory_slot != nullptr && *factory_slot != nullptr
                      ? (*factory_slot)(*broker)
                      : nullptr;
    check(plugin != nullptr, "factory returned a plug-in");
    if (plugin) plugin->makeVisionAlgos();

    check(waitForServer(rest_port, 3000), "follow REST server on :18082");

    std::string status = httpGet(rest_port, "/status");
    check(contains(status, "\"plugin\":\"follow\""), "/status identifies follow");
    check(contains(status, "\"motion_allowed\":true"),
          "/status reports motion interlock open");
    check(contains(status, "\"port\":18082"), "/status reports REST port");

    std::string camera_bad = httpGet(rest_port, "/camera-mode?mode=bogus");
    check(contains(camera_bad, "mode must be day|twilight|night"),
          "camera-mode rejects an unknown mode");
    std::string camera_absent = httpGet(rest_port, "/camera-mode?mode=twilight");
    check(contains(camera_absent, "\"ok\":false") &&
              (contains(camera_absent, "camera mode observable unavailable") ||
               contains(camera_absent, "camera mode observable not found")),
          "camera-mode fails closed without the stock observable");

    mock.clear();
    std::string dry = httpGet(rest_port, "/follow?speed=0.2");
    check(contains(dry, "\"dry_run\":true"), "/follow defaults to dry-run");
    check(contains(dry, "object=person"), "dry-run shows stock request");
    check(contains(dry, "\"mode\":\"stock\""), "/follow default is stock mode");
    check(mock.count() == 0, "dry-run sends no stock request");

    using ParsePersonFn = int (*)(const char*, int*, int*);
    auto parse_person = reinterpret_cast<ParsePersonFn>(
        dlsym(handle, "toadi_follow_parse_person"));
    check(parse_person != nullptr, "person alarm parser exported");
    const char* alarm =
        R"({"@type":"toadi:Alarm","category":"toadi:Object","objects":[)"
        R"({"label":"person","prob":93,"roi":[255,0,105,199]}]})";
    int person_prob = -1;
    int person_roi[4] = {-1, -1, -1, -1};
    const bool parsed =
        parse_person != nullptr && parse_person(alarm, &person_prob, person_roi);
    check(parsed && person_prob == 93 && person_roi[0] == 255 &&
              person_roi[1] == 0 && person_roi[2] == 105 &&
              person_roi[3] == 199,
          "person alarm parser reads prob and roi");
    check(parse_person != nullptr &&
              parse_person(
                  R"({"objects":[{"label":"toadicharger","prob":90,)"
                  R"("roi":[1,2,3,4]}]})",
                  &person_prob, person_roi) == 0,
          "non-person labels are ignored");
    check(parse_person != nullptr &&
              parse_person(
                  R"({"objects":[{"label":"person","prob":70,"roi":[1,2,3,4]},)"
                  R"({"label":"person","prob":90,"roi":[10,20,60,80]}]})",
                  &person_prob, person_roi) == 1 &&
              person_prob == 90 && person_roi[2] == 60,
          "largest person box is selected");

    mock.clear();
    std::string visual_dry =
        httpGet(rest_port, "/follow?mode=visual&speed=0.9&seconds=20");
    check(contains(visual_dry, "\"dry_run\":true") &&
              contains(visual_dry, "\"mode\":\"visual\""),
          "visual dry-run identifies the mode");
    check(contains(visual_dry, "\"step_distance_m\":0.12") &&
              contains(visual_dry, "\"step_speed\":0.10"),
          "visual dry-run shows the near step profile");
    check(contains(visual_dry, "\"speed\":0.20") &&
              contains(visual_dry, "\"max_speed\":0.20") &&
              contains(visual_dry, "\"adaptive\":true") &&
              contains(visual_dry, "\"far_step\":0.24"),
          "visual dry-run shows the adaptive speed/step profile");
    check(contains(visual_dry, "\"far_speed\":0.20") &&
              contains(visual_dry, "\"mid_speed\":0.15") &&
              contains(visual_dry, "\"near_speed\":0.10"),
          "visual dry-run exposes all adaptive profiles");
    check(mock.count() == 0, "visual dry-run sends no stock request");

    using ProfileFn = int (*)(int, int, double, double*, double*);
    auto profile = reinterpret_cast<ProfileFn>(
        dlsym(handle, "toadi_follow_visual_profile"));
    auto near_profile = [&](int w, int h, double cap, double want_speed,
                            double want_step, const char* what) {
        double ps = 0;
        double pd = 0;
        check(profile != nullptr &&
                  profile(w, h, cap, &ps, &pd) == 1 &&
                  std::fabs(ps - want_speed) < 1e-6 &&
                  std::fabs(pd - want_step) < 1e-6,
              what);
    };
    check(profile != nullptr, "visual profile helper exported");
    near_profile(160, 280, 0.20, 0.10, 0.12, "near ROI uses 0.10/0.12");
    near_profile(120, 220, 0.20, 0.15, 0.18, "mid ROI uses 0.15/0.18");
    near_profile(60, 120, 0.20, 0.20, 0.24, "far ROI uses 0.20/0.24");
    near_profile(120, 220, 0.10, 0.10, 0.12,
                 "requested speed cap scales the profile");

    using SmoothFn = double (*)(double, double);
    auto smooth_target = reinterpret_cast<SmoothFn>(
        dlsym(handle, "toadi_follow_smooth_target"));
    auto smooth_profile = [&](double cap, double ratio, double want,
                              const char* what) {
        const double got =
            smooth_target != nullptr ? smooth_target(cap, ratio) : -1.0;
        check(std::fabs(got - want) < 1e-6, what);
    };
    check(smooth_target != nullptr, "smooth target helper exported");
    smooth_profile(0.20, 0.50, 0.20, "smooth far ROI uses full speed");
    smooth_profile(0.20, 0.82, 0.20, "smooth full-speed boundary");
    smooth_profile(0.20, 0.885, 0.10, "smooth half-range deceleration");
    smooth_profile(0.20, 0.95, 0.00, "smooth stand-off reaches zero");
    smooth_profile(0.10, 0.885, 0.05, "smooth target respects speed cap");

    mock.clear();
    std::string visual =
        httpGet(rest_port, "/follow?mode=visual&seconds=20&confirm=1");
    check(contains(visual, "\"accepted\":true") &&
              contains(visual, "\"mode\":\"visual\""),
          "confirmed visual follow is accepted");
    std::string visual_status;
    for (int i = 0; i < 100; ++i) {
        visual_status = httpGet(rest_port, "/status");
        if (contains(visual_status, "\"phase\":\"error\"")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(contains(visual_status, "\"phase\":\"error\""),
          "visual mode fails closed without an observation stream");
    const std::vector<std::string> visual_paths = mock.paths();
    check(hasPath(visual_paths, "/api/activities/info") &&
              hasPath(visual_paths, "/navigation/startmanualdriving"),
          "visual preflight starts manual driving");
    check(hasPath(visual_paths, "/navigation/stop") &&
              hasPath(visual_paths, "/navigation/stopmanualdriving"),
          "visual failure stops motion and ends its manual session");
    mock.setActivityJson(
        "{\"scheduledActivity\":\"\",\"userActivity\":\"\"}");

    mock.clear();
    mock.setWsEnabled(true);
    mock.setActivityJson(
        "{\"scheduledActivity\":\"\",\"userActivity\":\"manualdriving\"}");
    std::string visual_live =
        httpGet(rest_port, "/follow?mode=visual&speed=0.20&seconds=20&confirm=1");
    check(contains(visual_live, "\"accepted\":true"),
          "visual follow starts with a live observation stream");
    bool saw_following = false;
    bool saw_forward = false;
    bool saw_smooth_forward = false;
    for (int i = 0; i < 120; ++i) {
        const std::string st = httpGet(rest_port, "/status");
        if (contains(st, "\"phase\":\"following\"")) saw_following = true;
        for (const auto& path : mock.paths()) {
            if (path.rfind("/navigation/forward?speed=", 0) != 0) continue;
            if (!smooth_mode && path.rfind("/navigation/forward?speed=0.15"
                                           "&distance=0.18",
                                           0) == 0) {
                saw_forward = true;
            }
            if (smooth_mode && contains(path, "&distance=0.50&turnRadius=")) {
                saw_smooth_forward = true;
            }
        }
        if (saw_following &&
            (smooth_mode ? saw_smooth_forward : saw_forward)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(saw_following, "visual loop locks onto a live person ROI");
    check(smooth_mode ? saw_smooth_forward : saw_forward,
          smooth_mode ? "smooth loop streams a turn-radius forward command"
                      : "visual loop sends a bounded forward step");
    long long forward_client_id = 0;
    for (const auto& path : mock.paths()) {
        if (path.rfind("/navigation/forward", 0) != 0) continue;
        const size_t at = path.find("clientId=");
        if (at != std::string::npos) {
            forward_client_id = std::strtoll(path.c_str() + at + 9, nullptr, 10);
        }
    }
    check(forward_client_id > 0 && forward_client_id <= 2147483647LL,
          "visual forward uses a stock-safe 32-bit clientId");
    const std::string live_status = httpGet(rest_port, "/status");
    check(contains(live_status, "\"person_prob\":95"),
          "status reports the live person probability");
    check(contains(live_status, "\"person_roi\":[250,0,120,220]"),
          "status reports the live ROI");
    if (smooth_mode) {
        check(contains(live_status, "\"smooth_enabled\":true") &&
                  contains(live_status, "\"smooth_active\":true") &&
                  contains(live_status, "\"smooth_speed\":"),
              "status reports the smooth controller");
    }
    mock.clear();
    std::string live_stop = httpGet(rest_port, "/stop");
    check(contains(live_stop, "\"stopped\":true"), "visual /stop reports stopped");
    check(hasPath(mock.paths(), "/navigation/stop"),
          "visual /stop stops motion");
    check(!hasPath(mock.paths(), "/navigation/stopmanualdriving"),
          "visual /stop leaves pre-existing manual driving alone");
    mock.setWsEnabled(false);
    mock.setActivityJson(
        "{\"scheduledActivity\":\"\",\"userActivity\":\"\"}");

    mock.clear();
    std::string drive =
        httpGet(rest_port, "/follow?speed=0.9&seconds=5&confirm=1");
    check(contains(drive, "\"accepted\":true"), "confirmed /follow accepted");
    check(contains(drive, "\"starting\":true"),
          "confirmed /follow returns immediately as starting");
    for (int i = 0; i < 30 && mock.count() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(mock.count() >= 3, "confirmed /follow ran the stock preflight");
    const std::vector<std::string> paths = mock.paths();
    check(hasPath(paths, "/api/activities/info"),
          "preflight reads the activity state");
    check(hasPath(paths, "/navigation/startmanualdriving"),
          "preflight starts manual driving when idle");
    check(hasPath(paths, "/navigation/autopilot?speed=0.20&object=person"),
          "stock request is speed-clamped follow autopilot");
    for (int i = 0; i < 20; ++i) {
        const std::string st = httpGet(rest_port, "/status");
        if (contains(st, "\"phase\":\"following\"")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(contains(httpGet(rest_port, "/status"), "\"phase\":\"following\""),
          "status reports following after the stock response");

    mock.clear();
    std::string stop = httpGet(rest_port, "/stop");
    check(contains(stop, "\"stopped\":true"), "/stop reports stopped");
    const std::vector<std::string> stop_paths = mock.paths();
    check(hasPath(stop_paths, "/navigation/stop"),
          "/stop forwards to stock /navigation/stop");
    check(hasPath(stop_paths, "/navigation/stopmanualdriving"),
          "/stop ends manual driving started by the plug-in");

    mock.clear();
    mock.setAutopilotDelay(1500);
    setenv("TOADI_FOLLOW_START_TIMEOUT_MS", "800", 1);
    std::string slow =
        httpGet(rest_port, "/follow?speed=0.2&seconds=30&confirm=1");
    check(contains(slow, "\"starting\":true"),
          "slow start is accepted asynchronously");
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    mock.setAutopilotDelay(0);
    unsetenv("TOADI_FOLLOW_START_TIMEOUT_MS");
    const std::string slow_status = httpGet(rest_port, "/status");
    check(contains(slow_status, "\"phase\":\"error\""),
          "slow start is marked as an error");
    check(contains(slow_status, "\"busy\":false"),
          "slow start failure clears busy");
    const std::vector<std::string> slow_paths = mock.paths();
    check(hasPath(slow_paths, "/navigation/autopilot") &&
              hasPath(slow_paths, "/navigation/stop"),
          "slow start is followed by stock stop");
    check(hasPath(slow_paths, "/navigation/stopmanualdriving"),
          "slow start failure also ends manual driving");

    mock.clear();
    std::string drive2 =
        httpGet(rest_port, "/follow?speed=0.05&seconds=5&confirm=1");
    check(contains(drive2, "\"starting\":true"),
          "second confirmed /follow accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(6500));
    const std::vector<std::string> auto_paths = mock.paths();
    check(hasPath(auto_paths, "/navigation/autopilot") &&
              hasPath(auto_paths, "/navigation/stop"),
          "auto-stop forwarded after the deadline");

    mock.clear();
    mock.setActivityJson(
        "{\"scheduledActivity\":\"\",\"userActivity\":\"mowing\"}");
    std::string busy = httpGet(rest_port, "/follow?speed=0.2&confirm=1");
    check(contains(busy, "\"ok\":false") &&
              contains(busy, "user activity active"),
          "another user activity is refused");
    check(mock.count() == 1 && hasPath(mock.paths(), "/api/activities/info"),
          "refused follow only reads the activity state");

    mock.clear();
    mock.setActivityJson(
        "{\"scheduledActivity\":\"\",\"userActivity\":\"manualdriving\"}");
    std::string existing =
        httpGet(rest_port, "/follow?speed=0.2&seconds=5&confirm=1");
    check(contains(existing, "\"starting\":true"),
          "follow accepted with manual driving already active");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const std::vector<std::string> existing_paths = mock.paths();
    check(hasPath(existing_paths, "/navigation/autopilot") &&
              !hasPath(existing_paths, "/navigation/startmanualdriving"),
          "existing manual driving is reused, not restarted");
    mock.clear();
    httpGet(rest_port, "/stop");
    check(!hasPath(mock.paths(), "/navigation/stopmanualdriving"),
          "stop leaves pre-existing manual driving alone");
    mock.setActivityJson(
        "{\"scheduledActivity\":\"\",\"userActivity\":\"\"}");

    std::remove(flag_path);
    mock.clear();
    std::string refused =
        httpGet(rest_port, "/follow?speed=0.2&confirm=1");
    check(contains(refused, "motion interlock"), "flag removal closes interlock");
    check(mock.count() == 0, "interlocked /follow sends no stock request");
    std::string status2 = httpGet(rest_port, "/status");
    check(contains(status2, "\"motion_allowed\":false"),
          "/status reports interlock closed");

    mock.stop();
    std::printf("%s (%d failure%s)\n", failures ? "FAIL" : "ALL PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
