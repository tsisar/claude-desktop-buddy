#!/usr/bin/env bash
# Wrapper around ble_test_bridge.py — auto-creates a venv with bleak on
# first run so the caller doesn't have to remember pip / activate steps.
# Usage is identical to the .py:
#   tools/ble_test_bridge.sh --scenario idle
#   tools/ble_test_bridge.sh --list

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

exec "$venv/bin/python" "$here/ble_test_bridge.py" "$@"
