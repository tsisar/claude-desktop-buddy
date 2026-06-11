#!/usr/bin/env bash
# Wrapper around prep_character.py — shares the venv created by the other
# tools, auto-installs deps on first run.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
venv="$here/.venv"

if [ ! -x "$venv/bin/python" ]; then
  echo "[prep] first-run setup: creating $venv"
  if ! python3 -m venv "$venv" 2>/dev/null; then
    cat <<EOF >&2
[prep] failed to create venv. On Debian/Ubuntu install the venv module:
  sudo apt-get install python3-venv
EOF
    exit 1
  fi
  "$venv/bin/pip" install --quiet --upgrade pip
  "$venv/bin/pip" install --quiet -r "$here/requirements.txt"
fi

"$venv/bin/python" -c 'import PIL' 2>/dev/null || \
  "$venv/bin/pip" install --quiet 'pillow>=10'

exec "$venv/bin/python" "$here/prep_character.py" "$@"
