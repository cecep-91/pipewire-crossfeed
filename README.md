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

It registers as a virtual sink named **Crossfeed** that you select as your output
device; internally it plays out via PipeWire's `@DEFAULT_SINK@` target, which
always resolves to your current default hardware output — it does not hardcode
a device, so it follows you if you plug in headphones, switch to speakers, etc.

> **If you also use EasyEffects** (or another virtual-sink effects chain) in
> front of Crossfeed: `@DEFAULT_SINK@` resolves to the actual default *device*,
> not "wherever new app streams get auto-routed." Don't remove `target.object`
> entirely and leave it target-less — on a system where EasyEffects claims new
> streams by default, an untargeted `crossfeed_out` will link back into
> EasyEffects' own sink, creating a feedback loop (EasyEffects → Crossfeed →
> EasyEffects → …) that produces silence instead of an error.

## Contents

| File | Purpose |
|---|---|
| `crossfeed.conf` | The filter-chain graph — drop-in for `~/.config/pipewire/filter-chain.conf.d/` |
| `crossfeed-gui.py` | GTK control panel: on/off switch, level (dB) and crossover frequency (Hz) sliders with precise spin-button entry |
| `crossfeed-ab.sh` | Headless one-shot bypass toggle (e.g. for a keybinding) |
| `install.sh` | Installs the conf + GUI launcher for the current user |

## Requirements

**System packages** (Debian/Ubuntu):

```sh
sudo apt install pipewire wireplumber python3-gi gir1.2-gtk-3.0 jq libnotify-bin
```

- `pipewire` / `wireplumber` — provide `libpipewire-module-filter-chain` and the
  `filter-chain.service` user unit that loads `filter-chain.conf.d/*.conf`.
  Most current desktop distros (Ubuntu 22.10+, Fedora, etc.) ship these already.
- `python3-gi` + `gir1.2-gtk-3.0` — GTK 3 bindings for the control panel
  (see `requirements.txt` for why this is an apt package, not a pip one).
- `jq` / `libnotify-bin` — used by `crossfeed-ab.sh` to read PipeWire's state
  and show a toggle notification.

## Install

1. Clone or copy this directory anywhere you like, then run the installer:

   ```sh
   ./install.sh
   ```

   This copies `crossfeed.conf` into `~/.config/pipewire/filter-chain.conf.d/`,
   restarts `filter-chain.service`, and adds a "Crossfeed Control" launcher to
   your app grid.

2. Open **Settings > Sound** (or `pavucontrol`) and set **Crossfeed** as your
   output device.

3. Launch **Crossfeed Control** from the app grid, or:

   ```sh
   python3 crossfeed-gui.py
   ```

   Use the switch to bypass/enable, and the two sliders (or their spin-button
   fields, for exact values) to adjust blend level and crossover frequency.

   For a quick headless toggle instead (e.g. bound to a keyboard shortcut):

   ```sh
   ./crossfeed-ab.sh
   ```

## Uninstall

```sh
rm ~/.config/pipewire/filter-chain.conf.d/crossfeed.conf
rm ~/.local/share/applications/crossfeed-control.desktop
systemctl --user restart filter-chain.service
```
