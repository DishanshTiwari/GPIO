/*
 * @tagline: Real-time GPIO-based Home Alarm System with concurrency, persistence, plugins, and admin console.
 * @intuition: Simulate GPIO sensors with a lock-free queue, event-driven polling, debouncing, memory-mapped persistence,
 *            a minimal HTTP status server, modular alerts with plugins, and a multi-level logging system.
 * @approach: Use C++23 features, lock-free data structures, memory mapping (mmap), dynamic plugin loading with dlopen,
 *            custom memory pools, and a role-based interactive CLI.
 * @complexity:
 *   - Time: O(1) per event processing, event loop runs indefinitely with asynchronous events.
 *   - Space: Fixed memory pool size with minimal dynamic allocations.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <unistd.h>

// ==== Utilities ====

namespace util {

// Thread-safe logging levels
enum class LogLevel : int {
    Error = 0,
    Warn,
    Info,
    Debug,
    Trace
};

/**
 * @brief Thread-safe logger with log level control and skip-under-load functionality.
 */
class Logger {
    std::mutex logMutex_;
    LogLevel level_{LogLevel::Info};
    std::atomic<bool> highLoad_{false};

    static inline const char* ToString(LogLevel lvl) {
        switch (lvl) {
            case LogLevel::Error: return "ERROR";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Trace: return "TRACE";
            default:             return "UNKNOWN";
        }
    }

public:
    void setLevel(LogLevel lvl) { level_ = lvl; }
    LogLevel level() const { return level_; }
    void setHighLoad(bool val) { highLoad_ = val; }

    template<typename... Args>
    void log(LogLevel lvl, Args&&... args) {
        if (lvl > level_) return;
        // Skip non-critical logs in high load
        if (highLoad_ && lvl > LogLevel::Warn) return;

        std::lock_guard lock(logMutex_);
        std::stringstream ss;

        auto now = std::chrono::system_clock::now();
        time_t now_c = std::chrono::system_clock::to_time_t(now);
        ss << "[" << std::string(std::ctime(&now_c)).substr(0, 24) << "] "
           << ToString(lvl) << ": ";
        (ss << ... << std::forward<Args>(args));
        ss << "\n";
        std::cout << ss.str();
    }
};

inline Logger gLogger;

}  // namespace util

// ==== Lock-free Queue for sensor events ====

namespace concurrent {

/**
 * @brief Single-producer single-consumer lock-free circular queue for small capacity.
 * @tparam T Type to store.
 * @tparam N Buffer Size.
 */
template<typename T, size_t N>
class LockFreeQueue {
    static_assert(N && ((N & (N - 1)) == 0), "N must be power of two");
    T buffer_[N];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool enqueue(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t nextTail = (tail + 1) & (N - 1);
        if (nextTail == head_.load(std::memory_order_acquire)) {
            // full
            return false;
        }
        buffer_[tail] = item;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    std::optional<T> dequeue() {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            // empty
            return {};
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

struct SensorEvent {
    SensorId sensor;
    SensorType type;
    bool activated;
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief Simulated GPIO manager with polling and debounce.
 */
class GPIOManager {
    concurrent::LockFreeQueue<SensorEvent, 256> eventQueue_;

    // Last activation timestamps for debounce
    std::unordered_map<SensorId, std::chrono::steady_clock::time_point> lastActivation_;
    static constexpr auto debounceDuration = std::chrono::milliseconds(50);

public:
    bool enqueueEvent(SensorEvent const& e) {
        auto now = std::chrono::steady_clock::now();
        auto lastIt = lastActivation_.find(e.sensor);
        if (lastIt != lastActivation_.end()) {
            if (now - lastIt->second < debounceDuration) {
                return false; // debounce ignore
            }
        }
        lastActivation_[e.sensor] = now;
        return eventQueue_.enqueue(e);
    }

    std::optional<SensorEvent> getNextEvent() {
        return eventQueue_.dequeue();
    }
};

GPIOManager& GetGPIOManager() {
    static GPIOManager mgr;
    return mgr;
}

}  // namespace gpio

// ==== Memory Mapped Persistence ====

namespace persistence {

/**
 * @brief Simple RAII wrapper for memory-mapped files.
 */
class MMapFile {
    int fd_ = -1;
    void* addr_ = nullptr;
    size_t length_ = 0;

public:
    MMapFile(char const* filename, size_t length)
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

    ~MMapFile() {
        if (addr_ && addr_ != MAP_FAILED) {
            munmap(addr_, length_);
            addr_ = nullptr;
        }
        if (fd_ >= 0) close(fd_);
    }

    template <typename T>
    T* data() {
        return reinterpret_cast<T*>(addr_);
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
 * @brief Core alarm controller tracking sensor states and alarm status.
 */
class AlarmController {
    using Clock = std::chrono::steady_clock;
    AlarmState state_ = AlarmState::Disarmed;
    std::mutex mutex_;
    std::map<gpio::SensorId, bool> sensorStatus_;  // true if triggered
    std::chrono::steady_clock::time_point triggeredAt_{};

    // For role-based admin
    std::string adminPin_ = "1234";

public:
    /// Arms the alarm if PIN matches; returns true on success.
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
    /// Disarms the alarm if PIN matches; resets trigger state.
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

    [[nodiscard]] AlarmState getState() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    /// Process input event from sensor
    void processSensorEvent(gpio::SensorEvent const& event) {
        std::lock_guard lock(mutex_);
        if(state_ != AlarmState::Armed)
            return;
        if(event.activated) {
            sensorStatus_[event.sensor] = true;
            triggeredAt_ = Clock::now();
            state_ = AlarmState::Triggered;
            util::gLogger.log(util::LogLevel::Warn,
                             "Alarm triggered by sensor ", event.sensor, " type ",
                             static_cast<int>(event.type));
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
 * @brief Basic interface for alert plugins.
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
 * @brief Plugin manager dynamically loads alert modules.
 */
class PluginManager {
    void* handle_ = nullptr;
    IAlertPlugin* plugin_ = nullptr;
    plugin_destroy_t destroy_ = nullptr;

public:
    bool load(std::string const& path) {
        if (handle_) return false; // already loaded
        handle_ = dlopen(path.c_str(), RTLD_NOW);
        if (!handle_) {
            util::gLogger.log(util::LogLevel::Error, "Failed to load plugin: ", dlerror());
            return false;
        }
        auto create = reinterpret_cast<plugin_create_t>(dlsym(handle_, "create_plugin"));
        if (!create) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger.log(util::LogLevel::Error, "Plugin get create_plugin failed");
            return false;
        }
        destroy_ = reinterpret_cast<plugin_destroy_t>(dlsym(handle_, "destroy_plugin"));
        if (!destroy_) {
            dlclose(handle_);
            handle_ = nullptr;
            util::gLogger.log(util::LogLevel::Error, "Plugin get destroy_plugin failed");
            return false;
        }
        plugin_ = create();
        plugin_->initialize();
        util::gLogger.log(util::LogLevel::Info, "Plugin loaded: ", path);
        return true;
    }

    void unload() {
        if(plugin_) {
            plugin_->shutdown();
            destroy_(plugin_);
            plugin_ = nullptr;
        }
        if(handle_) {
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
                util::gLogger.log(util::LogLevel::Error, "Plugin alert call threw exception");
            }
        }
    }

    ~PluginManager() {
        unload();
    }
};

}  // namespace plugin

// ==== Memory Pool (Simple Fixed Block Pool) ====

namespace mempool {

/**
 * @brief Simple fixed-block memory pool to eliminate runtime heap allocations for fixed-size objects.
 *        Not thread-safe; usage assumed single-threaded or externally synchronized.
 */
class FixedBlockPool {
    struct Block {
        Block* next;
    };
    void* pool_;
    Block* freeList_;
    size_t blockSize_;
    size_t blockCount_;

public:
    FixedBlockPool(size_t blockSize, size_t blockCount)
        : blockSize_(blockSize), blockCount_(blockCount) {
        pool_ = std::malloc(blockSize * blockCount);
        if (!pool_) throw std::bad_alloc();

        freeList_ = static_cast<Block*>(pool_);
        auto curr = freeList_;
        for (size_t i = 1; i < blockCount; ++i) {
            curr->next = reinterpret_cast<Block*>(reinterpret_cast<char*>(pool_) + i * blockSize);
            curr = curr->next;
        }
        curr->next = nullptr;
    }

    ~FixedBlockPool() {
        std::free(pool_);
    }

    void* allocate() {
        if (!freeList_) return nullptr;
        void* ret = freeList_;
        freeList_ = freeList_->next;
        return ret;
    }

    void deallocate(void* ptr) {
        auto block = static_cast<Block*>(ptr);
        block->next = freeList_;
        freeList_ = block;
    }
};

}  // namespace mempool

// ==== Embedded HTTP Server (Minimal) ====

namespace http {

/**
 * @brief Minimal HTTP server running on a separate thread for status and metrics.
 * Supports very basic GET /status endpoint.
 */
class HttpServer {
    int serverFd_ = -1;
    int port_;
    std::atomic<bool> running_ = false;
    std::thread serverThread_;

    std::function<std::string()> statusProvider_;

    void run();

public:
    explicit HttpServer(int port, std::function<std::string()> statusProvider)
        : port_(port)
        , statusProvider_(std::move(statusProvider)) {}

    bool start();
    void stop();
    ~HttpServer() { stop(); }
};

bool HttpServer::start() {
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

void HttpServer::stop() {
    if (running_) {
        running_ = false;
        shutdown(serverFd_, SHUT_RDWR);
        close(serverFd_);
        if (serverThread_.joinable()) serverThread_.join();
        util::gLogger.log(util::LogLevel::Info, "HTTP server stopped");
    }
}

void HttpServer::run() {
    while (running_) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(serverFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) continue;
        char buf[1024] = {};
        ssize_t count = read(clientFd, buf, sizeof(buf) - 1);
        if (count <= 0) {
            close(clientFd);
            continue;
        }
        std::string req{buf, size_t(count)};
        // Only basic GET /status supported
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
            const char resp[] =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            write(clientFd, resp, sizeof(resp) - 1);
        }
        close(clientFd);
    }
}

}  // namespace http

// ==== Admin Console ====

namespace admin {

/**
 * @brief Simple role-based admin console on stdin.
 * Supports commands with PIN-based role authentication.
 */
class AdminConsole {
    alarm::AlarmController& alarm_;
    plugin::PluginManager& pluginManager_;
    std::atomic<bool> running_ = false;
    std::thread consoleThread_;
    std::string currentPin_ = "";

    void run();

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

/**
 * Helper: split string to tokens
 */
std::vector<std::string> split(std::string_view sv) {
    std::vector<std::string> tokens;
    std::string tmp;
    for (auto c : sv) {
        if (std::isspace(c)) {
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

void AdminConsole::run() {
    util::gLogger.log(util::LogLevel::Info, "Admin Console started. Type 'help' for commands.");

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
                std::cout << "Failed to arm alarm: invalid pin or state\n";
            }
        } else if (cmd == "disarm" && tokens.size() == 2) {
            if (alarm_.disarm(tokens[1])) {
                std::cout << "Alarm disarmed\n";
            } else {
                std::cout << "Failed to disarm alarm: invalid pin or state\n";
            }
        } else if (cmd == "status") {
            auto state = alarm_.getState();
            std::string stateStr = (state == alarm::AlarmState::Armed) ? "Armed" :
                                   (state == alarm::AlarmState::Disarmed) ? "Disarmed" : "Triggered";
            std::cout << "Alarm state: " << stateStr << "\n";
            auto sensors = alarm_.getSensorStatus();
            for (auto const& [id, act] : sensors) {
                std::cout << "Sensor " << id << " status: " << (act ? "Activated" : "Inactive") << "\n";
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

}  // namespace admin

// ==== Main Event Loop and Integration ====

int main() {
    util::gLogger.setLevel(util::LogLevel::Debug);

    // Alarm Controller
    alarm::AlarmController alarmController;

    // Plugin manager
    plugin::PluginManager pluginManager;

    // Start Embedded HTTP Server for /status endpoint
    http::HttpServer httpServer(8080, [&alarmController]() {
        static const std::map<alarm::AlarmState, std::string> stateNames{
            {alarm::AlarmState::Disarmed, "Disarmed"},
            {alarm::AlarmState::Armed, "Armed"},
            {alarm::AlarmState::Triggered, "Triggered"}};
        std::stringstream ss;
        auto st = alarmController.getState();
        ss << "AlarmState: " << stateNames.at(st) << "\n";
        auto sensors = alarmController.getSensorStatus();
        for (auto const& [id, active] : sensors) {
            ss << "Sensor " << id << ": " << (active ? "Activated" : "Inactive") << "\n";
        }
        return ss.str();
    });
    if (!httpServer.start()) {
        util::gLogger.log(util::LogLevel::Error, "Failed to start HTTP Server");
        return 1;
    }

    // Admin Console
    admin::AdminConsole console(alarmController, pluginManager);
    console.start();

    // Simulated GPIO sensor input generator thread
    std::thread gpioThread([]() {
        auto& gpioMgr = gpio::GetGPIOManager();
        int sensorId = 1;
        gpio::SensorType types[] = {gpio::SensorType::Door, gpio::SensorType::Motion, gpio::SensorType::Smoke};
        size_t typeIdx = 0;

        while (true) {
            // Randomly activate sensor events for simulation
            bool activate = (rand() % 5 == 0);
            if (activate) {
                gpio::SensorEvent ev{
                    sensorId,
                    types[typeIdx],
                    true,
                    std::chrono::steady_clock::now()};
                gpioMgr.enqueueEvent(ev);
            }
            sensorId = (sensorId % 3) + 1;
            typeIdx = (typeIdx + 1) % 3;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    });

    // Main event loop processing GPIO events and updating alarm controller
    auto& gpioManager = gpio::GetGPIOManager();

    while (true) {
        auto evtOpt = gpioManager.getNextEvent();
        if (evtOpt) {
            alarmController.processSensorEvent(*evtOpt);
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


