# pipewire-crossfeed (v2.1 Standalone)

A high-performance, standalone headphone crossfeed audio processor for Linux with a native GTK3 GUI, system tray indicator, and universal sound server support.

Crossfeed blends a subtle low-passed acoustic delay of each stereo channel into the opposite channel (mimicking how sound naturally reaches both ears from speakers in a room), reducing listening fatigue from hard-panned recordings on headphones.

Designed from the ground up to be **100% standalone, distro-agnostic, configuration-free, and packaged as a single native binary**.

---

## Key Features

- **Single Unified Native Application**:
  - The entire suite (GUI, System Tray, audio DSP engine, service management, and CLI) is compiled into a single ultra-lightweight C++ binary (`bin/crossfeed`).
  - Zero Python dependencies and zero shell wrapper scripts.
- **Advanced Psychoacoustic & Spatial Audio DSP**:
  - **Interaural Time Delay (ITD)**: Microsecond-precision continuous fractional delay line (0–800 µs) simulating acoustic travel time around the skull.
  - **Phase Alignment All-Pass Filter**: 2nd-order all-pass filter (200–4000 Hz) eliminating comb filtering and preserving bass impact.
  - **Center Summing Trim**: Negative master gain (-6.0 to 0.0 dB) compensating for acoustic center build-up from L+R summing.
  - **Acoustic Head Shadow**: High-frequency cutoff filter (1000–8000 Hz) modeling ear pinna and skull absorption.
  - **"Not for me options :v" Expander**: Clean UI keeps advanced controls and emulation presets neatly organized and visible only when requested.
  - **Acoustic Emulation Presets**: One-click switching between Jan Meier (Corda), Chu Moy (HeadWize), Bauer BS2B, Siegfried Linkwitz Monitor, and Natural Studio 30°.
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
  - Built with optimized C++17, circular delay buffers, and Direct Form II Transposed biquad filters.
  - **Throughput**: > 80,000,000 stereo frames/sec.
  - **CPU Usage**: **< 0.06% of a single CPU core** in real-time playback.
  - **DSP Latency**: Sub-microsecond (2.9 µs per 256 frames).
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
sudo dpkg -i dist/crossfeed_2.1.0-1_amd64.deb

# Fedora / RHEL / openSUSE
sudo rpm -ivh dist/crossfeed-2.1.0-1.x86_64.rpm
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
- **"Not for me options :v" Expander**:
  - **Acoustic Emulation Presets**: Jan Meier (Corda), Chu Moy (HeadWize), Bauer BS2B, Linkwitz Monitor, Natural Studio 30°.
  - **Acoustic Delay (ITD)**: 0 to 800 µs delay slider.
  - **Phase Alignment**: 200 to 4000 Hz all-pass filter slider.
  - **Center Summing Trim**: -6.0 to 0.0 dB gain slider.
  - **Head Shadow Cutoff**: 1000 to 8000 Hz low-pass filter slider.
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
crossfeed set --level -12.0 --freq 650 --delay 280 --phase 1500 --trim -1.5 --shadow 3000

# Stop the engine cleanly
crossfeed stop

# Run DSP throughput and CPU performance benchmark
crossfeed bench
```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
