#include "pipewire_backend.hpp"
#include <spa/param/audio/format-utils.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <map>

namespace crossfeed {

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
    FILE* fp = popen("pactl info 2>/dev/null | grep 'Default Sink:' | cut -d' ' -f3", "r");
    if (fp) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            std::string s(buf);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) return s;
        } else {
            pclose(fp);
        }
    }

    // Fallback: first sink in pw-link -i
    fp = popen("pw-link -i 2>/dev/null | grep playback_FL | head -1 | cut -d: -f1", "r");
    if (fp) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            std::string s(buf);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            return s;
        }
        pclose(fp);
    }
    return "";
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
        if (!s.empty()) {
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
        active_target_sink_ = requested_target_;
    } else {
        active_target_sink_ = resolve_default_sink();
    }

    if (active_target_sink_.empty()) {
        std::cerr << "[crossfeed] Error: Could not determine active output device." << std::endl;
        return false;
    }

    target_playback_fl_ = active_target_sink_ + ":playback_FL";
    target_playback_fr_ = active_target_sink_ + ":playback_FR";

    std::cout << "[crossfeed] In-Line filtering active device: " << active_target_sink_ << std::endl;
    std::cout << "  (Zero additional sinks created; audio filters directly into your current output)" << std::endl;

    pw_init(nullptr, nullptr);

    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) return false;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, "crossfeed-dsp",
        PW_KEY_NODE_DESCRIPTION, "Crossfeed DSP Filter",
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
            NULL),
        NULL, 0
    );
    in_port_r_ = pw_filter_add_port(
        filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "in_FR",
            PW_KEY_AUDIO_CHANNEL, "FR",
            NULL),
        NULL, 0
    );

    out_port_l_ = pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "out_FL",
            PW_KEY_AUDIO_CHANNEL, "FL",
            NULL),
        NULL, 0
    );
    out_port_r_ = pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "out_FR",
            PW_KEY_AUDIO_CHANNEL, "FR",
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

    // Ensure crossfeed-dsp output is connected to target playback sink
    std::string connect_out_cmd = "pw-link crossfeed-dsp:out_FL " + target_playback_fl_ + " 2>/dev/null; "
                                + "pw-link crossfeed-dsp:out_FR " + target_playback_fr_ + " 2>/dev/null";
    system(connect_out_cmd.c_str());

    struct PortLinks {
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };

    while (running_.load()) {
        // Dynamically follow default output device if requested target is auto
        if (requested_target_.empty() || requested_target_ == "auto") {
            std::string cur_def = resolve_default_sink();
            if (!cur_def.empty() && cur_def != active_target_sink_) {
                restore_all_links();
                active_target_sink_ = cur_def;
                target_playback_fl_ = active_target_sink_ + ":playback_FL";
                target_playback_fr_ = active_target_sink_ + ":playback_FR";
                connect_out_cmd = "pw-link crossfeed-dsp:out_FL " + target_playback_fl_ + " 2>/dev/null; "
                                + "pw-link crossfeed-dsp:out_FR " + target_playback_fr_ + " 2>/dev/null";
                system(connect_out_cmd.c_str());
            }
        }

        // Parse full PipeWire link graph
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
                    if (!current_port.empty() && !target.empty()) {
                        graph[current_port].inputs.push_back(target);
                    }
                } else if (l.find("|->") != std::string::npos) {
                    size_t pos = l.find("|->");
                    std::string target = l.substr(pos + 3);
                    while (!target.empty() && target.front() == ' ') target.erase(target.begin());
                    while (!target.empty() && target.back() == ' ') target.pop_back();
                    if (!current_port.empty() && !target.empty()) {
                        graph[current_port].outputs.push_back(target);
                    }
                } else if (l[0] != ' ' && l[0] != '\t' && l[0] != '|') {
                    current_port = l;
                    while (!current_port.empty() && current_port.back() == ' ') current_port.pop_back();
                }
            }
            pclose(fp);
        }

        // 1. Intercept newly appearing sources that are trying to play to target_playback_fl_ / target_playback_fr_
        std::vector<std::string> to_intercept_fl;
        for (const auto& src : graph[target_playback_fl_].inputs) {
            if (src != "crossfeed-dsp:out_FL" && src.find("crossfeed-dsp") == std::string::npos) {
                to_intercept_fl.push_back(src);
            }
        }

        std::vector<std::string> to_intercept_fr;
        for (const auto& src : graph[target_playback_fr_].inputs) {
            if (src != "crossfeed-dsp:out_FR" && src.find("crossfeed-dsp") == std::string::npos) {
                to_intercept_fr.push_back(src);
            }
        }

        for (const auto& src_l : to_intercept_fl) {
            std::string link_cmd = "pw-link " + src_l + " crossfeed-dsp:in_FL 2>/dev/null; "
                                 + "pw-link -d " + src_l + " " + target_playback_fl_ + " 2>/dev/null";
            system(link_cmd.c_str());
            std::lock_guard<std::mutex> lock(intercepted_mutex_);
            intercepted_ports_fl_.insert(src_l);
        }

        for (const auto& src_r : to_intercept_fr) {
            std::string link_cmd = "pw-link " + src_r + " crossfeed-dsp:in_FR 2>/dev/null; "
                                 + "pw-link -d " + src_r + " " + target_playback_fr_ + " 2>/dev/null";
            system(link_cmd.c_str());
            std::lock_guard<std::mutex> lock(intercepted_mutex_);
            intercepted_ports_fr_.insert(src_r);
        }

        // 2. Prune duplicate/stale streams feeding crossfeed-dsp
        // If a port feeding crossfeed-dsp has been rerouted by session manager into
        // an intermediate processor (like EasyEffects, JamesDSP, or a filter-chain sink),
        // we MUST unlink it from crossfeed-dsp immediately to prevent duplicate audio / phase distortion!
        std::vector<std::string> stale_fl;
        for (const auto& src : graph["crossfeed-dsp:in_FL"].inputs) {
            bool has_other_destination = false;
            for (const auto& dst : graph[src].outputs) {
                if (dst != "crossfeed-dsp:in_FL") {
                    has_other_destination = true;
                    break;
                }
            }
            if (has_other_destination) {
                stale_fl.push_back(src);
            }
        }

        for (const auto& stale : stale_fl) {
            std::string unlink_cmd = "pw-link -d " + stale + " crossfeed-dsp:in_FL 2>/dev/null";
            system(unlink_cmd.c_str());
            std::lock_guard<std::mutex> lock(intercepted_mutex_);
            intercepted_ports_fl_.erase(stale);
        }

        std::vector<std::string> stale_fr;
        for (const auto& src : graph["crossfeed-dsp:in_FR"].inputs) {
            bool has_other_destination = false;
            for (const auto& dst : graph[src].outputs) {
                if (dst != "crossfeed-dsp:in_FR") {
                    has_other_destination = true;
                    break;
                }
            }
            if (has_other_destination) {
                stale_fr.push_back(src);
            }
        }

        for (const auto& stale : stale_fr) {
            std::string unlink_cmd = "pw-link -d " + stale + " crossfeed-dsp:in_FR 2>/dev/null";
            system(unlink_cmd.c_str());
            std::lock_guard<std::mutex> lock(intercepted_mutex_);
            intercepted_ports_fr_.erase(stale);
        }

        // Ensure crossfeed-dsp output remains connected to physical sink
        system(connect_out_cmd.c_str());

        // Check every 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void PipeWireBackend::restore_all_links() {
    std::lock_guard<std::mutex> lock(intercepted_mutex_);

    // Check current graph to see if any port is already routed to another destination (like EasyEffects)
    // If it is already routed elsewhere, DO NOT reconnect it to target_playback_fl_/fr_!
    FILE* fp = popen("pw-link -l 2>/dev/null", "r");
    std::map<std::string, std::vector<std::string>> outputs_map;
    if (fp) {
        char line[512];
        std::string cur;
        while (fgets(line, sizeof(line), fp)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            if (l.find("|->") != std::string::npos) {
                size_t pos = l.find("|->");
                std::string target = l.substr(pos + 3);
                while (!target.empty() && target.front() == ' ') target.erase(target.begin());
                while (!target.empty() && target.back() == ' ') target.pop_back();
                if (!cur.empty() && !target.empty()) outputs_map[cur].push_back(target);
            } else if (!l.empty() && l[0] != ' ' && l[0] != '\t' && l[0] != '|') {
                cur = l;
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            }
        }
        pclose(fp);
    }

    for (const auto& src_l : intercepted_ports_fl_) {
        bool routed_elsewhere = false;
        for (const auto& dst : outputs_map[src_l]) {
            if (dst != "crossfeed-dsp:in_FL") {
                routed_elsewhere = true;
                break;
            }
        }
        std::string cmd;
        if (!routed_elsewhere) {
            cmd = "pw-link " + src_l + " " + target_playback_fl_ + " 2>/dev/null; ";
        }
        cmd += "pw-link -d " + src_l + " crossfeed-dsp:in_FL 2>/dev/null";
        system(cmd.c_str());
    }
    intercepted_ports_fl_.clear();

    for (const auto& src_r : intercepted_ports_fr_) {
        bool routed_elsewhere = false;
        for (const auto& dst : outputs_map[src_r]) {
            if (dst != "crossfeed-dsp:in_FR") {
                routed_elsewhere = true;
                break;
            }
        }
        std::string cmd;
        if (!routed_elsewhere) {
            cmd = "pw-link " + src_r + " " + target_playback_fr_ + " 2>/dev/null; ";
        }
        cmd += "pw-link -d " + src_r + " crossfeed-dsp:in_FR 2>/dev/null";
        system(cmd.c_str());
    }
    intercepted_ports_fr_.clear();

    std::string disconnect_out = "pw-link -d crossfeed-dsp:out_FL " + target_playback_fl_ + " 2>/dev/null; "
                               + "pw-link -d crossfeed-dsp:out_FR " + target_playback_fr_ + " 2>/dev/null";
    system(disconnect_out.c_str());
}

bool PipeWireBackend::run() {
    if (!loop_ || !filter_) return false;

    running_.store(true);
    link_thread_ = std::thread(&PipeWireBackend::link_manager_loop, this);

    std::cout << "[crossfeed] In-line DSP filter running. Real-time audio active.\n";
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
