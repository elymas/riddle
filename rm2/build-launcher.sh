#!/bin/bash
# Build the touch-watcher launcher for the reMarkable 2 (armv7, libc only).
set -euo pipefail
cd "$(dirname "$0")/.."
docker exec rm2build bash -c 'source /opt/rm2-sdk/environment-setup-* && cd /riddle &&
  mkdir -p rm2/build &&
  $CC rm2/riddle-launcher.c -O2 -o rm2/build/riddle-launcher'
echo "built: rm2/build/riddle-launcher"
