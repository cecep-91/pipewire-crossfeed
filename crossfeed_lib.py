"""Client library and IPC communication for pipewire-crossfeed.

Communicates with the standalone crossfeed C++ engine via Unix domain socket.
Works across any Linux distribution and any sound server (PipeWire, PulseAudio, ALSA).
"""
import json
import os
import socket
import subprocess

__version__ = "2.0.0"

LEVEL_MIN_DB = -30.0
LEVEL_MAX_DB = -6.0
LEVEL_DEFAULT_DB = -10.0

FREQ_MIN = 200.0
FREQ_MAX = 2000.0
FULL_FREQ = 700.0


def get_socket_path():
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    if runtime_dir and os.path.isdir(runtime_dir):
        return os.path.join(runtime_dir, "crossfeed.sock")
    config_dir = os.path.expanduser("~/.config/pipewire-crossfeed")
    return os.path.join(config_dir, "crossfeed.sock")


def send_command(cmd, timeout=0.5):
    """Send an IPC command to the running crossfeed engine and return the response."""
    sock_path = get_socket_path()
    if not os.path.exists(sock_path):
        return None
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(sock_path)
        s.sendall((cmd + "\n").encode("utf-8"))
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        s.close()
        return data.decode("utf-8").strip()
    except (socket.error, OSError):
        return None


def get_status():
    """Returns status dict or None if engine is not running."""
    res = send_command("STATUS")
    if not res:
        return None
    try:
        return json.loads(res)
    except json.JSONDecodeError:
        return None


def set_params(enabled=None, level_db=None, freq_hz=None):
    parts = ["SET"]
    if enabled is not None:
        parts.append(f"enabled={'true' if enabled else 'false'}")
    if level_db is not None:
        parts.append(f"level={level_db:.1f}")
    if freq_hz is not None:
        parts.append(f"freq={freq_hz:.0f}")
    cmd = " ".join(parts)
    res = send_command(cmd)
    if res:
        try:
            return json.loads(res)
        except json.JSONDecodeError:
            pass
    return None


def toggle():
    res = send_command("TOGGLE")
    if res:
        try:
            return json.loads(res)
        except json.JSONDecodeError:
            pass
    return None


def start_engine():
    """Attempts to start the standalone crossfeed daemon."""
    candidates = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", "crossfeed"),
        os.path.expanduser("~/.local/bin/crossfeed"),
        "/usr/local/bin/crossfeed",
        "crossfeed",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            subprocess.Popen([c, "start"])
            return True
    try:
        subprocess.Popen(["crossfeed", "start"])
        return True
    except FileNotFoundError:
        return False


def stop_engine():
    return send_command("STOP")
