#!/usr/bin/env bash
# Wrapper around usb_test_bridge.py — shares the venv with ble_test_bridge.sh.
# Auto-installs the deps on first run.
#
# Cannot run alongside `make monitor` — only one process can hold the port.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
venv="$here/.venv"

if [ ! -x "$venv/bin/python" ]; then
  echo "[bridge] first-run setup: creating $venv"
  if ! python3 -m venv "$venv" 2>/dev/null; then
    cat <<EOF >&2
[bridge] failed to create venv. On Debian/Ubuntu install the venv module:
  sudo apt-get install python3-venv
EOF
    exit 1
  fi
  "$venv/bin/pip" install --quiet --upgrade pip
  "$venv/bin/pip" install --quiet -r "$here/requirements.txt"
  echo "[bridge] venv ready."
fi

# pyserial may not have been in requirements.txt at venv-creation time
# (BLE-only setups). Top it up on every run — fast no-op once installed.
"$venv/bin/python" -c 'import serial' 2>/dev/null || \
  "$venv/bin/pip" install --quiet pyserial

exec "$venv/bin/python" "$here/usb_test_bridge.py" "$@"
