#pragma once

#include "dsp.hpp"
#include "audio_backend.hpp"
#include "config.hpp"
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace crossfeed {

class IpcServer {
public:
    IpcServer(CrossfeedDSP* dsp, AudioBackend* backend, ConfigState* config);
    ~IpcServer();

    bool start(const std::string& socket_path);
    void stop();

    void set_stop_callback(std::function<void()> cb) { stop_callback_ = cb; }

private:
    void run_loop();
    std::string handle_command(const std::string& cmd);

    CrossfeedDSP* dsp_;
    AudioBackend* backend_;
    ConfigState* config_;
    std::string socket_path_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::function<void()> stop_callback_;
};

class IpcClient {
public:
    static bool send_command(const std::string& socket_path, const std::string& cmd, std::string& response);
};

} // namespace crossfeed
