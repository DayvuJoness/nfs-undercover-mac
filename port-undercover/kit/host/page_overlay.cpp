#include "../platform/os.h"
#include <algorithm>
#include <cstdio>
#include "page_overlay.h"
#include "menu_glass.h"
#include "../runtime/mods_seam.h"

#include <vector>

namespace {
bool g_enabled = false;
}

void host_page_set_enabled(bool enabled) {
    g_enabled = enabled;
}
bool host_page_enabled() {
    return g_enabled;
}

const void *host_page_overlay(const void *pixels, int w, int h, int bpp, int pitch,
                              const uint32_t *palette) {
    // With mods off this is not merely a frame nobody draws on: registering
    // the page's keyboard would consume F10 and, once open, every navigation
    // key, so a run with mods disabled would not deliver the input a run
    // without the foundation delivered. Nothing here happens at all.
    if (!g_enabled)
        return pixels;

    // The page's keyboard is registered here, on the first frame a host draws,
    // and nowhere else. The loader used to do it and no longer does: a host
    // that never presents has no page to drive, and every host that CAN draw
    // one reaches this function. First frame, not load time, because the
    // loader has certainly run by then - it runs before the entry point.
    static bool page_ready = false;
    if (!page_ready) {
        page_ready = true;
        mods_page_init();
    }

    if (!pixels || w <= 0 || h <= 0)
        return pixels;
    int stride = pitch > 0 ? pitch : w * (bpp / 8);
    if (stride <= 0)
        return pixels;

    // One buffer per thread. A presenter is called from the guest thread that
    // presented, and two of them presenting at once must not share storage.
    static thread_local std::vector<uint8_t> copy;
    size_t bytes = (size_t)stride * (size_t)h;
    const uint8_t *src = (const uint8_t *)pixels;
    copy.assign(src, src + bytes);
    // A no-op when the page is hidden, which is the usual case; the copy is
    // taken anyway so there is one path and not two, and so the "we never
    // write through the caller's pointer" rule holds without a condition.
    mods_page_draw(copy.data(), w, h, bpp, stride, palette);
    return copy.data();
}

bool host_page_rgba(std::vector<uint8_t> *rgba) {
    if (!rgba)
        return false;
    rgba->clear();
    if (!g_enabled || !mods_page_visible())
        return false;
    std::vector<uint16_t> pixels(640 * 480, 0);
    mods_page_draw(pixels.data(), 640, 480, 16, 640 * 2, nullptr);
    rgba->resize(640 * 480 * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        uint16_t c = pixels[i];
        (*rgba)[4 * i] = uint8_t(((c >> 11) & 31) * 255 / 31);
        (*rgba)[4 * i + 1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        (*rgba)[4 * i + 2] = uint8_t((c & 31) * 255 / 31);
        (*rgba)[4 * i + 3] = c ? 255 : 0;
    }
    return true;
}

int host_page_menu(int w, int h, uint64_t *generation, const uint8_t **pixels) {
    if (!g_enabled)
        return 0;
    const int r = mods_menu_render(w, h, generation, pixels);
    // RECOMP_MENU_DUMP=<dir> (2026-11-17): each newly drawn menu page as <dir>/menu_<n>.ppm (at most 40), so the
    // page can be checked without a screen capture (the glass look draws white text on a clear page: the dump puts
    // it over grey).
    static const char *dir = recomp_env("MENU_DUMP");
    static int dumped = 0;
    if (dir && r == 2 && *pixels && dumped < 40) {
        char path[1024];
        snprintf(path, sizeof path, "%s/menu_%02d.ppm", dir, dumped++);
        if (FILE *f = fopen(path, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            std::vector<uint8_t> row(size_t(w) * 3);
            for (int y = 0; y < h; ++y) {
                const uint8_t *src = *pixels + size_t(y) * w * 4;
                for (int x = 0; x < w; ++x) {
                    const int a = src[4 * x + 3];
                    for (int k = 0; k < 3; ++k) // over mid grey; the page is premultiplied or straight - either reads
                        row[size_t(x) * 3 + k] = uint8_t(std::min(255, src[4 * x + k] * a / 255 + 70 * (255 - a) / 255));
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
    }
    return r;
}

// The macOS host defines these (sdl/menu_glass_mac.mm); every other host keeps the compositor's page.
__attribute__((weak)) bool host_menu_glass_active(void) {
    return false;
}
__attribute__((weak)) void host_menu_glass_publish(int, int, const uint8_t *) {}
__attribute__((weak)) void host_menu_glass_hide(void) {}
__attribute__((weak)) void host_menu_glass_tick(void *) {}
