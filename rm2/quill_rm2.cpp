// quill_rm2.cpp — reMarkable 2 implementation of the quill C ABI that riddle's
// takeover backend links (quill_init/width/height/stride/buffer/swap/
// process_events). Drives the vendor e-ink engine in
// /usr/lib/plugins/scenegraph/libqsgepaper.so directly, xochitl stopped.
//
// Engine behavior verified on-device (OS 3.28.0.162, probe 2026-07-14):
//  - EPFramebuffer::instance() self-initializes: spawns the SWTCON worker
//    threads (EPFramebufferQuitterThread, vsync-flip, framegen) and probes the
//    temperature sensor. Dynamic type on this panel: EPFramebufferCarta1000.
//  - setBuffers(std::tuple<QImage,QImage>, QImage*) adopts caller-owned
//    images (format and stride observed switched in the live instance).
//  - swapBuffers(QRect, EPScreenMode, QFlags<UpdateFlag>) pushes a region.
//  - The library shuts the engine down cleanly from its own static
//    destructors, so there is no quill_shutdown in the ABI.
#include <QCoreApplication>
#include <QImage>
#include <QRect>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <tuple>

// ---- vendor ABI (no headers shipped; declarations match the exported,
// c++filt-verified dynamic symbols of the 3.28 libqsgepaper.so) ----
enum EPScreenMode {
    // Metaobject enum table order (measured): Pen Mono Animation UI Content Sleep.
    EPS_Pen = 0,
    EPS_Mono = 1,
    EPS_Animation = 2,
    EPS_UI = 3,
    EPS_Content = 4,
    EPS_Sleep = 5,
};

class EPFramebuffer {
public:
    enum UpdateFlag { UF_None = 0, UF_Full = 1 };
    static EPFramebuffer* instance();
    void setBuffers(std::tuple<QImage, QImage>, QImage*);
    void swapBuffers(QRect, ::EPScreenMode, QFlags<UpdateFlag>);
};

static const int W = 1404;
static const int H = 1872;

// Engine reads through its (implicitly shared) copies of these QImages, which
// alias g_front. We write through the raw pointer only — never through QImage
// non-const methods — so copy-on-write can never detach the engine's view.
static uint8_t* g_front = nullptr;
static QImage g_a, g_b, g_extra;
static QCoreApplication* g_app = nullptr;

extern "C" {

int quill_init() {
    if (g_app) return 0;
    static int argc = 1;
    static char arg0[] = "riddle";
    static char* argv[] = {arg0, nullptr};
    g_app = new QCoreApplication(argc, argv);

    EPFramebuffer* fb = EPFramebuffer::instance();
    if (!fb) return 1;

    g_front = (uint8_t*)malloc((size_t)W * H);
    uint8_t* back = (uint8_t*)malloc((size_t)W * H);
    if (!g_front || !back) return 2;
    memset(g_front, 0xFF, (size_t)W * H);
    memset(back, 0xFF, (size_t)W * H);

    // External-data QImages: the engine's default buffer is Grayscale8, we
    // hand it caller-owned memory in the same format.
    g_a = QImage(g_front, W, H, W, QImage::Format_Grayscale8);
    g_b = QImage(back, W, H, W, QImage::Format_Grayscale8);
    fb->setBuffers(std::make_tuple(g_a, g_b), &g_extra);
    return 0;
}

int quill_width() { return W; }
int quill_height() { return H; }
int quill_stride() { return W; }

uint8_t* quill_buffer() { return g_front; }

// riddle's modes: 0 = fastest ink, 3 = balanced text/anim, 4 = flashing clear.
// ponytail: Content for the flash pass relies on UF_Full for the actual flash;
// if ghosting survives full_refresh on hardware, revisit with
// EPScreenModeMap::setModeForRegion + the QRegion overload.
uint64_t quill_swap(int x, int y, int w, int h, int mode, int full) {
    EPFramebuffer* fb = EPFramebuffer::instance();
    if (!fb) return 0;
    EPScreenMode m = mode == 0 ? EPS_Pen : mode == 4 ? EPS_Content : EPS_UI;
    QFlags<EPFramebuffer::UpdateFlag> flags =
        full ? EPFramebuffer::UF_Full : EPFramebuffer::UF_None;
    fb->swapBuffers(QRect(x, y, w, h), m, flags);
    return 0;
}

void quill_process_events() {
    if (g_app) QCoreApplication::processEvents();
}

} // extern "C"
