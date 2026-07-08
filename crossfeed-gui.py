#!/usr/bin/env python3
"""Crossfeed control UI — GTK front-end for the pipewire crossfeed filter-chain.

Same live-tweak trick as crossfeed-ab.sh (pw-cli set-param on a running
filter-chain node, no restart/dropout) but exposed as a switch plus
level (dB) and crossover frequency (Hz) sliders. Every change is saved to
crossfeed_lib.STATE_PATH and reapplied on the next filter-chain start by
crossfeed-restore.py, so settings survive reboots/logout.

If you open more than one instance of this GUI (or run crossfeed-ab.sh
while a GUI is open), they coordinate through that same state file: each
instance polls its mtime and mirrors whatever the others last wrote,
rather than re-reading live pipewire params (which caused a GUI's own
just-applied change to look "external" and get reverted a second later).
"""
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

import crossfeed_lib as cf


class CrossfeedWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="Crossfeed")
        self.set_border_width(16)
        self.set_default_size(340, -1)
        self.set_resizable(False)

        self.node_id = cf.get_node_id()
        self._suppress = False
        self._state_mtime = None

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        self.add(box)

        if self.node_id is None:
            box.add(Gtk.Label(label="crossfeed_sink not found.\nIs filter-chain.service running?"))
            return

        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        header.pack_start(Gtk.Label(label="Crossfeed"), False, False, 0)
        self.switch = Gtk.Switch()
        self.switch.connect("state-set", self.on_switch)
        header.pack_end(self.switch, False, False, 0)
        box.add(header)

        box.add(Gtk.Label(label="Level (dB)", xalign=0))
        level_adj = Gtk.Adjustment(
            value=cf.LEVEL_DEFAULT_DB, lower=cf.LEVEL_MIN_DB, upper=cf.LEVEL_MAX_DB,
            step_increment=0.5, page_increment=2.0,
        )
        level_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.level_scale = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL, adjustment=level_adj)
        self.level_scale.set_digits(1)
        self.level_scale.set_hexpand(True)
        self.level_spin = Gtk.SpinButton(adjustment=level_adj, climb_rate=0.5, digits=1)
        level_row.pack_start(self.level_scale, True, True, 0)
        level_row.pack_start(self.level_spin, False, False, 0)
        box.add(level_row)
        level_adj.connect("value-changed", self.on_level_changed)

        box.add(Gtk.Label(label="Crossover Frequency (Hz)", xalign=0))
        freq_adj = Gtk.Adjustment(
            value=cf.FULL_FREQ, lower=cf.FREQ_MIN, upper=cf.FREQ_MAX,
            step_increment=10, page_increment=50,
        )
        freq_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.freq_scale = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL, adjustment=freq_adj)
        self.freq_scale.set_digits(0)
        self.freq_scale.set_hexpand(True)
        self.freq_spin = Gtk.SpinButton(adjustment=freq_adj, climb_rate=1, digits=0)
        freq_row.pack_start(self.freq_scale, True, True, 0)
        freq_row.pack_start(self.freq_spin, False, False, 0)
        box.add(freq_row)
        freq_adj.connect("value-changed", self.on_freq_changed)

        self.status = Gtk.Label(label="")
        self.status.set_xalign(0)
        box.add(self.status)

        self.restore_persisted_if_needed()
        self.sync_from_pipewire(initial=True)
        self._state_mtime = cf.state_mtime()
        GLib.timeout_add(1000, self.poll_state_file)

    def restore_persisted_if_needed(self):
        # Belt-and-suspenders: normally crossfeed-restore.service already
        # reapplied the saved settings when filter-chain.service started. If
        # that unit isn't installed/enabled, or the GUI won races it, apply
        # the saved settings ourselves so they aren't silently ignored.
        persisted = cf.load_persisted_state()
        if persisted is None:
            return
        if not cf.wait_for_conf_init(self.node_id, timeout=5.0):
            return
        enabled, level_db, freq_hz = persisted
        gain2, freq = cf.read_state(self.node_id)
        cur_gain2 = cf.db_to_linear(level_db) if enabled else 0.0
        if abs(gain2 - cur_gain2) > 0.01 or abs(freq - freq_hz) > 1.0:
            cf.apply_state(self.node_id, enabled, level_db, freq_hz)

    def _apply_and_update(self):
        enabled = self.switch.get_active()
        level_db = self.level_scale.get_value()
        freq_hz = self.freq_scale.get_value()
        cf.apply_state(self.node_id, enabled, level_db, freq_hz)
        cf.save_persisted_state(enabled, level_db, freq_hz)
        # We just wrote the state file ourselves — remember its mtime so
        # poll_state_file doesn't mistake our own change for an external one.
        self._state_mtime = cf.state_mtime()
        self.update_status(enabled, level_db, freq_hz)

    def sync_from_pipewire(self, initial=False):
        gain2, freq = cf.read_state(self.node_id)
        enabled = gain2 > cf.OFF_THRESHOLD_LINEAR
        level_db = cf.linear_to_db(gain2) if enabled else (
            cf.LEVEL_DEFAULT_DB if initial else self.level_scale.get_value()
        )
        level_db = max(cf.LEVEL_MIN_DB, min(cf.LEVEL_MAX_DB, level_db))
        freq = max(cf.FREQ_MIN, min(cf.FREQ_MAX, freq))

        self._suppress = True
        self.switch.set_active(enabled)
        self.level_scale.set_value(level_db)
        self.freq_scale.set_value(freq)
        self._suppress = False
        self.update_status(enabled, level_db, freq)

    def update_status(self, enabled, level_db, freq_hz):
        pct = cf.db_to_linear(level_db) / cf.FULL_GAIN2 * 100.0
        self.status.set_text(
            f"{level_db:.1f} dB ({pct:.0f}%) · {freq_hz:.0f} Hz · "
            f"{'ON' if enabled else 'OFF'}"
        )

    def on_switch(self, switch, state):
        if self._suppress:
            return False
        GLib.idle_add(self._apply_and_update)
        return False

    def on_level_changed(self, adjustment):
        if self._suppress:
            return
        self._apply_and_update()

    def on_freq_changed(self, adjustment):
        if self._suppress:
            return
        self._apply_and_update()

    def poll_state_file(self):
        # Picks up changes made by crossfeed-ab.sh or another GUI instance,
        # via the shared state file rather than re-reading live pipewire
        # params — comparing against a live read caused a GUI's own
        # just-applied change to look "external" and get reverted a second
        # later (pw-cli round-trips aren't instantaneous/fully reliable).
        if self._suppress:
            return True
        mtime = cf.state_mtime()
        if mtime is None or mtime == self._state_mtime:
            return True
        self._state_mtime = mtime
        persisted = cf.load_persisted_state()
        if persisted is None:
            return True
        enabled, level_db, freq_hz = persisted
        level_db = max(cf.LEVEL_MIN_DB, min(cf.LEVEL_MAX_DB, level_db))
        freq_hz = max(cf.FREQ_MIN, min(cf.FREQ_MAX, freq_hz))
        self._suppress = True
        self.switch.set_active(enabled)
        self.level_scale.set_value(level_db)
        self.freq_scale.set_value(freq_hz)
        self._suppress = False
        self.update_status(enabled, level_db, freq_hz)
        return True


if __name__ == "__main__":
    win = CrossfeedWindow()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()
