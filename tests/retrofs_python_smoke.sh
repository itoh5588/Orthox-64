#!/bin/bash
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"

bash "$(dirname "$0")/retrofs_python_version_smoke.sh" "$ISO"
bash "$(dirname "$0")/retrofs_python_exec_smoke.sh" "$ISO"
bash "$(dirname "$0")/retrofs_python_import_smoke.sh" "$ISO"
