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
#include <exception>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <array>
#include <cstring>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <unistd.h>
#include <ctime>

namespace util {

/**
 * @brief Log levels used by Logger.
 */
enum class LogLevel : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace
};

/**
 * @brief Thread-safe logger with level control and load-based skipping.
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
    void setLevel(LogLevel lvl) noexcept { level_ = lvl; }
    LogLevel level() const noexcept { return level_; }
    void setHighLoad(bool val) noexcept { highLoad_ = val; }

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

/**
 * @brief Accessor for singleton Logger instance.
 */
inline Logger& gLogger() {
    static Logger instance;
    return instance;
}

}  // namespace util

namespace concurrent {

/**
 * @brief Single-producer single-consumer lock-free circular queue.
 *        Capacity must be power of two.
 * @tparam T Type to store.
 * @tparam N Buffer Size (power of 2).
 */
template<typename T, size_t N>
class LockFreeQueue {
    static_assert((N & (N - 1)) == 0 && N != 0, "N must be power of two");
    std::array<T, N> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool enqueue(const T& item) noexcept {
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

    std::optional<T> dequeue() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            // queue empty
            return std::nullopt;
        }
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

/**
 * @brief Sensor event struct.
 */
struct SensorEvent {
    SensorId sensor;
    SensorType type;
    bool activated;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief GPIOManager manages sensor event queue and debounce filtering.
 */
class GPIOManager {
    concurrent::LockFreeQueue<SensorEvent, 256> eventQueue_;
    std::unordered_map<SensorId, std::chrono::steady_clock::time_point> lastActivation_;
    static constexpr std::chrono::milliseconds debounceDuration{50};

public:
    /**
     * @brief Enqueue a sensor event if it passes debounce.
     * @param e SensorEvent to enqueue.
     * @return true if event enqueued, false if debounced.
     */
    bool enqueueEvent(const SensorEvent& e) {
        auto now = std::chrono::steady_clock::now();
        if (auto lastIt = lastActivation_.find(e.sensor);
            lastIt != lastActivation_.end() && (now - lastIt->second < debounceDuration)) {
            return false; // debounce filter rejected
        }
        lastActivation_[e.sensor] = now;
        return eventQueue_.enqueue(e);
    }

    /**
     * @brief Dequeue next pending sensor event if available.
     * @return Optional sensor event.
     */
    std::optional<SensorEvent> getNextEvent() noexcept {
        return eventQueue_.dequeue();
    }
};

/**
 * @brief Accessor to singleton GPIOManager.
 */
inline GPIOManager& GetGPIOManager() {
    static GPIOManager instance;
    return instance;
}

} // namespace gpio

namespace persistence {

/**
 * @brief Base exception class for MMapFile errors.
 */
class MMapFileException : public std::runtime_error {
public:
    explicit MMapFileException(const std::string& message)
        : std::runtime_error(message) {}
};

class FileOpenException : public MMapFileException {
public:
    explicit FileOpenException(const std::string& filepath)
        : MMapFileException("Failed to open file: " + filepath) {}
};

class FileTruncateException : public MMapFileException {
public:
    explicit FileTruncateException(const std::string& filepath)
        : MMapFileException("Failed to truncate file: " + filepath) {}
};

class MMapException : public MMapFileException {
public:
    explicit MMapException(const std::string& filepath)
        : MMapFileException("Memory mapping failed for file: " + filepath) {}
};

/**
 * @brief RAII wrapper managing memory-mapped file resource.
 */
class MMapFile {
    int fd_{-1};
    void* addr_{nullptr};
    size_t length_{0};
    std::string filepath_;

public:
    MMapFile(const char* filename, size_t length)
        : length_(length)
        , filepath_(filename) {
        fd_ = open(filename, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            throw FileOpenException(filepath_);
        }

        if (ftruncate(fd_, length) < 0) {
            close(fd_);
            throw FileTruncateException(filepath_);
        }

        addr_ = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr_ == MAP_FAILED) {
            close(fd_);
            throw MMapException(filepath_);
        }
    }

    // Delete copy ctor and copy assignment (unique resource ownership)
    MMapFile(const MMapFile&) = delete;
    MMapFile& operator=(const MMapFile&) = delete;

    // Move ctor and assignment transfer ownership
    MMapFile(MMapFile&& other) noexcept
        : fd_(other.fd_), addr_(other.addr_), length_(other.length_), filepath_(std::move(other.filepath_)) {
        other.fd_ = -1;
        other.addr_ = nullptr;
        other.length_ = 0;
        other.filepath_.clear();
    }

    MMapFile& operator=(MMapFile&& other) noexcept {
        if (this != &other) {
            if (addr_ && addr_ != MAP_FAILED) munmap(addr_, length_);
            if (fd_ >= 0) close(fd_);

            fd_ = other.fd_;
            addr_ = other.addr_;
            length_ = other.length_;
            filepath_ = std::move(other.filepath_);

            other.fd_ = -1;
            other.addr_ = nullptr;
            other.length_ = 0;
            other.filepath_.clear();
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

    template<typename T>
    T* data() noexcept {
        return static_cast<T*>(addr_);
    }

    size_t size() const noexcept { return length_; }
};

} // namespace persistence

namespace alarm {

using namespace std::chrono_literals;

enum class AlarmState {
    Disarmed,
    Armed,
    Triggered
};

/**
 * @brief Core alarm controller managing armed/disarmed/triggered states.
 */
class AlarmController {
    mutable std::mutex mutex_;
    AlarmState state_{AlarmState::Disarmed};
    std::map<gpio::SensorId, bool> sensorStatus_;
    std::chrono::steady_clock::time_point triggeredAt_{};

    static constexpr std::string_view adminPin_{"1234"};

public:
    bool arm(std::string_view pin) {
        std::lock_guard lock(mutex_);
        if (pin != adminPin_) return false;
        if (state_ == AlarmState::Disarmed) {
            sensorStatus_.clear();
            state_ = AlarmState::Armed;
            util::gLogger().log(util::LogLevel::Info, "Alarm armed.");
            return true;
        }
        return false;
    }

    bool disarm(std::string_view pin) {
        std::lock_guard lock(mutex_);
        if (pin != adminPin_) return false;
        if (state_ != AlarmState::Disarmed) {
            sensorStatus_.clear();
            state_ = AlarmState::Disarmed;
            util::gLogger().log(util::LogLevel::Info, "Alarm disarmed.");
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
        if(state_ != AlarmState::Armed) return;
        if(event.activated) {
            sensorStatus_[event.sensor] = true;
            triggeredAt_ = std::chrono::steady_clock::now();
            state_ = AlarmState::Triggered;
            util::gLogger().log(util::LogLevel::Warn,
                "Alarm triggered by sensor ",
                event.sensor,
                " type ",
                static_cast<int>(event.type));
        }
    }

    std::map<gpio::SensorId, bool> getSensorStatus() const {
        std::lock_guard lock(mutex_);
        return sensorStatus_;
    }
};

}  // namespace alarm

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
 * @brief Dynamic plugin loader and manager.
 */
class PluginManager {
    void* handle_{nullptr};
    IAlertPlugin* plugin_{nullptr};
    plugin_destroy_t destroy_{nullptr};

public:
    PluginManager() = default;

    // Delete copy ctor and assignment (unique resource ownership)
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Move ctor and assignment transfer ownership
    PluginManager(PluginManager&& other) noexcept
        : handle_(other.handle_), plugin_(other.plugin_), destroy_(other.destroy_) {
        other.handle_ = nullptr;
        other.plugin_ = nullptr;
        other.destroy_ = nullptr;
    }

    PluginManager& operator=(PluginManager&& other) noexcept {
        if (this != &other) {
            unload();
            handle_ = other.handle_;
            plugin_ = other.plugin_;
            destroy_ = other.destroy_;
            other.handle_ = nullptr;
            other.plugin_ = nullptr;
            other.destroy_ = nullptr;
        }
        return *this;
    }

    bool load(const std::string& path) {
        if(handle_) return false;

        handle_ = dlopen(path.c_str(), RTLD_NOW);
        if (!handle_) {
            util::gLogger().log(util::LogLevel::Error, "Failed to load plugin: ", dlerror());
            return false;
        }
        auto create = reinterpret_cast<plugin_create_t>(dlsym(handle_, "create_plugin"));
        if (!create) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger().log(util::LogLevel::Error, "Plugin: create_plugin symbol missing");
            return false;
        }
        destroy_ = reinterpret_cast<plugin_destroy_t>(dlsym(handle_, "destroy_plugin"));
        if (!destroy_) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger().log(util::LogLevel::Error, "Plugin: destroy_plugin symbol missing");
            return false;
        }
        plugin_ = create();
        plugin_->initialize();
        util::gLogger().log(util::LogLevel::Info, "Plugin loaded: ", path);
        return true;
    }

    void unload() noexcept {
        if (plugin_) {
            plugin_->shutdown();
            destroy_(plugin_);
            plugin_ = nullptr;
        }
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger().log(util::LogLevel::Info, "Plugin unloaded");
        }
        destroy_ = nullptr;
    }

    bool isLoaded() const noexcept {
        return plugin_ != nullptr;
    }

    void alert(std::string_view message) noexcept {
        if (plugin_) {
            try {
                plugin_->alert(message);
            }
            catch (...) {
                util::gLogger().log(util::LogLevel::Error, "Alert plugin threw exception");
            }
        }
    }

    ~PluginManager() {
        unload();
    }
};

}  // namespace plugin

namespace http {

/**
 * @brief Minimal HTTP server serving a /status endpoint.
 */
class HttpServer {
    int serverFd_{-1};
    int port_;
    std::atomic<bool> running_{false};
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
            ssize_t count = read(clientFd, buf.data(), static_cast<size_t>(buf.size() - 1));
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
        util::gLogger().log(util::LogLevel::Info, "HTTP server started on port ", port_);
        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            shutdown(serverFd_, SHUT_RDWR);
            close(serverFd_);
            if (serverThread_.joinable()) serverThread_.join();
            util::gLogger().log(util::LogLevel::Info, "HTTP server stopped");
        }
    }

    ~HttpServer() { stop(); }
};

}  // namespace http

namespace admin {

/**
 * @brief Role-based admin console via stdin.
 */
class AdminConsole {
    alarm::AlarmController& alarm_;
    plugin::PluginManager& pluginManager_;
    std::atomic<bool> running_{false};
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

    // Refactored command handlers to reduce complexity
    void printHelp() const {
        std::cout << "Commands:\n"
                  << "  arm <pin>          : arm the alarm\n"
                  << "  disarm <pin>       : disarm the alarm\n"
                  << "  status             : show current status\n"
                  << "  loadplugin <path>  : load alert plugin\n"
                  << "  unloadplugin       : unload alert plugin\n"
                  << "  quit               : exit console\n";
    }

    void handleArm(const std::string& pin) {
        if (alarm_.arm(pin)) {
            std::cout << "Alarm armed\n";
        } else {
            std::cout << "Failed to arm alarm\n";
        }
    }

    void handleDisarm(const std::string& pin) {
        if (alarm_.disarm(pin)) {
            std::cout << "Alarm disarmed\n";
        } else {
            std::cout << "Failed to disarm alarm\n";
        }
    }

    void printStatus() {
        auto state = alarm_.getState();
        std::string stateStr = (state == alarm::AlarmState::Armed) ? "Armed" :
                               (state == alarm::AlarmState::Disarmed) ? "Disarmed" : "Triggered";
        std::cout << "Alarm state: " << stateStr << "\n";

        auto sensors = alarm_.getSensorStatus();
        for (const auto& [id, active] : sensors) {
            std::cout << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        }
    }

    void handleLoadPlugin(const std::string& path) {
        if (pluginManager_.load(path)) {
            std::cout << "Plugin loaded\n";
        } else {
            std::cout << "Plugin loading failed\n";
        }
    }

    void handleUnloadPlugin() {
        pluginManager_.unload();
        std::cout << "Plugin unloaded\n";
    }

    void run() {
        util::gLogger().log(util::LogLevel::Info, "Admin Console started. Type 'help'.");

        while (running_) {
            std::cout << "> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) break;

            auto tokens = split(line);
            if (tokens.empty()) continue;

            const auto& cmd = tokens[0];
            if (cmd == "help") {
                printHelp();
            } else if (cmd == "arm" && tokens.size() == 2) {
                handleArm(tokens[1]);
            } else if (cmd == "disarm" && tokens.size() == 2) {
                handleDisarm(tokens[1]);
            } else if (cmd == "status") {
                printStatus();
            } else if (cmd == "loadplugin" && tokens.size() == 2) {
                handleLoadPlugin(tokens[1]);
            } else if (cmd == "unloadplugin") {
                handleUnloadPlugin();
            } else if (cmd == "quit") {
                running_ = false;
            } else {
                std::cout << "Unknown command\n";
            }
        }

        util::gLogger().log(util::LogLevel::Info, "Admin Console terminated.");
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

int main() {
    util::gLogger().setLevel(util::LogLevel::Debug);

    alarm::AlarmController alarmController;
    plugin::PluginManager pluginManager;

    http::HttpServer httpServer(8080, [&alarmController]() {
        static const std::map<alarm::AlarmState, std::string> stateNames{
            {alarm::AlarmState::Disarmed, "Disarmed"},
            {alarm::AlarmState::Armed,    "Armed"},
            {alarm::AlarmState::Triggered,"Triggered"}};
        std::stringstream ss;
        auto st = alarmController.getState();
        ss << "AlarmState: " << stateNames.at(st) << "\n";
        auto sensors = alarmController.getSensorStatus();
        for (const auto& [id, active] : sensors)
            ss << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        return ss.str();
    });

    if (!httpServer.start()) {
        util::gLogger().log(util::LogLevel::Error, "Failed to start HTTP server");
        return 1;
    }

    admin::AdminConsole console(alarmController, pluginManager);
    console.start();

    std::thread gpioThread([]() {
        auto& gpioMgr = gpio::GetGPIOManager();
        std::array<gpio::SensorType, 3> types{gpio::SensorType::Door, gpio::SensorType::Motion, gpio::SensorType::Smoke};
        gpio::SensorId sensorId = 1;
        size_t typeIdx = 0;

        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, 4);

        while (true) {
            bool activate = (dist(rng) == 0);
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

    while (true) {
        if (auto evt = gpioManager.getNextEvent()) {
            alarmController.processSensorEvent(*evt);
            if (pluginManager.isLoaded()) {
                pluginManager.alert("Alarm event triggered");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    gpioThread.join();
    console.stop();
    httpServer.stop();

    return 0;
}



