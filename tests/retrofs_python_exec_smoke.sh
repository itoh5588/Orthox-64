#!/bin/bash
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"
RETROFS_CMDS="$(cat <<'EOF'
ls /
/bin/retrofsbasic
/bin/retrofsedge
/bin/musldircheck
/bin/pyenccheck
ls /bin
ls /lib/python3.12
EOF
)"
export RETROFS_CMDS="/bin/ash /etc/native-python-smoke.sh"
export GUEST_SMOKE_BODY="$(cat <<'EOF'
set -ex
echo python-exec-smoke-start
/bin/python3 -c "print(123)"
echo python-exec-smoke-end
EOF
)"
export EXPECTED_SERIAL_PATTERNS="$(cat <<'EOF'
python-exec-smoke-start
123
python-exec-smoke-end
EOF
)"
bash "$(dirname "$0")/retrofs_python_smoke_common.sh" "$ISO"
