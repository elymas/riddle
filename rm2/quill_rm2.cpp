// quill_rm2.cpp — reMarkable 2 implementation of the quill C ABI that riddle's
// takeover backend links. Drives the vendor e-ink engine in
// /usr/lib/plugins/scenegraph/libqsgepaper.so directly, xochitl stopped.
//
// Established on-device (OS 3.28.0.162, EPFramebufferCarta1000 panel), from a
// disassembly of swapBuffers_impl plus on-panel probes:
//  - EPFramebuffer::instance() self-initializes the SWTCON worker threads; an
//    explicit EPFramebufferSwtcon::initialize() DEADLOCKS.
//  - The REAL frame buffer the engine renders from is the inline QImage at
//    instance+0x50 — Format_RGB32 (BGRA, 4 bytes/px, stride 5616), NOT the
//    Grayscale8 QImage at +0x60. Writing +0x50's bits reaches the panel.
//    OS 3.29.1.0 grew EPScreenModeMap by 8 bytes, moving both QImages to
//    +0x58 / +0x68, so quill_init() now locates the RGB32 one at runtime.
//  - swapBuffers(QRegion, EPScreenModeMap, flags) is the flush. Internally it
//    clips the region to the framebuffer, splits it per screen-mode, and calls
//    EPFramebufferSwtcon::update() per rect — a true region-scoped update.
//      flags bit0 (FullUpdate 0x01) forces a full-screen GC16 flash.
//      flags 0x00 (PartialUpdate) = fast, region-scoped, non-flashing.
//  - EPScreenModeMap has no exported destructor, so we build one full-screen
//    map per waveform once at init and reuse it (the ctor pre-assigns the whole
//    screen to a slot; the actual update area comes from the QRegion argument).
#include <QCoreApplication>
#include <QImage>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <vector>

enum EPScreenMode { EPS_Pen = 0, EPS_Mono = 1, EPS_Animate = 2, EPS_Gray = 3, EPS_Content = 4, EPS_Full = 5 };
struct Translation { EPScreenMode mode; int slot; };
class EPFramebuffer { public: static EPFramebuffer* instance(); };

// Inline frameBuffer QImage member window (disassembly verified): +0x50 on
// OS 3.28.0.162, +0x58 on 3.29.1.0. 0x78 is the 3.28 object size, so the scan
// never reads past the object on either build.
static const int FB_SCAN_FROM = 0x20, FB_SCAN_TO = 0x78;
static const int MAP_BUF = 512;

static int g_w = 1404, g_h = 1872, g_stride = 5616;
static uint8_t* g_front = nullptr;
static EPFramebuffer* g_fb = nullptr;
static QCoreApplication* g_app = nullptr;

static void (*g_swapRegion)(void*, const QRegion&, const void*, unsigned) = nullptr;
static void (*g_mapCtor)(void*, QSize, std::vector<Translation>&&) = nullptr;
static void (*g_setMode)(void*, const QRegion&, EPScreenMode) = nullptr;

alignas(16) static char g_mapPen[MAP_BUF];
alignas(16) static char g_mapContent[MAP_BUF];
alignas(16) static char g_mapFull[MAP_BUF];

static void build_map(char* buf, EPScreenMode mode) {
    std::vector<Translation> empty;
    g_mapCtor(buf, QSize(g_w, g_h), std::move(empty));
    g_setMode(buf, QRegion(QRect(0, 0, g_w, g_h)), mode);
}

extern "C" {

int quill_init() {
    if (g_app) return 0;
    static int argc = 1;
    static char arg0[] = "riddle";
    static char* argv[] = {arg0, nullptr};
    g_app = new QCoreApplication(argc, argv);

    g_fb = EPFramebuffer::instance();
    if (!g_fb) return 1;

    // Find the inline QImage members by their vtable pointer (same libQt6Gui,
    // so a local QImage carries the identical one); take the RGB32 one.
    QImage probe;
    void* const qimageVtbl = *(void**)&probe;
    QImage* engineFb = nullptr;
    for (int off = FB_SCAN_FROM; off < FB_SCAN_TO && !engineFb; off += 4) {
        QImage* cand = (QImage*)((char*)g_fb + off);
        if (*(void**)cand == qimageVtbl && cand->format() == QImage::Format_RGB32) {
            engineFb = cand;
            fprintf(stderr, "quill: frame buffer QImage at instance+0x%x\n", off);
        }
    }
    if (!engineFb) return 2;
    g_w = engineFb->width();
    g_h = engineFb->height();
    g_stride = (int)engineFb->bytesPerLine();
    if (g_w <= 0 || g_h <= 0 || engineFb->format() != QImage::Format_RGB32) return 3;
    g_front = (uint8_t*)engineFb->constBits();   // never detaches
    if (!g_front) return 4;

    void* so = dlopen(nullptr, RTLD_NOW);
    g_swapRegion = (void (*)(void*, const QRegion&, const void*, unsigned))
        dlsym(so, "_ZN13EPFramebuffer11swapBuffersERK7QRegionRK15EPScreenModeMap6QFlagsINS_10UpdateFlagEE");
    g_mapCtor = (void (*)(void*, QSize, std::vector<Translation>&&))
        dlsym(so, "_ZN15EPScreenModeMapC1E5QSizeOSt6vectorINS_11TranslationESaIS2_EE");
    g_setMode = (void (*)(void*, const QRegion&, EPScreenMode))
        dlsym(so, "_ZN15EPScreenModeMap16setModeForRegionERK7QRegion12EPScreenMode");
    if (!g_swapRegion || !g_mapCtor || !g_setMode) return 5;

    build_map(g_mapPen, EPS_Pen);
    build_map(g_mapContent, EPS_Content);
    build_map(g_mapFull, EPS_Full);
    return 0;
}

int quill_width() { return g_w; }
int quill_height() { return g_h; }
int quill_stride() { return g_stride; }
uint8_t* quill_buffer() { return g_front; }

// riddle modes: 0 = fastest ink, 3 = balanced, 4 = flashing clear.
uint64_t quill_swap(int x, int y, int w, int h, int mode, int full) {
    if (!g_swapRegion) return 0;
    const char* map;
    unsigned flags;
    if (full || mode == 4) {
        map = g_mapFull;
        flags = 0x01; // FullUpdate: full-screen GC16 flash (de-ghost / clear)
    } else if (mode == 0) {
        map = g_mapPen;
        flags = 0x00; // PartialUpdate: fast, region-scoped, non-flashing ink
    } else {
        map = g_mapContent;
        flags = 0x00; // partial, higher-quality waveform
    }
    g_swapRegion(g_fb, QRegion(QRect(x, y, w, h)), map, flags);
    return 0;
}

void quill_process_events() {
    if (g_app) QCoreApplication::processEvents();
}

} // extern "C"
