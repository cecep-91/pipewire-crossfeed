#pragma once

#include <string>
#include <cstdint>

namespace crossfeed {

struct ConfigState {
    bool enabled = true;
    float level_db = -10.0f;
    float freq_hz = 700.0f;
    std::string backend = "auto";
    std::string target_sink = "auto";
    uint32_t sample_rate = 48000;
    uint32_t buffer_frames = 256;
};

class Config {
public:
    static std::string get_config_dir();
    static std::string get_state_file_path();
    static std::string get_socket_path();

    static bool load_state(ConfigState& state);
    static bool save_state(const ConfigState& state);
};

} // namespace crossfeed
