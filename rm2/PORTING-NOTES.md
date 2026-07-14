# riddle on reMarkable 2 — porting notes

Working port of riddle (takeover flavour) to the **reMarkable 2**, OS
**3.28.0.162** (kernel 5.4.70-v1.6.3-rm11x, armv7l, glibc 2.39, Qt 6.10.3).
Verified on-device: live pen ink (real-time, non-flashing), correct pen
orientation, five-finger exit, and the OpenAI-compatible oracle round-trip.

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
  exactly.
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

## Build

`rm2/build-rm2.sh` — cross-compiles inside an amd64 Docker container holding the
official **rM2 SDK 3.27** (Qt 6.8, forward-compatible with the device's 6.10).
The shim links the device's **own** `libqsgepaper.so` (pulled to
`rm2/vendor/`, git-ignored — proprietary) because the SDK 3.27 copy lacks the
3.28 symbols. `rm2/make-bundle-rm2.sh` stages `dist/riddle-rm2/`.

Deploy (no AppLoad needed):

```sh
scp -O -r dist/riddle-rm2 root@10.11.99.1:/home/root/riddle
ssh root@10.11.99.1 /home/root/riddle/riddle-takeover.sh
```

## Known rough edge

Tom's reply text (drawn with the fast Pen/A2 waveform, like live ink) can look
fragmented at the first/last glyphs — the A2 waveform is 2-level and doesn't
settle crisp text as well as GL16. Candidate fix: give the reply a
`Content`/`Grayscale` (GL16) waveform or a one-shot quality refresh of the reply
region when it finishes. Live ink itself is crisp.
