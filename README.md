# riddle on the reMarkable 2

A working port of [riddle](https://github.com/MaximeRivest/riddle) — the diary
of Tom Riddle — to the **reMarkable 2**.

Write on the page with your pen. After a pause, the diary drinks your ink,
thinks for a moment, and an answer writes itself back in a flowing hand,
stroke by stroke, then fades away. No screen glow, no keyboard, no chat UI —
just ink appearing on paper. See the
[upstream repository](https://github.com/MaximeRivest/riddle) for the original
app (reMarkable Paper Pro) and the full feature story.

**Tested on a reMarkable 2 running OS beta 3.28.0.162** (armv7, kernel
5.4.70-v1.6.3-rm11x, Qt 6.10.3). Other models and OS versions are untested and
may not work.

> ⚠️ **This modifies your device.** The port runs in takeover mode: it stops
> xochitl (the reMarkable UI) and drives the e-ink panel directly, as root.
> xochitl is restarted automatically on exit — even if riddle dies uncleanly —
> but keep SSH access working before you install anything. If the screen ever
> wedges: `ssh root@10.11.99.1 'systemctl start xochitl'`. Use entirely at
> your own risk. Not affiliated with reMarkable AS.

## What the port adds

- **A clean-room quill shim for the rM2's e-ink engine**
  (`rm2/quill_rm2.cpp`). The rM2's `libqsgepaper.so` has a different C++ ABI
  than the Paper Pro's, so the 7-function quill C ABI was reimplemented from a
  disassembly of `EPFramebufferCarta1000`. Real-time, non-flashing,
  region-scoped ink updates.
- **rM2 input handling** — Wacom I2C pen (axes rotated 90° vs the screen) and
  `pt_mt` touch, parsed as 16-byte 32-bit-kernel evdev records.
- **A native Anthropic (Claude) oracle backend** — streaming Messages API
  (`AnthropicOracle` in `src/oracle.rs`), selected by setting
  `RIDDLE_ANTHROPIC_KEY`. Measured ~1.0 s to first ink with Claude Haiku 4.5.
- **A 5-finger double-tap launcher daemon** (`rm2/riddle-launcher.c`) —
  AppLoad crash-loops on OS 3.28, so a tiny libc daemon watches the
  touchscreen (without grabbing it) and launches the diary on a 5-finger
  double-tap. A 5-finger tap inside the diary exits it.
- **`RIDDLE_FONT` runtime reply-font override** — swap the reply hand without
  rebuilding, e.g. point it at an on-device Noto Sans KR for Korean coverage.
- **Glyph fixes** — `.notdef` glyphs are skipped so smart quotes no longer
  render as tofu boxes (applies upstream too).
- **Snappier replies** — the reply lingers 1 s (was 4 s) before fading.

## Prerequisites

Two things the repository cannot ship:

1. **The vendor e-ink library** — `libqsgepaper.so` is reMarkable's
   proprietary engine and is not distributed here (git-ignored). It must come
   from *your own device* running OS 3.28.x (the 3.28 firmware added the
   symbols the shim resolves; the SDK 3.27 copy won't do):

   ```sh
   mkdir -p rm2/vendor
   scp -O root@10.11.99.1:/usr/lib/plugins/scenegraph/libqsgepaper.so rm2/vendor/
   ```

2. **The rM2 SDK 3.27 toolchain in an amd64 Docker container** (`rm2build`) —
   the cross-compiler and Qt the build scripts use:

   ```sh
   # Download the official rM2 3.27 toolchain installer:
   #   https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/rm2/remarkable-production-image-5.7.119-rm2-public-x86_64-toolchain.sh
   docker run -d --name rm2build --platform linux/amd64 -v "$PWD:/riddle" ubuntu:22.04 sleep infinity
   docker exec rm2build bash -c 'apt-get update -qq && apt-get install -y -qq xz-utils file binutils gcc &&
     bash /riddle/<toolchain>.sh -y -d /opt/rm2-sdk'
   ```

## Build & deploy

```sh
rm2/build-rm2.sh            # cross-compile riddle + the quill shim in the container
rm2/make-bundle-rm2.sh      # stage dist/riddle-rm2/

scp -O -r dist/riddle-rm2 root@10.11.99.1:/home/root/riddle
ssh root@10.11.99.1 /home/root/riddle/riddle-takeover.sh   # run it once, from SSH
```

No AppLoad or xovi needed.

### The launcher (open the diary from the device)

```sh
rm2/build-launcher.sh                      # build riddle-launcher (SDK gcc, libc only)
DEVICE=10.11.99.1 rm2/install-launcher.sh  # scp + enable the systemd service
```

Then a **5-finger double-tap** anywhere opens the diary; a **5-finger tap**
inside it leaves. Disable with `systemctl disable --now riddle-launcher`.

## The oracle (the "spirit" in the diary)

Replies come from a **vision** LLM that reads the committed page as a PNG —
there is no OCR step, so a text-only model won't work. Config lives in
`oracle.env` next to the binary (see `oracle.env.example`; never commit a real
key). Backend selection at startup:

1. **Anthropic (Claude)** — set `RIDDLE_ANTHROPIC_KEY`. Native Messages API,
   streamed. Options: `RIDDLE_ANTHROPIC_MODEL` (default `claude-opus-4-8`;
   `claude-haiku-4-5` is faster and cheaper), `RIDDLE_ANTHROPIC_BASE`,
   `RIDDLE_ANTHROPIC_VERSION`, `RIDDLE_ANTHROPIC_MAX_TOKENS`.
2. **Any OpenAI-compatible API** — set `RIDDLE_OPENAI_KEY` (plus
   `RIDDLE_OPENAI_BASE` / `RIDDLE_OPENAI_MODEL`). Works with OpenAI,
   OpenRouter, Gemini's OpenAI endpoint, and z.ai GLM
   (`glm-4.5v` — must be a vision model).
3. **[pi](https://github.com/badlogic/pi-mono)** — used when no HTTP key is
   set.

Measured first-ink latency on-device: Claude Haiku 4.5 ~1.0 s · Claude Opus
4.8 ~3.3 s · GLM-4.5v ~7.8 s.

Verify a backend without launching the diary:

```sh
./riddle --oracle-test <page.png>
```

## Reply font

The built-in reply hand is Dancing Script (SIL OFL 1.1 — see `fonts/OFL.txt`).
To swap it without rebuilding, set `RIDDLE_FONT=/path/to.ttf` in `oracle.env` —
for example an on-device Noto Sans KR
(`/home/root/ttf/NotoSansKR-VariableFont_wght.ttf`) for a legible hand with
Korean coverage. Falls back to the built-in font if the path is unreadable.

## Porting internals

[`rm2/PORTING-NOTES.md`](rm2/PORTING-NOTES.md) documents the whole journey:
how the panel is actually driven (`swapBuffers`, `EPScreenModeMap`, the real
framebuffer at `instance + 0x50`), the dead ends so nobody repeats them, input
quirks, and the build details.

## Branches

- **`rm2-port`** (default) — this port.
- **`main`** — tracks upstream
  [MaximeRivest/riddle](https://github.com/MaximeRivest/riddle).

## License

MIT (see `LICENSE`), same as upstream — riddle is by
[Maxime Rivest](https://github.com/MaximeRivest). The vendor library the shim
interposes (`libqsgepaper.so`) is proprietary, **not** included, and must come
from your own device.
