#!/usr/bin/env python3
"""Reapply the last saved crossfeed level/frequency after filter-chain.service starts.

Run as a systemd --user oneshot unit (installed by install.sh) so that
settings saved from the GUI survive reboots and logout/login, instead of
reverting to crossfeed.conf's baked-in defaults every time the filter-chain
is (re)loaded.
"""
import sys
import time

import crossfeed_lib as cf

WAIT_SECONDS = 5.0
POLL_INTERVAL = 0.1


def main():
    node_id = None
    deadline = time.monotonic() + WAIT_SECONDS
    while time.monotonic() < deadline:
        node_id = cf.get_node_id()
        if node_id is not None:
            break
        time.sleep(POLL_INTERVAL)

    if node_id is None:
        print("crossfeed-restore: crossfeed_sink not found, giving up", file=sys.stderr)
        return 1

    if not cf.wait_for_conf_init(node_id, timeout=WAIT_SECONDS, poll_interval=POLL_INTERVAL):
        print("crossfeed-restore: filter-chain never finished initializing, giving up", file=sys.stderr)
        return 1

    persisted = cf.load_persisted_state()
    if persisted is None:
        return 0  # nothing saved yet — leave crossfeed.conf's own defaults in place

    enabled, level_db, freq_hz = persisted
    cf.apply_state(node_id, enabled, level_db, freq_hz)
    return 0


if __name__ == "__main__":
    sys.exit(main())
