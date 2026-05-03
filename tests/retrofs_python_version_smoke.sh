#!/bin/bash
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"
export RETROFS_CMDS="/bin/ash /etc/native-python-smoke.sh"
export GUEST_SMOKE_BODY="$(cat <<'EOF'
set -ex
echo python-version-smoke-start
/bin/python3 --version
echo python-version-smoke-end
EOF
)"
export EXPECTED_SERIAL_PATTERNS="$(cat <<'EOF'
python-version-smoke-start
Python 3.12.3
python-version-smoke-end
EOF
)"
bash "$(dirname "$0")/retrofs_python_smoke_common.sh" "$ISO"
