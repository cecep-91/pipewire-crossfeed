#!/usr/bin/env python3
"""Crossfeed control UI — GTK front-end for the standalone crossfeed audio engine.

Communicates with the C++ crossfeed daemon via Unix domain socket IPC.
Works across any Linux distribution and sound server without touching configuration files.
"""
import sys
import os

# Add local directory to import path if running from repo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

import crossfeed_lib as cf


def _clamp(value, lo, hi):
    return max(lo, min(hi, value))


class CrossfeedWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="Crossfeed Control")
        self.set_resizable(False)
        self.set_default_size(400, -1)
        self.set_icon_name("audio-volume-high")

        self.header = Gtk.HeaderBar(show_close_button=True, title="Crossfeed")
        self.set_titlebar(self.header)

        self.switch = Gtk.Switch(valign=Gtk.Align.CENTER)
        self.switch.set_no_show_all(True)
        self.switch.connect("state-set", self.on_switch)
        self.header.pack_end(self.switch)

        self._suppress = False
        self._polling = False
        self._last_status = None

        status = cf.get_status()
        if status is None:
            self._build_stopped_page()
        else:
            self._build_controls(status)

        GLib.timeout_add(1000, self.poll_engine)

    def _set_page(self, widget):
        child = self.get_child()
        if child is not None:
            self.remove(child)
        self.add(widget)
        widget.show_all()

    def _build_stopped_page(self):
        self.switch.hide()
        self.header.set_subtitle("engine stopped")

        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        page.set_margin_top(24)
        page.set_margin_bottom(24)
        page.set_margin_start(24)
        page.set_margin_end(24)

        icon = Gtk.Image.new_from_icon_name("audio-volume-muted-symbolic", Gtk.IconSize.DIALOG)
        msg = Gtk.Label(label="The Crossfeed audio engine is currently stopped.")
        msg.set_justify(Gtk.Justification.CENTER)

        start_btn = Gtk.Button(label="Start Crossfeed Engine", halign=Gtk.Align.CENTER)
        start_btn.get_style_context().add_class("suggested-action")
        start_btn.connect("clicked", self.on_start_clicked)

        page.pack_start(icon, False, False, 0)
        page.pack_start(msg, False, False, 0)
        page.pack_start(start_btn, False, False, 6)
        self._set_page(page)

    def on_start_clicked(self, _button):
        cf.start_engine()
        GLib.timeout_add(300, self._check_after_start)

    def _check_after_start(self):
        st = cf.get_status()
        if st is not None:
            self._build_controls(st)
            return False
        return True

    def _slider_section(self, title, caption, adjustment, digits, climb_rate, mark):
        section = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)

        head = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        title_label = Gtk.Label(xalign=0)
        title_label.set_markup(f"<b>{title}</b>")
        spin = Gtk.SpinButton(adjustment=adjustment, climb_rate=climb_rate, digits=digits)
        head.pack_start(title_label, True, True, 0)
        head.pack_end(spin, False, False, 0)

        cap = Gtk.Label(xalign=0)
        cap.set_markup(f"<small>{GLib.markup_escape_text(caption)}</small>")
        cap.get_style_context().add_class("dim-label")

        scale = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL, adjustment=adjustment)
        scale.set_digits(digits)
        scale.set_draw_value(False)
        scale.set_hexpand(True)
        scale.add_mark(mark, Gtk.PositionType.BOTTOM, None)

        section.pack_start(head, False, False, 0)
        section.pack_start(cap, False, False, 0)
        section.pack_start(scale, False, False, 0)
        return section, scale, spin

    def _build_controls(self, status):
        self.switch.set_no_show_all(False)
        self.switch.show()

        self.sliders_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        self.sliders_box.set_margin_top(14)
        self.sliders_box.set_margin_bottom(18)
        self.sliders_box.set_margin_start(18)
        self.sliders_box.set_margin_end(18)

        cur_level = float(status.get("level_db", cf.LEVEL_DEFAULT_DB))
        level_adj = Gtk.Adjustment(
            value=cur_level, lower=cf.LEVEL_MIN_DB, upper=cf.LEVEL_MAX_DB,
            step_increment=0.5, page_increment=2.0,
        )
        section, self.level_scale, self.level_spin = self._slider_section(
            "Level (dB)", "How strongly each channel is mixed into the other",
            level_adj, digits=1, climb_rate=0.5, mark=cf.LEVEL_DEFAULT_DB,
        )
        self.sliders_box.pack_start(section, False, False, 0)
        level_adj.connect("value-changed", self.on_value_changed)

        cur_freq = float(status.get("freq_hz", cf.FULL_FREQ))
        freq_adj = Gtk.Adjustment(
            value=cur_freq, lower=cf.FREQ_MIN, upper=cf.FREQ_MAX,
            step_increment=10, page_increment=50,
        )
        section, self.freq_scale, self.freq_spin = self._slider_section(
            "Crossover frequency (Hz)", "Only frequencies below this bleed across",
            freq_adj, digits=0, climb_rate=1, mark=cf.FULL_FREQ,
        )
        self.sliders_box.pack_start(section, False, False, 0)
        freq_adj.connect("value-changed", self.on_value_changed)

        self._set_page(self.sliders_box)

        enabled = bool(status.get("enabled", True))
        self._show_state(enabled, cur_level, cur_freq, status)

    def _show_state(self, enabled, level_db, freq_hz, status=None):
        level_db = _clamp(level_db, cf.LEVEL_MIN_DB, cf.LEVEL_MAX_DB)
        freq_hz = _clamp(freq_hz, cf.FREQ_MIN, cf.FREQ_MAX)
        self._suppress = True
        self.switch.set_active(enabled)
        self.level_scale.set_value(level_db)
        self.freq_scale.set_value(freq_hz)
        self._suppress = False
        self.sliders_box.set_sensitive(enabled)
        self.update_status(enabled, level_db, freq_hz, status)

    def _apply_and_update(self):
        enabled = self.switch.get_active()
        level_db = self.level_scale.get_value()
        freq_hz = self.freq_scale.get_value()
        st = cf.set_params(enabled=enabled, level_db=level_db, freq_hz=freq_hz)
        self.sliders_box.set_sensitive(enabled)
        self.update_status(enabled, level_db, freq_hz, st)

    def update_status(self, enabled, level_db, freq_hz, status=None):
        if enabled:
            target = ""
            if status and status.get("target"):
                tgt = status.get("target")
                if "." in tgt:
                    tgt = tgt.split(".")[-1]
                target = f" · {tgt}"
            self.header.set_subtitle(f"{level_db:.1f} dB · {freq_hz:.0f} Hz{target}")
        else:
            self.header.set_subtitle("bypassed")

    def on_switch(self, switch, state):
        if self._suppress:
            return False
        GLib.idle_add(self._apply_and_update)
        return False

    def on_value_changed(self, adjustment):
        if self._suppress:
            return
        self._apply_and_update()

    def poll_engine(self):
        if self._suppress:
            return True
        st = cf.get_status()
        if st is None:
            if self._last_status is not None:
                self._last_status = None
                self._build_stopped_page()
            return True

        if self._last_status is None:
            self._last_status = st
            self._build_controls(st)
            return True

        # Check if external change occurred
        changed = (
            st.get("enabled") != self._last_status.get("enabled") or
            abs(float(st.get("level_db", 0)) - float(self._last_status.get("level_db", 0))) > 0.05 or
            abs(float(st.get("freq_hz", 0)) - float(self._last_status.get("freq_hz", 0))) > 1.0
        )
        if changed:
            self._last_status = st
            self._show_state(
                bool(st.get("enabled")),
                float(st.get("level_db")),
                float(st.get("freq_hz")),
                st
            )
        return True


if __name__ == "__main__":
    if "--version" in sys.argv:
        print(f"pipewire-crossfeed {cf.__version__}")
        sys.exit(0)
    win = CrossfeedWindow()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()
