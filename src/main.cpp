#include "dsp.hpp"
#include "config.hpp"
#include "ipc.hpp"
#include "audio_backend.hpp"
#include "benchmark.hpp"
#include "gui.hpp"
#include <iostream>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <iomanip>

using namespace crossfeed;

static std::atomic<bool> g_shutdown_requested{false};
static AudioBackend* g_active_backend = nullptr;

static void sig_handler(int /*signo*/) {
    g_shutdown_requested.store(true);
    if (g_active_backend) {
        g_active_backend->stop();
    }
}

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [command] [options]\n\n"
              << "A standalone, ultra-low-latency headphone crossfeed audio processor\n"
              << "compatible with any Linux distribution and sound server.\n\n"
              << "Commands:\n"
              << "  gui, app          Launch the graphical user interface with system tray (default)\n"
              << "  run               Run the crossfeed engine in the foreground\n"
              << "  start             Start the crossfeed engine in the background (daemon)\n"
              << "  stop              Stop the running crossfeed engine\n"
              << "  restart           Restart the crossfeed engine\n"
              << "  status            Display engine status and active audio routing\n"
              << "  toggle            Toggle crossfeed filtering on/off (bypass)\n"
              << "  on                Enable crossfeed filtering\n"
              << "  off               Bypass crossfeed filtering (direct passthrough)\n"
              << "  set               Adjust filter parameters live\n"
              << "  bench             Run DSP throughput and CPU performance benchmark\n"
              << "  help, --help      Show this help message\n"
              << "  --version         Show version information\n\n"
              << "Run Options:\n"
              << "  --backend <name>  Audio backend: 'auto' (default), 'pulse', 'alsa'\n"
              << "  --target <sink>   Target output sink (default: auto non-crossfeed sink)\n"
              << "  --level <dB>      Crossfeed blend level in dB (-30.0 to -6.0, default: -10.0)\n"
              << "  --freq <Hz>       Crossover frequency in Hz (200 to 2000, default: 700)\n"
              << "  --delay <us>      Acoustic delay in microseconds (0 to 800, default: 280)\n"
              << "  --phase <Hz>      Phase alignment all-pass frequency (200 to 4000, default: 1500)\n"
              << "  --trim <dB>       Center summing gain trim in dB (-6.0 to 0.0, default: -1.5)\n"
              << "  --shadow <Hz>     Head acoustic shadow cutoff in Hz (1000 to 8000, default: 3000)\n"
              << "  --rate <Hz>       Sample rate (default: 48000)\n"
              << "  --buffer <frames> Buffer size (default: 256)\n"
              << "  --bypass          Start in bypassed state\n\n"
              << "Set Options:\n"
              << "  --level <dB>      Set blend level in dB\n"
              << "  --freq <Hz>       Set crossover frequency in Hz\n"
              << "  --delay <us>      Set acoustic delay in microseconds\n"
              << "  --phase <Hz>      Set phase alignment all-pass frequency in Hz\n"
              << "  --trim <dB>       Set center summing gain trim in dB\n"
              << "  --shadow <Hz>     Set head shadow cutoff in Hz\n"
              << "  --enabled <0|1>   Set filter active (1) or bypassed (0)\n\n"
              << "Status Options:\n"
              << "  --json            Output status in JSON format\n";
}

static int cmd_run(int argc, char** argv) {
    ConfigState config;
    Config::load_state(config);

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--backend" && i + 1 < argc) {
            config.backend = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            config.target_sink = argv[++i];
        } else if (arg == "--level" && i + 1 < argc) {
            config.level_db = std::stof(argv[++i]);
        } else if (arg == "--freq" && i + 1 < argc) {
            config.freq_hz = std::stof(argv[++i]);
        } else if (arg == "--delay" && i + 1 < argc) {
            config.delay_us = std::stof(argv[++i]);
        } else if (arg == "--phase" && i + 1 < argc) {
            config.phase_apf_hz = std::stof(argv[++i]);
        } else if (arg == "--trim" && i + 1 < argc) {
            config.center_trim_db = std::stof(argv[++i]);
        } else if (arg == "--shadow" && i + 1 < argc) {
            config.shadow_hz = std::stof(argv[++i]);
        } else if (arg == "--rate" && i + 1 < argc) {
            config.sample_rate = std::stoul(argv[++i]);
        } else if (arg == "--buffer" && i + 1 < argc) {
            config.buffer_frames = std::stoul(argv[++i]);
        } else if (arg == "--bypass") {
            config.enabled = false;
        } else if (arg == "--enabled") {
            config.enabled = true;
        }
    }

    std::cout << "[crossfeed] Starting standalone crossfeed audio engine...\n"
              << "  Backend: " << config.backend << "\n"
              << "  Target: " << config.target_sink << "\n"
              << "  Level: " << config.level_db << " dB\n"
              << "  Freq: " << config.freq_hz << " Hz\n"
              << "  Delay: " << config.delay_us << " µs\n"
              << "  Phase APF: " << config.phase_apf_hz << " Hz\n"
              << "  Center Trim: " << config.center_trim_db << " dB\n"
              << "  Head Shadow: " << config.shadow_hz << " Hz\n"
              << "  Filter: " << (config.enabled ? "ENABLED" : "BYPASSED") << "\n";

    CrossfeedDSP dsp;
    dsp.set_all_params(static_cast<float>(config.sample_rate), config.level_db, config.freq_hz,
                       config.delay_us, config.phase_apf_hz, config.center_trim_db, config.shadow_hz,
                       config.enabled);

    auto backend = create_backend(config.backend);
    if (!backend) {
        std::cerr << "[crossfeed] Error: Failed to create backend '" << config.backend << "'\n";
        return 1;
    }

    if (!backend->init(&dsp, config.target_sink, config.sample_rate, config.buffer_frames)) {
        std::cerr << "[crossfeed] Error: Failed to initialize backend.\n";
        return 1;
    }

    g_active_backend = backend.get();

    struct sigaction sa{};
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::string sock_path = Config::get_socket_path();
    IpcServer ipc(&dsp, backend.get(), &config);
    ipc.set_stop_callback([backend_ptr = backend.get()]() {
        if (backend_ptr) backend_ptr->stop();
    });

    if (!ipc.start(sock_path)) {
        std::cerr << "[crossfeed] Warning: Failed to start IPC socket server at " << sock_path << "\n";
    } else {
        std::cout << "[crossfeed] IPC control listening at " << sock_path << "\n";
    }

    bool ok = backend->run();

    ipc.stop();
    g_active_backend = nullptr;
    std::cout << "[crossfeed] Engine shutdown cleanly.\n";
    return ok ? 0 : 1;
}

static int cmd_start(int argc, char** argv) {
    std::string resp;
    if (IpcClient::send_command(Config::get_socket_path(), "PING", resp) && resp == "PONG") {
        std::cout << "Crossfeed engine is already running.\n";
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork process.\n";
        return 1;
    }
    if (pid > 0) {
        // Parent: wait a moment for daemon to start and verify
        for (int i = 0; i < 20; ++i) {
            usleep(100000); // 100ms
            if (IpcClient::send_command(Config::get_socket_path(), "PING", resp) && resp == "PONG") {
                std::cout << "Crossfeed engine started successfully in background (PID: " << pid << ").\n";
                return 0;
            }
        }
        std::cout << "Crossfeed background process launched (PID: " << pid << ").\n";
        return 0;
    }

    // Child process: redirect standard streams and run
    setsid();
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    char* run_argv[argc + 1];
    run_argv[0] = argv[0];
    run_argv[1] = const_cast<char*>("run");
    for (int i = 2; i < argc; ++i) {
        run_argv[i] = argv[i];
    }
    run_argv[argc] = nullptr;

    return cmd_run(argc, run_argv);
}

static int cmd_stop() {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "STOP", resp)) {
        std::cout << "Crossfeed is not running.\n";
        return 0;
    }
    std::cout << "Sent stop command to Crossfeed engine.\n";
    for (int i = 0; i < 20; ++i) {
        usleep(100000);
        if (!IpcClient::send_command(Config::get_socket_path(), "PING", resp)) {
            std::cout << "Crossfeed stopped successfully.\n";
            return 0;
        }
    }
    return 0;
}

static int cmd_status(bool json_output) {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "STATUS", resp)) {
        if (json_output) {
            std::cout << "{\"status\":\"stopped\"}\n";
        } else {
            std::cout << "Crossfeed is NOT running.\n"
                      << "Start it with: crossfeed start\n";
        }
        return 1;
    }

    if (json_output) {
        std::cout << resp << "\n";
        return 0;
    }

    // Human-friendly formatted output
    std::cout << "========================================\n"
              << "       Crossfeed Engine Status          \n"
              << "========================================\n";

    auto get_field = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        size_t pos = resp.find(search);
        if (pos == std::string::npos) return "";
        pos = resp.find(':', pos + search.length());
        if (pos == std::string::npos) return "";
        size_t start = pos + 1;
        while (start < resp.size() && (resp[start] == ' ' || resp[start] == '\t' || resp[start] == '\r' || resp[start] == '\n')) start++;
        if (start >= resp.size()) return "";
        if (resp[start] == '\"') {
            size_t end = resp.find('\"', start + 1);
            if (end == std::string::npos) return "";
            return resp.substr(start + 1, end - start - 1);
        } else {
            size_t end = resp.find_first_of(",}\r\n", start);
            if (end == std::string::npos) end = resp.size();
            return resp.substr(start, end - start);
        }
    };

    std::string st = get_field("status");
    std::string en = get_field("enabled");
    std::string lvl = get_field("level_db");
    std::string frq = get_field("freq_hz");
    std::string del = get_field("delay_us");
    std::string phs = get_field("phase_apf_hz");
    std::string trm = get_field("center_trim_db");
    std::string shd = get_field("shadow_hz");
    std::string bk = get_field("backend");
    std::string tgt = get_field("target");
    std::string sr = get_field("sample_rate");
    std::string buf = get_field("buffer_frames");

    std::cout << "  State:          " << (st == "running" ? "● Running" : "○ Stopped") << "\n"
              << "  Filter:         " << (en == "true" ? "ON (Processing active)" : "OFF (Bypassed)") << "\n"
              << "  Blend Level:    " << lvl << " dB\n"
              << "  Crossover:      " << frq << " Hz\n"
              << "  Delay (ITD):    " << (del.empty() ? "280" : del) << " µs\n"
              << "  Phase All-Pass: " << (phs.empty() ? "1500" : phs) << " Hz\n"
              << "  Center Trim:    " << (trm.empty() ? "-1.5" : trm) << " dB\n"
              << "  Head Shadow:    " << (shd.empty() ? "3000" : shd) << " Hz\n"
              << "  Backend:        " << bk << "\n"
              << "  Output Sink:    " << tgt << "\n"
              << "  Sample Rate:    " << sr << " Hz\n"
              << "  Buffer Size:    " << buf << " frames\n"
              << "========================================\n";
    return 0;
}

static int cmd_toggle() {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "TOGGLE", resp)) {
        std::cerr << "Error: Crossfeed engine is not running.\n"
                  << "Start it first with: crossfeed start\n";
        return 1;
    }
    bool enabled = resp.find("\"enabled\": true") != std::string::npos;
    std::cout << "Crossfeed: " << (enabled ? "ON" : "OFF (Bypassed)") << "\n";
    return 0;
}

static int cmd_set_enabled(bool enabled) {
    std::string cmd = enabled ? "ON" : "OFF";
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), cmd, resp)) {
        std::cerr << "Error: Crossfeed engine is not running.\n";
        return 1;
    }
    std::cout << "Crossfeed: " << (enabled ? "ON" : "OFF (Bypassed)") << "\n";
    return 0;
}

static int cmd_set(int argc, char** argv) {
    std::string cmd = "SET";
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--level" && i + 1 < argc) {
            cmd += " level=" + std::string(argv[++i]);
        } else if (arg == "--freq" && i + 1 < argc) {
            cmd += " freq=" + std::string(argv[++i]);
        } else if (arg == "--delay" && i + 1 < argc) {
            cmd += " delay=" + std::string(argv[++i]);
        } else if (arg == "--phase" && i + 1 < argc) {
            cmd += " phase=" + std::string(argv[++i]);
        } else if (arg == "--trim" && i + 1 < argc) {
            cmd += " trim=" + std::string(argv[++i]);
        } else if (arg == "--shadow" && i + 1 < argc) {
            cmd += " shadow=" + std::string(argv[++i]);
        } else if (arg == "--enabled" && i + 1 < argc) {
            cmd += " enabled=" + std::string(argv[++i]);
        }
    }
    if (cmd == "SET") {
        std::cerr << "Usage: crossfeed set [--level <dB>] [--freq <Hz>] [--delay <us>] [--phase <Hz>] [--trim <dB>] [--shadow <Hz>] [--enabled <0|1>]\n";
        return 1;
    }

    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), cmd, resp)) {
        std::cerr << "Error: Crossfeed engine is not running.\n";
        return 1;
    }
    std::cout << "Settings applied successfully.\n";
    return cmd_status(false);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return run_gui(argc, argv);
    }

    std::string cmd = argv[1];
    if (cmd == "gui" || cmd == "app" || cmd == "--gui") {
        return run_gui(argc, argv);
    } else if (cmd == "run") {
        return cmd_run(argc, argv);
    } else if (cmd == "start" || cmd == "--daemon") {
        return cmd_start(argc, argv);
    } else if (cmd == "stop") {
        return cmd_stop();
    } else if (cmd == "restart") {
        cmd_stop();
        usleep(200000);
        return cmd_start(argc, argv);
    } else if (cmd == "status") {
        bool json = (argc > 2 && std::string(argv[2]) == "--json");
        return cmd_status(json);
    } else if (cmd == "toggle") {
        return cmd_toggle();
    } else if (cmd == "on") {
        return cmd_set_enabled(true);
    } else if (cmd == "off") {
        return cmd_set_enabled(false);
    } else if (cmd == "set") {
        return cmd_set(argc, argv);
    } else if (cmd == "bench") {
        run_benchmark();
        return 0;
    } else if (cmd == "--version" || cmd == "-v") {
        std::cout << "pipewire-crossfeed 2.1.0 (standalone)\n";
        return 0;
    } else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << cmd << "\n\n";
        print_help(argv[0]);
        return 1;
    }
}
