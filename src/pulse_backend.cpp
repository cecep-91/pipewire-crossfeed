#include "pulse_backend.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <pulse/error.h>

namespace crossfeed {

PulseBackend::PulseBackend() = default;

PulseBackend::~PulseBackend() {
    stop();
}

struct ContextQuery {
    pa_mainloop* ml = nullptr;
    pa_mainloop_api* api = nullptr;
    std::string default_sink;
    std::vector<SinkDevice> sinks;
    uint32_t loaded_module_index = PA_INVALID_INDEX;
    bool server_info_done = false;
    bool sink_list_done = false;
    bool done = false;
    bool error = false;
};

static void server_info_cb(pa_context* /*c*/, const pa_server_info* i, void* userdata) {
    auto* q = static_cast<ContextQuery*>(userdata);
    if (i && i->default_sink_name) {
        q->default_sink = i->default_sink_name;
    }
    q->server_info_done = true;
    if (q->sink_list_done) {
        q->done = true;
    }
}

static void sink_list_cb(pa_context* /*c*/, const pa_sink_info* i, int eol, void* userdata) {
    auto* q = static_cast<ContextQuery*>(userdata);
    if (eol > 0) {
        q->sink_list_done = true;
        if (q->server_info_done) {
            q->done = true;
        }
        return;
    }
    if (i) {
        SinkDevice s;
        s.name = i->name ? i->name : "";
        s.description = i->description ? i->description : "";
        q->sinks.push_back(s);
    }
}

static void context_state_cb(pa_context* c, void* userdata) {
    auto* q = static_cast<ContextQuery*>(userdata);
    pa_context_state_t state = pa_context_get_state(c);
    switch (state) {
        case PA_CONTEXT_READY:
            pa_context_get_server_info(c, server_info_cb, userdata);
            pa_context_get_sink_info_list(c, sink_list_cb, userdata);
            break;
        case PA_CONTEXT_FAILED:
            q->error = true;
            q->done = true;
            break;
        case PA_CONTEXT_TERMINATED:
            q->done = true;
            break;
        default:
            break;
    }
}

bool PulseBackend::query_sinks(std::string& default_sink, std::vector<SinkDevice>& sinks) {
    ContextQuery q;
    q.ml = pa_mainloop_new();
    if (!q.ml) return false;
    q.api = pa_mainloop_get_api(q.ml);

    pa_context* ctx = pa_context_new(q.api, "CrossfeedSinkDiscovery");
    if (!ctx) {
        pa_mainloop_free(q.ml);
        return false;
    }

    pa_context_set_state_callback(ctx, context_state_cb, &q);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(ctx);
        pa_mainloop_free(q.ml);
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    while (!q.done && !q.error) {
        pa_mainloop_iterate(q.ml, 1, nullptr);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break; // timeout
        }
    }

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(q.ml);

    if (q.error || !q.done) return false;

    default_sink = q.default_sink;
    sinks = q.sinks;
    for (auto& s : sinks) {
        s.is_default = (s.name == default_sink);
    }
    return true;
}

std::vector<SinkDevice> PulseBackend::list_sinks() {
    std::string def;
    std::vector<SinkDevice> sinks;
    query_sinks(def, sinks);
    return sinks;
}



bool PulseBackend::load_null_sink() {
    // 1. Check if module-null-sink for crossfeed is already loaded
    FILE* fp = popen("pactl list short modules 2>/dev/null | grep module-null-sink | grep 'sink_name=crossfeed' | awk '{print $1}'", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            try {
                null_sink_module_index_ = std::stoul(buf);
                own_module_ = true;
                pclose(fp);
                return true;
            } catch (...) {}
        }
        pclose(fp);
    }

    // 2. Load module-null-sink
    fp = popen("pactl load-module module-null-sink sink_name=crossfeed sink_properties=device.description=Crossfeed 2>/dev/null", "r");
    if (!fp) return false;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
        try {
            null_sink_module_index_ = std::stoul(buf);
            own_module_ = true;
            pclose(fp);
            return true;
        } catch (...) {}
    }
    pclose(fp);
    return false;
}

void PulseBackend::unload_null_sink() {
    if (null_sink_module_index_ != PA_INVALID_INDEX) {
        std::string cmd = "pactl unload-module " + std::to_string(null_sink_module_index_) + " >/dev/null 2>&1";
        system(cmd.c_str());
        null_sink_module_index_ = PA_INVALID_INDEX;
        own_module_ = false;
    } else {
        // Fallback: unload any crossfeed null sink
        system("pactl list short modules 2>/dev/null | grep module-null-sink | grep 'sink_name=crossfeed' | awk '{print $1}' | while read -r id; do [ -n \"$id\" ] && pactl unload-module \"$id\" 2>/dev/null; done");
    }
}

std::string PulseBackend::resolve_target_sink(const std::string& requested, const std::string& default_sink, const std::vector<SinkDevice>& sinks) {
    auto is_self = [](const std::string& name) {
        return name == "crossfeed" || name == "crossfeed_sink" || name == "Crossfeed";
    };

    // If explicit target requested and not self, check if it exists
    if (!requested.empty() && requested != "auto" && !is_self(requested)) {
        for (const auto& s : sinks) {
            if (s.name == requested) {
                return requested;
            }
        }
        std::cerr << "[crossfeed] Requested target sink '" << requested << "' not found, falling back to auto." << std::endl;
    }

    // If default sink is valid and NOT self, use default sink!
    if (!default_sink.empty() && !is_self(default_sink)) {
        return default_sink;
    }

    // Otherwise, default sink IS self (user set Crossfeed as system default!)
    // Select the first non-crossfeed sink from the system
    for (const auto& s : sinks) {
        if (!is_self(s.name)) {
            std::cout << "[crossfeed] Default sink is Crossfeed. Routing playback to physical sink: "
                      << s.name << " (" << s.description << ")" << std::endl;
            return s.name;
        }
    }

    return "";
}

bool PulseBackend::init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) {
    dsp_ = dsp;
    requested_target_ = target_sink;
    sample_rate_ = sample_rate;
    buffer_frames_ = buffer_frames;

    // 1. Discover sinks and resolve target
    std::string default_sink;
    std::vector<SinkDevice> sinks;
    if (!query_sinks(default_sink, sinks)) {
        std::cerr << "[crossfeed] Failed to query sound server for sinks." << std::endl;
        return false;
    }

    active_target_ = resolve_target_sink(requested_target_, default_sink, sinks);
    if (active_target_.empty()) {
        std::cerr << "[crossfeed] Error: No suitable playback sink found." << std::endl;
        return false;
    }

    std::cout << "[crossfeed] Active output target: " << active_target_ << std::endl;

    // 2. Ensure virtual sink exists
    if (!load_null_sink()) {
        std::cerr << "[crossfeed] Failed to create or find 'crossfeed' virtual sink." << std::endl;
        return false;
    }

    dsp_->set_params(static_cast<float>(sample_rate_), dsp_->get_level_db(), dsp_->get_freq_hz(), dsp_->is_enabled());
    return true;
}

bool PulseBackend::run() {
    if (!dsp_ || active_target_.empty()) {
        return false;
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = sample_rate_;
    ss.channels = 2;

    pa_buffer_attr ba;
    ba.maxlength = static_cast<uint32_t>(-1);
    ba.tlength = buffer_frames_ * 2 * sizeof(float) * 2;
    ba.prebuf = static_cast<uint32_t>(-1);
    ba.minreq = static_cast<uint32_t>(-1);
    ba.fragsize = buffer_frames_ * sizeof(float) * 2;

    int error = 0;
    std::string monitor_source = "crossfeed.monitor";

    rec_stream_ = pa_simple_new(
        nullptr,
        "Crossfeed",
        PA_STREAM_RECORD,
        monitor_source.c_str(),
        "Crossfeed Capture",
        &ss,
        nullptr,
        &ba,
        &error
    );

    if (!rec_stream_) {
        std::cerr << "[crossfeed] Failed to create record stream from " << monitor_source
                  << ": " << pa_strerror(error) << std::endl;
        return false;
    }

    play_stream_ = pa_simple_new(
        nullptr,
        "Crossfeed",
        PA_STREAM_PLAYBACK,
        active_target_.c_str(),
        "Crossfeed Output",
        &ss,
        nullptr,
        &ba,
        &error
    );

    if (!play_stream_) {
        std::cerr << "[crossfeed] Failed to create playback stream to " << active_target_
                  << ": " << pa_strerror(error) << std::endl;
        pa_simple_free(rec_stream_);
        rec_stream_ = nullptr;
        return false;
    }

    running_.store(true);
    std::cout << "[crossfeed] Engine started. Processing audio ("
              << sample_rate_ << " Hz, " << buffer_frames_ << " frames/buffer)..." << std::endl;

    std::vector<float> in_buf(buffer_frames_ * 2);
    std::vector<float> out_buf(buffer_frames_ * 2);
    const size_t bytes = buffer_frames_ * sizeof(float) * 2;

    while (running_.load()) {
        if (pa_simple_read(rec_stream_, in_buf.data(), bytes, &error) < 0) {
            if (running_.load()) {
                std::cerr << "[crossfeed] Record error: " << pa_strerror(error) << std::endl;
            }
            break;
        }

        dsp_->process_interleaved(in_buf.data(), out_buf.data(), buffer_frames_);

        if (pa_simple_write(play_stream_, out_buf.data(), bytes, &error) < 0) {
            if (running_.load()) {
                std::cerr << "[crossfeed] Playback error: " << pa_strerror(error) << std::endl;
            }
            break;
        }
    }

    stop();
    return true;
}

void PulseBackend::stop() {
    running_.store(false);
    unload_null_sink();
    if (play_stream_) {
        pa_simple_drain(play_stream_, nullptr);
        pa_simple_free(play_stream_);
        play_stream_ = nullptr;
    }
    if (rec_stream_) {
        pa_simple_free(rec_stream_);
        rec_stream_ = nullptr;
    }
}

} // namespace crossfeed
