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

It installs as a plain, always-selectable sink named **Crossfeed**: pick it
as your output device in Settings > Sound (or `pavucontrol`), the same way on
every WirePlumber version and every distro. If you also chain an effects app
in front (EasyEffects, JamesDSP, ...), point *that app's* output device at
Crossfeed instead — see [EasyEffects and friends](#easyeffects-and-friends)
below. Crossfeed doesn't rely on WirePlumber's "smart filter" auto-insertion
feature, which behaves inconsistently across versions and can't intercept
effects apps that manage their own output link anyway — using an explicit
sink avoids both problems.

## Contents

| File | Purpose |
|---|---|
| `crossfeed.conf` | The null-sink + filter-chain graph — a PipeWire `context.objects`/`context.modules` drop-in |
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

1. Route audio through the filter: open **Settings > Sound** (or
   `pavucontrol`) and set **Crossfeed** as your output device. This is the
   same step regardless of WirePlumber version or distro.

   Do **not** set Crossfeed as your *system default* output — leave that on
   your real device (speakers/headphones). The DSP plays to whatever the
   system default is, so pointing the default at Crossfeed itself creates a
   feedback loop (audible as rapid clicking/stutter).

   If you also chain an effects app in front (EasyEffects, JamesDSP, ...),
   point *that app's* output device at Crossfeed instead of setting it as
   your own — see [EasyEffects and friends](#easyeffects-and-friends).

2. Launch **Crossfeed Control** from the app menu, or run `crossfeed-gui`.
   Use the switch to bypass/enable, and the two sliders (or their spin-button
   fields, for exact values) to adjust blend level and crossover frequency.

   For a quick headless toggle instead (e.g. bound to a keyboard shortcut),
   run `crossfeed-ab`.

## EasyEffects and friends

EasyEffects (and similar apps like JamesDSP) claims application streams into
its own sink and links its output directly to its configured device — it
manages its own routing rather than following whatever your default sink is.
Crossfeed's null sink handles this fine: set EasyEffects' **output device**
(top of the Output tab) to **Crossfeed**, same as you would for any other
app. The chain becomes:

```
apps → EasyEffects → Crossfeed → (crossfeed DSP) → default output
```

Two rules:

- Keep your *system default* output on the real device (speakers/headphones),
  not on Crossfeed — the DSP plays to `@DEFAULT_SINK@`, so pointing the
  default at Crossfeed itself would loop (this is the same rule as the
  install step above, worth repeating here since it's the most common way
  people trip over it).
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
