#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

HTSIM_EXEC="${HTSIM_EXEC:-${REPO_ROOT}/sim/htsim-backend/sim/datacenter/htsim_uec}"
GOAL_BIN="${GOAL_BIN:-${REPO_ROOT}/../workloads/llama7b_2n8g/llama2_7b_2n8g.bin}"
HTSIM_MATRIX="${HTSIM_MATRIX:-${SCRIPT_DIR}/topologies/ocs_2servers.json}"
HTSIM_ROUTES="${HTSIM_ROUTES-${SCRIPT_DIR}/topologies/ocs_2servers_routes.json}"
HTSIM_RANK_SERVER_MAP="${HTSIM_RANK_SERVER_MAP:-}"
LINKSPEED_MBPS="${HTSIM_LINKSPEED:-200000}"
MEMORY_MB="${HTSIM_MEMORY_MB:-1536}"
OUTPUT_DIR="${HTSIM_OUTPUT_DIR:-${SCRIPT_DIR}/runs/llama7b_n2_matrix}"

CHECK_ONLY=0
if [[ "${1:-}" == --check-only ]]; then
    CHECK_ONLY=1
    shift
fi
if [[ $# -ne 0 ]]; then
    echo "Usage: $0 [--check-only]" >&2
    exit 1
fi
CHECK_ARGS=(--goal "${GOAL_BIN}" --matrix "${HTSIM_MATRIX}")
if [[ -n "${HTSIM_ROUTES}" ]]; then
    CHECK_ARGS+=(--routes "${HTSIM_ROUTES}")
fi
if [[ -n "${HTSIM_RANK_SERVER_MAP}" ]]; then
    CHECK_ARGS+=(--rank-server-map "${HTSIM_RANK_SERVER_MAP}")
fi
python3 "${SCRIPT_DIR}/check_matrix_replay.py" "${CHECK_ARGS[@]}"
if [[ "${CHECK_ONLY}" -eq 1 ]]; then
    exit 0
fi

for path in "${HTSIM_EXEC}" "${GOAL_BIN}" "${HTSIM_MATRIX}"; do
    if [[ ! -f "${path}" ]]; then
        echo "[ERROR] Missing file: ${path}" >&2
        exit 1
    fi
done
if [[ -n "${HTSIM_ROUTES}" && ! -f "${HTSIM_ROUTES}" ]]; then
    echo "[ERROR] Missing route table: ${HTSIM_ROUTES}" >&2
    exit 1
fi
if [[ -n "${HTSIM_RANK_SERVER_MAP}" && ! -f "${HTSIM_RANK_SERVER_MAP}" ]]; then
    echo "[ERROR] Missing rank-to-server map: ${HTSIM_RANK_SERVER_MAP}" >&2
    exit 1
fi

absolute_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "$PWD" "$1" ;;
    esac
}
HTSIM_EXEC="$(absolute_path "${HTSIM_EXEC}")"
GOAL_BIN="$(absolute_path "${GOAL_BIN}")"
HTSIM_MATRIX="$(absolute_path "${HTSIM_MATRIX}")"
if [[ -n "${HTSIM_ROUTES}" ]]; then
    HTSIM_ROUTES="$(absolute_path "${HTSIM_ROUTES}")"
fi
if [[ -n "${HTSIM_RANK_SERVER_MAP}" ]]; then
    HTSIM_RANK_SERVER_MAP="$(absolute_path "${HTSIM_RANK_SERVER_MAP}")"
fi

mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
LOG_FILE="${OUTPUT_DIR}/htsim_matrix_uec.log"
ARGS=(
    -o "${OUTPUT_DIR}/logout.dat"
    -matrix "${HTSIM_MATRIX}"
    -goal "${GOAL_BIN}"
    -linkspeed "${LINKSPEED_MBPS}"
    -strat ecmp_host
    -mtu 4096
    -paths 128
    -q 1000000
    -seed 4
)
if [[ -n "${HTSIM_ROUTES}" ]]; then
    ARGS+=(-routes "${HTSIM_ROUTES}")
fi
if [[ -n "${HTSIM_RANK_SERVER_MAP}" ]]; then
    ARGS+=(-rank_server_map "${HTSIM_RANK_SERVER_MAP}")
fi

echo "[INFO] Replaying GOAL trace: ${GOAL_BIN}"
echo "[INFO] Server adjacency matrix: ${HTSIM_MATRIX}"
if [[ -n "${HTSIM_ROUTES}" ]]; then
    echo "[INFO] Global route table: ${HTSIM_ROUTES}"
else
    echo "[INFO] No route file supplied; Htsim will derive shortest paths from the matrix."
fi
echo "[INFO] Per-lane link speed: ${LINKSPEED_MBPS} Mbps"
if [[ ! "${MEMORY_MB}" =~ ^[0-9]+$ ]]; then
    echo "[ERROR] HTSIM_MEMORY_MB must be a nonnegative integer (0 disables the limit)." >&2
    exit 1
fi
echo "[INFO] Address-space limit: ${MEMORY_MB} MiB (0 = unlimited); core dumps disabled"
(
    ulimit -c 0
    if [[ "${MEMORY_MB}" -gt 0 ]]; then
        ulimit -v "$((MEMORY_MB * 1024))"
    fi
    cd "${OUTPUT_DIR}"
    exec "${HTSIM_EXEC}" "${ARGS[@]}" > "${LOG_FILE}" 2>&1
)
echo "[INFO] Htsim completed. Log: ${LOG_FILE}"
rg -n "Configured GOAL rank placement|It terminates!|Flow Info Size|Maximum finishing time" "${LOG_FILE}" || true
