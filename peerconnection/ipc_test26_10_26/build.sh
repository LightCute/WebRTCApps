#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

g++ -std=c++17 -O2 -Wall -o ipc_test main.cc unix_socket_server.cc -lpthread

echo "Done: ${SCRIPT_DIR}/ipc_test"
