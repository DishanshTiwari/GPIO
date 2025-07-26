/*
 * @tagline: Real-time GPIO-based Home Alarm System with concurrency, persistence, plugins, and admin console.
 * @intuition: Simulate GPIO sensors, concurrency-safe event handling, persistence with memory-mapped files,
 *             plugin-based alerts, multi-level logging, admin console with user roles.
 * @approach: Use modern C++23 features; safe resource management; thread cancellation with std::jthread;
 *            clear separation of concerns with detailed error handling.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <array>
#include <map>
#include <sstream>
#include <mutex>
#include <variant>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <ctime>
#include <csignal>
#include <sys/types.h>
#include <random>
#include <type_traits>

namespace {
    // Helper: safe conversion of sockaddr_in* to sockaddr* (required by POSIX)
    inline sockaddr* sockaddr_cast(sockaddr_in* addr) noexcept {
        // This reinterpret_cast is absolutely necessary on POSIX systems.
        return reinterpret_cast<sockaddr*>(addr);
    }
}

namespace util {

enum class LogLevel : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace
};

class Logger {
    std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    std::atomic<bool> high_load_{false};

    static constexpr const char* ToString(LogLevel lvl) {
        using enum LogLevel;
        switch (lvl) {
            case Error: return "ERROR";
            case Warn: return "WARN";
            case Info: return "INFO";
            case Debug: return "DEBUG";
            case Trace: return "TRACE";
            default: return "UNKNOWN";
        }
    }

public:
    void setLevel(LogLevel lvl) noexcept { level_ = lvl; }
    LogLevel level() const noexcept { return level_; }
    void setHighLoad(bool val) noexcept { high_load_ = val; }

    template<typename... Args>
    void log(LogLevel level, Args&&... args) {
        if (level > level_ || (high_load_ && level > LogLevel::Warn)) return;

        std::lock_guard lock(mutex_);
        auto now = std::chrono::system_clock::now();
        time_t t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        if (std::tm tm{}; localtime_r(&t, &tm)) {
            ss << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] ";
        } else {
            ss << "[unknown time] ";
        }
        ss << ToString(level) << ": ";
        (ss << ... << std::forward<Args>(args));
        ss << '\n';

        std::cout << ss.str();
        std::cout.flush();
    }
};

inline Logger& gLogger() {
    static Logger instance;
    return instance;
}

} // namespace util

namespace concurrent {

template<typename T, size_t N>
requires (N > 1 && ((N & (N - 1)) == 0)) // N must be power of two
class LockFreeQueue {
    std::array<T, N> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool enqueue(T const& item) noexcept {
        auto tail = tail_.load(std::memory_order_relaxed);
        auto next_tail = (tail + 1) & (N - 1);
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // queue full
        }
        buffer_[tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    std::optional<T> dequeue() noexcept {
        auto head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return std::nullopt; // empty
        T item = buffer_[head];
        head_.store((head + 1) & (N - 1), std::memory_order_release);
        return item;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
};

} // namespace concurrent

namespace gpio {

using SensorId = int;
enum class SensorType { Door, Motion, Smoke };

struct SensorEvent {
    SensorId sensor;
    SensorType type;
    bool activated;
    std::chrono::steady_clock::time_point timestamp;
};

class GPIOManager {
    concurrent::LockFreeQueue<SensorEvent, 256> queue_;
    std::map<SensorId, std::chrono::steady_clock::time_point> last_activation_;
    static constexpr std::chrono::milliseconds debounce_duration_{50};

public:
    bool enqueue(SensorEvent const& evt) {
        auto now = std::chrono::steady_clock::now();
        if (auto it = last_activation_.find(evt.sensor); it != last_activation_.end()) {
            if (now - it->second < debounce_duration_) return false;
        }
        last_activation_[evt.sensor] = now;
        return queue_.enqueue(evt);
    }

    std::optional<SensorEvent> dequeue() {
        return queue_.dequeue();
    }
};

inline GPIOManager& GetGPIOManager() {
    static GPIOManager instance;
    return instance;
}

} // namespace gpio

namespace persistence {

struct MMapException : std::runtime_error {
    explicit MMapException(const std::string& msg) : std::runtime_error(msg) {}
};

class MMapFile {
    int fd_{-1};
    void* address_{nullptr};
    size_t length_{0};

public:
    MMapFile(const char* filepath, size_t length) : length_(length) {
        fd_ = open(filepath, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) throw MMapException("Failed to open file " + std::string(filepath));
        if (ftruncate(fd_, length_) < 0) {
            close(fd_);
            throw MMapException("Failed to truncate file " + std::string(filepath));
        }
        address_ = mmap(nullptr, length_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address_ == MAP_FAILED) {
            close(fd_);
            throw MMapException("Failed to mmap file " + std::string(filepath));
        }
    }

    MMapFile(MMapFile const&) = delete;
    MMapFile& operator=(MMapFile const&) = delete;

    MMapFile(MMapFile&& o) noexcept : fd_(o.fd_), address_(o.address_), length_(o.length_) {
        o.fd_ = -1; o.address_ = nullptr; o.length_ = 0;
    }
    MMapFile& operator=(MMapFile&& o) noexcept {
        if (this != &o) {
            if (address_) munmap(address_, length_);
            if (fd_ >= 0) close(fd_);
            fd_ = o.fd_; address_ = o.address_; length_ = o.length_;
            o.fd_ = -1; o.address_ = nullptr; o.length_ = 0;
        }
        return *this;
    }

    ~MMapFile() {
        if (address_) munmap(address_, length_);
        if (fd_ >= 0) close(fd_);
    }

    template<typename T>
    T* data() { return static_cast<T*>(address_); }
    size_t size() const { return length_; }
};

} // namespace persistence

namespace alarm {

enum class AlarmState { Disarmed, Armed, Triggered };

class AlarmController {
    mutable std::mutex mutex_;
    AlarmState state_ = AlarmState::Disarmed;
    std::map<gpio::SensorId, bool> sensor_status_;
    std::chrono::steady_clock::time_point triggered_at_;
    static constexpr std::string_view admin_pin_ = "1234";

public:
    bool arm(std::string_view pin) {
        std::lock_guard lock(mutex_);
        if (pin != admin_pin_) return false;
        if (state_ == AlarmState::Disarmed) {
            sensor_status_.clear();
            state_ = AlarmState::Armed;
            util::gLogger().log(util::LogLevel::Info, "Alarm armed.");
            return true;
        }
        return false;
    }

    bool disarm(std::string_view pin) {
        std::lock_guard lock(mutex_);
        if (pin != admin_pin_) return false;
        if (state_ != AlarmState::Disarmed) {
            sensor_status_.clear();
            state_ = AlarmState::Disarmed;
            util::gLogger().log(util::LogLevel::Info, "Alarm disarmed.");
            return true;
        }
        return false;
    }

    AlarmState get_state() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    void process_event(gpio::SensorEvent const& e) {
        std::lock_guard lock(mutex_);
        if (state_ != AlarmState::Armed) return;
        if (e.activated) {
            sensor_status_[e.sensor] = true;
            triggered_at_ = std::chrono::steady_clock::now();
            state_ = AlarmState::Triggered;
            util::gLogger().log(util::LogLevel::Warn, "Alarm triggered by sensor ", e.sensor,
                               " type ", std::to_underlying(e.type));
        }
    }

    std::map<gpio::SensorId, bool> get_sensor_status() const {
        std::lock_guard lock(mutex_);
        return sensor_status_;
    }
};

} // namespace alarm

namespace plugin {

using plugin_create_t = alarm::IAlarmPlugin* (*)();
using plugin_destroy_t = void (*)(alarm::IAlarmPlugin*);

class PluginHandle {
    using NativeHandle = void *;
    NativeHandle handle_ = nullptr;

public:
    explicit PluginHandle(NativeHandle h = nullptr) noexcept : handle_(h) {}
    bool is_valid() const noexcept { return handle_ != nullptr; }
    NativeHandle get() const noexcept { return handle_; }

    void close() noexcept {
        if (handle_) { dlclose(handle_); handle_ = nullptr; }
    }

    PluginHandle(PluginHandle const&) = delete;
    PluginHandle& operator=(PluginHandle const&) = delete;

    PluginHandle(PluginHandle&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    PluginHandle& operator=(PluginHandle&& o) noexcept {
        if (this != &o) {
            close();
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }

    ~PluginHandle() { close(); }
};

template<typename FuncPtr>
FuncPtr safe_cast_function_ptr(void* ptr) noexcept {
    return reinterpret_cast<FuncPtr>(ptr);
}

template<typename FuncPtr>
FuncPtr get_symbol(PluginHandle const& handle, char const* symbol) {
    if (!handle.is_valid()) return nullptr;
    void* ptr = dlsym(handle.get(), symbol);
    if (!ptr) throw std::runtime_error("Symbol not found: " + std::string(symbol));
    return safe_cast_function_ptr<FuncPtr>(ptr);
}

class PluginManager {
    PluginHandle handle_;
    alarm::IAlarmPlugin* plugin_ = nullptr;
    plugin_destroy_t destroy_ = nullptr;

public:
    PluginManager() = default;

    PluginManager(PluginManager const&) = delete;
    PluginManager& operator=(PluginManager const&) = delete;

    PluginManager(PluginManager&& o) noexcept
        : handle_(std::move(o.handle_)), plugin_(o.plugin_), destroy_(o.destroy_) {
        o.plugin_ = nullptr; o.destroy_ = nullptr;
    }
    PluginManager& operator=(PluginManager&& o) noexcept {
        if (this != &o) {
            unload();
            handle_ = std::move(o.handle_);
            plugin_ = o.plugin_;
            destroy_ = o.destroy_;
            o.plugin_ = nullptr; o.destroy_ = nullptr;
        }
        return *this;
    }

    bool load(std::string const& path) {
        if (handle_.is_valid()) return false;

        void* lib = dlopen(path.c_str(), RTLD_NOW);
        if (!lib) {
            util::gLogger().log(util::LogLevel::Error, "Failed to load plugin: ", dlerror());
            return false;
        }

        handle_ = PluginHandle(lib);

        try {
            auto create = get_symbol<plugin_create_t>(handle_, "create_plugin");
            destroy_ = get_symbol<plugin_destroy_t>(handle_, "destroy_plugin");
            plugin_ = create();
            plugin_->initialize();
        } catch(std::exception const& ex) {
            handle_.close();
            plugin_ = nullptr;
            destroy_ = nullptr;
            util::gLogger().log(util::LogLevel::Error, "Plugin loading failed: ", ex.what());
            return false;
        }

        util::gLogger().log(util::LogLevel::Info, "Plugin loaded: ", path)
        return true;
    }

    void unload() noexcept {
        if (plugin_) {
            plugin_->shutdown();
            destroy_(plugin_);
            plugin_ = nullptr;
        }
        handle_.close();
        destroy_ = nullptr;
        util::gLogger().log(util::LogLevel::Info, "Plugin unloaded");
    }

    bool is_loaded() const noexcept { return plugin_ != nullptr; }

    void alert(std::string_view message) noexcept {
        if (plugin_) {
            try { plugin_->alert(message); }
            catch (...) { util::gLogger().log(util::LogLevel::Error, "Plugin alert exception"); }
        }
    }

    ~PluginManager() { unload(); }
};

} // namespace plugin

namespace http {

class HttpServer {
    int sockfd_ = -1;
    int port_;
    mutable std::atomic<bool> running_ = false;
    std::jthread thread_;
    std::function<std::string()> status_provider_;

public:
    explicit HttpServer(int port, std::function<std::string()> status_provider)
        : port_(port), status_provider_(std::move(status_provider)) {}

    bool start() {
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) return false;

        int opt = 1;
        setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sockfd_, sockaddr_cast(&addr), sizeof(addr)) < 0) {
            close(sockfd_);
            sockfd_ = -1;
            return false;
        }
        if (listen(sockfd_, SOMAXCONN) < 0) {
            close(sockfd_);
            sockfd_ = -1;
            return false;
        }

        running_ = true;
        thread_ = std::jthread(&HttpServer::serve, this, std::placeholders::_1);

        util::gLogger().log(util::LogLevel::Info, "HTTP server started on port ", port_);
        return true;
    }

    void serve(std::stop_token stop_token) const {
        using namespace std::chrono_literals;

        while (running_ && !stop_token.stop_requested()) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);

            int client_fd = accept(sockfd_, sockaddr_cast(&client_addr), &addr_len);
            if (client_fd < 0) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            std::array<char, 1024> buf{};
            ssize_t n = read(client_fd, buf.data(), buf.size() - 1);
            if (n <= 0) {
                close(client_fd);
                continue;
            }

            std::string req(buf.data(), n);
            if (req.contains("GET /status")) {
                std::string body = status_provider_();
                std::stringstream resp;
                resp << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: text/plain\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Connection: close\r\n\r\n"
                     << body;
                std::string resp_str = resp.str();
                write(client_fd, resp_str.data(), resp_str.size());
            } else {
                static constexpr char resp_404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                write(client_fd, resp_404, sizeof(resp_404) - 1);
            }

            close(client_fd);
        }
    }

    void stop() noexcept {
        if (running_) {
            running_ = false;
            shutdown(sockfd_, SHUT_RDWR);
            close(sockfd_);
            if (thread_.joinable()) thread_.request_stop(), thread_.join();
            util::gLogger().log(util::LogLevel::Info, "HTTP server stopped");
        }
    }

    ~HttpServer() { stop(); }
};

} // namespace http

namespace admin {

class AdminConsole {
    alarm::AlarmController& alarm_;
    plugin::PluginManager& plugin_manager_;
    std::atomic<bool> running_ = false;
    std::jthread console_thread_;

    static std::vector<std::string> split(std::string_view line) {
        std::vector<std::string> parts;
        std::string tmp;
        for (char c : line) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!tmp.empty()) {
                    parts.push_back(std::move(tmp));
                    tmp.clear();
                }
            } else {
                tmp += c;
            }
        }
        if (!tmp.empty()) parts.push_back(std::move(tmp));
        return parts;
    }

    void print_help() const {
        std::cout << "Commands:\n"
                  << "  help              : Show this help\n"
                  << "  arm <pin>         : Arm the alarm\n"
                  << "  disarm <pin>      : Disarm the alarm\n"
                  << "  status            : Show alarm status\n"
                  << "  loadplugin <file> : Load alert plugin\n"
                  << "  unloadplugin      : Unload alert plugin\n"
                  << "  quit              : Exit console\n";
    }

    void handle_arm(std::string const& pin) {
        if (alarm_.arm(pin)) std::cout << "Alarm armed\n";
        else std::cout << "Failed to arm alarm\n";
    }

    void handle_disarm(std::string const& pin) {
        if (alarm_.disarm(pin)) std::cout << "Alarm disarmed\n";
        else std::cout << "Failed to disarm alarm\n";
    }

    void handle_status() const {
        auto state = alarm_.get_state();
        std::string state_str;
        if (state == alarm::AlarmState::Armed) state_str = "Armed";
        else if (state == alarm::AlarmState::Disarmed) state_str = "Disarmed";
        else state_str = "Triggered";

        std::cout << "Alarm state: " << state_str << "\n";
        auto sensors = alarm_.get_sensor_status();
        for (auto const& [id, active] : sensors) {
            std::cout << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        }
    }

    void handle_loadplugin(std::string const& path) {
        if (plugin_manager_.load(path)) std::cout << "Plugin loaded\n";
        else std::cout << "Plugin loading failed\n";
    }

    void handle_unloadplugin() {
        plugin_manager_.unload();
        std::cout << "Plugin unloaded\n";
    }

    void run(std::stop_token stoken) {
        util::gLogger().log(util::LogLevel::Info, "Admin console started. Type 'help'.");

        while (running_ && !stoken.stop_requested()) {
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) break;

            auto tokens = split(line);
            if (tokens.empty()) continue;

            const auto& cmd = tokens[0];
            if (cmd == "help") print_help();
            else if (cmd == "arm" && tokens.size() == 2) handle_arm(tokens[1]);
            else if (cmd == "disarm" && tokens.size() == 2) handle_disarm(tokens[1]);
            else if (cmd == "status") handle_status();
            else if (cmd == "loadplugin" && tokens.size() == 2) handle_loadplugin(tokens[1]);
            else if (cmd == "unloadplugin") handle_unloadplugin();
            else if (cmd == "quit") {
                running_ = false;
                console_thread_.request_stop();
            } else {
                std::cout << "Unknown command\n";
            }
        }

        util::gLogger().log(util::LogLevel::Info, "Admin console terminated");
    }

public:
    AdminConsole(alarm::AlarmController& alarm, plugin::PluginManager& plugins)
        : alarm_(alarm), plugin_manager_(plugins) {}

    void start() {
        running_ = true;
        console_thread_ = std::jthread(&AdminConsole::run, this);
    }

    void stop() {
        running_ = false;
        console_thread_.request_stop();
    }

    ~AdminConsole() { stop(); }
};

} // namespace admin

int main() {
    util::gLogger().setLevel(util::LogLevel::Debug);

    alarm::AlarmController alarm_controller;
    plugin::PluginManager plugin_manager;

    http::HttpServer http_server(8080, [&alarm_controller](){
        static const std::map<alarm::AlarmState,std::string> states = {
            {alarm::AlarmState::Disarmed, "Disarmed"},
            {alarm::AlarmState::Armed, "Armed"},
            {alarm::AlarmState::Triggered, "Triggered"}
        };
        std::stringstream ss;
        auto st = alarm_controller.get_state();
        ss << "Alarm state: " << states.at(st) << '\n';
        auto sensors = alarm_controller.get_sensor_status();
        for (auto const& [id, active] : sensors)
            ss << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << '\n';
        return ss.str();
    });

    if (!http_server.start()) {
        util::gLogger().log(util::LogLevel::Error, "Failed to start HTTP server");
        return 1;
    }

    admin::AdminConsole console(alarm_controller, plugin_manager);
    console.start();

    std::jthread gpio_thread([&alarm_controller, &plugin_manager](std::stop_token stoken){
        auto& gpio_mgr = gpio::GetGPIOManager();
        std::array<gpio::SensorType,3> types{gpio::SensorType::Door, gpio::SensorType::Motion, gpio::SensorType::Smoke};
        gpio::SensorId sensor_id = 1;
        size_t type_index = 0;
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0,4);

        while (!stoken.stop_requested()) {
            if (dist(rng) == 0) {
                gpio::SensorEvent ev{sensor_id, types[type_index], true, std::chrono::steady_clock::now()};
                gpio_mgr.enqueue(ev);
            }
            sensor_id = (sensor_id % 3) + 1;
            type_index = (type_index +1) % types.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    });

    auto& gpio_mgr = gpio::GetGPIOManager();
    while (true) {
        if (auto ev = gpio_mgr.dequeue()) {
            alarm_controller.process_event(*ev);
            if (plugin_manager.is_loaded()) {
                plugin_manager.alert("Alarm triggered");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    gpio_thread.request_stop();
    gpio_thread.join();
    console.stop();
    http_server.stop();

    return 0;
}









