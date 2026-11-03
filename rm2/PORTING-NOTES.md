# riddle on reMarkable 2 — porting notes

Working port of riddle (takeover flavour) to the **reMarkable 2**, OS
**3.28.0.162** (kernel 5.4.70-v1.6.3-rm11x, armv7l, glibc 2.39, Qt 6.10.3).
Verified on-device: live pen ink (real-time, non-flashing), correct pen
orientation, five-finger exit, and the OpenAI-compatible oracle round-trip.
Also runs on OS **3.29.1.0** (kernel 5.4.70-v1.6.4-rm11x, same Qt 6.10.3) —
see "OS 3.29 layout shift" below.

## The hard part: driving the e-ink panel

The original app targets the reMarkable Paper Pro (ferrari, aarch64) via the
sibling **quill** shim over `libqsgepaper.so`. The rM2's `libqsgepaper.so`
(`/usr/lib/plugins/scenegraph/`, `EPFramebufferCarta1000` panel) has a
different C++ ABI, so `rm2/quill_rm2.cpp` is a clean reimplementation of the
same 7-function quill C ABI. Getting a fast, non-flashing partial update took a
disassembly of `EPFramebufferCarta1000::swapBuffers_impl`. The load-bearing
facts (all verified on the device):

- **`EPFramebuffer::instance()` self-initializes** the SWTCON worker threads
  (`EPFramebufferQuitterThread`, `vsync-flip`, `framegen`). Calling
  `EPFramebufferSwtcon::initialize()` yourself **deadlocks** — don't.
- **The real frame buffer is the inline `QImage` at `instance + 0x50`** —
  `Format_RGB32` (BGRA, 4 bytes/px, stride 5616 at 1404×1872). This is what
  `swapBuffers_impl` reads (via the pointer at `+0x5c`). The `Grayscale8`
  QImage at `+0x60` is a *secondary* buffer and is **not** what the panel
  renders from — writing it was the single biggest dead end in the port.
  riddle's existing `Surface::Rgb32` (bytes B,G,R,0xFF) matches this buffer
  exactly. (Offsets are for OS 3.28; on 3.29 they moved by +8 — see below.)
- **Flush = `swapBuffers(QRegion, EPScreenModeMap, QFlags<UpdateFlag>)`.**
  Internally it clips the region to the framebuffer, splits it per screen mode,
  and calls `EPFramebufferSwtcon::update()` per rect. The update **region comes
  from the `QRegion` argument** — it is a true region-scoped update, not
  full-screen.
  - `flags` bit 0 (`FullUpdate` = 0x01) forces a full-screen **GC16 flash**
    (used for the de-ghost / clear pass).
  - `flags` `0x00` (`PartialUpdate`) is the fast, region-scoped, **non-flashing**
    ink path.
- **`EPScreenMode`** (device metaobject): `Pen=0, Mono=1, Animation=2,
  Grayscale=3, Content=4, Full=5`. Pen(0) for ink, Content(4) for text.
- **`EPScreenModeMap`** has no exported destructor, so we build one full-screen
  map per waveform once at init and reuse it. Its ctor pre-assigns the whole
  screen to a slot; the actual update area still comes from the `QRegion` passed
  to `swapBuffers`.

Dead ends (documented so nobody repeats them):

- `swapBuffers(QRect, EPScreenMode, flags)` — reads a buffer that is null on
  this build; shows nothing.
- `EPFramebufferSwtcon::update()` + `sync()` by hand — `update()` only *queues*;
  `swapBuffers` is what flushes.
- `setBuffers(tuple, QImage*)` — repoints the engine's buffers and broke the
  display (rendered the empty render-target we passed).
- Writing the `+0x60` Grayscale8 buffer — only ever showed via a full-screen
  flash; never partial.

## Input

- Pen: `Wacom I2C Digitizer` (event1). EVIOCGABS-measured range X 0–20966 /
  Y 0–15725, rotated 90° vs the portrait screen — `pen.rs` swaps axes and flips.
  Polarity is env-tunable (`RIDDLE_PEN_INVX` / `RIDDLE_PEN_INVY`) but the
  defaults are correct on-device.
- Touch: `pt_mt` (event2), MT type B, coordinates already in screen pixels
  (0–1403 × 0–1871), Y inverted.
- All raw `input_event` parsing uses the 16-byte 32-bit-kernel record
  (`src/evdev.rs`), not the 24-byte 64-bit layout.

## Prerequisites

### 1. The vendor lib (`rm2/vendor/libqsgepaper.so`)

The shim links against **your own device's** `libqsgepaper.so` — it is
reMarkable's proprietary e-ink engine and is **not distributed here**
(git-ignored). The SDK 3.27 copy won't do: the 3.28 firmware added the
`swapBuffers`/`EPScreenModeMap` symbols the shim resolves, so it must come from
a device running OS 3.28.x. Pull it once, from a device on the same OS you'll
run on:

```sh
mkdir -p rm2/vendor
scp -O root@10.11.99.1:/usr/lib/plugins/scenegraph/libqsgepaper.so rm2/vendor/
```

If the offsets/symbols drift on a future OS, re-pull and re-verify the layout
constants in `quill_rm2.cpp` (`FB_SCAN_FROM`/`FB_SCAN_TO`, the RGB32 format
check) with `nm -D rm2/vendor/libqsgepaper.so | c++filt`.

### OS 3.29 layout shift

OS 3.29.1.0 ships a new `libqsgepaper.so`. The four symbols the shim uses are
unchanged, but `EPScreenModeMap` grew from 48 to 56 bytes, and it is the
`EPFramebuffer` member at `+0x20`, so everything after it moved by 8:

| member | 3.28.0.162 | 3.29.1.0 |
|---|---|---|
| RGB32 frame-buffer `QImage` | `+0x50` | `+0x58` |
| pointer read by `swapBuffers_impl` | `+0x5c` | `+0x64` |
| Grayscale8 `QImage` | `+0x60` | `+0x68` |
| `std::deque<EPScreenModeMap>` (new, global screen-mode stack) | — | `+0x7c` |

With the old hard-coded `+0x50`, riddle exits with `quill_init failed` on
3.29. `quill_init()` now finds the member at runtime instead: it scans the
object for words equal to a local `QImage`'s vtable pointer (same
`libQt6Gui`, so they match) and takes the first one whose format is RGB32. The
chosen offset is logged as `quill: frame buffer QImage at instance+0x..`.
Verified on 3.29.1.0 (`+0x58`). Both offsets fall inside the 3.28 object, so
one build covers both OS versions.

To check a new OS quickly, compare the `EPFramebuffer::EPFramebuffer()`
disassembly (the `add r0, r4, #N` before each `QImage::QImage()` call) between
the old and new lib with the SDK's `arm-remarkable-linux-gnueabi-objdump -d -C`.

### 2. The rM2 SDK 3.27 Docker image (`rm2sdk:latest`)

```sh
# Download the official rM2 3.27 toolchain installer, then inside an amd64 container:
#   https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/rm2/\
#     remarkable-production-image-5.7.119-rm2-public-x86_64-toolchain.sh
docker run -d --name rm2build --platform linux/amd64 -v "$PWD:/riddle" ubuntu:22.04 sleep infinity
docker exec rm2build bash -c 'apt-get update -qq && apt-get install -y -qq xz-utils file binutils gcc &&
  bash /riddle/<toolchain>.sh -y -d /opt/rm2-sdk'
```

`build-rm2.sh` / `build-launcher.sh` reuse the running `rm2build` container.

## Build

`rm2/build-rm2.sh` — cross-compiles inside the amd64 Docker container holding the
official **rM2 SDK 3.27** (Qt 6.8, forward-compatible with the device's 6.10).
The shim links the device's own `libqsgepaper.so` from `rm2/vendor/` (see
Prerequisites). `rm2/make-bundle-rm2.sh` stages `dist/riddle-rm2/`.

Deploy (no AppLoad needed):

```sh
scp -O -r dist/riddle-rm2 root@10.11.99.1:/home/root/riddle
ssh root@10.11.99.1 /home/root/riddle/riddle-takeover.sh
```

## Launching from the device (5-finger tap)

AppLoad would give an app-menu icon but crash-loops on OS 3.28, so there's no
on-device launcher. `rm2/riddle-launcher.c` is a tiny libc daemon that watches
the `pt_mt` touch device **without grabbing it** (xochitl keeps working) and
launches `riddle-takeover.sh` on a 5-finger tap. While riddle runs it grabs the
device, so the watcher goes quiet until riddle exits — the same 5-finger tap
that exits riddle can't accidentally relaunch it.

```sh
rm2/build-launcher.sh                    # build riddle-launcher (SDK gcc, libc only)
DEVICE=10.11.99.1 rm2/install-launcher.sh  # scp + enable the systemd service
```

Rising-edge on ≥5 simultaneous slots; a resting palm can rarely trip it. If
false positives bite, gate on a brief low-motion tap instead (noted in the
source). Disable: `systemctl disable --now riddle-launcher`.

## Reply tofu (fixed)

Tom's reply showed rectangle boxes around the first/last characters — the model
wraps replies in smart quotes (“ ”) and Dancing Script has no glyph for them, so
`rasterize_line` drew the font's `.notdef` box. `src/script.rs` now skips
`.notdef` glyphs (GlyphId 0) in both `rasterize_line` and `measure`. Not
rM2-specific — the same fix applies on ferrari.

## Reply font override (RIDDLE_FONT)

The reply hand is the embedded Dancing Script (`include_bytes!`). To swap it
without rebuilding, set `RIDDLE_FONT=/path/to.ttf` in `oracle.env` — riddle
loads it as a `FontVec` at startup (falls back to the built-in if the path is
unreadable). Used e.g. to point at the on-device Noto Sans KR
(`/home/root/ttf/NotoSansKR-VariableFont_wght.ttf`) for a more legible hand with
Korean coverage. `src/script.rs` / `main.rs` / `help.rs` moved from `FontRef`
(borrowed embedded bytes) to `FontVec` (owned) to allow the runtime file load.

## Oracle backends (the "spirit")

riddle reads the committed page as a PNG and sends it to a **vision** model —
there is no OCR/transcription step, so a text-only model won't work. Selection
priority (`src/oracle.rs` → `Oracle::spawn`):

1. **Anthropic (Claude)** — set `RIDDLE_ANTHROPIC_KEY`. Native Messages API
   (`/v1/messages`, `x-api-key` + `anthropic-version`, base64 `image` blocks,
   `content_block_delta` SSE) — **not** OpenAI-compatible. `AnthropicOracle` was
   added specifically for this; every current Claude model is vision-capable.
   Env: `RIDDLE_ANTHROPIC_MODEL` (default `claude-opus-4-8`; `claude-haiku-4-5`
   is faster/cheaper), `RIDDLE_ANTHROPIC_BASE`, `RIDDLE_ANTHROPIC_VERSION`,
   `RIDDLE_ANTHROPIC_MAX_TOKENS`. No `thinking`/`temperature` sent (fast first
   ink; both would be rejected or slow on Opus 4.8).
2. **OpenAI-compatible HTTP** — set `RIDDLE_OPENAI_KEY`. `image_url` data-URI,
   `/chat/completions`. Works with OpenAI, OpenRouter, Gemini's OpenAI endpoint,
   and **z.ai GLM** (coding plan): `RIDDLE_OPENAI_BASE=https://api.z.ai/api/coding/paas/v4`,
   `RIDDLE_OPENAI_MODEL=glm-4.5v` (vision VLM; `glm-5v-turbo` is not in the
   coding plan). GLM must be a `-v`/VLM model — text-only GLM has no vision.
3. **pi** — used when no HTTP key is set.

Measured first-ink latency on-device (crude "HI TOM" test PNG):
Claude Haiku 4.5 ~1.0s · Claude Opus 4.8 ~3.3s · GLM-4.5v ~7.8s.

Config lives in `oracle.env` next to the binary (see `oracle.env.example`).
Verify a backend without launching the diary:
`./riddle --oracle-test <page.png>`.
