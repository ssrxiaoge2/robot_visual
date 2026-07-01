#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IP="${1:-}"
PORT="${2:-30000}"
TIMEOUT_MS="${3:-5000}"
TRIGGER_HEX="${4:-41}"

if [[ -z "${IP}" ]]; then
    echo "Usage: $0 <scanner-ip> [port=30000] [timeout-ms=5000] [trigger-hex=41]" >&2
    exit 64
fi

case "$(uname -m)" in
    x86_64|amd64) ARCH=x64 ;;
    i386|i486|i586|i686) ARCH=x32 ;;
    *)
        echo "Unsupported architecture: $(uname -m)" >&2
        exit 65
        ;;
esac

RUNTIME_DIR="${SCRIPT_DIR}/verify-runtime/${ARCH}"
mkdir -p "${RUNTIME_DIR}"

cp -f "${SCRIPT_DIR}/${ARCH}/N-ScanHub.so" "${RUNTIME_DIR}/N-ScanHub.so"
cp -f "${SCRIPT_DIR}/${ARCH}/libusb-1.0.so" "${RUNTIME_DIR}/libusb-1.0.so"
ln -sfn libusb-1.0.so "${RUNTIME_DIR}/libusb-1.0.so.0"

make -C "${SCRIPT_DIR}" -f Makefile.verify
cp -f "${SCRIPT_DIR}/NScanNetworkVerify" "${RUNTIME_DIR}/NScanNetworkVerify"

echo "Architecture: ${ARCH}"
echo "Runtime: ${RUNTIME_DIR}"
echo "Scanner: ${IP}:${PORT}, timeout=${TIMEOUT_MS}ms"
echo "Trigger hex: ${TRIGGER_HEX}"
echo

cd "${RUNTIME_DIR}"
LD_LIBRARY_PATH="${RUNTIME_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    ./NScanNetworkVerify "${IP}" "${PORT}" "${TIMEOUT_MS}" "${TRIGGER_HEX}"
