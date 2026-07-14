#!/bin/bash
# Build riddle (takeover flavour) for the reMarkable 2, from a macOS/anything
# host, inside an amd64 Docker container holding the official rM2 SDK 3.27.
#
# Prereqs (one-time, done by setup):
#   - docker image `rm2sdk:latest` with the SDK at /opt/rm2-sdk
#     (install remarkable-production-image-5.7.119-rm2-public-x86_64-toolchain.sh)
#   - rm2/vendor/libqsgepaper.so pulled from the TARGET DEVICE (OS 3.28) —
#     the SDK 3.27 copy lacks the swapBuffers(QRect,...) overload we link.
#
# Outputs:
#   rm2/build/libquill.so                                  (the rM2 shim)
#   target/armv7-unknown-linux-gnueabihf/release/riddle-takeover
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD

[ -f rm2/vendor/libqsgepaper.so ] || {
    echo "rm2/vendor/libqsgepaper.so missing — pull it from the device:"
    echo "  scp root@10.11.99.1:/usr/lib/plugins/scenegraph/libqsgepaper.so rm2/vendor/"
    exit 1
}

docker inspect rm2build >/dev/null 2>&1 ||
    docker run -d --name rm2build --platform linux/amd64 -v "$REPO:/riddle" -w /riddle rm2sdk:latest sleep infinity
docker start rm2build >/dev/null
RUN() { docker exec rm2build bash -c "$1"; }

# 1. Shim: SDK g++, link against the device's own libqsgepaper.so. The device
#    Qt (6.10) is newer than the SDK's (6.8); its extra undefined symbols
#    resolve at runtime on the device.
RUN 'source /opt/rm2-sdk/environment-setup-* && mkdir -p rm2/build &&
  $CXX -std=c++17 -O2 -fPIC -shared rm2/quill_rm2.cpp -o rm2/build/libquill.so \
    -I$SDKTARGETSYSROOT/usr/include/QtCore -I$SDKTARGETSYSROOT/usr/include/QtGui \
    -I$SDKTARGETSYSROOT/usr/include \
    -lQt6Core -lQt6Gui \
    -Lrm2/vendor -l:libqsgepaper.so \
    -Wl,--allow-shlib-undefined \
    -Wl,-rpath,/usr/lib/plugins/scenegraph'
echo "built: rm2/build/libquill.so"

# 2. riddle: Rust armv7hf, linked by the SDK gcc (glibc 2.31-era host cross
#    toolchains also work — device glibc is 2.39 — but the SDK one is already
#    here and carries the right sysroot).
RUN 'set -e
  if [ ! -x /root/.cargo/bin/cargo ]; then
    apt-get update -qq >/dev/null && apt-get install -y -qq curl ca-certificates gcc >/dev/null
    curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable >/dev/null
  fi
  export PATH=/root/.cargo/bin:$PATH
  rustup target add armv7-unknown-linux-gnueabihf >/dev/null 2>&1 || true
  source /opt/rm2-sdk/environment-setup-*
  printf "#!/bin/bash\nexec %s \"\$@\"\n" "$CC" > /tmp/rm2-cc.sh && chmod +x /tmp/rm2-cc.sh
  export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABIHF_LINKER=/tmp/rm2-cc.sh
  export QUILL_LIB_DIR=/riddle/rm2/build
  export QSGEPAPER_LIB_DIR=/riddle/rm2/vendor
  export SDK_SYSROOT_LIB=$SDKTARGETSYSROOT/usr/lib
  cargo build --release --target armv7-unknown-linux-gnueabihf --features takeover
  cp target/armv7-unknown-linux-gnueabihf/release/riddle \
     target/armv7-unknown-linux-gnueabihf/release/riddle-takeover'
echo "built: target/armv7-unknown-linux-gnueabihf/release/riddle-takeover"
