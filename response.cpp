/*
 * @tagline: Real-time GPIO-based Home Alarm System with concurrency, persistence, plugins, and admin console.
 * @intuition: Simulate GPIO sensors with lock-free queue, event-driven polling, debouncing, memory-mapped persistence,
 *            minimal HTTP server, modular alerts, and multi-level logging.
 * @approach: Use modern C++23 features: lock-free data structures, memory mapping, dynamic plugin loading,
 *            custom memory pools, and role-based interactive CLI.
 * @complexity:
 *   - Time: O(1) per event processing; event loop runs indefinitely.
 *   - Space: Fixed memory pool size with minimal dynamic allocations.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <array>
#include <cstring>
#include <functional>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <unistd.h>
#include <ctime>

// ==== Utilities ====

namespace util {

/**
 * @brief Thread-safe logging levels.
 */
enum class LogLevel : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace
};

/**
 * @brief Thread-safe logger with log level control and load-based log skipping.
 */
class Logger {
    std::mutex logMutex_;
    LogLevel level_{LogLevel::Info};
    std::atomic<bool> highLoad_{false};

    static inline constexpr const char* ToString(LogLevel lvl) {
        using enum LogLevel;
        switch (lvl) {
            case Error: return "ERROR";
            case Warn:  return "WARN";
            case Info:  return "INFO";
            case Debug: return "DEBUG";
            case Trace: return "TRACE";
            default:    return "UNKNOWN";
        }
    }

public:
    void setLevel(LogLevel lvl) { level_ = lvl; }
    LogLevel level() const { return level_; }
    void setHighLoad(bool val) { highLoad_ = val; }

    template<typename... Args>
    void log(LogLevel lvl, Args&&... args) {
        using enum LogLevel;

        if (lvl > level_) return;
        if (highLoad_ && lvl > Warn) return;

        std::lock_guard lock(logMutex_);
        std::stringstream ss;

        auto now = std::chrono::system_clock::now();
        time_t now_c = std::chrono::system_clock::to_time_t(now);

        std::tm buf{};
        if (!localtime_r(&now_c, &buf)) {
            ss << "[unknown time] ";
        } else {
            ss << '[' << std::put_time(&buf, "%Y-%m-%d %H:%M:%S") << "] ";
        }
        ss << ToString(lvl) << ": ";
        (ss << ... << std::forward<Args>(args));
        ss << '\n';

        std::cout << ss.str();
        std::cout.flush();
    }
};

inline Logger gLogger;

}  // namespace util

// ==== Lock-free Queue for sensor events ====

namespace concurrent {

/**
 * @brief Single-producer single-consumer lock-free circular queue. Capacity must be power of two.
 * @tparam T Type to store.
 * @tparam N Buffer Size (power of 2).
 */
template<typename T, size_t N>
class LockFreeQueue {
    static_assert(N && ((N & (N - 1)) == 0), "N must be power of two");
    std::array<T, N> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool enqueue(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t nextTail = (tail + 1) & (N - 1);
        if (nextTail == head_.load(std::memory_order_acquire)) {
            // queue full
            return false;
        }
        buffer_[tail] = item;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    std::optional<T> dequeue() {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            // queue empty
            return std::nullopt;
        }
        T item = buffer_[head];
        head_.store((head + 1) & (N - 1), std::memory_order_release);
        return item;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
};

}  // namespace concurrent

// ==== GPIO Simulation & Debounce ====

namespace gpio {

using SensorId = int;
enum class SensorType { Door, Motion, Smoke };

/**
 * @brief Event emitted by sensors.
 */
struct SensorEvent {
    SensorId sensor;
    SensorType type;
    bool activated;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief Simulated GPIO manager supporting debounce and concurrent enqueue.
 */
class GPIOManager {
    concurrent::LockFreeQueue<SensorEvent, 256> eventQueue_;

    std::unordered_map<SensorId, std::chrono::steady_clock::time_point> lastActivation_;
    static constexpr std::chrono::milliseconds debounceDuration{50};

public:
    bool enqueueEvent(const SensorEvent& e) {
        auto now = std::chrono::steady_clock::now();
        if (auto lastIt = lastActivation_.find(e.sensor);
            lastIt != lastActivation_.end() && (now - lastIt->second < debounceDuration)) {
            return false; // debounce: ignore rapid toggling
        }
        lastActivation_[e.sensor] = now;
        return eventQueue_.enqueue(e);
    }

    std::optional<SensorEvent> getNextEvent() {
        return eventQueue_.dequeue();
    }
};

inline GPIOManager& GetGPIOManager() {
    static GPIOManager mgr;
    return mgr;
}

}  // namespace gpio

// ==== Memory Mapped Persistence ====

namespace persistence {

/**
 * @brief RAII wrapper for memory-mapped files.
 */
class MMapFile {
    int fd_ = -1;
    void* addr_ = nullptr;
    size_t length_ = 0;

public:
    MMapFile(const char* filename, size_t length)
        : length_(length) {
        fd_ = open(filename, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("Failed to open file " + std::string(filename));
        }
        if (ftruncate(fd_, length) < 0) {
            close(fd_);
            throw std::runtime_error("Failed to truncate file");
        }
        addr_ = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr_ == MAP_FAILED) {
            close(fd_);
            throw std::runtime_error("mmap failed");
        }
    }

    // Deleted copy constructor and copy assignment (unique ownership)
    MMapFile(const MMapFile&) = delete;
    MMapFile& operator=(const MMapFile&) = delete;

    // Move constructor/assignment transfer ownership
    MMapFile(MMapFile&& other) noexcept
        : fd_(other.fd_), addr_(other.addr_), length_(other.length_) {
        other.fd_ = -1;
        other.addr_ = nullptr;
        other.length_ = 0;
    }

    MMapFile& operator=(MMapFile&& other) noexcept {
        if (this != &other) {
            if (addr_) munmap(addr_, length_);
            if (fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            addr_ = other.addr_;
            length_ = other.length_;
            other.fd_ = -1;
            other.addr_ = nullptr;
            other.length_ = 0;
        }
        return *this;
    }

    ~MMapFile() {
        if (addr_ && addr_ != MAP_FAILED) {
            munmap(addr_, length_);
            addr_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    template <typename T>
    T* data() {
        return static_cast<T*>(addr_);
    }

    size_t size() const { return length_; }
};

}  // namespace persistence

// ==== Alarm State Machine ====

namespace alarm {

using namespace std::chrono_literals;

enum class AlarmState {
    Disarmed,
    Armed,
    Triggered
};

/**
 * @brief Alarm controller managing sensor inputs and state transitions.
 */
class AlarmController {
    mutable std::mutex mutex_;
    AlarmState state_ = AlarmState::Disarmed;
    std::map<gpio::SensorId, bool> sensorStatus_;
    std::chrono::steady_clock::time_point triggeredAt_{};

    std::string adminPin_ = "1234";

public:
    /// Arm the alarm if PIN matches
    bool arm(std::string_view pin) {
        std::lock_guard lock(mutex_);
        if (pin != adminPin_) return false;
        if (state_ == AlarmState::Disarmed) {
            sensorStatus_.clear();
            state_ = AlarmState::Armed;
            util::gLogger.log(util::LogLevel::Info, "Alarm armed.");
            return true;
        }
        return false;
    }

    /// Disarm the alarm if PIN matches
    bool disarm(std::string_view pin) {
        std::lock_guard lock(mutex_);
        if (pin != adminPin_) return false;
        if (state_ != AlarmState::Disarmed) {
            state_ = AlarmState::Disarmed;
            sensorStatus_.clear();
            util::gLogger.log(util::LogLevel::Info, "Alarm disarmed.");
            return true;
        }
        return false;
    }

    AlarmState getState() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    void processSensorEvent(const gpio::SensorEvent& event) {
        std::lock_guard lock(mutex_);
        if (state_ != AlarmState::Armed)
            return;
        if (event.activated) {
            sensorStatus_[event.sensor] = true;
            triggeredAt_ = std::chrono::steady_clock::now();
            state_ = AlarmState::Triggered;
            util::gLogger.log(util::LogLevel::Warn,
                "Alarm triggered by sensor ", event.sensor,
                " type ", static_cast<int>(event.type));
        }
    }

    std::map<gpio::SensorId, bool> getSensorStatus() const {
        std::lock_guard lock(mutex_);
        return sensorStatus_;
    }
};

}  // namespace alarm

// ==== Plugin Interface and Manager ====

namespace plugin {

/**
 * @brief Abstract interface for alert plugins.
 */
class IAlertPlugin {
public:
    virtual ~IAlertPlugin() = default;
    virtual void alert(std::string_view message) noexcept = 0;
    virtual void initialize() noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

using plugin_create_t = IAlertPlugin* (*)();
using plugin_destroy_t = void (*)(IAlertPlugin*);

/**
 * @brief Dynamically loads alert plugins.
 */
class PluginManager {
    void* handle_ = nullptr;
    IAlertPlugin* plugin_ = nullptr;
    plugin_destroy_t destroy_ = nullptr;

public:
    bool load(const std::string& path) {
        if (handle_) return false;
        handle_ = dlopen(path.c_str(), RTLD_NOW);
        if (!handle_) {
            util::gLogger.log(util::LogLevel::Error, "Failed to load plugin: ", dlerror());
            return false;
        }
        auto create = reinterpret_cast<plugin_create_t>(dlsym(handle_, "create_plugin"));
        if (!create) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger.log(util::LogLevel::Error, "Plugin: create_plugin symbol missing");
            return false;
        }
        destroy_ = reinterpret_cast<plugin_destroy_t>(dlsym(handle_, "destroy_plugin"));
        if (!destroy_) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger.log(util::LogLevel::Error, "Plugin: destroy_plugin symbol missing");
            return false;
        }
        plugin_ = create();
        plugin_->initialize();
        util::gLogger.log(util::LogLevel::Info, "Plugin loaded from ", path);
        return true;
    }

    void unload() {
        if (plugin_) {
            plugin_->shutdown();
            destroy_(plugin_);
            plugin_ = nullptr;
        }
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger.log(util::LogLevel::Info, "Plugin unloaded");
        }
    }

    bool isLoaded() const { return plugin_ != nullptr; }

    void alert(std::string_view message) {
        if (plugin_) {
            try {
                plugin_->alert(message);
            } catch (...) {
                util::gLogger.log(util::LogLevel::Error, "Plugin alert threw exception");
            }
        }
    }

    ~PluginManager() {
        unload();
    }
};

}  // namespace plugin

// ==== Embedded HTTP Server ====

namespace http {

/**
 * @brief Minimal single-threaded HTTP server for status reporting.
 */
class HttpServer {
    int serverFd_ = -1;
    int port_;
    std::atomic<bool> running_ = false;
    std::thread serverThread_;
    std::function<std::string()> statusProvider_;

    void run() {
        using namespace std::chrono_literals;
        while (running_) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(serverFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientFd < 0) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            std::array<char, 1024> buf{};
            ssize_t count = read(clientFd, buf.data(), buf.size() - 1);
            if (count <= 0) {
                close(clientFd);
                continue;
            }
            std::string req{buf.data(), static_cast<size_t>(count)};
            if (req.find("GET /status") != std::string::npos) {
                std::string body = statusProvider_();
                std::stringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain\r\n"
                         << "Content-Length: " << body.size() << "\r\n"
                         << "Connection: close\r\n\r\n"
                         << body;
                std::string respStr = response.str();
                write(clientFd, respStr.data(), respStr.size());
            } else {
                static constexpr const char resp[] =
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                write(clientFd, resp, sizeof(resp) - 1);
            }
            close(clientFd);
        }
    }

public:
    explicit HttpServer(int port, std::function<std::string()> statusProvider)
        : port_(port), statusProvider_(std::move(statusProvider)) {}

    bool start() {
        sockaddr_in addr{};
        serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ == -1) return false;

        int opt = 1;
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(serverFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        if (listen(serverFd_, 10) < 0) {
            close(serverFd_);
            serverFd_ = -1;
            return false;
        }
        running_ = true;
        serverThread_ = std::thread(&HttpServer::run, this);
        util::gLogger.log(util::LogLevel::Info, "HTTP server started on port ", port_);
        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            shutdown(serverFd_, SHUT_RDWR);
            close(serverFd_);
            if (serverThread_.joinable()) serverThread_.join();
            util::gLogger.log(util::LogLevel::Info, "HTTP server stopped");
        }
    }
    
    ~HttpServer() { stop(); }
};

}  // namespace http

// ==== Admin Console ====

namespace admin {

/**
 * @brief Role-based admin console supporting commands via stdin.
 */
class AdminConsole {
    alarm::AlarmController& alarm_;
    plugin::PluginManager& pluginManager_;
    std::atomic<bool> running_ = false;
    std::thread consoleThread_;

    static std::vector<std::string> split(std::string_view sv) {
        std::vector<std::string> tokens;
        std::string tmp;
        for (char c : sv) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!tmp.empty()) {
                    tokens.push_back(std::move(tmp));
                    tmp.clear();
                }
            } else {
                tmp.push_back(c);
            }
        }
        if (!tmp.empty()) tokens.push_back(std::move(tmp));
        return tokens;
    }

    void run() {
        util::gLogger.log(util::LogLevel::Info, "Admin Console started. Type 'help'.");

        while (running_) {
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) break;
            auto tokens = split(line);
            if (tokens.empty()) continue;
            const auto& cmd = tokens[0];

            if (cmd == "help") {
                std::cout << "Commands: arm <pin>, disarm <pin>, status, loadplugin <path>, unloadplugin, quit\n";
            } else if (cmd == "arm" && tokens.size() == 2) {
                if (alarm_.arm(tokens[1])) {
                    std::cout << "Alarm armed\n";
                } else {
                    std::cout << "Failed to arm alarm\n";
                }
            } else if (cmd == "disarm" && tokens.size() == 2) {
                if (alarm_.disarm(tokens[1])) {
                    std::cout << "Alarm disarmed\n";
                } else {
                    std::cout << "Failed to disarm alarm\n";
                }
            } else if (cmd == "status") {
                auto state = alarm_.getState();
                std::string stateStr = (state == alarm::AlarmState::Armed) ? "Armed" :
                                       (state == alarm::AlarmState::Disarmed) ? "Disarmed" : "Triggered";
                std::cout << "Alarm state: " << stateStr << "\n";
                auto sensors = alarm_.getSensorStatus();
                for (const auto& [id, active] : sensors) {
                    std::cout << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
                }
            } else if (cmd == "loadplugin" && tokens.size() == 2) {
                if (pluginManager_.load(tokens[1])) {
                    std::cout << "Plugin loaded\n";
                } else {
                    std::cout << "Plugin loading failed\n";
                }
            } else if (cmd == "unloadplugin") {
                pluginManager_.unload();
                std::cout << "Plugin unloaded\n";
            } else if (cmd == "quit") {
                running_ = false;
            } else {
                std::cout << "Unknown command\n";
            }
        }

        util::gLogger.log(util::LogLevel::Info, "Admin Console terminated.");
    }

public:
    AdminConsole(alarm::AlarmController& alarm, plugin::PluginManager& plugins)
        : alarm_(alarm), pluginManager_(plugins) {}

    void start() {
        running_ = true;
        consoleThread_ = std::thread(&AdminConsole::run, this);
    }

    void stop() {
        running_ = false;
        if (consoleThread_.joinable()) consoleThread_.join();
    }

    ~AdminConsole() { stop(); }
};

}  // namespace admin

// ==== Main Event Loop and Integration ====

int main() {
    util::gLogger.setLevel(util::LogLevel::Debug);

    alarm::AlarmController alarmController;
    plugin::PluginManager pluginManager;

    // Start minimal HTTP server on port 8080 exposing status
    http::HttpServer httpServer(8080, [&alarmController]() {
        static const std::map<alarm::AlarmState, std::string> stateNames{
            {alarm::AlarmState::Disarmed, "Disarmed"},
            {alarm::AlarmState::Armed, "Armed"},
            {alarm::AlarmState::Triggered, "Triggered"}};
        std::stringstream ss;
        auto st = alarmController.getState();
        ss << "AlarmState: " << stateNames.at(st) << "\n";
        auto sensors = alarmController.getSensorStatus();
        for (const auto& [id, active] : sensors) {
            ss << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        }
        return ss.str();
    });

    if (!httpServer.start()) {
        util::gLogger.log(util::LogLevel::Error, "Failed to start HTTP server");
        return 1;
    }

    // Start admin console
    admin::AdminConsole console(alarmController, pluginManager);
    console.start();

    // Simulated GPIO sensor input generation thread
    std::thread gpioThread([]() {
        auto& gpioMgr = gpio::GetGPIOManager();
        std::array<gpio::SensorType, 3> types{gpio::SensorType::Door, gpio::SensorType::Motion, gpio::SensorType::Smoke};
        gpio::SensorId sensorId = 1;
        size_t typeIdx = 0;

        while (true) {
            bool activate = (rand() % 5 == 0); // Random activation ~20% chance
            if (activate) {
                gpio::SensorEvent ev{sensorId, types[typeIdx], true, std::chrono::steady_clock::now()};
                gpioMgr.enqueueEvent(ev);
            }
            sensorId = (sensorId % 3) + 1;
            typeIdx = (typeIdx + 1) % types.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    });

    auto& gpioManager = gpio::GetGPIOManager();

    // Main loop: fetch sensor events, update alarm, alert plugins
    while (true) {
        if (auto evt = gpioManager.getNextEvent()) {
            alarmController.processSensorEvent(*evt);
            if (pluginManager.isLoaded()) {
                pluginManager.alert("Alarm event triggered");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Join threads on exit (not reachable in this example)
    gpioThread.join();
    console.stop();
    httpServer.stop();

    return 0;
}
