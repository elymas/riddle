#!/bin/bash
# Install the 5-finger-tap launcher on the reMarkable 2. Run from the repo root
# after rm2/build-launcher.sh has produced rm2/build/riddle-launcher.
#   DEVICE=10.11.99.1 rm2/install-launcher.sh
set -euo pipefail
cd "$(dirname "$0")/.."
DEVICE=${DEVICE:-10.11.99.1}

[ -f rm2/build/riddle-launcher ] || { echo "build first: rm2/build-launcher.sh" >&2; exit 1; }

scp -O rm2/build/riddle-launcher root@"$DEVICE":/home/root/riddle/riddle-launcher
scp -O rm2/riddle-launcher.service root@"$DEVICE":/etc/systemd/system/riddle-launcher.service
ssh root@"$DEVICE" '
  chmod +x /home/root/riddle/riddle-launcher
  systemctl daemon-reload
  systemctl enable --now riddle-launcher
  systemctl is-active riddle-launcher
'
echo "installed. 5-finger tap on the home screen now opens the diary."
echo "disable with: ssh root@$DEVICE systemctl disable --now riddle-launcher"
