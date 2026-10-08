#include "config.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace crossfeed {

std::string Config::get_config_dir() {
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        return std::string(xdg_config) + "/pipewire-crossfeed";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/pipewire-crossfeed";
    }
    return "/tmp/pipewire-crossfeed";
}

std::string Config::get_state_file_path() {
    return get_config_dir() + "/state.json";
}

std::string Config::get_socket_path() {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && *runtime_dir) {
        return std::string(runtime_dir) + "/crossfeed.sock";
    }
    return get_config_dir() + "/crossfeed.sock";
}

static void ensure_dir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool Config::load_state(ConfigState& state) {
    std::string path = get_state_file_path();
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Simple robust JSON extractor for our key/value pairs
    auto extract_val = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        size_t pos = content.find(search);
        if (pos == std::string::npos) return "";
        pos = content.find(':', pos + search.length());
        if (pos == std::string::npos) return "";
        size_t start = pos + 1;
        while (start < content.size() && (content[start] == ' ' || content[start] == '\t' || content[start] == '\r' || content[start] == '\n')) {
            start++;
        }
        if (start >= content.size()) return "";
        if (content[start] == '\"') {
            size_t end = content.find('\"', start + 1);
            if (end == std::string::npos) return "";
            return content.substr(start + 1, end - start - 1);
        } else {
            size_t end = content.find_first_of(",}\r\n", start);
            if (end == std::string::npos) end = content.size();
            return trim(content.substr(start, end - start));
        }
    };

    std::string val;
    val = extract_val("enabled");
    if (!val.empty()) {
        state.enabled = (val == "true" || val == "1");
    }

    val = extract_val("level_db");
    if (!val.empty()) {
        try { state.level_db = std::stof(val); } catch (...) {}
    }

    val = extract_val("freq_hz");
    if (!val.empty()) {
        try { state.freq_hz = std::stof(val); } catch (...) {}
    }

    val = extract_val("backend");
    if (!val.empty()) {
        state.backend = val;
    }

    val = extract_val("target_sink");
    if (!val.empty()) {
        state.target_sink = val;
    }

    val = extract_val("sample_rate");
    if (!val.empty()) {
        try { state.sample_rate = std::stoul(val); } catch (...) {}
    }

    val = extract_val("buffer_frames");
    if (!val.empty()) {
        try { state.buffer_frames = std::stoul(val); } catch (...) {}
    }

    return true;
}

bool Config::save_state(const ConfigState& state) {
    ensure_dir(get_config_dir());
    std::string path = get_state_file_path();
    std::string tmp_path = path + ".tmp";

    std::ofstream file(tmp_path);
    if (!file.is_open()) {
        return false;
    }

    file << "{\n"
         << "  \"enabled\": " << (state.enabled ? "true" : "false") << ",\n"
         << "  \"level_db\": " << state.level_db << ",\n"
         << "  \"freq_hz\": " << state.freq_hz << ",\n"
         << "  \"backend\": \"" << state.backend << "\",\n"
         << "  \"target_sink\": \"" << state.target_sink << "\",\n"
         << "  \"sample_rate\": " << state.sample_rate << ",\n"
         << "  \"buffer_frames\": " << state.buffer_frames << "\n"
         << "}\n";

    file.close();
    rename(tmp_path.c_str(), path.c_str());
    return true;
}

} // namespace crossfeed
