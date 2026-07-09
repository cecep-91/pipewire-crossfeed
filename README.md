# pipewire-crossfeed

A headphone crossfeed filter for PipeWire, built with `libpipewire-module-filter-chain`,
plus a small GTK control panel and a shell one-shot toggle.

Crossfeed blends a low-passed amount of each channel into the other (mimicking how
sound naturally reaches both ears from stereo speakers), so hard-panned studio
recordings feel less harsh on headphones. This one implements it as:

- `dirL`/`dirR` — each channel's own "direct" path, with a small low-shelf cut
  (default -1.5 dB below 700 Hz) so it doesn't overpower the added crossfeed.
- `xL`/`xR` — the *opposite* channel, low-passed (default 700 Hz) before being
  mixed in, so only bass/mids bleed across.
- `outL`/`outR` — mixers recombining direct + crossfed signal.

With WirePlumber ≥ 0.5 it registers as a **smart filter**: WirePlumber inserts
it automatically in front of your default output for all audio — nothing to
select, and it follows you when you plug in headphones, switch to speakers,
etc. On WirePlumber 0.4 it appears as a virtual sink named **Crossfeed** that
you select as your output device instead.

> **If you also use EasyEffects** (or another virtual-sink effects chain),
> install with `./install.sh --easyeffects` — see
> [EasyEffects and friends](#easyeffects-and-friends) below. Effect apps like
> EasyEffects manage their own stream routing, which both bypasses smart-filter
> insertion and can't target a filter-chain node directly.

## Contents

| File | Purpose |
|---|---|
| `crossfeed.conf` | The filter-chain graph — a PipeWire `context.modules` drop-in (smart filter on WirePlumber ≥ 0.5) |
| `crossfeed-easyeffects.conf` | Variant exposing Crossfeed as a real selectable sink, for chaining behind EasyEffects etc. |
| `crossfeed-gui.py` | GTK control panel: on/off switch, level (dB) and crossover frequency (Hz) sliders with precise spin-button entry |
| `crossfeed-ab.sh` | Headless one-shot bypass toggle (e.g. for a keybinding) |
| `crossfeed-restore.py` | Reapplies your saved settings when the filter (re)starts |
| `install.sh` | Installs everything for the current user (`--uninstall` to remove) |

## Requirements

PipeWire + WirePlumber (with the `pw-cli`/`pw-dump` tools), GTK 3 Python
bindings for the control panel, and `jq`/`libnotify` for the quick toggle:

| Distro | Install command |
|---|---|
| Debian / Ubuntu | `sudo apt install pipewire wireplumber python3-gi gir1.2-gtk-3.0 jq libnotify-bin` |
| Void | `sudo xbps-install -S pipewire wireplumber python3-gobject gtk+3 jq libnotify` |
| Arch | `sudo pacman -S --needed pipewire wireplumber python-gobject gtk3 jq libnotify` |
| Fedora | `sudo dnf install pipewire wireplumber python3-gobject gtk3 jq libnotify` |
| openSUSE | `sudo zypper install pipewire wireplumber python3-gobject typelib-1_0-Gtk-3_0 jq libnotify-tools` |
| Alpine | `doas apk add pipewire wireplumber py3-gobject3 gtk+3.0 jq libnotify` |

If you install a **binary release** (see below), the Python/GTK bindings are
bundled — only PipeWire itself is needed on the host.

`requirements.txt` explains why PyGObject should come from your package
manager rather than pip.

## Install

### From a binary release

Grab the tarball for your architecture from the
[releases page](../../releases), then:

```sh
tar xzf pipewire-crossfeed-*-linux-*.tar.gz
cd pipewire-crossfeed-*/
./install.sh
```

The binaries are glibc builds (x86_64 and aarch64). On musl systems
(e.g. Void musl) install from source instead — it's just Python.

### From source

Clone this repo (or download a source tarball) and run:

```sh
./install.sh
```

Either way the installer:

- installs `crossfeed.conf` where your system will load it (see
  [Init systems](#init-systems) below),
- puts `crossfeed-gui`, `crossfeed-ab` and `crossfeed-restore` commands in
  `~/.local/bin/`,
- adds a **Crossfeed Control** launcher to your app menu,
- sets up state restore so your settings survive reboots.

Then:

1. Route audio through the filter:
   - **WirePlumber ≥ 0.5** (check `wireplumber --version`): nothing to do —
     the filter is inserted in front of your default output automatically
     (it appears under *Filters* in `wpctl status`, not as a selectable sink).
   - **WirePlumber 0.4**: open **Settings > Sound** (or `pavucontrol`) and set
     **Crossfeed** as your output device.
   - **Using EasyEffects?** Neither of the above will work — install with
     `./install.sh --easyeffects` instead and see
     [EasyEffects and friends](#easyeffects-and-friends).

2. Launch **Crossfeed Control** from the app menu, or run `crossfeed-gui`.
   Use the switch to bypass/enable, and the two sliders (or their spin-button
   fields, for exact values) to adjust blend level and crossover frequency.

   For a quick headless toggle instead (e.g. bound to a keyboard shortcut),
   run `crossfeed-ab`.

## EasyEffects and friends

EasyEffects (and similar apps like JamesDSP) claims application streams into
its own sink and links its output directly to its configured device. That
routing never "follows the default sink", so WirePlumber's smart-filter
insertion can't intercept it — and EasyEffects can't target a filter-chain
node either (WirePlumber hides those from device lists). The result: with the
standard conf, audio silently bypasses Crossfeed.

`crossfeed-easyeffects.conf` solves this by splitting Crossfeed in two: a
plain null sink named **Crossfeed** that shows up as a normal output device,
and a hidden filter-chain that taps its monitor and plays the processed
signal to your real default output.

```sh
./install.sh --easyeffects
```

Then in EasyEffects, set the **output device** (top of the Output tab) to
**Crossfeed**. The chain becomes:

```
apps → EasyEffects → Crossfeed → (crossfeed DSP) → default output
```

Two rules with this variant:

- Keep your *system default* output on the real device (speakers/headphones),
  not on Crossfeed — the DSP plays to `@DEFAULT_SINK@`, so pointing the
  default at Crossfeed itself would loop.
- EasyEffects only re-reads its config on restart; use its UI to switch the
  output device, or quit it first if you edit
  `~/.config/easyeffects/db/easyeffectsrc` by hand (it overwrites the file
  on exit).

## Init systems

**systemd** (Ubuntu, Fedora, Arch, openSUSE, ...): the conf goes in
`~/.config/pipewire/filter-chain.conf.d/`, loaded by the stock
`filter-chain.service` user unit, and a `crossfeed-restore.service` oneshot
reapplies your saved settings every time that service (re)starts.

**Everything else** (Void/runit, Artix, Alpine, or any setup where PipeWire
isn't systemd-managed): the conf goes in `~/.config/pipewire/pipewire.conf.d/`
instead, so the main PipeWire daemon loads the filter graph itself — no
service manager involved. Saved settings are reapplied at login via an XDG
autostart entry (`~/.config/autostart/crossfeed-restore.desktop`). If your
window manager doesn't run XDG autostart entries, add `crossfeed-restore` to
its startup script; if you restart PipeWire mid-session, run it again by hand.
After the first install, restart PipeWire (or log out and back in) to load
the filter.

## Uninstall

```sh
./install.sh --uninstall
```

Saved settings in `~/.config/pipewire-crossfeed/` are kept; delete that
directory too if you don't want them.

## Releases (maintainers)

CI (`.github/workflows/ci.yml`) shellchecks the scripts and does a full
PyInstaller build on every push. To cut a release:

```sh
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds self-contained x86_64 + aarch64 binary tarballs
(PyInstaller on ubuntu-22.04, so they run on any distro with glibc ≥ 2.35)
and publishes a GitHub release with them attached. `scripts/build-binaries.sh`
reproduces the build locally.
