"""Shared pipewire plumbing and persisted-state handling for pipewire-crossfeed.

Used by crossfeed-gui.py (interactive) and crossfeed-restore.py (headless,
run by systemd right after filter-chain.service starts) so both agree on
how settings are read/applied/saved.
"""
import json
import math
import os
import re
import subprocess
import time

NODE_NAME = "crossfeed_sink"

# Values baked into crossfeed.conf.
FULL_GAIN2 = 0.316       # outL/outR "Gain 2" at the conf's default level (~-10 dB)
FULL_DIR_GAIN = -1.5     # dirL/dirR "Gain" at that same default level
FULL_FREQ = 700.0        # dirL/dirR/xL/xR "Freq" in the conf

LEVEL_MIN_DB = -30.0     # subtle, barely-there crossfeed
LEVEL_MAX_DB = -6.0      # strong crossfeed
LEVEL_DEFAULT_DB = 20 * math.log10(FULL_GAIN2)  # ~ -10.0 dB, the conf's own default

FREQ_MIN = 200.0
FREQ_MAX = 2000.0

OFF_THRESHOLD_LINEAR = 0.01  # below this, treat outL:Gain 2 as "bypassed"

STATE_PATH = os.path.expanduser("~/.config/pipewire-crossfeed/state.json")

PARAM_RE = re.compile(
    r'"(outL:Gain 2|dirL:Freq)"\s*\n\s*Float ([\-0-9.]+)'
)


def get_node_id():
    out = subprocess.run(["pw-dump"], capture_output=True, text=True, check=True).stdout
    for obj in json.loads(out):
        if obj.get("info", {}).get("props", {}).get("node.name") == NODE_NAME:
            return obj["id"]
    return None


def db_to_linear(db):
    return 10 ** (db / 20.0)


def linear_to_db(linear):
    return 20 * math.log10(linear) if linear > 0 else LEVEL_MIN_DB


def read_state(node_id):
    out = subprocess.run(
        ["pw-cli", "enum-params", str(node_id), "Props"],
        capture_output=True, text=True,
    ).stdout
    vals = dict(PARAM_RE.findall(out))
    gain2 = float(vals.get("outL:Gain 2", FULL_GAIN2))
    freq = float(vals.get("dirL:Freq", FULL_FREQ))
    return gain2, freq


def wait_for_conf_init(node_id, timeout=5.0, poll_interval=0.1):
    """Block until the filter-chain module has seeded its own conf defaults.

    The node can appear in pw-dump before the module finishes setting its
    initial Freq/Q/Gain/biquad-coefficient values. Applying an override
    before that finishes only ever touches Freq/Gain (not Q or the b/a
    coefficients), permanently zeroing those out. crossfeed.conf's Freq is
    never legitimately 0, so use it as the "finished initializing" signal.
    Returns True once seen, False on timeout.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        _, freq = read_state(node_id)
        if freq > 0:
            return True
        time.sleep(poll_interval)
    return False


def apply_state(node_id, enabled, level_db, freq_hz):
    linear = db_to_linear(level_db)
    fraction = linear / FULL_GAIN2
    if enabled:
        gain2 = linear
        dir_gain = FULL_DIR_GAIN * fraction
    else:
        gain2 = 0.0
        dir_gain = 0.0
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
    subprocess.run(
        ["pw-cli", "set-param", str(node_id), "Props", params],
        capture_output=True, text=True,
    )


def save_persisted_state(enabled, level_db, freq_hz):
    os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
    tmp_path = STATE_PATH + ".tmp"
    with open(tmp_path, "w") as f:
        json.dump({"enabled": enabled, "level_db": level_db, "freq_hz": freq_hz}, f)
    os.replace(tmp_path, STATE_PATH)


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
