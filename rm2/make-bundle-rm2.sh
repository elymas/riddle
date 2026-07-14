#!/usr/bin/env bash
# Stage the reMarkable 2 takeover bundle into dist/riddle-rm2/.
# Prereq: rm2/build-rm2.sh has produced the armv7 binary and shim.
#
# Deploy (manual, no AppLoad needed):
#   scp -O -r dist/riddle-rm2 root@<device>:/home/root/riddle
#   ssh root@<device> /home/root/riddle/riddle-takeover.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=target/armv7-unknown-linux-gnueabihf/release/riddle-takeover
[ -f "$BIN" ] || { echo "build first: rm2/build-rm2.sh" >&2; exit 1; }
[ -f rm2/build/libquill.so ] || { echo "missing rm2/build/libquill.so" >&2; exit 1; }

rm -rf dist/riddle-rm2
mkdir -p dist/riddle-rm2
install -m 755 "$BIN" dist/riddle-rm2/riddle
install -m 755 rm2/build/libquill.so dist/riddle-rm2/
install -m 755 scripts/appload-launch.sh scripts/riddle-takeover.sh dist/riddle-rm2/
install -m 644 external.manifest.json icon.png oracle.env.example settings.schema.json dist/riddle-rm2/

echo "staged: $(du -sh dist/riddle-rm2 | cut -f1) in dist/riddle-rm2/"
