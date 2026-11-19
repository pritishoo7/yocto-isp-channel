#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}"

export QB_MEM="${QB_MEM:--m 2048}"
export QB_SMP="${QB_SMP:--smp 4}"
export QB_SLIRP_OPT="${QB_SLIRP_OPT:--netdev user,id=net0,hostfwd=tcp::2222-:22}"

echo "Starting Track Q QEMU (qemuarm64) isp-image..."
echo "  Memory : ${QB_MEM}"
echo "  CPUs   : ${QB_SMP}"
echo "  SSH    : ssh -p 2222 root@localhost"
echo "  Exit   : Press Ctrl+A then X"

if command -v runqemu >/dev/null 2>&1; then
    exec runqemu nographic slirp
else
    exec kas shell kas-qemu.yml -c "runqemu nographic slirp"
fi
