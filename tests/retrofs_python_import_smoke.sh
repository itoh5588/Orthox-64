#!/bin/bash
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"
export RETROFS_CMDS="/bin/ash /etc/native-python-smoke.sh"
export RETROFS_SETTLE_SECONDS="${RETROFS_SETTLE_SECONDS:-20}"
export GUEST_SMOKE_BODY="$(cat <<'EOF'
set -ex
echo python-import-smoke-start
/bin/python3 -c "import encodings, json, sys; print(encodings.__name__); print(json.dumps({'major': sys.version_info[0], 'minor': sys.version_info[1]}, sort_keys=True)); print('stdlib-import-ok')"
echo python-import-smoke-end
EOF
)"
export EXPECTED_SERIAL_PATTERNS="$(cat <<'EOF'
python-import-smoke-start
encodings
{"major": 3, "minor": 12}
stdlib-import-ok
python-import-smoke-end
EOF
)"
bash "$(dirname "$0")/retrofs_python_smoke_common.sh" "$ISO"
