#include "pipewire_backend.hpp"
#include <spa/param/audio/format-utils.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <map>
#include <cctype>

namespace crossfeed {

bool is_valid_port_name(const std::string& name) {
    if (name.empty() || name.length() > 256) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '_' && c != '-' && c != '.' && c != ':' && c != ' ') {
            return false;
        }
    }
    return true;
}

static inline void run_sys_cmd(const std::string& cmd) {
    int res = system(cmd.c_str());
    (void)res;
}

static std::string find_best_physical_sink() {
    FILE* fp = popen("pw-link -i 2>/dev/null | grep playback_FL | cut -d: -f1", "r");
    if (!fp) return "";
    char buf[256] = {0};
    std::string fallback_sink;
    std::string preferred_sink;

    while (fgets(buf, sizeof(buf), fp)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (s.empty() || !is_valid_port_name(s)) continue;
        if (s == "crossfeed" || s == "crossfeed_sink" || s == "Crossfeed") continue;

        if (fallback_sink.empty()) {
            fallback_sink = s;
        }

        std::string lower = s;
        for (char& c : lower) c = std::tolower(static_cast<unsigned char>(c));

        // Prefer headphone, speaker, analog, and non-HDMI outputs
        if (lower.find("hdmi") == std::string::npos) {
            if (lower.find("headphone") != std::string::npos ||
                lower.find("speaker") != std::string::npos ||
                lower.find("analog") != std::string::npos) {
                pclose(fp);
                return s; // Ideal match found
            }
            if (preferred_sink.empty()) {
                preferred_sink = s;
            }
        }
    }
    pclose(fp);

    if (!preferred_sink.empty()) return preferred_sink;
    return fallback_sink;
}

bool PipeWireBackend::safe_pw_link(const std::string& src, const std::string& dst, bool disconnect) {
    if (!is_valid_port_name(src) || !is_valid_port_name(dst)) {
        return false;
    }
    std::string cmd = "pw-link ";
    if (disconnect) {
        cmd += "-d ";
    }
    cmd += "\"" + src + "\" \"" + dst + "\" 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

static void on_filter_process(void *userdata, struct spa_io_position *position) {
    auto *backend = static_cast<PipeWireBackend*>(userdata);
    backend->process_audio(position);
}

static const struct pw_filter_events filter_events = {
    .version = PW_VERSION_FILTER_EVENTS,
    .destroy = nullptr,
    .state_changed = nullptr,
    .io_changed = nullptr,
    .param_changed = nullptr,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_filter_process,
    .drained = nullptr,
    .command = nullptr
};

PipeWireBackend::PipeWireBackend() = default;

PipeWireBackend::~PipeWireBackend() {
    stop();
}

std::string PipeWireBackend::resolve_default_sink() {
    auto is_self = [](const std::string& name) {
        return name == "crossfeed" || name == "crossfeed_sink" || name == "Crossfeed";
    };

    FILE* fp = popen("pactl info 2>/dev/null | grep 'Default Sink:' | cut -d' ' -f3", "r");
    if (fp) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            std::string s(buf);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (!s.empty() && is_valid_port_name(s)) {
                if (is_self(s)) {
                    // Default sink is currently crossfeed itself, so no device change occurred.
                    if (!active_target_sink_.empty()) {
                        return active_target_sink_;
                    }
                    return find_best_physical_sink();
                }
                return s;
            }
        } else {
            pclose(fp);
        }
    }

    if (!active_target_sink_.empty()) {
        return active_target_sink_;
    }
    return find_best_physical_sink();
}

bool PipeWireBackend::load_null_sink() {
    // 1. Check if module-null-sink for crossfeed is already loaded
    FILE* fp = popen("pactl list short modules 2>/dev/null | grep module-null-sink | grep 'sink_name=crossfeed' | awk '{print $1}'", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            try {
                null_sink_module_index_ = std::stoul(buf);
                own_module_ = true;
                pclose(fp);
            } catch (...) {}
        } else {
            pclose(fp);
        }
    }

    // 2. Load module-null-sink
    if (null_sink_module_index_ == 0xFFFFFFFFU) {
        fp = popen("pactl load-module module-null-sink sink_name=crossfeed sink_properties=device.description=Crossfeed 2>/dev/null", "r");
        if (fp) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), fp)) {
                try {
                    null_sink_module_index_ = std::stoul(buf);
                    own_module_ = true;
                } catch (...) {}
            }
            pclose(fp);
        }
    }

    if (null_sink_module_index_ == 0xFFFFFFFFU) {
        return false;
    }

    // 3. Move active sink inputs to crossfeed so ongoing audio immediately routes through filter
    run_sys_cmd("pactl list short sink-inputs 2>/dev/null | awk '{print $1}' | while read -r id; do [ -n \"$id\" ] && pactl move-sink-input \"$id\" crossfeed 2>/dev/null; done");

    // 4. Set crossfeed as default sink
    run_sys_cmd("pactl set-default-sink crossfeed 2>/dev/null");
    return true;
}

void PipeWireBackend::unload_null_sink() {
    // 1. Restore original default sink and move active streams back
    std::string restore_sink = original_default_sink_;
    if (restore_sink.empty() || restore_sink == "crossfeed" || restore_sink == "Crossfeed") {
        restore_sink = active_target_sink_;
    }
    if (restore_sink.empty() || restore_sink == "crossfeed" || restore_sink == "Crossfeed") {
        restore_sink = find_best_physical_sink();
    }

    if (!restore_sink.empty() && restore_sink != "crossfeed" && restore_sink != "Crossfeed") {
        std::string move_cmd = "pactl list short sink-inputs 2>/dev/null | awk '{print $1}' | while read -r id; do [ -n \"$id\" ] && pactl move-sink-input \"$id\" \"" + restore_sink + "\" 2>/dev/null; done";
        run_sys_cmd(move_cmd);
        std::string def_cmd = "pactl set-default-sink \"" + restore_sink + "\" 2>/dev/null";
        run_sys_cmd(def_cmd);
    }

    // 2. Unload module
    if (null_sink_module_index_ != 0xFFFFFFFFU) {
        std::string cmd = "pactl unload-module " + std::to_string(null_sink_module_index_) + " >/dev/null 2>&1";
        run_sys_cmd(cmd);
        null_sink_module_index_ = 0xFFFFFFFFU;
        own_module_ = false;
    } else {
        run_sys_cmd("pactl list short modules 2>/dev/null | grep module-null-sink | grep 'sink_name=crossfeed' | awk '{print $1}' | while read -r id; do [ -n \"$id\" ] && pactl unload-module \"$id\" 2>/dev/null; done");
    }
}

std::vector<SinkDevice> PipeWireBackend::list_sinks() {
    std::vector<SinkDevice> sinks;
    FILE* fp = popen("pw-link -i 2>/dev/null | grep playback_FL | cut -d: -f1", "r");
    if (!fp) return sinks;
    char buf[256];
    std::string def = resolve_default_sink();
    while (fgets(buf, sizeof(buf), fp)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && is_valid_port_name(s) && s != "crossfeed" && s != "Crossfeed" && s != "crossfeed_sink") {
            SinkDevice dev;
            dev.name = s;
            dev.description = s;
            dev.is_default = (s == def);
            sinks.push_back(dev);
        }
    }
    pclose(fp);
    return sinks;
}

bool PipeWireBackend::init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) {
    dsp_ = dsp;
    sample_rate_ = sample_rate;
    buffer_frames_ = buffer_frames;
    requested_target_ = target_sink;

    if (!requested_target_.empty() && requested_target_ != "auto") {
        if (!is_valid_port_name(requested_target_)) {
            std::cerr << "[crossfeed] Error: Invalid target sink name: " << requested_target_ << std::endl;
            return false;
        }
        active_target_sink_ = requested_target_;
    } else {
        active_target_sink_ = resolve_default_sink();
    }

    if (active_target_sink_.empty() || !is_valid_port_name(active_target_sink_)) {
        std::cerr << "[crossfeed] Error: Could not determine active output device." << std::endl;
        return false;
    }

    original_default_sink_ = active_target_sink_;
    target_playback_fl_ = active_target_sink_ + ":playback_FL";
    target_playback_fr_ = active_target_sink_ + ":playback_FR";

    std::cout << "[crossfeed] Native PipeWire filter active for device: " << active_target_sink_ << std::endl;

    if (!load_null_sink()) {
        std::cerr << "[crossfeed] Failed to initialize 'crossfeed' virtual sink." << std::endl;
        return false;
    }

    pw_init(nullptr, nullptr);

    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) return false;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, "crossfeed-dsp",
        PW_KEY_NODE_DESCRIPTION, "Crossfeed DSP Filter",
        PW_KEY_NODE_PASSIVE, "true",
        PW_KEY_NODE_AUTOCONNECT, "false",
        NULL
    );

    filter_ = pw_filter_new_simple(
        pw_main_loop_get_loop(loop_),
        "crossfeed-dsp",
        props,
        &filter_events,
        this
    );
    if (!filter_) {
        pw_main_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }

    in_port_l_ = pw_filter_add_port(
        filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "in_FL",
            PW_KEY_AUDIO_CHANNEL, "FL",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );
    in_port_r_ = pw_filter_add_port(
        filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "in_FR",
            PW_KEY_AUDIO_CHANNEL, "FR",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );

    out_port_l_ = pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "out_FL",
            PW_KEY_AUDIO_CHANNEL, "FL",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );
    out_port_r_ = pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "out_FR",
            PW_KEY_AUDIO_CHANNEL, "FR",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );

    if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        std::cerr << "[crossfeed] Failed to connect pw_filter." << std::endl;
        return false;
    }

    dsp_->set_params(static_cast<float>(sample_rate_), dsp_->get_level_db(), dsp_->get_freq_hz(), dsp_->is_enabled());
    return true;
}

void PipeWireBackend::process_audio(struct spa_io_position *position) {
    uint32_t n_samples = position ? position->clock.duration : buffer_frames_;
    if (n_samples == 0) return;

    if (position && position->clock.rate.denom > 0 && position->clock.rate.denom != sample_rate_) {
        sample_rate_ = position->clock.rate.denom;
        dsp_->set_params(static_cast<float>(sample_rate_), dsp_->get_level_db(), dsp_->get_freq_hz(), dsp_->is_enabled());
    }

    float *in_l = static_cast<float*>(pw_filter_get_dsp_buffer(in_port_l_, n_samples));
    float *in_r = static_cast<float*>(pw_filter_get_dsp_buffer(in_port_r_, n_samples));
    float *out_l = static_cast<float*>(pw_filter_get_dsp_buffer(out_port_l_, n_samples));
    float *out_r = static_cast<float*>(pw_filter_get_dsp_buffer(out_port_r_, n_samples));

    if (!out_l || !out_r) return;

    if (in_l && in_r) {
        dsp_->process_planar(in_l, in_r, out_l, out_r, n_samples);
    } else if (in_l) {
        dsp_->process_planar(in_l, in_l, out_l, out_r, n_samples);
    } else if (in_r) {
        dsp_->process_planar(in_r, in_r, out_l, out_r, n_samples);
    } else {
        std::memset(out_l, 0, n_samples * sizeof(float));
        std::memset(out_r, 0, n_samples * sizeof(float));
    }
}

void PipeWireBackend::link_manager_loop() {
    // Initial delay for filter registration in the graph
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Connect crossfeed:monitor -> crossfeed-dsp input
    safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL");
    safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR");

    // Connect crossfeed-dsp output -> physical target sink
    safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_);
    safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_);

    struct PortLinks {
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };

    while (running_.load()) {
        // Dynamically follow default physical output device if requested target is auto
        if (requested_target_.empty() || requested_target_ == "auto") {
            std::string cur_def = resolve_default_sink();
            if (!cur_def.empty() && is_valid_port_name(cur_def) && cur_def != active_target_sink_) {
                safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_, true);
                safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_, true);

                active_target_sink_ = cur_def;
                original_default_sink_ = cur_def;
                target_playback_fl_ = active_target_sink_ + ":playback_FL";
                target_playback_fr_ = active_target_sink_ + ":playback_FR";

                safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_);
                safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_);

                // Re-assert crossfeed as default sink so subsequent new streams go to crossfeed
                run_sys_cmd("pactl set-default-sink crossfeed 2>/dev/null");
            }
        }

        // Parse PipeWire link graph to ensure bridge links remain intact
        std::map<std::string, PortLinks> graph;
        FILE* fp = popen("pw-link -l 2>/dev/null", "r");
        if (fp) {
            char line[512];
            std::string current_port;

            while (fgets(line, sizeof(line), fp)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
                if (l.empty()) continue;

                if (l.find("|<-") != std::string::npos) {
                    size_t pos = l.find("|<-");
                    std::string target = l.substr(pos + 3);
                    while (!target.empty() && target.front() == ' ') target.erase(target.begin());
                    while (!target.empty() && target.back() == ' ') target.pop_back();
                    if (!current_port.empty() && !target.empty() && is_valid_port_name(target)) {
                        graph[current_port].inputs.push_back(target);
                    }
                } else if (l.find("|->") != std::string::npos) {
                    size_t pos = l.find("|->");
                    std::string target = l.substr(pos + 3);
                    while (!target.empty() && target.front() == ' ') target.erase(target.begin());
                    while (!target.empty() && target.back() == ' ') target.pop_back();
                    if (!current_port.empty() && !target.empty() && is_valid_port_name(target)) {
                        graph[current_port].outputs.push_back(target);
                    }
                } else if (l[0] != ' ' && l[0] != '\t' && l[0] != '|') {
                    current_port = l;
                    while (!current_port.empty() && current_port.back() == ' ') current_port.pop_back();
                }
            }
            pclose(fp);
        }

        // 1. Maintain input bridge: crossfeed:monitor -> crossfeed-dsp:in
        // Ensure ONLY crossfeed:monitor is connected to crossfeed-dsp:in.
        // If rogue streams connected directly to crossfeed-dsp:in, redirect them to crossfeed virtual sink.
        bool in_fl_linked = false;
        auto in_fl_it = graph.find("crossfeed-dsp:in_FL");
        if (in_fl_it != graph.end()) {
            for (const auto& src : in_fl_it->second.inputs) {
                if (src == "crossfeed:monitor_FL") {
                    in_fl_linked = true;
                } else {
                    // Rogue stream bypassing virtual mixer directly into DSP port!
                    safe_pw_link(src, "crossfeed-dsp:in_FL", true);
                    safe_pw_link(src, "crossfeed:playback_FL");
                }
            }
        }
        if (!in_fl_linked) {
            safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL");
        }

        bool in_fr_linked = false;
        auto in_fr_it = graph.find("crossfeed-dsp:in_FR");
        if (in_fr_it != graph.end()) {
            for (const auto& src : in_fr_it->second.inputs) {
                if (src == "crossfeed:monitor_FR") {
                    in_fr_linked = true;
                } else {
                    safe_pw_link(src, "crossfeed-dsp:in_FR", true);
                    safe_pw_link(src, "crossfeed:playback_FR");
                }
            }
        }
        if (!in_fr_linked) {
            safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR");
        }

        // 2. Maintain output bridge: crossfeed-dsp:out -> target_playback
        bool out_fl_linked = false;
        auto out_fl_it = graph.find("crossfeed-dsp:out_FL");
        if (out_fl_it != graph.end()) {
            for (const auto& dst : out_fl_it->second.outputs) {
                if (dst == target_playback_fl_) { out_fl_linked = true; break; }
            }
        }
        if (!out_fl_linked) {
            safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_);
        }

        bool out_fr_linked = false;
        auto out_fr_it = graph.find("crossfeed-dsp:out_FR");
        if (out_fr_it != graph.end()) {
            for (const auto& dst : out_fr_it->second.outputs) {
                if (dst == target_playback_fr_) { out_fr_linked = true; break; }
            }
        }
        if (!out_fr_linked) {
            safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void PipeWireBackend::restore_all_links() {
    safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL", true);
    safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR", true);
    safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_, true);
    safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_, true);
    unload_null_sink();
}

bool PipeWireBackend::run() {
    if (!loop_ || !filter_) return false;

    running_.store(true);
    link_thread_ = std::thread(&PipeWireBackend::link_manager_loop, this);

    std::cout << "[crossfeed] Native PipeWire DSP filter running. Real-time audio active.\n";
    pw_main_loop_run(loop_);

    stop();
    return true;
}

void PipeWireBackend::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (link_thread_.joinable()) {
        link_thread_.join();
    }

    restore_all_links();

    if (loop_) {
        pw_main_loop_quit(loop_);
    }

    if (filter_) {
        pw_filter_destroy(filter_);
        filter_ = nullptr;
    }

    if (loop_) {
        pw_main_loop_destroy(loop_);
        loop_ = nullptr;
    }

    pw_deinit();
}

} // namespace crossfeed
