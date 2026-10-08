#include "audio_backend.hpp"
#include "pipewire_backend.hpp"
#include "pulse_backend.hpp"
#include "alsa_backend.hpp"
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <iostream>

namespace crossfeed {

static bool has_pipewire_server() {
    uid_t uid = getuid();
    std::string pw_sock = "/run/user/" + std::to_string(uid) + "/pipewire-0";
    struct stat st;
    return (stat(pw_sock.c_str(), &st) == 0);
}

static bool has_pulse_server() {
    if (std::getenv("PULSE_SERVER")) return true;
    uid_t uid = getuid();
    std::string pulse_sock = "/run/user/" + std::to_string(uid) + "/pulse/native";
    struct stat st;
    return (stat(pulse_sock.c_str(), &st) == 0);
}

std::unique_ptr<AudioBackend> create_backend(const std::string& name) {
    if (name == "pipewire") {
        return std::make_unique<PipeWireBackend>();
    } else if (name == "pulse") {
        return std::make_unique<PulseBackend>();
    } else if (name == "alsa") {
        return std::make_unique<AlsaBackend>();
    } else { // "auto"
        if (has_pipewire_server()) {
            std::cout << "[crossfeed] PipeWire server detected. Using Native In-Line Filter." << std::endl;
            return std::make_unique<PipeWireBackend>();
        } else if (has_pulse_server()) {
            std::cout << "[crossfeed] PulseAudio server detected. Using PulseAudio backend." << std::endl;
            return std::make_unique<PulseBackend>();
        } else {
            std::cout << "[crossfeed] No sound server detected. Using ALSA Direct PCM." << std::endl;
            return std::make_unique<AlsaBackend>();
        }
    }
}

} // namespace crossfeed
