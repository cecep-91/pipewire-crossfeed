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
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

import crossfeed_lib as cf


def _clamp(value, lo, hi):
    return max(lo, min(hi, value))


class CrossfeedWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="Crossfeed")
        self.set_resizable(False)
        self.set_default_size(380, -1)
        self.set_icon_name("audio-volume-high")

        self.header = Gtk.HeaderBar(show_close_button=True, title="Crossfeed")
        self.set_titlebar(self.header)

        # The switch lives in the header bar on both pages, but is only
        # shown once the filter node is actually reachable.
        self.switch = Gtk.Switch(valign=Gtk.Align.CENTER)
        self.switch.set_no_show_all(True)
        self.switch.connect("state-set", self.on_switch)
        self.header.pack_end(self.switch)

        self._suppress = False
        self._state_mtime = None
        self._polling = False

        self.node_id = cf.get_node_id()
        if self.node_id is None:
            self._build_error_page()
        else:
            self._build_controls()

    # ------------------------------------------------------------- pages

    def _set_page(self, widget):
        child = self.get_child()
        if child is not None:
            self.remove(child)
        self.add(widget)
        widget.show_all()

    def _build_error_page(self):
        self.switch.hide()
        self.header.set_subtitle("filter not loaded")

        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        page.set_margin_top(24)
        page.set_margin_bottom(24)
        page.set_margin_start(24)
        page.set_margin_end(24)

        icon = Gtk.Image.new_from_icon_name(
            "audio-volume-muted-symbolic", Gtk.IconSize.DIALOG)
        msg = Gtk.Label(
            label="The crossfeed filter isn't loaded.\n"
                  "Restart PipeWire (on systemd: filter-chain.service),\n"
                  "then try again."
        )
        msg.set_justify(Gtk.Justification.CENTER)
        retry = Gtk.Button(label="Try Again", halign=Gtk.Align.CENTER)
        retry.get_style_context().add_class("suggested-action")
        retry.connect("clicked", self.on_retry)

        page.pack_start(icon, False, False, 0)
        page.pack_start(msg, False, False, 0)
        page.pack_start(retry, False, False, 6)
        self._set_page(page)

    def _slider_section(self, title, caption, adjustment, digits, climb_rate, mark):
        """A titled slider block: bold title + spin button, dim caption,
        and a full-width scale with a tick at the conf default value."""
        section = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)

        head = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        title_label = Gtk.Label(xalign=0)
        title_label.set_markup(f"<b>{title}</b>")
        spin = Gtk.SpinButton(adjustment=adjustment,
                              climb_rate=climb_rate, digits=digits)
        head.pack_start(title_label, True, True, 0)
        head.pack_end(spin, False, False, 0)

        cap = Gtk.Label(xalign=0)
        cap.set_markup(f"<small>{GLib.markup_escape_text(caption)}</small>")
        cap.get_style_context().add_class("dim-label")

        scale = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL,
                          adjustment=adjustment)
        scale.set_digits(digits)
        scale.set_draw_value(False)
        scale.set_hexpand(True)
        scale.add_mark(mark, Gtk.PositionType.BOTTOM, None)

        section.pack_start(head, False, False, 0)
        section.pack_start(cap, False, False, 0)
        section.pack_start(scale, False, False, 0)
        return section, scale, spin

    def _build_controls(self):
        self.switch.set_no_show_all(False)
        self.switch.show()

        # Dimmed as a whole while crossfeed is bypassed; the switch stays
        # in the header bar so it's always reachable.
        self.sliders_box = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL, spacing=14)
        self.sliders_box.set_margin_top(14)
        self.sliders_box.set_margin_bottom(18)
        self.sliders_box.set_margin_start(18)
        self.sliders_box.set_margin_end(18)

        level_adj = Gtk.Adjustment(
            value=cf.LEVEL_DEFAULT_DB, lower=cf.LEVEL_MIN_DB, upper=cf.LEVEL_MAX_DB,
            step_increment=0.5, page_increment=2.0,
        )
        section, self.level_scale, self.level_spin = self._slider_section(
            "Level (dB)", "How strongly each channel is mixed into the other",
            level_adj, digits=1, climb_rate=0.5, mark=cf.LEVEL_DEFAULT_DB,
        )
        self.sliders_box.pack_start(section, False, False, 0)
        level_adj.connect("value-changed", self.on_value_changed)

        freq_adj = Gtk.Adjustment(
            value=cf.FULL_FREQ, lower=cf.FREQ_MIN, upper=cf.FREQ_MAX,
            step_increment=10, page_increment=50,
        )
        section, self.freq_scale, self.freq_spin = self._slider_section(
            "Crossover frequency (Hz)", "Only frequencies below this bleed across",
            freq_adj, digits=0, climb_rate=1, mark=cf.FULL_FREQ,
        )
        self.sliders_box.pack_start(section, False, False, 0)
        freq_adj.connect("value-changed", self.on_value_changed)

        self._set_page(self.sliders_box)

        self.restore_persisted_if_needed()
        persisted = cf.load_persisted_state()
        if persisted is not None:
            # Initialize from the saved state, not the live params: while
            # bypassed the live gain is 0, which says nothing about the
            # level the user last chose — showing the default here meant
            # the next toggle-on silently overwrote the saved level.
            enabled, level_db, freq_hz = persisted
            self._show_state(enabled, level_db, freq_hz)
        else:
            self.sync_from_pipewire(initial=True)

        self._state_mtime = cf.state_mtime()
        if not self._polling:
            self._polling = True
            GLib.timeout_add(1000, self.poll_state_file)

    def on_retry(self, _button):
        self.node_id = cf.get_node_id()
        if self.node_id is not None:
            self.header.set_subtitle(None)
            self._build_controls()

    # ------------------------------------------------------------- state

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

    def _show_state(self, enabled, level_db, freq_hz):
        """Reflect a state in the widgets without re-applying it."""
        level_db = _clamp(level_db, cf.LEVEL_MIN_DB, cf.LEVEL_MAX_DB)
        freq_hz = _clamp(freq_hz, cf.FREQ_MIN, cf.FREQ_MAX)
        self._suppress = True
        self.switch.set_active(enabled)
        self.level_scale.set_value(level_db)
        self.freq_scale.set_value(freq_hz)
        self._suppress = False
        self.sliders_box.set_sensitive(enabled)
        self.update_status(enabled, level_db, freq_hz)

    def _apply_and_update(self):
        enabled = self.switch.get_active()
        level_db = self.level_scale.get_value()
        freq_hz = self.freq_scale.get_value()
        if not cf.apply_state(self.node_id, enabled, level_db, freq_hz):
            # PipeWire may have restarted under us, leaving our node id
            # stale — re-resolve it and retry once.
            node_id = cf.get_node_id()
            if node_id is not None and node_id != self.node_id:
                self.node_id = node_id
                cf.apply_state(self.node_id, enabled, level_db, freq_hz)
        cf.save_persisted_state(enabled, level_db, freq_hz)
        # We just wrote the state file ourselves — remember its mtime so
        # poll_state_file doesn't mistake our own change for an external one.
        self._state_mtime = cf.state_mtime()
        self.sliders_box.set_sensitive(enabled)
        self.update_status(enabled, level_db, freq_hz)

    def sync_from_pipewire(self, initial=False):
        gain2, freq = cf.read_state(self.node_id)
        enabled = gain2 > cf.OFF_THRESHOLD_LINEAR
        if enabled:
            level_db = cf.linear_to_db(gain2)
        elif initial:
            level_db = cf.LEVEL_DEFAULT_DB
        else:
            level_db = self.level_scale.get_value()
        self._show_state(enabled, level_db, freq)

    def update_status(self, enabled, level_db, freq_hz):
        if enabled:
            pct = cf.db_to_linear(level_db) / cf.FULL_GAIN2 * 100.0
            self.header.set_subtitle(
                f"{level_db:.1f} dB ({pct:.0f}%) · {freq_hz:.0f} Hz")
        else:
            self.header.set_subtitle("bypassed")

    # ----------------------------------------------------------- signals

    def on_switch(self, switch, state):
        if self._suppress:
            return False
        GLib.idle_add(self._apply_and_update)
        return False

    def on_value_changed(self, adjustment):
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
        self._show_state(enabled, level_db, freq_hz)
        return True


if __name__ == "__main__":
    if "--version" in sys.argv:
        print(f"pipewire-crossfeed {cf.__version__}")
        sys.exit(0)
    win = CrossfeedWindow()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()
