"""Shared pipewire plumbing and persisted-state handling for pipewire-crossfeed.

Used by crossfeed-gui.py; crossfeed-ab.sh mirrors the same math and file
formats in POSIX sh (see the "keep in sync" comments in both).

State model: every change is applied to the live filter-chain node with
pw-cli AND baked into the installed PipeWire conf (rendered from
crossfeed.conf.in), so PipeWire always comes back up in the last saved
state after a restart/reboot — no restore service, no init-system
involvement. state.json is the coordination point between concurrently
running tools (GUI instances, crossfeed-ab).
"""
import json
import math
import os
import subprocess

# Stamped with the tag version by scripts/build-binaries.sh at release time.
__version__ = "0.0.0-dev"

# The filter-chain node exposing the DSP params. The selectable sink users
# pick as their output device is named "crossfeed" — this is the hidden DSP
# node behind it.
NODE_NAME = "crossfeed_sink"

# Values the conf template renders at the default level.
FULL_GAIN2 = 0.316       # outL/outR "Gain 2" at the default level (~-10 dB)
FULL_DIR_GAIN = -1.5     # dirL/dirR "Gain" at that same default level
FULL_FREQ = 700.0        # dirL/dirR/xL/xR "Freq" default

LEVEL_MIN_DB = -30.0     # subtle, barely-there crossfeed
LEVEL_MAX_DB = -6.0      # strong crossfeed
LEVEL_DEFAULT_DB = 20 * math.log10(FULL_GAIN2)  # ~ -10.0 dB

FREQ_MIN = 200.0
FREQ_MAX = 2000.0

OFF_THRESHOLD_LINEAR = 0.01  # below this, treat outL:Gain 2 as "bypassed"

STATE_PATH = os.path.expanduser("~/.config/pipewire-crossfeed/state.json")
CONF_PATH = os.path.expanduser("~/.config/pipewire/pipewire.conf.d/crossfeed.conf")
DATA_DIR = os.path.expanduser("~/.local/share/pipewire-crossfeed")
TEMPLATE_NAME = "crossfeed.conf.in"


def get_node_id():
    try:
        out = subprocess.run(["pw-dump"], capture_output=True, text=True, check=True).stdout
        objs = json.loads(out)
    except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError):
        # Daemon not running, pw-dump missing, or output truncated mid-write.
        return None
    for obj in objs:
        if obj.get("info", {}).get("props", {}).get("node.name") == NODE_NAME:
            return obj["id"]
    return None


def db_to_linear(db):
    return 10 ** (db / 20.0)


def linear_to_db(linear):
    return 20 * math.log10(linear) if linear > 0 else LEVEL_MIN_DB


def compute_gains(enabled, level_db):
    """(gain2, dir_gain) for a state: the linear crossfeed mix gain, and the
    direct path's low-shelf cut scaled by the same fraction of full level.
    Keep in sync with the awk math in crossfeed-ab.sh."""
    if not enabled:
        return 0.0, 0.0
    linear = db_to_linear(level_db)
    return linear, FULL_DIR_GAIN * (linear / FULL_GAIN2)


def read_state(node_id):
    """Current (gain2, freq) of the live node, from pw-dump's JSON.

    Falls back to the conf defaults if the node/daemon is unreachable —
    callers treat the result as display state, not ground truth.
    """
    try:
        out = subprocess.run(
            ["pw-dump", str(node_id)], capture_output=True, text=True, check=True,
        ).stdout
        objs = json.loads(out)
    except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError):
        return FULL_GAIN2, FULL_FREQ
    gain2, freq = FULL_GAIN2, FULL_FREQ
    for obj in objs:
        for prop in obj.get("info", {}).get("params", {}).get("Props") or []:
            params = prop.get("params")
            if not isinstance(params, list):
                continue
            pairs = dict(zip(params[::2], params[1::2]))
            try:
                if "outL:Gain 2" in pairs:
                    gain2 = float(pairs["outL:Gain 2"])
                if "dirL:Freq" in pairs:
                    freq = float(pairs["dirL:Freq"])
            except (TypeError, ValueError):
                continue
    return gain2, freq


def apply_state(node_id, enabled, level_db, freq_hz):
    """Push the settings to the live filter-chain node.

    Returns True if pw-cli accepted them, False otherwise (daemon gone,
    stale node id after a PipeWire restart, ...) so callers can re-resolve
    the node id and retry instead of failing silently.
    """
    gain2, dir_gain = compute_gains(enabled, level_db)
    params = (
        '{ params = [ '
        f'"outL:Gain 2" {gain2:.6f} '
        f'"outR:Gain 2" {gain2:.6f} '
        f'"dirL:Gain" {dir_gain:.6f} '
        f'"dirR:Gain" {dir_gain:.6f} '
        f'"dirL:Freq" {freq_hz:.1f} '
        f'"dirR:Freq" {freq_hz:.1f} '
        f'"xL:Freq" {freq_hz:.1f} '
        f'"xR:Freq" {freq_hz:.1f} '
        "] }"
    )
    try:
        proc = subprocess.run(
            ["pw-cli", "set-param", str(node_id), "Props", params],
            capture_output=True, text=True,
        )
    except FileNotFoundError:
        return False
    return proc.returncode == 0


def _template_path():
    for cand in (
        os.path.join(DATA_DIR, TEMPLATE_NAME),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), TEMPLATE_NAME),
    ):
        if os.path.isfile(cand):
            return cand
    return None


def render_conf(enabled, level_db, freq_hz):
    """Bake a state into the installed PipeWire conf (atomically), so the
    filter comes up in exactly this state on the next PipeWire start.
    Replaces the old restore-at-boot mechanism. Returns False if the conf
    template can't be found (state.json is still authoritative then).
    Keep the placeholder names in sync with crossfeed.conf.in and the awk
    render in crossfeed-ab.sh / install.sh."""
    template = _template_path()
    if template is None:
        return False
    gain2, dir_gain = compute_gains(enabled, level_db)
    with open(template) as f:
        text = f.read()
    text = (
        text.replace("@GAIN2@", f"{gain2:.6f}")
            .replace("@DIR_GAIN@", f"{dir_gain:.6f}")
            .replace("@FREQ@", f"{freq_hz:.1f}")
    )
    os.makedirs(os.path.dirname(CONF_PATH), exist_ok=True)
    tmp_path = CONF_PATH + ".tmp"
    with open(tmp_path, "w") as f:
        f.write(text)
    os.replace(tmp_path, CONF_PATH)
    return True


def persist_state(enabled, level_db, freq_hz):
    """Record a state everywhere it needs to outlive this process: the
    state file (tool coordination + UI restore) and the installed conf
    (PipeWire restart/reboot)."""
    os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
    tmp_path = STATE_PATH + ".tmp"
    with open(tmp_path, "w") as f:
        json.dump({"enabled": enabled, "level_db": level_db, "freq_hz": freq_hz}, f)
    os.replace(tmp_path, STATE_PATH)
    render_conf(enabled, level_db, freq_hz)


def load_persisted_state():
    """Returns (enabled, level_db, freq_hz), or None if nothing's been saved yet."""
    try:
        with open(STATE_PATH) as f:
            data = json.load(f)
        return (
            bool(data.get("enabled", True)),
            float(data.get("level_db", LEVEL_DEFAULT_DB)),
            float(data.get("freq_hz", FULL_FREQ)),
        )
    except (FileNotFoundError, json.JSONDecodeError, ValueError, TypeError):
        return None


def state_mtime():
    """mtime of STATE_PATH, or None if it doesn't exist yet.

    Lets multiple GUI instances (and crossfeed-ab.sh) coordinate through the
    state file itself rather than by re-reading live pipewire params, which
    is what caused a GUI's own just-applied change to look like an "external"
    change and get reverted a second later.
    """
    try:
        return os.path.getmtime(STATE_PATH)
    except FileNotFoundError:
        return None
