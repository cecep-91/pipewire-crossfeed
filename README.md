# pipewire-crossfeed (v2.0 Standalone)

A high-performance, standalone headphone crossfeed audio processor for Linux with a native GTK3 GUI, system tray indicator, and universal sound server support.

Crossfeed blends a subtle low-passed acoustic delay of each stereo channel into the opposite channel (mimicking how sound naturally reaches both ears from speakers in a room), reducing listening fatigue from hard-panned recordings on headphones.

Designed from the ground up to be **100% standalone, distro-agnostic, configuration-free, and packaged as a single native binary**.

---

## Key Features

- **Single Unified Native Application**:
  - The entire suite (GUI, System Tray, audio DSP engine, service management, and CLI) is compiled into a single ultra-lightweight C++ binary (`bin/crossfeed`).
  - Zero Python dependencies and zero shell wrapper scripts.
- **System Tray Integration**:
  - Full Ayatana AppIndicator status notifier support across GNOME, KDE Plasma, XFCE, Sway, Hyprland, and other Wayland/X11 desktops.
  - **Window Hide-on-Close**: Closing the window hides it into the system tray instead of exiting, keeping crossfeed running in the background.
  - Context menu allows quick toggling (On / Bypass), preset switching, showing the window, starting/stopping the engine, and cleanly quitting.
- **Zero Configuration Dependencies**:
  - No drop-in files in `~/.config/pipewire/` or `/etc`. No server restarts or session re-logins required. When the engine runs, it attaches to your active audio output dynamically; when stopped, it restores default routing cleanly.
- **Universal Sound Server Support**: Runs on **any Linux distribution** across **any sound server**:
  - **PipeWire** (Direct Native SPA Filter with zero-latency in-line processing)
  - **PulseAudio** (Native low-latency client)
  - **ALSA** (Direct hardware PCM for minimal setups with no audio daemon)
  - Automatic backend detection (`--backend auto`, `pulse`, or `alsa`).
- **Seamless Coexistence with Audio Processors (EasyEffects, JamesDSP, etc.)**:
  - Automatic stream disambiguation and cycle detection.
  - When EasyEffects or external DSP processors are running, Crossfeed automatically prioritizes processed streams without comb-filtering or volume bleed.
- **Ultra-High Performance C++ DSP**:
  - Built with optimized C++17 and Robert Bristow-Johnson Direct Form II Transposed biquad filters.
  - **Throughput**: > 180,000,000 stereo frames/sec.
  - **CPU Usage**: **< 0.03% of a single CPU core** in real-time playback.
  - **DSP Latency**: Sub-microsecond (1.4 µs) computation per buffer.
- **Packaging Support for Major Linux Distributions**:
  - Built-in generators for `.xbps` (Void Linux), `.deb` (Debian/Ubuntu), and `.rpm` (Fedora/RHEL/openSUSE).

---

## Distro Packaging

You can build distribution packages directly using `make`:

```sh
# Build all packages (.xbps, .deb, and .rpm) into dist/
make pkg

# Or build individual packages:
make xbps   # Void Linux (.xbps)
make deb    # Debian, Ubuntu, Linux Mint (.deb)
make rpm    # Fedora, RHEL, openSUSE (.rpm)
```

Packages are placed in the `dist/` directory and install:
- `/usr/bin/crossfeed`
- `/usr/share/applications/crossfeed.desktop`
- `/usr/share/icons/hicolor/scalable/apps/crossfeed.svg`
- `/usr/share/licenses/crossfeed/LICENSE`

### Installing Generated Packages

```sh
# Void Linux
sudo xbps-install -R dist crossfeed

# Debian / Ubuntu
sudo dpkg -i dist/crossfeed_2.0.0-1_amd64.deb

# Fedora / RHEL / openSUSE
sudo rpm -ivh dist/crossfeed-2.0.0-1.x86_64.rpm
```

---

## Build from Source & Local Installation

### Prerequisites

| Distro | Dependencies |
|---|---|
| **Void Linux** | `sudo xbps-install -S base-devel pipewire-devel pulseaudio-devel alsa-lib-devel gtk+3-devel libayatana-appindicator-devel` |
| **Ubuntu / Debian** | `sudo apt install build-essential libpipewire-0.3-dev libpulse-dev libasound2-dev libgtk-3-dev libayatana-appindicator3-dev` |
| **Fedora** | `sudo dnf install gcc-c++ make pipewire-devel pulseaudio-libs-devel alsa-lib-devel gtk3-devel libayatana-appindicator-devel` |
| **Arch Linux** | `sudo pacman -S --needed base-devel pipewire libpulse alsa-lib gtk3 libayatana-appindicator` |

### Build & Install

```sh
git clone https://github.com/ikuu/pipewire-crossfeed.git
cd pipewire-crossfeed
make
./install.sh
```

To uninstall:
```sh
./install.sh --uninstall
```

---

## Usage

### 1. Graphical Interface & System Tray

Launch the GUI:
```sh
crossfeed
```
Or search for **Crossfeed** in your desktop application launcher / app menu.

- **Master Switch**: Toggle crossfeed filtering on/off (bypass).
- **Blend Level Slider**: Adjust opposite-ear feed intensity (-30.0 dB to -6.0 dB, default -10.0 dB).
- **Crossover Frequency Slider**: Adjust acoustic cutoff frequency (200 Hz to 2000 Hz, default 700 Hz).
- **Presets**: One-click Subtle, Default (Bauer), and Strong presets.
- **System Tray**: Closing the window hides it into the notification tray. Right-click the tray icon to toggle, change presets, or quit.

### 2. Command Line Controls

The single binary also serves as a complete CLI control utility:

```sh
# Start the background daemon
crossfeed start

# Check status and active audio routing
crossfeed status

# Toggle crossfeed filtering (great for keyboard shortcuts)
crossfeed toggle

# Force on or off (bypassed)
crossfeed on
crossfeed off

# Adjust parameters on the fly
crossfeed set --level -12.0 --freq 650

# Stop the engine cleanly
crossfeed stop

# Run DSP throughput and CPU performance benchmark
crossfeed bench
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
