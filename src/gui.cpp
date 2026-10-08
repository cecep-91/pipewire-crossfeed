#include "gui.hpp"
#include "ipc.hpp"
#include "config.hpp"
#include "dsp.hpp"
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <unistd.h>
#include <limits.h>

namespace crossfeed {

struct CrossfeedPreset {
    const char* name;
    const char* description;
    float level_db;
    float freq_hz;
    float delay_us;
    float phase_apf_hz;
    float center_trim_db;
    float shadow_hz;
};

static const CrossfeedPreset g_presets[] = {
    {"Jan Meier", "Jan Meier (Corda) — natural presentation for fatigue-free listening", -9.5f, 650.0f, 280.0f, 1500.0f, -1.5f, 3200.0f},
    {"Chu Moy", "Chu Moy (HeadWize) — classic analog RC circuit crossfeed emulation", -6.0f, 700.0f, 260.0f, 2000.0f, -2.0f, 2800.0f},
    {"Bauer BS2B", "Bauer BS2B — stereophonic-to-binaural high blend simulation", -4.5f, 700.0f, 350.0f, 1200.0f, -2.5f, 2500.0f},
    {"Linkwitz", "Siegfried Linkwitz — loudspeaker simulation for wide spatial staging", -7.0f, 1200.0f, 220.0f, 1600.0f, -1.8f, 4000.0f},
    {"Studio 30°", "Natural Studio — simulates near-field stereo monitors at 30° triangle", -8.0f, 850.0f, 250.0f, 1800.0f, -1.5f, 3500.0f},
};

struct GuiEngineState {
    bool running = false;
    bool enabled = true;
    float level_db = DEFAULT_LEVEL_DB;
    float freq_hz = DEFAULT_FREQ_HZ;
    float delay_us = DEFAULT_DELAY_US;
    float phase_apf_hz = DEFAULT_PHASE_APF_HZ;
    float center_trim_db = DEFAULT_CENTER_TRIM_DB;
    float shadow_hz = DEFAULT_SHADOW_HZ;
    std::string backend = "pipewire";
    std::string target = "";
    uint32_t sample_rate = 48000;
    uint32_t buffer_frames = 256;
};

class CrossfeedGuiApp {
public:
    static CrossfeedGuiApp* instance;

    GtkWidget* window = nullptr;
    GtkWidget* header_bar = nullptr;
    GtkWidget* master_switch = nullptr;
    GtkWidget* main_stack = nullptr;
    GtkWidget* controls_box = nullptr;
    GtkWidget* stopped_box = nullptr;

    // Primary Sliders
    GtkAdjustment* level_adj = nullptr;
    GtkWidget* level_scale = nullptr;
    GtkWidget* level_spin = nullptr;

    GtkAdjustment* freq_adj = nullptr;
    GtkWidget* freq_scale = nullptr;
    GtkWidget* freq_spin = nullptr;

    // Advanced 'Not for me options :v' Sliders
    GtkWidget* expander = nullptr;
    GtkWidget* preset_desc_label = nullptr;

    GtkAdjustment* delay_adj = nullptr;
    GtkWidget* delay_scale = nullptr;
    GtkWidget* delay_spin = nullptr;

    GtkAdjustment* phase_adj = nullptr;
    GtkWidget* phase_scale = nullptr;
    GtkWidget* phase_spin = nullptr;

    GtkAdjustment* trim_adj = nullptr;
    GtkWidget* trim_scale = nullptr;
    GtkWidget* trim_spin = nullptr;

    GtkAdjustment* shadow_adj = nullptr;
    GtkWidget* shadow_scale = nullptr;
    GtkWidget* shadow_spin = nullptr;

    // Status display widgets
    GtkWidget* status_dot_label = nullptr;
    GtkWidget* backend_val_label = nullptr;
    GtkWidget* target_val_label = nullptr;
    GtkWidget* latency_val_label = nullptr;

    // Tray Indicator
    AppIndicator* indicator = nullptr;
    GtkWidget* tray_menu = nullptr;
    GtkWidget* tray_status_item = nullptr;
    GtkWidget* tray_toggle_item = nullptr;
    GtkWidget* tray_engine_item = nullptr;

    GuiEngineState state;
    bool suppress_events = false;

    CrossfeedGuiApp() {
        instance = this;
    }

    static std::string get_self_executable_path() {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            return std::string(buf);
        }
        return "crossfeed";
    }

    static std::string parse_json_value(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + search.length());
        if (pos == std::string::npos) return "";
        size_t start = pos + 1;
        while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) start++;
        if (start >= json.size()) return "";
        if (json[start] == '\"') {
            size_t end = json.find('\"', start + 1);
            if (end == std::string::npos) return "";
            return json.substr(start + 1, end - start - 1);
        } else {
            size_t end = json.find_first_of(",}\r\n", start);
            if (end == std::string::npos) end = json.size();
            return json.substr(start, end - start);
        }
    }

    bool query_engine_state(GuiEngineState& out_state) {
        std::string resp;
        if (!IpcClient::send_command(Config::get_socket_path(), "STATUS", resp)) {
            out_state.running = false;
            return false;
        }

        out_state.running = (parse_json_value(resp, "status") == "running");
        out_state.enabled = (parse_json_value(resp, "enabled") == "true");

        std::string lvl = parse_json_value(resp, "level_db");
        if (!lvl.empty()) {
            try { out_state.level_db = std::stof(lvl); } catch (...) {}
        }

        std::string frq = parse_json_value(resp, "freq_hz");
        if (!frq.empty()) {
            try { out_state.freq_hz = std::stof(frq); } catch (...) {}
        }

        std::string del = parse_json_value(resp, "delay_us");
        if (!del.empty()) {
            try { out_state.delay_us = std::stof(del); } catch (...) {}
        }

        std::string phs = parse_json_value(resp, "phase_apf_hz");
        if (!phs.empty()) {
            try { out_state.phase_apf_hz = std::stof(phs); } catch (...) {}
        }

        std::string trm = parse_json_value(resp, "center_trim_db");
        if (!trm.empty()) {
            try { out_state.center_trim_db = std::stof(trm); } catch (...) {}
        }

        std::string shd = parse_json_value(resp, "shadow_hz");
        if (!shd.empty()) {
            try { out_state.shadow_hz = std::stof(shd); } catch (...) {}
        }

        out_state.backend = parse_json_value(resp, "backend");
        out_state.target = parse_json_value(resp, "target");

        std::string sr = parse_json_value(resp, "sample_rate");
        if (!sr.empty()) {
            try { out_state.sample_rate = std::stoul(sr); } catch (...) {}
        }

        std::string buf = parse_json_value(resp, "buffer_frames");
        if (!buf.empty()) {
            try { out_state.buffer_frames = std::stoul(buf); } catch (...) {}
        }

        return true;
    }

    void start_engine() {
        std::string exe = get_self_executable_path();
        std::string cmd = "\"" + exe + "\" start";
        int res = system(cmd.c_str());
        (void)res;
        usleep(150000);
        refresh_from_engine();
    }

    void stop_engine() {
        std::string resp;
        IpcClient::send_command(Config::get_socket_path(), "STOP", resp);
        usleep(100000);
        refresh_from_engine();
    }

    void restart_engine() {
        stop_engine();
        usleep(200000);
        start_engine();
    }

    void apply_params(bool enabled, float level_db, float freq_hz,
                      float delay_us, float phase_apf_hz, float center_trim_db,
                      float shadow_hz) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << "SET enabled=" << (enabled ? "1" : "0")
           << " level=" << level_db
           << std::setprecision(0)
           << " freq=" << freq_hz
           << std::setprecision(1)
           << " delay=" << delay_us
           << std::setprecision(0)
           << " phase=" << phase_apf_hz
           << std::setprecision(1)
           << " trim=" << center_trim_db
           << std::setprecision(0)
           << " shadow=" << shadow_hz;
        std::string resp;
        IpcClient::send_command(Config::get_socket_path(), ss.str(), resp);
        refresh_from_engine();
    }

    void toggle_enabled() {
        std::string resp;
        IpcClient::send_command(Config::get_socket_path(), "TOGGLE", resp);
        refresh_from_engine();
    }

    void update_ui() {
        suppress_events = true;

        if (!state.running) {
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header_bar), "Engine Stopped");
            gtk_widget_hide(master_switch);
            gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "stopped");

            if (indicator) {
                app_indicator_set_icon_full(indicator, "audio-volume-muted", "Crossfeed stopped");
            }
            if (tray_status_item) {
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_status_item), "Crossfeed: Stopped");
            }
            if (tray_engine_item) {
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_engine_item), "Start Engine");
            }
            if (tray_toggle_item) {
                gtk_widget_set_sensitive(tray_toggle_item, FALSE);
            }
        } else {
            gtk_widget_show(master_switch);
            gtk_switch_set_active(GTK_SWITCH(master_switch), state.enabled);
            gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "controls");

            // Update primary sliders
            if (std::fabs(gtk_adjustment_get_value(level_adj) - state.level_db) > 0.05) {
                gtk_adjustment_set_value(level_adj, state.level_db);
            }
            if (std::fabs(gtk_adjustment_get_value(freq_adj) - state.freq_hz) > 1.0) {
                gtk_adjustment_set_value(freq_adj, state.freq_hz);
            }

            // Update expanded 'Not for me options' sliders
            if (delay_adj && std::fabs(gtk_adjustment_get_value(delay_adj) - state.delay_us) > 1.0) {
                gtk_adjustment_set_value(delay_adj, state.delay_us);
            }
            if (phase_adj && std::fabs(gtk_adjustment_get_value(phase_adj) - state.phase_apf_hz) > 5.0) {
                gtk_adjustment_set_value(phase_adj, state.phase_apf_hz);
            }
            if (trim_adj && std::fabs(gtk_adjustment_get_value(trim_adj) - state.center_trim_db) > 0.05) {
                gtk_adjustment_set_value(trim_adj, state.center_trim_db);
            }
            if (shadow_adj && std::fabs(gtk_adjustment_get_value(shadow_adj) - state.shadow_hz) > 10.0) {
                gtk_adjustment_set_value(shadow_adj, state.shadow_hz);
            }

            gtk_widget_set_sensitive(controls_box, state.enabled);

            // Subtitle
            std::ostringstream sub;
            sub << std::fixed << std::setprecision(1);
            if (state.enabled) {
                sub << state.level_db << " dB · " << std::setprecision(0) << state.freq_hz << " Hz · " << state.delay_us << " µs";
                if (!state.target.empty()) {
                    std::string tgt_short = state.target;
                    size_t last_dot = tgt_short.find_last_of('.');
                    if (last_dot != std::string::npos && last_dot + 1 < tgt_short.size()) {
                        tgt_short = tgt_short.substr(last_dot + 1);
                    }
                    sub << " · " << tgt_short;
                }
            } else {
                sub << "Bypassed (Passthrough)";
            }
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header_bar), sub.str().c_str());

            // Details box
            if (status_dot_label) {
                if (state.enabled) {
                    gtk_label_set_markup(GTK_LABEL(status_dot_label), "<span foreground='#4CAF50' weight='bold'>● ACTIVE</span>");
                } else {
                    gtk_label_set_markup(GTK_LABEL(status_dot_label), "<span foreground='#FF9800' weight='bold'>○ BYPASSED</span>");
                }
            }
            if (backend_val_label) {
                std::string bk = state.backend;
                if (bk == "pipewire") bk = "PipeWire (Direct Graph SPA)";
                else if (bk == "pulse") bk = "PulseAudio (Low Latency)";
                else if (bk == "alsa") bk = "ALSA (Direct Device)";
                gtk_label_set_text(GTK_LABEL(backend_val_label), bk.c_str());
            }
            if (target_val_label) {
                std::string tgt = state.target.empty() ? "(Auto Default Sink)" : state.target;
                gtk_label_set_text(GTK_LABEL(target_val_label), tgt.c_str());
            }
            if (latency_val_label) {
                float ms = (state.sample_rate > 0) ? (static_cast<float>(state.buffer_frames) * 1000.0f / state.sample_rate) : 5.3f;
                std::ostringstream lat;
                lat << state.buffer_frames << " frames (" << std::fixed << std::setprecision(1) << ms << " ms @ " << (state.sample_rate / 1000) << " kHz)";
                gtk_label_set_text(GTK_LABEL(latency_val_label), lat.str().c_str());
            }

            // Tray indicator
            if (indicator) {
                if (state.enabled) {
                    app_indicator_set_icon_full(indicator, "audio-headphones", "Crossfeed active");
                } else {
                    app_indicator_set_icon_full(indicator, "audio-volume-muted", "Crossfeed bypassed");
                }
            }
            if (tray_status_item) {
                std::ostringstream t_stat;
                t_stat << "Crossfeed: " << (state.enabled ? "Active (" : "Bypassed (")
                       << std::fixed << std::setprecision(1) << state.level_db << " dB, "
                       << std::setprecision(0) << state.freq_hz << " Hz, "
                       << state.delay_us << " µs)";
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_status_item), t_stat.str().c_str());
            }
            if (tray_engine_item) {
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_engine_item), "Stop Engine");
            }
            if (tray_toggle_item) {
                gtk_widget_set_sensitive(tray_toggle_item, TRUE);
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_toggle_item), state.enabled ? "Bypass Filter" : "Enable Filter");
            }
        }

        suppress_events = false;
    }

    void refresh_from_engine() {
        query_engine_state(state);
        update_ui();
    }

    // Callbacks
    static gboolean on_poll_timer(gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (!app->suppress_events) {
            GuiEngineState new_st;
            app->query_engine_state(new_st);
            if (new_st.running != app->state.running ||
                new_st.enabled != app->state.enabled ||
                std::fabs(new_st.level_db - app->state.level_db) > 0.05f ||
                std::fabs(new_st.freq_hz - app->state.freq_hz) > 1.0f ||
                std::fabs(new_st.delay_us - app->state.delay_us) > 1.0f ||
                std::fabs(new_st.phase_apf_hz - app->state.phase_apf_hz) > 5.0f ||
                std::fabs(new_st.center_trim_db - app->state.center_trim_db) > 0.05f ||
                std::fabs(new_st.shadow_hz - app->state.shadow_hz) > 10.0f ||
                new_st.target != app->state.target) {
                app->state = new_st;
                app->update_ui();
            }
        }
        return TRUE;
    }

    static gboolean on_window_delete(GtkWidget* widget, GdkEvent* /*event*/, gpointer /*data*/) {
        // HIDE TO SYSTEM TRAY on window close! Do not exit!
        gtk_widget_hide(widget);
        return TRUE;
    }

    static gboolean on_switch_state_set(GtkSwitch* /*sw*/, gboolean state, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return FALSE;
        app->apply_current_ui_params(state);
        return FALSE;
    }

    void apply_current_ui_params(bool en) {
        float lvl = static_cast<float>(gtk_adjustment_get_value(level_adj));
        float frq = static_cast<float>(gtk_adjustment_get_value(freq_adj));
        float del = delay_adj ? static_cast<float>(gtk_adjustment_get_value(delay_adj)) : state.delay_us;
        float phs = phase_adj ? static_cast<float>(gtk_adjustment_get_value(phase_adj)) : state.phase_apf_hz;
        float trm = trim_adj ? static_cast<float>(gtk_adjustment_get_value(trim_adj)) : state.center_trim_db;
        float shd = shadow_adj ? static_cast<float>(gtk_adjustment_get_value(shadow_adj)) : state.shadow_hz;
        apply_params(en, lvl, frq, del, phs, trm, shd);
    }

    static void on_any_slider_changed(GtkAdjustment* /*adj*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return;
        bool en = gtk_switch_get_active(GTK_SWITCH(app->master_switch));
        app->apply_current_ui_params(en);
    }

    static void on_preset_button_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = instance;
        auto* preset = static_cast<const CrossfeedPreset*>(user_data);
        bool en = gtk_switch_get_active(GTK_SWITCH(app->master_switch));
        app->apply_params(en, preset->level_db, preset->freq_hz,
                          preset->delay_us, preset->phase_apf_hz,
                          preset->center_trim_db, preset->shadow_hz);
        if (app->preset_desc_label) {
            gtk_label_set_text(GTK_LABEL(app->preset_desc_label), preset->description);
        }
    }

    static void on_start_engine_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->start_engine();
    }

    static void on_restart_engine_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->restart_engine();
    }

    static void on_stop_engine_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->stop_engine();
    }

    static void on_tray_show_window(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        gtk_window_present(GTK_WINDOW(app->window));
    }

    static void on_tray_toggle(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->toggle_enabled();
    }

    static void on_tray_engine_action(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->state.running) {
            app->stop_engine();
        } else {
            app->start_engine();
        }
    }

    static void on_tray_quit(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->stop_engine();
        gtk_main_quit();
    }

    void build_tray() {
        indicator = app_indicator_new("pipewire-crossfeed", "audio-headphones", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
        app_indicator_set_title(indicator, "Crossfeed");

        tray_menu = gtk_menu_new();

        tray_status_item = gtk_menu_item_new_with_label("Crossfeed: Initializing...");
        gtk_widget_set_sensitive(tray_status_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_status_item);

        GtkWidget* sep1 = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), sep1);

        GtkWidget* item_show = gtk_menu_item_new_with_label("Show Window");
        g_signal_connect(item_show, "activate", G_CALLBACK(on_tray_show_window), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_show);

        tray_toggle_item = gtk_menu_item_new_with_label("Toggle Bypass");
        g_signal_connect(tray_toggle_item, "activate", G_CALLBACK(on_tray_toggle), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_toggle_item);

        // Presets submenu
        GtkWidget* item_presets = gtk_menu_item_new_with_label("Presets");
        GtkWidget* presets_menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_presets), presets_menu);

        for (const auto& preset : g_presets) {
            std::ostringstream ss;
            ss << preset.name << " (" << std::fixed << std::setprecision(0) << preset.freq_hz << " Hz, "
               << std::setprecision(1) << preset.level_db << " dB, "
               << std::setprecision(0) << preset.delay_us << " µs)";
            GtkWidget* it = gtk_menu_item_new_with_label(ss.str().c_str());
            g_signal_connect(it, "activate", G_CALLBACK(on_preset_button_clicked), const_cast<CrossfeedPreset*>(&preset));
            gtk_menu_shell_append(GTK_MENU_SHELL(presets_menu), it);
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_presets);

        GtkWidget* sep2 = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), sep2);

        tray_engine_item = gtk_menu_item_new_with_label("Stop Engine");
        g_signal_connect(tray_engine_item, "activate", G_CALLBACK(on_tray_engine_action), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_engine_item);

        GtkWidget* sep3 = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), sep3);

        GtkWidget* item_quit = gtk_menu_item_new_with_label("Quit Crossfeed");
        g_signal_connect(item_quit, "activate", G_CALLBACK(on_tray_quit), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_quit);

        gtk_widget_show_all(tray_menu);
        app_indicator_set_menu(indicator, GTK_MENU(tray_menu));
    }

    void build_ui() {
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Crossfeed Control");
        gtk_window_set_default_size(GTK_WINDOW(window), 480, 520);
        gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
        gtk_window_set_icon_name(GTK_WINDOW(window), "audio-headphones");

        // Intercept delete-event to hide window to tray instead of quitting
        g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), this);

        // Header bar
        header_bar = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), "Crossfeed");
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header_bar), "Initializing...");
        gtk_window_set_titlebar(GTK_WINDOW(window), header_bar);

        // Master bypass switch in header bar
        master_switch = gtk_switch_new();
        gtk_widget_set_valign(master_switch, GTK_ALIGN_CENTER);
        g_signal_connect(master_switch, "state-set", G_CALLBACK(on_switch_state_set), this);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), master_switch);

        // Stack container for running vs stopped views
        main_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(main_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_container_add(GTK_CONTAINER(window), main_stack);

        // ================= CONTROLS VIEW =================
        GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

        controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
        gtk_widget_set_margin_start(controls_box, 20);
        gtk_widget_set_margin_end(controls_box, 20);
        gtk_widget_set_margin_top(controls_box, 16);
        gtk_widget_set_margin_bottom(controls_box, 16);
        gtk_container_add(GTK_CONTAINER(scrolled), controls_box);

        // --- Level Slider Section ---
        {
            GtkWidget* sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget* lbl = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(lbl), "<b>Blend Level (dB)</b>");
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

            level_adj = gtk_adjustment_new(-10.0, -30.0, -6.0, 0.5, 2.0, 0.0);
            level_spin = gtk_spin_button_new(level_adj, 0.5, 1);
            gtk_box_pack_end(GTK_BOX(row), level_spin, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(sec), row, FALSE, FALSE, 0);

            GtkWidget* cap = gtk_label_new("Feed intensity into opposite ear (-10.0 dB is standard)");
            gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
            gtk_box_pack_start(GTK_BOX(sec), cap, FALSE, FALSE, 0);

            level_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, level_adj);
            gtk_scale_set_digits(GTK_SCALE(level_scale), 1);
            gtk_scale_set_draw_value(GTK_SCALE(level_scale), FALSE);
            gtk_scale_add_mark(GTK_SCALE(level_scale), -10.0, GTK_POS_BOTTOM, "Default");
            gtk_box_pack_start(GTK_BOX(sec), level_scale, FALSE, FALSE, 0);

            g_signal_connect(level_adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
            gtk_box_pack_start(GTK_BOX(controls_box), sec, FALSE, FALSE, 0);
        }

        // --- Frequency Slider Section ---
        {
            GtkWidget* sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget* lbl = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(lbl), "<b>Crossover Frequency (Hz)</b>");
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

            freq_adj = gtk_adjustment_new(700.0, 200.0, 2000.0, 10.0, 50.0, 0.0);
            freq_spin = gtk_spin_button_new(freq_adj, 10.0, 0);
            gtk_box_pack_end(GTK_BOX(row), freq_spin, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(sec), row, FALSE, FALSE, 0);

            GtkWidget* cap = gtk_label_new("Frequencies below this cutoff cross over (700 Hz head shadow)");
            gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
            gtk_box_pack_start(GTK_BOX(sec), cap, FALSE, FALSE, 0);

            freq_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, freq_adj);
            gtk_scale_set_digits(GTK_SCALE(freq_scale), 0);
            gtk_scale_set_draw_value(GTK_SCALE(freq_scale), FALSE);
            gtk_scale_add_mark(GTK_SCALE(freq_scale), 700.0, GTK_POS_BOTTOM, "700 Hz");
            gtk_box_pack_start(GTK_BOX(sec), freq_scale, FALSE, FALSE, 0);

            g_signal_connect(freq_adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
            gtk_box_pack_start(GTK_BOX(controls_box), sec, FALSE, FALSE, 0);
        }

        // ================= EXPANDABLE 'NOT FOR ME OPTIONS :v' =================
        {
            expander = gtk_expander_new(nullptr);
            gtk_expander_set_use_markup(GTK_EXPANDER(expander), TRUE);
            gtk_expander_set_label(GTK_EXPANDER(expander), "<span weight='bold'>Not for me options :v</span>");

            GtkWidget* exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
            gtk_widget_set_margin_start(exp_box, 10);
            gtk_widget_set_margin_end(exp_box, 10);
            gtk_widget_set_margin_top(exp_box, 10);
            gtk_widget_set_margin_bottom(exp_box, 10);

            // --- Presets Section (Shown only in this expanded menu!) ---
            {
                GtkWidget* p_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
                GtkWidget* p_title = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(p_title), "<b>Acoustic Emulation Presets</b>");
                gtk_label_set_xalign(GTK_LABEL(p_title), 0.0);
                gtk_box_pack_start(GTK_BOX(p_section), p_title, FALSE, FALSE, 0);

                GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                for (const auto& preset : g_presets) {
                    GtkWidget* btn = gtk_button_new_with_label(preset.name);
                    g_signal_connect(btn, "clicked", G_CALLBACK(on_preset_button_clicked), const_cast<CrossfeedPreset*>(&preset));
                    gtk_box_pack_start(GTK_BOX(btn_box), btn, TRUE, TRUE, 0);
                }
                gtk_box_pack_start(GTK_BOX(p_section), btn_box, FALSE, FALSE, 0);

                preset_desc_label = gtk_label_new("Select an emulation preset to auto-tune parameters");
                gtk_label_set_xalign(GTK_LABEL(preset_desc_label), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(preset_desc_label), TRUE);
                gtk_style_context_add_class(gtk_widget_get_style_context(preset_desc_label), "dim-label");
                gtk_box_pack_start(GTK_BOX(p_section), preset_desc_label, FALSE, FALSE, 2);

                gtk_box_pack_start(GTK_BOX(exp_box), p_section, FALSE, FALSE, 2);
            }

            // --- 1. Interaural Time Delay (ITD) Slider ---
            {
                GtkWidget* sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget* lbl = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(lbl), "<b>Acoustic Delay / ITD (µs)</b>");
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

                delay_adj = gtk_adjustment_new(280.0, 0.0, 800.0, 10.0, 50.0, 0.0);
                delay_spin = gtk_spin_button_new(delay_adj, 10.0, 0);
                gtk_box_pack_end(GTK_BOX(row), delay_spin, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(sec), row, FALSE, FALSE, 0);

                GtkWidget* cap = gtk_label_new("Acoustic travel time around skull to opposite ear (200-400 µs natural)");
                gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
                gtk_box_pack_start(GTK_BOX(sec), cap, FALSE, FALSE, 0);

                delay_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, delay_adj);
                gtk_scale_set_digits(GTK_SCALE(delay_scale), 0);
                gtk_scale_set_draw_value(GTK_SCALE(delay_scale), FALSE);
                gtk_scale_add_mark(GTK_SCALE(delay_scale), 260.0, GTK_POS_BOTTOM, "Chu Moy");
                gtk_scale_add_mark(GTK_SCALE(delay_scale), 280.0, GTK_POS_BOTTOM, "Meier");
                gtk_scale_add_mark(GTK_SCALE(delay_scale), 350.0, GTK_POS_BOTTOM, "BS2B");
                gtk_box_pack_start(GTK_BOX(sec), delay_scale, FALSE, FALSE, 0);

                g_signal_connect(delay_adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
                gtk_box_pack_start(GTK_BOX(exp_box), sec, FALSE, FALSE, 0);
            }

            // --- 2. Phase Alignment All-Pass Filter (Hz) ---
            {
                GtkWidget* sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget* lbl = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(lbl), "<b>Phase Alignment / All-Pass (Hz)</b>");
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

                phase_adj = gtk_adjustment_new(1500.0, 200.0, 4000.0, 50.0, 200.0, 0.0);
                phase_spin = gtk_spin_button_new(phase_adj, 50.0, 0);
                gtk_box_pack_end(GTK_BOX(row), phase_spin, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(sec), row, FALSE, FALSE, 0);

                GtkWidget* cap = gtk_label_new("Phase rotation break frequency to eliminate comb filtering & preserve bass");
                gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
                gtk_box_pack_start(GTK_BOX(sec), cap, FALSE, FALSE, 0);

                phase_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, phase_adj);
                gtk_scale_set_digits(GTK_SCALE(phase_scale), 0);
                gtk_scale_set_draw_value(GTK_SCALE(phase_scale), FALSE);
                gtk_scale_add_mark(GTK_SCALE(phase_scale), 1500.0, GTK_POS_BOTTOM, "1500 Hz");
                gtk_box_pack_start(GTK_BOX(sec), phase_scale, FALSE, FALSE, 0);

                g_signal_connect(phase_adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
                gtk_box_pack_start(GTK_BOX(exp_box), sec, FALSE, FALSE, 0);
            }

            // --- 3. Center Summing Trim (dB) ---
            {
                GtkWidget* sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget* lbl = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(lbl), "<b>Center Summing Trim (dB)</b>");
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

                trim_adj = gtk_adjustment_new(-1.5, -6.0, 0.0, 0.5, 1.0, 0.0);
                trim_spin = gtk_spin_button_new(trim_adj, 0.5, 1);
                gtk_box_pack_end(GTK_BOX(row), trim_spin, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(sec), row, FALSE, FALSE, 0);

                GtkWidget* cap = gtk_label_new("Negative gain to compensate for acoustic center build-up from L+R summing");
                gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
                gtk_box_pack_start(GTK_BOX(sec), cap, FALSE, FALSE, 0);

                trim_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, trim_adj);
                gtk_scale_set_digits(GTK_SCALE(trim_scale), 1);
                gtk_scale_set_draw_value(GTK_SCALE(trim_scale), FALSE);
                gtk_scale_add_mark(GTK_SCALE(trim_scale), -1.5, GTK_POS_BOTTOM, "-1.5 dB");
                gtk_box_pack_start(GTK_BOX(sec), trim_scale, FALSE, FALSE, 0);

                g_signal_connect(trim_adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
                gtk_box_pack_start(GTK_BOX(exp_box), sec, FALSE, FALSE, 0);
            }

            // --- 4. Acoustic Head Shadow Cutoff (Hz) ---
            {
                GtkWidget* sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget* lbl = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(lbl), "<b>Head Shadow Cutoff (Hz)</b>");
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

                shadow_adj = gtk_adjustment_new(3000.0, 1000.0, 8000.0, 100.0, 500.0, 0.0);
                shadow_spin = gtk_spin_button_new(shadow_adj, 100.0, 0);
                gtk_box_pack_end(GTK_BOX(row), shadow_spin, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(sec), row, FALSE, FALSE, 0);

                GtkWidget* cap = gtk_label_new("High-frequency absorption by listener's head geometry (1000-8000 Hz)");
                gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
                gtk_box_pack_start(GTK_BOX(sec), cap, FALSE, FALSE, 0);

                shadow_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, shadow_adj);
                gtk_scale_set_digits(GTK_SCALE(shadow_scale), 0);
                gtk_scale_set_draw_value(GTK_SCALE(shadow_scale), FALSE);
                gtk_scale_add_mark(GTK_SCALE(shadow_scale), 3000.0, GTK_POS_BOTTOM, "3000 Hz");
                gtk_box_pack_start(GTK_BOX(sec), shadow_scale, FALSE, FALSE, 0);

                g_signal_connect(shadow_adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
                gtk_box_pack_start(GTK_BOX(exp_box), sec, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(expander), exp_box);
            gtk_box_pack_start(GTK_BOX(controls_box), expander, FALSE, FALSE, 4);
        }

        // --- Status Details Card ---
        {
            GtkWidget* frame = gtk_frame_new(nullptr);
            GtkWidget* grid = gtk_grid_new();
            gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
            gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
            gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

            // Row 0: Status & Dot
            GtkWidget* l_st = gtk_label_new("Status:");
            gtk_label_set_xalign(GTK_LABEL(l_st), 0.0);
            status_dot_label = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(status_dot_label), 0.0);
            gtk_grid_attach(GTK_GRID(grid), l_st, 0, 0, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), status_dot_label, 1, 0, 1, 1);

            // Row 1: Backend
            GtkWidget* l_bk = gtk_label_new("Backend:");
            gtk_label_set_xalign(GTK_LABEL(l_bk), 0.0);
            backend_val_label = gtk_label_new("PipeWire");
            gtk_label_set_xalign(GTK_LABEL(backend_val_label), 0.0);
            gtk_grid_attach(GTK_GRID(grid), l_bk, 0, 1, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), backend_val_label, 1, 1, 1, 1);

            // Row 2: Target Sink
            GtkWidget* l_tgt = gtk_label_new("Output:");
            gtk_label_set_xalign(GTK_LABEL(l_tgt), 0.0);
            target_val_label = gtk_label_new("(Auto)");
            gtk_label_set_xalign(GTK_LABEL(target_val_label), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(target_val_label), PANGO_ELLIPSIZE_MIDDLE);
            gtk_grid_attach(GTK_GRID(grid), l_tgt, 0, 2, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), target_val_label, 1, 2, 1, 1);

            // Row 3: Latency
            GtkWidget* l_lat = gtk_label_new("Latency:");
            gtk_label_set_xalign(GTK_LABEL(l_lat), 0.0);
            latency_val_label = gtk_label_new("256 frames (5.3 ms)");
            gtk_label_set_xalign(GTK_LABEL(latency_val_label), 0.0);
            gtk_grid_attach(GTK_GRID(grid), l_lat, 0, 3, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), latency_val_label, 1, 3, 1, 1);

            gtk_container_add(GTK_CONTAINER(frame), grid);
            gtk_box_pack_start(GTK_BOX(controls_box), frame, FALSE, FALSE, 4);
        }

        // --- Action Buttons (Restart / Stop) ---
        {
            GtkWidget* abox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_halign(abox, GTK_ALIGN_END);

            GtkWidget* btn_restart = gtk_button_new_with_label("Restart Engine");
            g_signal_connect(btn_restart, "clicked", G_CALLBACK(on_restart_engine_clicked), this);
            gtk_box_pack_start(GTK_BOX(abox), btn_restart, FALSE, FALSE, 0);

            GtkWidget* btn_stop = gtk_button_new_with_label("Stop Engine");
            g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_engine_clicked), this);
            gtk_box_pack_start(GTK_BOX(abox), btn_stop, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(controls_box), abox, FALSE, FALSE, 0);
        }

        gtk_stack_add_named(GTK_STACK(main_stack), scrolled, "controls");

        // ================= STOPPED VIEW =================
        stopped_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(stopped_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(stopped_box, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(stopped_box, 30);
        gtk_widget_set_margin_end(stopped_box, 30);
        gtk_widget_set_margin_top(stopped_box, 30);
        gtk_widget_set_margin_bottom(stopped_box, 30);

        GtkWidget* icon = gtk_image_new_from_icon_name("audio-headphones", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(stopped_box), icon, FALSE, FALSE, 0);

        GtkWidget* msg_title = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(msg_title), "<span size='large' weight='bold'>Crossfeed Engine Stopped</span>");
        gtk_box_pack_start(GTK_BOX(stopped_box), msg_title, FALSE, FALSE, 0);

        GtkWidget* msg_sub = gtk_label_new("The standalone audio filter engine is currently inactive.\nClick below to start it.");
        gtk_label_set_justify(GTK_LABEL(msg_sub), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(msg_sub), "dim-label");
        gtk_box_pack_start(GTK_BOX(stopped_box), msg_sub, FALSE, FALSE, 0);

        GtkWidget* start_btn = gtk_button_new_with_label("Start Crossfeed Engine");
        gtk_widget_set_size_request(start_btn, 220, 42);
        gtk_style_context_add_class(gtk_widget_get_style_context(start_btn), "suggested-action");
        g_signal_connect(start_btn, "clicked", G_CALLBACK(on_start_engine_clicked), this);
        gtk_box_pack_start(GTK_BOX(stopped_box), start_btn, FALSE, FALSE, 8);

        gtk_stack_add_named(GTK_STACK(main_stack), stopped_box, "stopped");

        // Build Tray Icon and Menu
        build_tray();

        // Initial sync: if not running, auto-start engine for instant playback
        if (!query_engine_state(state)) {
            start_engine();
        } else {
            update_ui();
        }

        // Periodic poll every 1000ms
        g_timeout_add(1000, on_poll_timer, this);
    }
};

CrossfeedGuiApp* CrossfeedGuiApp::instance = nullptr;

int run_gui(int argc, char** argv) {
    g_set_prgname("crossfeed");
    g_set_application_name("Crossfeed");

    gtk_init(&argc, &argv);

    CrossfeedGuiApp app;
    app.build_ui();

    gtk_widget_show_all(app.window);

    gtk_main();
    return 0;
}

} // namespace crossfeed
