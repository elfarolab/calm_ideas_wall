#!/usr/bin/env bash
set -euo pipefail

# Full paths
BASE_DIR="/opt/calm_ideas_wall"
SRC_FILE="${BASE_DIR}/projects.jsonl"
BACKUP_DIR="${BASE_DIR}/backups"
LOG_DIR="${BASE_DIR}/logs"
LOG_FILE="${LOG_DIR}/backup_projects.log"

# Ensure required directories exist before doing anything else
if ! mkdir -p "${LOG_DIR}" "${BACKUP_DIR}"; then
    echo "ERROR: cannot create required directories under ${BASE_DIR}" >&2
    exit 1
fi

# Log everything from here on
exec >>"${LOG_FILE}" 2>&1

echo "=== Backup started: $(date '+%Y-%m-%d %H:%M:%S') ==="

if [[ ! -f "${SRC_FILE}" ]]; then
    echo "ERROR: Source file not found: ${SRC_FILE}"
    exit 1
fi

# Timestamp format: YYMMDD_HHMMSS
STAMP="$(date +%y%m%d_%H%M%S)"
BACKUP_FILE="${BACKUP_DIR}/projects_backup_${STAMP}.bz2"

# Create compressed backup
bzip2 -c "${SRC_FILE}" > "${BACKUP_FILE}"

echo "Created backup: ${BACKUP_FILE}"

# Keep only the last 3 backups
mapfile -t backups < <(
    find "${BACKUP_DIR}" -maxdepth 1 -type f -name 'projects_backup_*.bz2' -printf '%f\n' |
    sort
)

if (( ${#backups[@]} > 3 )); then
    for f in "${backups[@]:0:$(( ${#backups[@]} - 3 ))}"; do
        rm -f -- "${BACKUP_DIR}/${f}"
        echo "Removed old backup: ${f}"
    done
fi

echo "=== Backup finished: $(date '+%Y-%m-%d %H:%M:%S') ==="

