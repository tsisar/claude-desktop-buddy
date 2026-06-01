#!/usr/bin/env bash
# Wrapper around usb_xfer_send.py — shares the venv created by the other
# tools, auto-installs deps on first run.
#
# Mutually exclusive with `make monitor` (only one process per port).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
venv="$here/.venv"

if [ ! -x "$venv/bin/python" ]; then
  echo "[xfer-send] first-run setup: creating $venv"
  if ! python3 -m venv "$venv" 2>/dev/null; then
    cat <<EOF >&2
[xfer-send] failed to create venv. On Debian/Ubuntu install the venv module:
  sudo apt-get install python3-venv
EOF
    exit 1
  fi
  "$venv/bin/pip" install --quiet --upgrade pip
  "$venv/bin/pip" install --quiet -r "$here/requirements.txt"
fi

# pyserial may not have been in requirements.txt at venv-creation time;
# top it up on every run (fast no-op once installed).
"$venv/bin/python" -c 'import serial' 2>/dev/null || \
  "$venv/bin/pip" install --quiet pyserial

exec "$venv/bin/python" "$here/usb_xfer_send.py" "$@"
