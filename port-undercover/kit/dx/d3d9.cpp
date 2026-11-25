#include "../runtime/mods_seam.h"
#include <string>
#include <mutex>
#include <atomic>
// d3d9.cpp - Direct3D 9, for a game that renders through shaders.
//
// The kit's other Direct3D module (d3d.cpp) models version 2: a fixed-function
// recorder reached by QueryInterface on a DirectDraw object. Version 9 shares
// none of that. It is its own DLL with its own factory function, its own
// object model, and a device whose drawing is programmable, so it lives here.
//
// What works today: the factory object, everything a game asks it before it
// commits to a device (adapter count, identifier, display mode, device type
// and format checks, caps) and CreateDevice itself. The device's vtable is
// complete and correctly ordered - a guest calls these by slot index, so the
// order is the interface - but only the handful of methods below do anything;
// every other slot reports itself once and returns D3D_OK. That turns what
// was a crash into a list of exactly which device methods this game uses,
// which is what the drawing work needs next.
//
// Nothing here rasterizes. No pixels are produced by this file yet.
#include "com.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"
#include "../runtime/display_seam.h"
#include "../runtime/win32.h"
#include "../platform/os.h"
#include "../platform/ab_phase.h"
#include "host_api.h"
#include "d3d9_pipeline.h"
#include "../runtime/native_seam.h"
#include "d3d9_raster.h"
#include "d3d9_shader.h"
#include "d3d9_volume.h"
#include "host_d9.h"
#include <unordered_map>
#include <map>
#include <tuple>

#include <string.h>
#include <iterator>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>

#define IID_BYTES(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                         \
    {(uint8_t)((a) & 0xff),                                                                        \
     (uint8_t)(((a) >> 8) & 0xff),                                                                 \
     (uint8_t)(((a) >> 16) & 0xff),                                                                \
     (uint8_t)(((a) >> 24) & 0xff),                                                                \
     (uint8_t)((b) & 0xff),                                                                        \
     (uint8_t)(((b) >> 8) & 0xff),                                                                 \
     (uint8_t)((c) & 0xff),                                                                        \
     (uint8_t)(((c) >> 8) & 0xff),                                                                 \
     d0,                                                                                           \
     d1,                                                                                           \
     d2,                                                                                           \
     d3,                                                                                           \
     d4,                                                                                           \
     d5,                                                                                           \
     d6,                                                                                           \
     d7}

// {81BDCBCA-64D4-426d-AE8D-AD0147F4275C} and {D0223B96-BF7A-43fd-92BD-A43B0D82B9EB}
static const uint8_t IID_IDirect3D9_[16] =
    IID_BYTES(0x81bdcbca, 0x64d4, 0x426d, 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c);
static const uint8_t IID_IDirect3DDevice9_[16] =
    IID_BYTES(0xd0223b96, 0xbf7a, 0x43fd, 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb);

#define MAKE_D3DHRESULT(code) (0x88760000u | (uint32_t)(code))
static const uint32_t D3D_OK9 = 0u;
static const uint32_t D3DERR_NOTAVAILABLE = MAKE_D3DHRESULT(2154);
static const uint32_t D3DERR_INVALIDCALL = MAKE_D3DHRESULT(2156);
// The draw limits this device reports in its caps (GetDeviceCaps) and enforces in the four draw calls (Test155):
// one definition, so what the guest is told and what is enforced cannot drift apart.
static const uint32_t D9_MAX_PRIMITIVE_COUNT = 1048575;
static const uint32_t D9_MAX_VERTEX_INDEX = 1048575;
static const uint32_t D3DERR_WASSTILLDRAWING = MAKE_D3DHRESULT(540);
static const uint32_t D3DERR_NOTFOUND9 = MAKE_D3DHRESULT(2150);
static const uint32_t D3DLOCK_DONOTWAIT = 0x4000u;

// Defined with the resources further down.
struct ComObj;
static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s);
static bool gpu_on();
static HostD9Target gpu_target(ComObj *dev);
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]);
static void gpu_target_drawn(ComObj *dev, bool color, bool depth);
static void gpu_sync(ComObj *o);

// One scratch block for structures a method fills in for the guest.
static uint32_t g_scratch = 0, g_scratch_size = 0;
static uint32_t scratch9(uint32_t n) {
    if (g_scratch_size < n) {
        if (g_scratch)
            heap_free(g_scratch);
        g_scratch = heap_alloc(n, true, 16);
        g_scratch_size = g_scratch ? n : 0;
    }
    if (g_scratch)
        memset(gm_ptr(g_scratch), 0, g_scratch_size);
    return g_scratch;
}

static ComObj *this_d3d9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9) ? o : nullptr;
}
static ComObj *this_device9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9DEVICE) ? o : nullptr;
}

// ---------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------
// One adapter, the host window's display. A game that enumerates further gets
// an honest "there is only one".
void D9_RegisterSoftwareDevice(X86 *c) {
    com_ret(c, D3DERR_NOTAVAILABLE);
}
void D9_GetAdapterCount(X86 *c) {
    set_eax(c, 1);
}

// D3DADAPTER_IDENTIFIER9: two 512-byte strings, a 32-byte device name, the
// driver version, four ids, the identifier GUID and the WHQL level.
void D9_GetAdapterIdentifier(X86 *c) {
    uint32_t out = arg(c, 3);
    if (!out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(out), 0, 1100);
    // Games of this era pick their feature set by vendor and device id, and
    // turn off whatever they do not recognise (Most Wanted offers shadows
    // only on NVIDIA and ATI). The renderer covers what a GeForce 7800 GTX
    // offered, hardware shadow maps included, so that is the card it names.
    gm_put_str(out, "nv4_disp.dll", 512);
    gm_put_str(out + 512, "NVIDIA GeForce 7800 GTX", 512);
    gm_put_str(out + 1024, "\\\\.\\DISPLAY1", 32);
    // Driver[512], Description[512], DeviceName[32], then:
    wr32(out + 1056, 0x000a249b); // DriverVersion 6.14.10.9371 (low, high)
    wr32(out + 1060, 0x0006000e);
    wr32(out + 1064, 0x10de); // VendorId
    wr32(out + 1068, 0x0091); // DeviceId
    wr32(out + 1076, 0xa1);   // Revision
    wr32(out + 1096, 1);      // WHQLLevel: certified
    com_ret(c, D3D_OK9);
}

// The display modes the adapter offers: what a monitor of the era listed, 4:3 through 16:9 and 16:10, all at
// 60 Hz in X8R8G8B8 (22), sorted by width then height as Windows enumerates them. The host scales whatever back
// buffer the game picks into its window, so any of them is honest. A single 1280x960 entry left NFS Undercover's
// resolution menu with that one choice, letterboxed on a 16:10 MacBook screen (2026-11-15).
// Hosts without a screen (the tests, the headless runner) define neither.
extern "C" __attribute__((weak)) int host_display_screen_size(int *, int *) {
    return 0;
}
extern "C" __attribute__((weak)) int host_display_fullscreen_size(int *, int *) {
    return 0;
}
static const uint32_t k_modes_fixed[][2] = {
    {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800},
    {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200},
    {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600},
};
// The Mac's own screen joins the list (2026-11-15, the person: "в разрешении макбука, 3024 x 1964 и 1512 x 982"):
// its full size in pixels, the same below the menu-bar notch, half of it (the size in points), and three quarters,
// each rounded to even. Built on first use, once the host knows the screen (host_display_screen_size).
static const std::vector<std::pair<uint32_t, uint32_t>> &display_modes() {
    static std::vector<std::pair<uint32_t, uint32_t>> modes;
    static bool with_screen = false;
    if (modes.empty() || !with_screen) {
        modes.clear();
        for (const auto &m : k_modes_fixed)
            modes.push_back({m[0], m[1]});
        // MacBook Pro screens in pixels and in points, whole and below the notch (14-inch, 16-inch) - only while
        // this Mac's own screen is not known yet (2026-11-17: the 16-inch's sizes on a 14-inch were only squeezed
        // and showed half the notch; a game that lists Mac sizes, NFS Undercover's core.nfsu, keeps those the
        // adapter offers).
        static const uint32_t k_mac[][2] = {{3024, 1964}, {3024, 1900}, {1512, 982},  {1512, 950},
                                            {3456, 2234}, {3456, 2160}, {1728, 1117}, {1728, 1080}};
        int sw = 0, sh = 0, fw = 0, fh = 0;
        if (!host_display_screen_size(&sw, &sh) || sw < 640 || sh < 480)
            for (const auto &m : k_mac)
                modes.push_back({m[0], m[1]});
        if (host_display_screen_size(&sw, &sh) && sw >= 640 && sh >= 480) {
            with_screen = true;
            auto even = [](double v) { return (uint32_t)(v / 2.0 + 0.5) * 2u; };
            modes.push_back({(uint32_t)sw, (uint32_t)sh});
            modes.push_back({even(sw * 0.75), even(sh * 0.75)});
            modes.push_back({even(sw * 0.5), even(sh * 0.5)});
            if (host_display_fullscreen_size(&fw, &fh) && fw == sw && fh > 0 && fh < sh) {
                modes.push_back({(uint32_t)fw, (uint32_t)fh});
                modes.push_back({even(fw * 0.5), even(fh * 0.5)});
            }
        }
        std::sort(modes.begin(), modes.end());
        modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
    }
    return modes;
}
// The desktop mode GetAdapterDisplayMode reports: unchanged, so a game that starts at "the desktop resolution"
// keeps the size it always had here.
static const uint32_t k_desktop_w = 1280, k_desktop_h = 960;

// (this, Adapter, Format): modes exist only in the desktop's X8R8G8B8.
void D9_GetAdapterModeCount(X86 *c) {
    const auto &modes = display_modes();
    static size_t logged = 0;
    if (modes.size() != logged) {
        logged = modes.size();
        std::string list;
        for (const auto &m : modes)
            list += " " + std::to_string(m.first) + "x" + std::to_string(m.second);
        LOGW("d3d9: %zu display modes:%s", modes.size(), list.c_str());
    }
    set_eax(c, arg(c, 2) == 22 ? (uint32_t)modes.size() : 0);
}

// D3DDISPLAYMODE: width, height, refresh rate, format. X8R8G8B8 is 22.
static void put_display_mode(uint32_t p, uint32_t w, uint32_t h) {
    if (!p)
        return;
    wr32(p + 0, w);
    wr32(p + 4, h);
    wr32(p + 8, 60);
    wr32(p + 12, 22);
}
// (this, Adapter, Format, Mode, pMode)
void D9_EnumAdapterModes(X86 *c) {
    const auto &modes = display_modes();
    if (arg(c, 2) != 22 || arg(c, 3) >= modes.size() || !arg(c, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    put_display_mode(arg(c, 4), modes[arg(c, 3)].first, modes[arg(c, 3)].second);
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterDisplayMode(X86 *c) {
    put_display_mode(arg(c, 2), k_desktop_w, k_desktop_h);
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceType(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormat(X86 *c) {
    if (arg(c, 5) == 4 && (!d9volume::pixel_bytes(arg(c, 6)) || (arg(c, 4) & ~0x210u))) {
        com_ret(c, D3DERR_NOTAVAILABLE);
        return;
    }
    com_ret(c, D3D_OK9);
}
// D3DMULTISAMPLE_TYPE and quality as a sample count: 2x and 4x, which every
// Apple GPU renders. NONMASKABLE (1) offers them as quality levels 0 and 1.
static uint32_t multisample_count(uint32_t type, uint32_t quality) {
    if (type == 1)
        return quality == 0 ? 2 : 4;
    if (type >= 2)
        return type >= 4 ? 4 : 2;
    return 0;
}
// (this, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
// pQualityLevels)
void D9_CheckDeviceMultiSampleType(X86 *c) {
    uint32_t type = arg(c, 5), quality = arg(c, 6);
    uint32_t levels = type == 0 ? 1 : type == 1 ? 2 : (type == 2 || type == 4) ? 1 : 0;
    if (!levels) {
        com_ret(c, 0x8876086au); // D3DERR_NOTAVAILABLE
        return;
    }
    if (quality && gm_valid(quality, 4))
        wr32(quality, levels);
    com_ret(c, D3D_OK9);
}
void D9_CheckDepthStencilMatch(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormatConversion(X86 *c) {
    com_ret(c, D3D_OK9);
}

// D3DCAPS9 (304 bytes), as a GeForce 7800 GTX reports it for what the
// renderer implements: shader model 3.0, trilinear and 16x anisotropic
// filtering, the usual blend, compare and stencil sets, non-power-of-two
// textures, four render targets.
static void put_caps9(uint32_t p) {
    if (!p)
        return;
    memset(gm_ptr(p), 0, 304);
    auto f = [&](uint32_t off, float v) { wrf32(p + off, v); };
    wr32(p + 0, 1);            // DeviceType: D3DDEVTYPE_HAL
    wr32(p + 8, 0x00020000u);  // Caps: READ_SCANLINE
    wr32(p + 12, 0x00020000u); // Caps2: FULLSCREENGAMMA
    wr32(
        p + 16,
        0x000003a0u); // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, COPY_TO_VIDMEM, COPY_TO_SYSTEMMEM, LINEAR_TO_SRGB
    wr32(p + 20, 0x8000000fu); // PresentationIntervals: IMMEDIATE, ONE..FOUR
    wr32(p + 28, 0x001bbef0u); // DevCaps: HWTRANSFORMANDLIGHT, PUREDEVICE, HWRASTERIZATION, ...
    wr32(
        p + 32,
        0x0003ccf2u); // PrimitiveMiscCaps: masks, cull modes, blend op, separate alpha, independent write masks
    wr32(
        p + 36,
        0x0f732191u); // RasterCaps: dither, zbias, fog, anisotropy, scissor, slope-scale depth bias
    wr32(p + 40, 0x000000ffu); // ZCmpCaps: all
    wr32(p + 44, 0x00003fffu); // SrcBlendCaps: all, BLENDFACTOR
    wr32(p + 48, 0x00003fffu); // DestBlendCaps
    wr32(p + 52, 0x000000ffu); // AlphaCmpCaps
    wr32(p + 56, 0x00084208u); // ShadeCaps
    wr32(
        p + 60,
        0x0001ec45u); // TextureCaps: PERSPECTIVE, ALPHA, CUBEMAP, MIPMAP, MIPCUBEMAP, PROJECTED, VOLUMEMAP, ...
    wr32(
        p + 64,
        0x03030700u); // TextureFilterCaps: MIN point/linear/aniso, MIP point/linear, MAG point/linear
    wr32(p + 68, 0x03030300u);  // CubeTextureFilterCaps
    wr32(p + 72, 0x03030300u);  // VolumeTextureFilterCaps
    wr32(p + 76, 0x0000003fu);  // TextureAddressCaps
    wr32(p + 80, 0x0000003fu);  // VolumeTextureAddressCaps
    wr32(p + 84, 0x0000001fu);  // LineCaps
    wr32(p + 88, 4096);         // MaxTextureWidth
    wr32(p + 92, 4096);         // MaxTextureHeight
    wr32(p + 96, 512);          // MaxVolumeExtent
    wr32(p + 100, 8192);        // MaxTextureRepeat
    wr32(p + 104, 8192);        // MaxTextureAspectRatio
    wr32(p + 108, 16);          // MaxAnisotropy
    f(112, 1e10f);              // MaxVertexW
    f(116, -1e8f);              // GuardBandLeft
    f(120, -1e8f);              // GuardBandTop
    f(124, 1e8f);               // GuardBandRight
    f(128, 1e8f);               // GuardBandBottom
    wr32(p + 136, 0x000001ffu); // StencilCaps: all ops, TWOSIDED
    wr32(p + 140, 0x00080008u); // FVFCaps: 8 texture coordinates, PSIZE
    wr32(p + 144, 0x03feffffu); // TextureOpCaps
    wr32(p + 148, 8);           // MaxTextureBlendStages
    wr32(p + 152, 8);           // MaxSimultaneousTextures
    wr32(p + 156, 0x0000007bu); // VertexProcessingCaps
    wr32(p + 160, 8);           // MaxActiveLights
    wr32(p + 164, 6);           // MaxUserClipPlanes
    wr32(p + 168, 4);           // MaxVertexBlendMatrices
    f(176, 8192.0f);            // MaxPointSize
    wr32(p + 180, D9_MAX_PRIMITIVE_COUNT); // MaxPrimitiveCount (enforced by draw_refused)
    wr32(p + 184, D9_MAX_VERTEX_INDEX);    // MaxVertexIndex (enforced by draw_refused)
    wr32(p + 188, 16);          // MaxStreams
    wr32(p + 192, 255);         // MaxStreamStride
    wr32(p + 196, 0xfffe0300u); // VertexShaderVersion vs_3_0
    wr32(p + 200, 256);         // MaxVertexShaderConst
    wr32(p + 204, 0xffff0300u); // PixelShaderVersion ps_3_0
    f(208, 3.4e38f);            // PixelShaderMaxValue
    wr32(
        p + 212,
        0x00000051u); // DevCaps2: STREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES, VERTEXELEMENTSCANSHARESTREAMOFFSET
    wr32(p + 232, 1);           // NumberOfAdaptersInGroup
    wr32(p + 236, 0x0000030fu); // DeclTypes
    wr32(p + 240, 4);           // NumSimultaneousRTs
    wr32(p + 244, 0x03000300u); // StretchRectFilterCaps: point, linear
    wr32(p + 248, 1);           // VS20Caps.Caps: PREDICATION
    wr32(p + 252, 24);          // VS20Caps.DynamicFlowControlDepth
    wr32(p + 256, 32);          // VS20Caps.NumTemps
    wr32(p + 260, 4);           // VS20Caps.StaticFlowControlDepth
    wr32(p + 264, 0x1f);        // PS20Caps.Caps
    wr32(p + 268, 24);          // PS20Caps.DynamicFlowControlDepth
    wr32(p + 272, 32);          // PS20Caps.NumTemps
    wr32(p + 276, 4);           // PS20Caps.StaticFlowControlDepth
    wr32(p + 280, 512);         // PS20Caps.NumInstructionSlots
    wr32(p + 284, 0x00000300u); // VertexTextureFilterCaps
    wr32(p + 288, 0xffffffffu); // MaxVShaderInstructionsExecuted
    wr32(p + 292, 0xffffffffu); // MaxPShaderInstructionsExecuted
    wr32(p + 296, 32768);       // MaxVertexShader30InstructionSlots
    wr32(p + 300, 32768);       // MaxPixelShader30InstructionSlots
}
void D9_GetDeviceCaps(X86 *c) {
    put_caps9(arg(c, 3));
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterMonitor(X86 *c) {
    set_eax(c, 1); // a non-null HMONITOR
}

void D9_CreateDevice(X86 *c) {
    ComObj *d3d = this_d3d9(c);
    uint32_t present = arg(c, 5), out = arg(c, 6);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_out_ptr(out, 0);
    // Direct3D 9 resets the calling thread's FPU at device creation unless the
    // behaviour flags (arg 4) hold D3DCREATE_FPU_PRESERVE (2): it masks every
    // floating-point exception (and selects single precision). NFS Carbon
    // unmasks invalid/zero-divide/overflow with _controlfp(3, 0x8001f) just
    // before its CreateDevice and relies on this reset; left unmasked, a CRT
    // maths call with an invalid argument raised STATUS_FLOAT_INVALID_OPERATION
    // and the CRT ended the game (first safehouse visit, 2026-10-10). Only the
    // masks are reset here; precision stays as the guest set it.
    if (!(arg(c, 4) & 2u))
        c->fpu_cw = uint16_t(c->fpu_cw | 0x3fu);
    ComObj *dev = com_new(K_D3D9DEVICE);
    if (!dev) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    dev->dev_d3d = d3d->id;
    // D3DPRESENT_PARAMETERS: back buffer size and format first, the device
    // window at 28 and Windowed at 32.
    if (present) {
        LOGW("d3d9: CreateDevice asks for presentation interval %08x, refresh %u Hz, windowed %u",
             rd32(present + 0x34), rd32(present + 0x30), rd32(present + 0x20));
        dev->width = rd32(present + 0);
        dev->height = rd32(present + 4);
        dev->bpp = 32;
        dev->hwnd = rd32(present + 28);
        dev->samples = multisample_count(rd32(present + 16), rd32(present + 20));
    }
    uint32_t view = com_view(dev, IF_D3DDEVICE9);
    if (!view) {
        com_release(dev);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    {
        ComObj *bb = device_backbuffer(dev);
        D9Pipeline &pl = d9_pipeline(dev->id);
        bind_color_target(dev, 0, bb); // Test159: the device holds its bound target
        pl.scissor[2] = (int32_t)dev->width;
        pl.scissor[3] = (int32_t)dev->height;
        if (present && rd32(present + 36)) {
            device_depthbuffer(dev);
            pl.rs[7] = 1; // ZENABLE defaults on with an automatic depth buffer
            pl.states_changed();
        }
    }
    LOGW("d3d9: CreateDevice %ux%u, window %08x, %s renderer", dev->width, dev->height, dev->hwnd,
         host_d9_active() ? "GPU" : "CPU");
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);

// ---------------------------------------------------------------------------
// IDirect3DDevice9
//
// Every slot is named and in its interface order, because a guest calls them
// by index. A stub reports itself once and returns D3D_OK: a game that is
// told "no" by a method it expects to work usually stops, and stopping tells
// us less than carrying on does.
// ---------------------------------------------------------------------------
#define D9_STUB(name, argc)                                                                        \
    void Dev_##name(X86 *c) {                                                                      \
        log_once("d3d9.dev." #name, "d3d9: IDirect3DDevice9::" #name " is not implemented");       \
        (void)argc;                                                                                \
        com_ret(c, D3D_OK9);                                                                       \
    }

D9_STUB(EvictManagedResources, 1)
D9_STUB(GetDisplayMode, 3)
D9_STUB(GetCreationParameters, 2)
D9_STUB(SetCursorProperties, 4)
D9_STUB(SetCursorPosition, 4)
D9_STUB(ShowCursor, 2)
D9_STUB(CreateAdditionalSwapChain, 3)
D9_STUB(GetSwapChain, 3)
D9_STUB(GetRasterStatus, 3)
D9_STUB(SetDialogBoxMode, 2)
// The game's brightness (Carbon's Brightness slider): SetGammaRamp(swapchain, flags,
// D3DGAMMARAMP *), 3 x 256 WORDs. Kept for GetGammaRamp; the presenter applies it.
uint16_t g_gamma_ramp[768];
bool g_gamma_set = false;
void Dev_SetGammaRamp(X86 *c) {
    const uint32_t ramp = arg(c, 3);
    if (ramp && gm_valid(ramp, sizeof g_gamma_ramp)) {
        memcpy(g_gamma_ramp, gm_ptr(ramp), sizeof g_gamma_ramp);
        g_gamma_set = true;
        // What the game asked for, once per distinct ramp shape (red 0/128/255, green 255, blue 255): a ramp
        // the compositor applies to every draw can black the whole screen while the frames themselves are fine.
        static uint64_t last_logged = ~0ull;
        const uint64_t shape = (uint64_t)g_gamma_ramp[0] | ((uint64_t)g_gamma_ramp[128] << 16) |
                               ((uint64_t)g_gamma_ramp[255] << 32) | ((uint64_t)g_gamma_ramp[511] << 48);
        if (shape != last_logged) {
            last_logged = shape;
            LOGW("d3d9: SetGammaRamp red 0/64/128/255 = %u/%u/%u/%u, green 255 = %u, blue 255 = %u (flags %u)",
                 g_gamma_ramp[0], g_gamma_ramp[64], g_gamma_ramp[128], g_gamma_ramp[255], g_gamma_ramp[511],
                 g_gamma_ramp[767], arg(c, 2));
        }
        host_set_gamma_ramp(g_gamma_ramp);
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetGammaRamp(X86 *c) {
    const uint32_t ramp = arg(c, 2);
    if (ramp && gm_valid(ramp, sizeof g_gamma_ramp)) {
        if (!g_gamma_set)
            for (int i = 0; i < 768; ++i)
                g_gamma_ramp[i] = uint16_t((i % 256) * 257);
        memcpy(gm_ptr(ramp), g_gamma_ramp, sizeof g_gamma_ramp);
    }
    com_ret(c, D3D_OK9);
}
void Dev_CreateVolumeTexture(X86 *c);
D9_STUB(UpdateSurface, 5)
void Dev_UpdateTexture(X86 *c);
void Dev_GetRenderTargetData(X86 *c);
D9_STUB(GetFrontBufferData, 3)
D9_STUB(ColorFill, 4)
void Dev_GetRenderTarget(X86 *c);
D9_STUB(GetTransform, 3)
D9_STUB(MultiplyTransform, 3)
D9_STUB(SetMaterial, 2)
D9_STUB(GetMaterial, 2)
D9_STUB(SetLight, 3)
D9_STUB(GetLight, 3)
D9_STUB(LightEnable, 3)
D9_STUB(GetLightEnable, 3)
D9_STUB(SetClipPlane, 3)
D9_STUB(GetClipPlane, 3)
void Dev_GetRenderState(X86 *c);
D9_STUB(CreateStateBlock, 3)
D9_STUB(BeginStateBlock, 1)
D9_STUB(EndStateBlock, 2)
D9_STUB(SetClipStatus, 2)
D9_STUB(GetClipStatus, 2)
void Dev_GetTexture(X86 *c);
D9_STUB(GetTextureStageState, 4)
void Dev_GetSamplerState(X86 *c);
D9_STUB(ValidateDevice, 2)
D9_STUB(SetPaletteEntries, 3)
D9_STUB(GetPaletteEntries, 3)
D9_STUB(SetCurrentTexturePalette, 2)
D9_STUB(GetCurrentTexturePalette, 2)
D9_STUB(GetScissorRect, 2)
D9_STUB(SetSoftwareVertexProcessing, 2)
D9_STUB(GetSoftwareVertexProcessing, 1)
D9_STUB(SetNPatchMode, 2)
D9_STUB(GetNPatchMode, 1)
D9_STUB(ProcessVertices, 6)
void Dev_GetVertexDeclaration(X86 *c);
D9_STUB(SetFVF, 2)
D9_STUB(GetFVF, 2)
void Dev_CreateVertexShader(X86 *c);
void Dev_SetVertexShader(X86 *c);
void Dev_GetVertexShader(X86 *c);
D9_STUB(GetVertexShaderConstantF, 4)
D9_STUB(SetVertexShaderConstantI, 4)
D9_STUB(GetVertexShaderConstantI, 4)
D9_STUB(GetStreamSource, 5)
// (this, StreamNumber, Setting): D3D9 hardware instancing. Stream 0 gets
// D3DSTREAMSOURCE_INDEXEDDATA | instance count, the per-instance streams
// D3DSTREAMSOURCE_INSTANCEDATA | 1; 1 restores ordinary per-vertex data. Kept
// in the pipeline and carried by each draw (HostD9Stream::freq). NFS Undercover
// draws repeated scenery and traffic this way; as a stub every instanced mesh
// was drawn once, reading its per-instance data per vertex (stray polygons).
void Dev_SetStreamSourceFreq(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stream = arg(c, 1), setting = arg(c, 2);
    // D3D9 refuses both flags at once, a zero count, and instance data on stream 0.
    if (!dev || stream >= 8 || (setting & 0xc0000000u) == 0xc0000000u ||
        ((setting & 0xc0000000u) && !(setting & 0x3fffffffu)) ||
        (stream == 0 && (setting & 0x80000000u))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9_pipeline(dev->id).stream[stream].freq = setting ? setting : 1;
    com_ret(c, D3D_OK9);
}

// (this, StreamNumber, pSetting)
void Dev_GetStreamSourceFreq(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stream = arg(c, 1), out = arg(c, 2);
    if (!dev || stream >= 8 || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).stream[stream].freq);
    com_ret(c, D3D_OK9);
}
D9_STUB(GetIndices, 2)
void Dev_CreatePixelShader(X86 *c);
void Dev_SetPixelShader(X86 *c);
void Dev_GetPixelShader(X86 *c);
D9_STUB(GetPixelShaderConstantF, 4)
D9_STUB(SetPixelShaderConstantI, 4)
D9_STUB(GetPixelShaderConstantI, 4)
D9_STUB(DrawRectPatch, 4)
D9_STUB(DrawTriPatch, 4)
D9_STUB(DeletePatch, 2)

// (this, pRect)
void Dev_SetScissorRect(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t r = arg(c, 1);
    if (dev && r)
        for (int i = 0; i < 4; ++i)
            d9_pipeline(dev->id).scissor[i] = (int32_t)rd32(r + 4u * (uint32_t)i);
    com_ret(c, dev && r ? D3D_OK9 : D3DERR_INVALIDCALL);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void bind_depth_stencil(ComObj *dev, ComObj *s);
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s);
static std::unordered_map<uint32_t, uint32_t> &auto_depth_surfaces();
static std::unordered_map<uint32_t, uint32_t> &resource_usage();
static void device_forget_buffers(ComObj *dev);

// (this, pPresentationParameters): new back buffer and depth buffer at the
// new size; every resource the game made itself survives, as with
// D3DPOOL_MANAGED.
void Dev_Reset(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t present = arg(c, 1);
    if (!dev || !present) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    LOGW("d3d9: Reset %ux%u, presentation interval %08x", rd32(present + 0), rd32(present + 4), rd32(present + 0x34));
    dev->width = rd32(present + 0);
    dev->height = rd32(present + 4);
    dev->samples = multisample_count(rd32(present + 16), rd32(present + 20));
    // Test159: Reset drops the device's bindings of render targets 1-3 and puts the new back buffer on 0.
    for (uint32_t i = 1; i < 4; ++i)
        bind_color_target(dev, i, nullptr);
    device_forget_buffers(dev);
    dev->present_deadline = FrameDeadline{};
    dev->present_slots = SlotPacer{};
    dev->present_fps = 0;
    dev->present_locked = -1;
    ComObj *bb = device_backbuffer(dev);
    D9Pipeline &pl = d9_pipeline(dev->id);
    bind_color_target(dev, 0, bb);
    pl.viewport_set = false;
    pl.scissor[0] = pl.scissor[1] = 0;
    pl.scissor[2] = (int32_t)dev->width;
    pl.scissor[3] = (int32_t)dev->height;
    if (rd32(present + 36)) { // EnableAutoDepthStencil
        device_depthbuffer(dev);
        pl.rs[7] = 1;
        pl.states_changed();
    } else
        bind_depth_stencil(dev, nullptr);
    LOGW("d3d9: Reset to %ux%u", dev->width, dev->height);
    // Test157c diagnostics (RECOMP_D3D9_STATS): live objects after each Reset, to tell a per-reset retention from
    // normal reloading (the review's 20-reset discriminator).
    static const bool reset_stats = recomp_env("D3D9_STATS") != nullptr;
    if (reset_stats && g_com_objects) {
        uint32_t surfaces = 0, textures = 0, vbs = 0, ibs = 0, all = 0;
        for (const auto &o : *g_com_objects)
            if (o && o->alive) {
                ++all;
                surfaces += o->kind == K_D3D9SURFACE;
                textures += o->kind == K_D3D9TEXTURE;
                vbs += o->kind == K_D3D9VB;
                ibs += o->kind == K_D3D9IB;
            }
        LOGW("d3d9 stats: after Reset: %u live objects (%u surfaces, %u textures, %u vertex / %u index buffers); "
             "bound depth %u, automatic depth %u",
             all, surfaces, textures, vbs, ibs, dev->zbuffer_obj, auto_depth_surfaces()[dev->id]);
        // Objects made since the previous Reset that are still alive, grouped by kind/size/format/usage/refs: what a
        // Reset leaves behind (default-pool render targets should not survive two Resets).
        static uint32_t prev_mark = 0;
        const uint32_t mark = (uint32_t)g_com_objects->size();
        std::map<std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t, int32_t>, uint32_t> groups;
        for (uint32_t id = prev_mark + 1; id <= mark; ++id) {
            const ComObj *o = (*g_com_objects)[id - 1].get();
            if (!o || !o->alive)
                continue;
            auto u = resource_usage().find(o->id);
            ++groups[{(int)o->kind, o->width, o->height, o->rmask, u == resource_usage().end() ? 0u : u->second, o->refs}];
        }
        std::vector<std::pair<uint32_t, std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t, int32_t>>> top;
        for (const auto &g : groups)
            top.push_back({g.second, g.first});
        std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t i = 0; i < top.size() && i < 8; ++i)
            LOGW("d3d9 stats:   survivors since the last Reset: %u x kind %d %ux%u format %u usage %u refs %d", top[i].first,
                 std::get<0>(top[i].second), std::get<1>(top[i].second), std::get<2>(top[i].second),
                 std::get<3>(top[i].second), std::get<4>(top[i].second), std::get<5>(top[i].second));
        prev_mark = mark;
    }
    com_ret(c, D3D_OK9);
}

void Dev_TestCooperativeLevel(X86 *c) {
    com_ret(c, this_device9(c) ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_GetAvailableTextureMem(X86 *c) {
    set_eax(c, 256u * 1024u * 1024u);
}
void Dev_GetNumberOfSwapChains(X86 *c) {
    set_eax(c, 1);
}
void Dev_GetDirect3D(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d3d = dev ? com_get(dev->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = com_view(d3d, IF_D3D9);
    com_addref(d3d);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}
void Dev_GetDeviceCaps(X86 *c) {
    put_caps9(arg(c, 1));
    com_ret(c, D3D_OK9);
}
void Dev_BeginScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = true;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_EndScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = false;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// Defined with the resources below.
static ComObj *device_backbuffer(ComObj *dev);
static uint8_t *resource_bytes(ComObj *o);

// (this, Count, pRects, Flags, Color, Z, Stencil). Only the colour target is
// real so far: a 32-bit surface is filled with the colour, over the listed
// D3DRECTs or the whole surface.
// The "void colour" (2026-11-22, NFS Undercover's police helicopter): a game that was never meant to be seen from
// above clears its scene to black, and high up the edge of its world shows that black below the sky. While a mod
// turns this on (recomp_d3d9_set_void_colour), a black whole-target colour+depth clear of a target the size of the
// back buffer - the scene's first clear - uses the given colour instead (a haze matching the horizon). Other clears
// (shadow maps, small targets, rect clears, non-black colours) are untouched.
static std::atomic<uint32_t> g_void_colour{0};
static std::atomic<int> g_void_on{0};
// A core mod's value for a pixel shader register (2026-11-22, NFS Undercover's helicopter: how much distance haze the
// fog pass adds high up). Draws with that pixel shader (code key) get c<reg> = (x, y, z, w) until it is set again;
// a shader patch reads it. Up to 8 at once; all zeros removes one.
struct ModPsReg {
    uint64_t ps;
    uint32_t reg;
    float v[4];
};
std::vector<ModPsReg> g_mod_ps_regs;
std::mutex g_mod_ps_regs_m;
extern "C" __attribute__((used, visibility("default"))) int recomp_d3d9_ps_reg_set(uint64_t ps_code, uint32_t reg,
                                                                                    float x, float y, float z, float w) {
    if (reg >= 224)
        return 0;
    std::lock_guard<std::mutex> lock(g_mod_ps_regs_m);
    for (size_t i = 0; i < g_mod_ps_regs.size(); ++i)
        if (g_mod_ps_regs[i].ps == ps_code && g_mod_ps_regs[i].reg == reg) {
            if (x == 0 && y == 0 && z == 0 && w == 0)
                g_mod_ps_regs.erase(g_mod_ps_regs.begin() + (long)i);
            else
                g_mod_ps_regs[i].v[0] = x, g_mod_ps_regs[i].v[1] = y, g_mod_ps_regs[i].v[2] = z, g_mod_ps_regs[i].v[3] = w;
            return 1;
        }
    if ((x == 0 && y == 0 && z == 0 && w == 0) || g_mod_ps_regs.size() >= 8)
        return 1;
    g_mod_ps_regs.push_back({ps_code, reg, {x, y, z, w}});
    return 1;
}

extern "C" __attribute__((used, visibility("default"))) void recomp_d3d9_set_void_colour(uint32_t argb, int on) {
    g_void_colour.store(argb);
    g_void_on.store(on ? 1 : 0);
}

void Dev_Clear(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t count = arg(c, 1), rects = arg(c, 2), flags = arg(c, 3), color = arg(c, 4);
    if (g_void_on.load() && dev && color == 0 && (flags & 3u) == 3u && !count) {
        ComObj *bb = device_backbuffer(dev);
        ComObj *rt = com_get(dev->render_target);
        static int said;
        if (said < 6 && getenv("RECOMP_VOID_LOG")) {
            ++said;
            fprintf(stderr, "[d3d9] void clear: rt %ux%u bb %ux%u\n", rt ? rt->width : 0, rt ? rt->height : 0,
                    bb ? bb->width : 0, bb ? bb->height : 0);
        }
        if (bb && rt && rt->width == bb->width && rt->height == bb->height)
            color = g_void_colour.load();
    }
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        HostD9Target target = gpu_target(dev);
        int32_t vp[4];
        target_viewport(dev, target, vp);
        std::vector<int32_t> rs;
        if (count && rects && gm_fits(rects, count * 16u))
            for (uint32_t i = 0; i < count * 4; ++i)
                rs.push_back((int32_t)rd32(rects + 4 * i));
        float z;
        uint32_t zbits = arg(c, 5);
        memcpy(&z, &zbits, 4);
        host_d9_clear(&target, vp, (uint32_t)(rs.size() / 4), rs.empty() ? nullptr : rs.data(),
                      flags, color, z, arg(c, 6));
        gpu_target_drawn(dev, flags & 1u, flags & 6u);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *s = com_get(dev->render_target);
    if (!s)
        s = device_backbuffer(dev);
    static uint32_t g_clears = 0;
    if (++g_clears <= 24)
        LOGW("d3d9: clear %u: flags %x colour %08x rects %u, target %u (%ux%u, %u bpp)%s", g_clears,
             flags, color, count, s ? s->id : 0, s ? s->width : 0, s ? s->height : 0,
             s ? s->bpp : 0, (s && s->id == dev->palette_obj) ? ", the back buffer" : "");
    uint8_t *bytes = resource_bytes(s);
    if ((flags & 1u) && bytes && s->bpp == 32) {
        uint32_t n = (count && rects) ? count : 1;
        for (uint32_t r = 0; r < n; ++r) {
            int32_t x1 = 0, y1 = 0, x2 = (int32_t)s->width, y2 = (int32_t)s->height;
            if (count && rects) {
                uint32_t q = rects + r * 16u;
                x1 = std::max(x1, (int32_t)rd32(q));
                y1 = std::max(y1, (int32_t)rd32(q + 4));
                x2 = std::min(x2, (int32_t)rd32(q + 8));
                y2 = std::min(y2, (int32_t)rd32(q + 12));
            }
            for (int32_t y = y1; y < y2; ++y) {
                uint8_t *row = bytes + (size_t)y * s->pitch;
                for (int32_t x = x1; x < x2; ++x)
                    memcpy(row + 4 * x, &color, 4);
            }
        }
        d9_raster_invalidate(s->id);
    }
    com_ret(c, D3D_OK9);
}

// (this, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion). The back
// buffer goes to the host as a 32-bit frame: the first thing this module puts
// on screen.
void profile_note_frame(); // runtime/profile.cpp: the load journal's frame count
// Loading screens unpaced (2026-11-18). A game whose loader advances once per frame (NFS Undercover's does) loads only
// as fast as its loading screen is presented: with vertical sync a 3.5 s scene load waited for the screen 120 times a
// second, the main thread idle ~70% of the first second. A mod that knows a loading screen is up asks for frames
// without the limit or the wait for the screen for the next `ms` milliseconds (core.nfsu: the world stopped and the
// game reading over 20 MB/s); the presenter shows the newest. Exported for mod dylibs (used: not dead-stripped).
static std::atomic<int64_t> g_unpaced_until_ns{0};
extern "C" __attribute__((used, visibility("default"))) void recomp_present_unpaced_for(uint32_t ms) {
    static const bool off = recomp_env("PRESENT_LOADS_PACED") != nullptr; // RECOMP_PRESENT_LOADS_PACED=1: old
    if (!off)
        g_unpaced_until_ns.store(os_monotonic_ns() + int64_t(ms) * 1000000, std::memory_order_relaxed);
}
extern "C" int d3dx9_ctab_names(const uint8_t *code, size_t size, uint64_t key, char *out, size_t out_size);
extern "C" int d3dx9_ctab_float_register(const uint8_t *code, size_t size, uint64_t key, const char *name,
                                         uint32_t *reg, uint32_t *count);
// Presents so far (diagnostics: RECOMP_D3D9_CONST_PROBE picks a frame by it).
static std::atomic<uint64_t> g_d9_presents{0};
// How far the camera's car is under a roof (0 open .. 1 a tunnel or a garage for a while), set by the car lights
// (RECOMP_D3D9_CAR_LIGHTS) at the fog pass and read by PS_CONST_SCALE rules "name:factor@roof" (2026-11-19).
static std::atomic<float> g_d9_roof{0.0f};
// Car headlights as lights (2026-11-18, NFS Undercover first; RECOMP_D3D9_CAR_LIGHTS="ps=<hex>,car=<ps constant>,
// first=<register>[,env=<ps constant>,dark=,bright=,gain=,front=,side=,height=,always=1]"): during a frame, the world
// matrix (vertex constant cmWorldMat) of every draw whose pixel shader has the `car` constant (the car-paint shaders) is
// collected, one per car (within 2.5 m = the same car), with the luminance of its `env` constant (the game's local
// light colour for that car), and the view projection (cmWorldViewProj of a draw whose cmWorldMat is the identity).
// Cars are followed from frame to frame; a car's lamps come on after 1.5 s with its light below `dark` and go out
// after 2 s above `bright`. When the pass with pixel shader `ps` draws (the game's fog pass, which reads the scene's
// depth: mods/core/nfsu/shaders/<ps>.patch adds the light), registers first.. get the inverse view projection, the
// camera and lamp count, the view projection, (shadowed lamps, 0), two lamps per lit car (nearest first,
// up to 32 within 600 m) and their colour - the layout is in the code below. A car's local x axis is its forward, y
// its side, z up (row-vector matrices, translation in row 3). Core setting "headlights" (live).
// The sky probe (the host renderer's host_d9_sky overrides this): unknown without one (tests, other backends).
extern "C" __attribute__((weak)) void host_d9_sky(float *share, float *lum) {
    if (share)
        *share = -1.0f;
    if (lum)
        *lum = -1.0f;
}
// The probe grid (host renderer): 96 x 64 cells of the latest probed frame - depth at the centre; the brightest pixel's
// depth, brightness and colour. The weak default has none.
extern "C" __attribute__((weak)) uint64_t host_d9_probe_grid(int *w, int *h, float *, float *, float *, float *) {
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    return 0;
}
namespace carlights {
struct Car {
    float m[4][4];
    float dist2;
    float env = -1.0f;  // the luminance of the game's local light colour for this car (-1: unknown)
    uint32_t prims = 0; // triangles of the part its matrix came from (the body has the most)
    float level = 0.0f; // its lamps, 0 off .. 1 on
};
struct State {
    uint64_t frame = ~0ull;
    std::vector<Car> cars;
    float vp[4][4] = {};
    bool have_vp = false;
    float sun = -1.0f; // the world's sun colour luminance (cvWorldSunColour) this frame, -1 unknown
};
static State &state() {
    static State s;
    return s;
}
static bool identity3(const float (*m)[4]) {
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            if (std::fabs(m[r][k] - (r == k ? 1.0f : 0.0f)) > 1e-4f)
                return false;
    return true;
}
static bool invert4(const float a[4][4], float out[4][4]) {
    float inv[16], m[16];
    memcpy(m, a, sizeof m);
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = (double)m[0] * inv[0] + (double)m[1] * inv[4] + (double)m[2] * inv[8] + (double)m[3] * inv[12];
    if (std::fabs(det) < 1e-20)
        return false;
    for (int i = 0; i < 16; ++i)
        (&out[0][0])[i] = float(inv[i] / det);
    return true;
}
} // namespace carlights

void Dev_Present(X86 *c) {
    profile_note_frame();
    g_d9_presents.fetch_add(1, std::memory_order_relaxed);
    ComObj *dev = this_device9(c);
    if (dev) {
        const bool unpaced = os_monotonic_ns() < g_unpaced_until_ns.load(std::memory_order_relaxed);
        const int fps = (recomp_env("PIN_CLOCK") || unpaced) ? 0 : mods_display_fps();
        // Vertical sync (frame limit "screen", 2026-11-17): Present waits until the screen took the previous frame,
        // as a D3D9 Present at the vertical blank does, so each refresh of the window's screen - 60 Hz, 120 Hz or
        // whatever it is now - gets exactly one new frame. The slot pacer below ran on its own idea of the slots
        // and showed 114-118 of 120 (frames sealed near a slot edge landed two in one refresh, then none). A thread
        // that runs free waits on the presenter's signal; one holding the baton polls in the scheduler instead.
        // RECOMP_PRESENT_SLOTS=1 keeps the slot pacer for the screen limit (A/B).
        // A fixed limit the presenter paces at (120 on a 120 Hz screen, 60 on a 60 Hz one, 60 or 40 on 120 Hz) is
        // synchronised the same way (2026-11-17): the person chose "120 FPS" rather than "screen" and got the slot
        // pacer, 846 frames dropped and 768 held twice a minute - dips toward 60 on a 120 Hz screen.
        // 3.0.2: when the presenter paces at the limit itself, its refresh slots on its clock decide.
        // 2026-11-17: vertical sync is its own switch (mods_display_vsync). With it on, a limit BELOW the refresh
        // (40 or 60 on a 120 Hz screen) also waits for its own slot - whole refreshes on the presenter's clock -
        // before the wait for the screen: that wait alone let the game run at the refresh whatever the limit (the
        // person set 40 and 60 and saw 120). With it off, the limit paces on its own clock.
        double pnow = 0, pperiod = 0;
        static const bool own_clock = recomp_env("PRESENT_OWN_CLOCK") != nullptr;
        const bool clock_ok = fps > 0 && !own_clock && host_present_slot_clock(&pnow, &pperiod);
        static const bool slots_only = recomp_env("PRESENT_SLOTS") != nullptr;
        const bool vsync = fps > 0 && !slots_only && !recomp_env("PIN_CLOCK") && mods_display_vsync();
        // Under vsync the presenter's period is the refresh; the limit's slot is a whole number of refreshes.
        double slot_period = 0;
        if (vsync && clock_ok && pperiod > 0 && fps * pperiod < 0.97) {
            const double k = std::floor(1.0 / (fps * pperiod) + 0.5);
            if (k >= 2 && std::fabs(k * pperiod * fps - 1.0) < 0.03)
                slot_period = k * pperiod;
        }
        const bool below_refresh = vsync && pperiod > 0 && fps * pperiod < 0.97;
        const int locked = !vsync && clock_ok && std::fabs(pperiod * fps - 1.0) < 0.03;
        if (vsync && (dev->present_locked != 2 || dev->present_fps != fps)) {
            dev->present_slots = SlotPacer{}; // slots of another length mean nothing now
            dev->present_deadline = FrameDeadline{};
        }
        if (slot_period > 0) {
            for (int tries = 0; tries < 4; ++tries) {
                const uint32_t ms = dev->present_slots.wait_ms(pnow, slot_period);
                if (!ms)
                    break;
                guest_sleep_ms(ms);
                if (!host_present_slot_clock(&pnow, &pperiod))
                    break;
            }
            dev->present_slots.presented(pnow, slot_period);
        } else if (below_refresh) {
            const uint32_t ms = dev->present_deadline.wait_ms(os_monotonic_ns());
            if (ms)
                guest_sleep_ms(ms);
            dev->present_deadline.begin(fps, os_monotonic_ns());
        }
        if (vsync) {
            // Adaptive depth: one frame waiting for the screen at most (the least delay) while the game keeps up;
            // for a second after a refresh found no new frame (the presenter repeated one), one more may wait, so a
            // single slow frame is covered by the one queued ahead of it instead of staying up two refreshes.
            // RECOMP_VSYNC_DEPTH=1 keeps one always, =2 two always.
            static const int depth_env = [] {
                const char *e = recomp_env("VSYNC_DEPTH");
                return e ? atoi(e) : 0;
            }();
            const int64_t vnow = os_monotonic_ns();
            const uint64_t reps = host_present_repeats();
            if (reps != dev->vsync_repeats_seen) {
                if (dev->vsync_repeats_seen)
                    dev->vsync_deep_until = vnow + 1000000000LL;
                dev->vsync_repeats_seen = reps;
            }
            const int queued = depth_env == 1 ? 0 : depth_env == 2 ? 1 : vnow < dev->vsync_deep_until ? 1 : 0;
            if (guest_thread_runs_free()) {
                host_present_wait_queue(0.1, 1, queued);
            } else {
                for (int tries = 0; tries < 100 && !host_present_wait_queue(0, 0, queued); ++tries)
                    guest_sleep_ms(1);
            }
            if (dev->present_locked != 2 || dev->present_fps != fps) {
                dev->present_locked = 2;
                dev->present_fps = fps;
                LOGW("d3d9: frame limit %d fps: vertical sync with the screen's refresh", fps);
            }
        }
        // Otherwise (RECOMP_PRESENT_SLOTS=1) follow the presenter's refresh slots on its clock (SlotPacer,
        // frame_deadline.h); RECOMP_PRESENT_OWN_CLOCK=1 keeps the separate clock (A/B).
        if (!vsync && (fps != dev->present_fps || locked != dev->present_locked)) {
            dev->present_deadline = FrameDeadline{};
            dev->present_slots = SlotPacer{};
            dev->present_fps = fps;
            dev->present_locked = locked;
            LOGW("d3d9: completed-frame limit %d fps (0 = unlimited)%s", fps,
                 locked ? ", on the display's refresh slots" : "");
        }
        // Wait for the previous frame's deadline, then reserve the next
        // slot. Rendering work counts toward the period, the first frame is
        // immediate, and late frames never accumulate a catch-up backlog.
        // Yield the guest baton so audio/input workers continue to run.
        // Only presentation is paced; game clocks and simulation are intact.
        if (vsync) {
            // Paced by the screen above.
        } else if (locked) {
            // Wait again if the scheduler woke the guest a little before the slot (at most three short waits).
            for (int tries = 0; tries < 4; ++tries) {
                const uint32_t ms = dev->present_slots.wait_ms(pnow, pperiod);
                if (!ms)
                    break;
                guest_sleep_ms(ms);
                if (!host_present_slot_clock(&pnow, &pperiod))
                    break;
            }
            dev->present_slots.presented(pnow, pperiod);
        } else {
            const uint32_t ms = dev->present_deadline.wait_ms(os_monotonic_ns());
            if (ms)
                guest_sleep_ms(ms);
            dev->present_deadline.begin(fps, os_monotonic_ns());
        }
        // 3.0.2: once a minute, what the display got (the owner's "59.5" check): new frames shown, frames the
        // presenter dropped (two in one slot), refreshes that repeated a frame (the game was late).
        static int64_t pace_t0 = 0;
        static uint64_t pace_u0 = 0, pace_d0 = 0, pace_r0 = 0;
        const int64_t pace_now = os_monotonic_ns();
        if (!pace_t0 || pace_now - pace_t0 >= 60000000000LL) {
            const uint64_t u = host_present_unique_completed(), d = host_present_drops(), r = host_present_repeats();
            if (pace_t0 && u >= pace_u0)
                LOGW("d3d9: frame pacing, last %.0f s: %llu frames shown (%.2f fps), %llu dropped, %llu repeated (%s)",
                     (pace_now - pace_t0) / 1e9, (unsigned long long)(u - pace_u0),
                     (u - pace_u0) / ((pace_now - pace_t0) / 1e9), (unsigned long long)(d - pace_d0),
                     (unsigned long long)(r - pace_r0),
                     dev->present_locked == 2 ? "vsync" : dev->present_locked == 1 ? "refresh slots" : "own clock");
            pace_t0 = pace_now, pace_u0 = u, pace_d0 = d, pace_r0 = r;
        }
    }
    ComObj *bb = dev ? device_backbuffer(dev) : nullptr;
    if (bb && gpu_on()) {
        gpu_sync(bb);
        ddraw_external_present_begin();
        host_present(nullptr, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        host_d9_present(bb->id, bb->width, bb->height);
        ddraw_external_present_end();
        // Test86 HDR: the core mod's pre-HUD scene marker (private render state 250) lasts one frame.
        if (dev) {
            D9Pipeline &p = d9_pipeline(dev->id);
            if (p.rs[250] || p.rs[251]) {
                p.rs[250] = p.rs[251] = 0;
                p.states_changed();
            }
        }
        // From NFSMW 3.0.4: the settings menu pauses the game here, between two frames (runtime/guest_pause.h).
        host_guest_pause_point();
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *bbytes = resource_bytes(bb);
    if (bb && bbytes) {
        uint32_t lit = 0, n = bb->width * bb->height;
        for (uint32_t i = 0; i < n; i += 97) {
            uint32_t v;
            memcpy(&v, bbytes + (size_t)i * 4, 4);
            if (v & 0x00ffffffu)
                ++lit;
        }
        LOGW("d3d9: present back buffer %u (%ux%u, %u bpp), %u of %u sampled pixels lit, target "
             "now %u",
             bb->id, bb->width, bb->height, bb->bpp, lit, (n + 96) / 97, dev->render_target);
    }
    if (bb && bbytes && bb->bpp == 32) {
        ddraw_external_present_begin();
        host_present(bbytes, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        ddraw_external_present_end();
    }
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// Device resources
//
// A game creates textures, buffers and surfaces before it draws anything, and
// then writes its geometry and its images into them. None of that reaches a
// GPU yet, but the storage is real guest memory, so a Lock hands back a
// pointer the game can fill and the contents survive to be drawn later.
// ---------------------------------------------------------------------------
// A resource's contents live in host memory, as a driver's would: the guest
// heap is the game's, and a texture set would exhaust it. The guest sees the
// bytes only while the resource is locked, through a copy made on the first
// lock and written back on the last unlock.
static uint32_t resource_alloc(ComObj *o, uint32_t bytes) {
    if (!bytes)
        return 0;
    o->blob.assign(bytes, 0);
    o->pixels = 0;
    o->pixels_bytes = bytes;
    return 1;
}

// The bytes to read or write right now: the guest copy while locked.
static uint8_t *resource_bytes(ComObj *o) {
    if (!o)
        return nullptr;
    if (o->pixels)
        return gm_ptr(o->pixels);
    return o->blob.empty() ? nullptr : o->blob.data();
}

static void gpu_before_lock(ComObj *s);
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi, bool appended);
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi) {
    gpu_after_unlock(o, lo, hi, false);
}
static void gpu_forget(ComObj *o);

static uint32_t stage_lock(ComObj *o) {
    if (!o || o->blob.empty())
        return 0;
    if (o->lock_count == 0)
        gpu_before_lock(o);
    if (o->lock_count++ == 0) {
        o->pixels = heap_alloc((uint32_t)o->blob.size(), false, 16);
        if (!o->pixels) {
            o->lock_count = 0;
            return 0;
        }
        memcpy(gm_ptr(o->pixels), o->blob.data(), o->blob.size());
    }
    return o->pixels;
}

static void stage_unlock(ComObj *o) {
    if (!o || o->lock_count <= 0)
        return;
    if (--o->lock_count == 0 && o->pixels) {
        memcpy(o->blob.data(), gm_ptr(o->pixels), o->blob.size());
        heap_free(o->pixels);
        o->pixels = 0;
        d9_raster_invalidate(o->id);
        gpu_after_unlock(o, 0, (uint32_t)o->blob.size());
    }
}

static void readback_forget(ComObj *o);
static void d3d9_resource_destroy(ComObj *o) {
    readback_forget(o);
    if (o->pixels)
        heap_free(o->pixels);
    o->pixels = 0;
    o->lock_count = 0;
    o->pixels_bytes = 0;
    std::vector<uint8_t>().swap(o->blob);
    d9_raster_invalidate(o->id);
    gpu_forget(o);
}

// Bytes per pixel for the formats this game creates. D3DFMT_A8R8G8B8 is 21,
// X8R8G8B8 22, R5G6B5 23, A8 28, D24S8 75, and the DXT block formats carry
// their four characters.
static uint32_t format_bytes(uint32_t fmt) {
    switch (fmt) {
    case 27:
    case 28:
    case 41:
    case 50:
    case 52: // R3G3B2, A8, P8, L8, A4L4
        return 1;
    case 23:
    case 24:
    case 25:
    case 26:
    case 29:
    case 30:
    case 40:
    case 51:
    case 60:
    case 61:
    case 70:
    case 73:
    case 80:
    case 81:
    case 111:
        return 2;
    case 36:
    case 113: // A16B16G16R16, A16B16G16R16F
        return 8;
    case 116: // A32B32G32R32F
        return 16;
    default:
        return 4;
    }
}

// The DXT formats, stored as blocks of 4x4 pixels.
static uint32_t dxt_block_bytes(uint32_t fmt) {
    switch (fmt) {
    case 0x31545844u:
        return 8; // DXT1
    case 0x32545844u:
    case 0x33545844u:
    case 0x34545844u:
    case 0x35545844u:
        return 16;
    default:
        return 0;
    }
}

static ComObj *make_surface(ComObj *dev, uint32_t w, uint32_t h, uint32_t fmt) {
    ComObj *s = com_new(K_D3D9SURFACE);
    if (!s)
        return nullptr;
    s->dev_d3d = dev ? dev->id : 0;
    s->width = w ? w : 1;
    s->height = h ? h : 1;
    s->rmask = fmt; // the D3DFORMAT, which a Direct3D 9 surface has no other field for
    if (uint32_t block = dxt_block_bytes(fmt)) {
        // A compressed surface is rows of blocks; Pitch is the bytes in one.
        uint32_t bw = (s->width + 3) / 4, bh = (s->height + 3) / 4;
        s->bpp = 0;
        s->pitch = bw * block;
        resource_alloc(s, s->pitch * bh);
    } else {
        s->bpp = format_bytes(fmt) * 8;
        s->pitch = s->width * format_bytes(fmt);
        resource_alloc(s, s->pitch * s->height);
    }
    return s;
}

// ---- What the CPU renderer needs to know about surfaces and textures --------
bool d9_surface_info(uint32_t surface_id, D9SurfaceInfo *out) {
    ComObj *s = com_get(surface_id);
    if (!s || s->kind != K_D3D9SURFACE)
        return false;
    out->data = resource_bytes(s);
    out->size = s->blob.size();
    out->width = s->width;
    out->height = s->height;
    out->pitch = s->pitch;
    out->format = s->rmask;
    return true;
}

uint32_t d9_texture_level0(uint32_t texture_id) {
    ComObj *t = com_get(texture_id);
    if (!t)
        return 0;
    if (t->kind == K_D3D9SURFACE)
        return t->id;
    if (t->kind != K_D3D9TEXTURE)
        return 0;
    if (t->caps) // a cube: face zero, level zero, once it exists
        return t->surfaces.empty() ? 0 : t->surfaces[0];
    return t->back_obj;
}

// The surface behind a 2D texture's level, or null.
static ComObj *texture_level(ComObj *tex, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || tex->caps)
        return nullptr;
    if (level < tex->surfaces.size())
        return com_get(tex->surfaces[level]);
    return level == 0 ? com_get(tex->back_obj) : nullptr;
}

// ---- The GPU's copies ------------------------------------------------------
// With a GPU renderer running, every texture, surface and buffer has a mirror
// on the host under the object's id; a surface that belongs to a texture is a
// slot of its container's mirror. The shim's bytes stay the reference for what
// the game wrote, the GPU's for what was drawn: a slot the game unlocked is
// uploaded before the next draw that reads it, and a slot the GPU drew into is
// read back before the game locks it.
static const uint32_t USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2;

struct GpuMirror {
    bool defined = false;
    std::vector<uint8_t> cpu_dirty;      // by slot
    std::vector<uint8_t> gpu_newer;      // by slot
    uint32_t dirty_lo = 0, dirty_hi = 0; // buffers: the changed byte range
    bool dirty_overwrites = false;       // buffers: some of that range came from a lock without NOOVERWRITE
};
static std::unordered_map<uint32_t, GpuMirror> &gpu_mirrors() {
    static auto *m = new std::unordered_map<uint32_t, GpuMirror>();
    return *m;
}
// Test75: a resource's mirror without the hash lookup (map nodes do not move; gpu_forget clears the
// cached pointer before it erases). RECOMP_AB_SWITCHES=mirrors (TEST ONLY) takes the lookup every time.
static GpuMirror &mirror_of(ComObj *o) {
    if (o->gpu_mirror && !recomp_ab::old(recomp_ab::MIRRORS))
        return *static_cast<GpuMirror *>(o->gpu_mirror);
    GpuMirror &m = gpu_mirrors()[o->id];
    o->gpu_mirror = &m;
    return m;
}
// D3DUSAGE of every texture and surface, by id.
static std::unordered_map<uint32_t, uint32_t> &resource_usage() {
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static bool gpu_on() {
    return host_d9_active() != 0;
}
static bool is_depth_format(uint32_t fmt) {
    return (fmt >= 70 && fmt <= 82) || fmt == 0x5a544e49u /* INTZ */ ||
           fmt == 0x34324644u /* DF24 */ || fmt == 0x36314644u /* DF16 */;
}

// Extra state for volume textures and their mip-level interfaces. The texture
// owns one reference to each mip. A mip with any external references holds one
// reference to the texture, released when its last external reference goes away.
static_assert(K_D3D9VOLUME < 64, "COM kind bitset capacity");
struct VolumeState {
    uint32_t depth = 0, pool = 0, lod = 0, priority = 0, lock_flags = 0;
};
static std::unordered_map<uint32_t, VolumeState> &volumes() {
    static auto *v = new std::unordered_map<uint32_t, VolumeState>();
    return *v;
}
static ComObj *volume_level(ComObj *o, uint32_t level) {
    return o && o->kind == K_D3D9VOLUMETEXTURE && level < o->surfaces.size()
               ? com_get(o->surfaces[level])
               : nullptr;
}
static void volume_destroy(ComObj *o) {
    for (uint32_t id : o->surfaces)
        com_destroy(com_get(id));
    o->surfaces.clear();
    volumes().erase(o->id);
    d3d9_resource_destroy(o);
}
// Test159: a 2D or cube texture takes its levels/faces with it, as a volume texture does (volume_destroy). Before,
// they were never destroyed: each texture left its level surfaces alive after its own final Release.
static void texture_destroy(ComObj *o) {
    for (uint32_t id : o->surfaces)
        com_destroy(com_get(id));
    o->surfaces.clear();
    o->back_obj = 0;
    d3d9_resource_destroy(o);
}
static void volume_addref(ComObj *o) {
    if (o && o->refs == 1)
        com_addref(com_get(o->front_obj));
    com_addref(o);
}
static void Vol_AddRef(X86 *c) {
    ComObj *o = com_this_arg(c);
    volume_addref(o);
    com_ret(c, o ? o->refs - 1 : 0);
}
static void Vol_Release(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o || o->refs <= 1) {
        com_ret(c, 0);
        return;
    }
    const uint32_t external = --o->refs - 1;
    if (!external)
        com_release(com_get(o->front_obj)); // may destroy this mip as well
    com_ret(c, external);
}
static void Vol_QueryInterface(X86 *c) {
    ComObj *o = com_this_arg(c);
    const bool internal_only = o && o->refs == 1;
    com_QueryInterface(c);
    if (internal_only && c->r[0] == S_OK)
        com_addref(com_get(o->front_obj));
}

// A surface's place in its mirror: the container's id and the slot, or the
// surface's own id and slot zero.
static uint32_t mirror_slot(ComObj *s, uint32_t *slot) {
    *slot = 0;
    if (!s)
        return 0;
    if ((s->kind == K_D3D9SURFACE || s->kind == K_D3D9VOLUME) && s->front_obj) {
        ComObj *tex = com_get(s->front_obj);
        if (tex && (tex->kind == K_D3D9TEXTURE || tex->kind == K_D3D9VOLUMETEXTURE)) {
            for (size_t i = 0; i < tex->surfaces.size(); ++i)
                if (tex->surfaces[i] == s->id) {
                    *slot = (uint32_t)i;
                    return tex->id;
                }
        }
    }
    return s->id;
}
static HostD9Surface surface_ref(ComObj *s) {
    HostD9Surface r = {0, 0, 0};
    if (!s)
        return r;
    uint32_t slot;
    r.id = mirror_slot(s, &slot);
    ComObj *owner = com_get(r.id);
    if (owner && owner->kind == K_D3D9TEXTURE && owner->caps) {
        r.face = slot % 6;
        r.level = slot / 6;
    } else {
        r.level = slot;
    }
    return r;
}
static uint32_t slot_count(ComObj *o) {
    if (o->kind == K_D3D9TEXTURE || o->kind == K_D3D9VOLUMETEXTURE)
        return (uint32_t)std::max<size_t>(o->surfaces.size(), 1);
    return 1;
}
static ComObj *slot_surface(ComObj *o, uint32_t slot) {
    if (o->kind != K_D3D9TEXTURE && o->kind != K_D3D9VOLUMETEXTURE)
        return o;
    return slot < o->surfaces.size() ? com_get(o->surfaces[slot]) : nullptr;
}

// Creates the mirror of a texture or stand-alone surface on first use.
static GpuMirror &gpu_define(ComObj *o) {
    GpuMirror &m = mirror_of(o);
    if (m.defined)
        return m;
    HostD9TextureDesc d{};
    d.id = o->id;
    d.format = o->rmask ? o->rmask : 22;
    d.width = o->width ? o->width : 1;
    d.height = o->height ? o->height : 1;
    uint32_t usage = resource_usage()[o->id];
    if (is_depth_format(d.format))
        usage |= USAGE_DEPTHSTENCIL;
    d.usage = (usage & USAGE_RENDERTARGET ? HOST_D9_USAGE_RENDERTARGET : 0) |
              (usage & USAGE_DEPTHSTENCIL ? HOST_D9_USAGE_DEPTH : 0);
    d.samples = o->kind == K_D3D9SURFACE ? o->samples : 0;
    if (o->kind == K_D3D9VOLUMETEXTURE) {
        d.kind = HOST_D9_TEX_VOLUME;
        d.depth = volumes().at(o->id).depth;
        d.levels = slot_count(o);
    } else if (o->kind == K_D3D9TEXTURE && o->caps) {
        d.kind = HOST_D9_TEX_CUBE;
        d.levels = o->pal_flags ? o->pal_flags : 1;
    } else {
        d.kind = HOST_D9_TEX_2D;
        d.levels = o->kind == K_D3D9TEXTURE ? slot_count(o) : 1;
    }
    host_d9_texture_define(&d);
    m.defined = true;
    uint32_t n = o->kind == K_D3D9TEXTURE && o->caps ? 6 * d.levels : slot_count(o);
    // A target's first contents are whatever is drawn; a texture's are what
    // the game wrote, so every slot starts out needing an upload.
    m.cpu_dirty.assign(n, d.usage ? 0 : 1);
    m.gpu_newer.assign(n, 0);
    return m;
}

// TexMod packs (2026-11-19, host/texmod.cpp; weak defaults for builds without it): a texture whose top level, as the
// game wrote it, hashes (CRC32 of width x height x bits / 8 bytes, as TexMod counts it) to an image in a switched-on
// pack is drawn with that image instead. The game's own texture is kept and used again as soon as the pack is off.
extern "C" __attribute__((weak)) const uint8_t *host_texmod_find(uint32_t, uint32_t *, int *) {
    return nullptr;
}
extern "C" __attribute__((weak)) int host_texmod_active(void) {
    return 0;
}
extern "C" __attribute__((weak)) uint32_t host_texmod_state(void) {
    return 0;
}
namespace texmod {
// CRC32 (the zlib polynomial), eight bytes a step.
static uint32_t g_tab[8][256];
static void init_tab() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
        for (int t = 1; t < 8; ++t)
            g_tab[t][i] = (g_tab[t - 1][i] >> 8) ^ g_tab[0][g_tab[t - 1][i] & 0xff];
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    static const bool ready = (init_tab(), true);
    (void)ready;
    uint32_t c = 0xffffffffu;
    while (n >= 8) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= c;
        c = g_tab[7][a & 0xff] ^ g_tab[6][(a >> 8) & 0xff] ^ g_tab[5][(a >> 16) & 0xff] ^ g_tab[4][a >> 24] ^
            g_tab[3][b & 0xff] ^ g_tab[2][(b >> 8) & 0xff] ^ g_tab[1][(b >> 16) & 0xff] ^ g_tab[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--)
        c = g_tab[0][(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
static uint32_t bits_of(uint32_t fmt) {
    switch (fmt) {
    case 0x31545844: return 4; // DXT1
    case 0x32545844: case 0x33545844: case 0x34545844: case 0x35545844: return 8; // DXT2-5
    case 20: return 24;
    case 21: case 22: case 32: case 33: return 32;
    case 23: case 24: case 25: case 26: case 51: return 16;
    case 28: case 41: case 50: return 8;
    default: return 0;
    }
}
struct Repl {
    uint32_t hash = 0;     // TexMod hash of the top level as last uploaded (0: not hashed)
    uint32_t host_id = 0;  // the replacement's host texture (0: none made)
    uint32_t state = ~0u;  // host_texmod_state() the decision below was made at
    bool use = false;      // draw the replacement
};
static std::unordered_map<uint32_t, Repl> &table() {
    static std::unordered_map<uint32_t, Repl> t;
    return t;
}
static uint32_t g_next_id = 0x7f000000u; // host ids of replacements (guest objects stay far below)
static uint32_t g_state = 0;
static uint64_t g_hashed = 0, g_matched = 0;
static bool active() {
    static const bool on = host_texmod_active() != 0;
    return on;
}
// Called with the top level's bytes as they go to the GPU.
static void hashed(ComObj *o, const uint8_t *bytes, size_t avail) {
    const uint32_t bits = bits_of(o->rmask ? o->rmask : 22);
    if (!bits || !o->width || !o->height)
        return;
    const size_t n = size_t(bits) * o->width * o->height / 8;
    if (n > avail)
        return;
    Repl &r = table()[o->id];
    const uint32_t h = crc32(bytes, n);
    if (h != r.hash) {
        r.hash = h;
        r.state = ~0u; // decide again
        r.use = false;
    }
    ++g_hashed;
}
// Builds the replacement texture from a DDS file; 0 when its format is not one the GPU takes as it is.
static uint32_t make(const uint8_t *dds, uint32_t size) {
    if (size < 128)
        return 0;
    auto u32 = [&](uint32_t off) { uint32_t v; memcpy(&v, dds + off, 4); return v; };
    const uint32_t h = u32(12), w = u32(16), mips = std::max(1u, u32(28)), pf_flags = u32(80), fourcc = u32(84),
                   rgb_bits = u32(88), rmask = u32(92), amask = u32(104);
    uint32_t fmt = 0, block = 0, bpp = 0;
    bool expand24 = false;
    if (pf_flags & 4) {
        fmt = fourcc;
        block = fourcc == 0x31545844 ? 8 : (fourcc >= 0x32545844 && fourcc <= 0x35545844) ? 16 : 0;
        if (!block)
            return 0;
    } else if (rgb_bits == 32 && rmask == 0x00ff0000) {
        fmt = amask ? 21 : 22;
        bpp = 4;
    } else if (rgb_bits == 24 && rmask == 0x00ff0000) {
        fmt = 22;
        bpp = 4;
        expand24 = true;
    } else {
        return 0;
    }
    if (!w || !h || w > 8192 || h > 8192)
        return 0;
    HostD9TextureDesc d{};
    d.id = g_next_id++;
    d.kind = HOST_D9_TEX_2D;
    d.width = w;
    d.height = h;
    d.format = fmt;
    // levels the file really holds
    uint32_t levels = 0;
    size_t at = 128;
    std::vector<uint8_t> conv;
    struct Lv {
        size_t at;
        uint32_t pitch, bytes;
    };
    std::vector<Lv> lv;
    for (uint32_t l = 0; l < mips; ++l) {
        const uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
        uint32_t pitch, bytes;
        if (block) {
            pitch = std::max(1u, (lw + 3) / 4) * block;
            bytes = pitch * std::max(1u, (lh + 3) / 4);
        } else {
            pitch = lw * (expand24 ? 3 : 4);
            bytes = pitch * lh;
        }
        if (at + bytes > size)
            break;
        lv.push_back({at, pitch, bytes});
        at += bytes;
        ++levels;
    }
    if (!levels)
        return 0;
    d.levels = levels;
    host_d9_texture_define(&d);
    for (uint32_t l = 0; l < levels; ++l) {
        if (expand24) {
            const uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            conv.assign(size_t(lw) * lh * 4, 0xff);
            for (size_t i = 0; i < size_t(lw) * lh; ++i)
                memcpy(&conv[i * 4], dds + lv[l].at + i * 3, 3);
            host_d9_texture_upload(d.id, 0, l, conv.data(), lw * 4);
        } else {
            host_d9_texture_upload(d.id, 0, l, dds + lv[l].at, lv[l].pitch);
        }
    }
    return d.id;
}
// The texture a draw samples for the game's texture `o`.
static uint32_t bound(ComObj *o) {
    // packs switched in the menu: re-decide the replacements about twice a second
    static uint64_t asked_at = ~0ull;
    const uint64_t frame = g_d9_presents.load(std::memory_order_relaxed);
    if (frame / 30 != asked_at) {
        asked_at = frame / 30;
        g_state = host_texmod_state();
    }
    auto it = table().find(o->id);
    if (it == table().end() || !it->second.hash)
        return o->id;
    Repl &r = it->second;
    if (r.state != g_state) {
        r.state = g_state;
        uint32_t size = 0;
        int known = 0;
        // TexMod's CRC32 is left without the final inversion; the standard one is tried too
        const uint8_t *dds = host_texmod_find(~r.hash, &size, &known);
        if (!known)
            dds = host_texmod_find(r.hash, &size, &known);
        if (dds && !r.host_id) {
            r.host_id = make(dds, size);
            if (r.host_id) {
                ++g_matched;
                if (g_matched <= 40 || g_matched % 100 == 0)
                    LOGW("d3d9: texmod: texture %08x (%ux%u) replaced (%llu so far, %llu textures hashed)", r.hash,
                         o->width, o->height, (unsigned long long)g_matched, (unsigned long long)g_hashed);
            } else {
                LOGW("d3d9: texmod: texture %08x: the pack's image is in a format not supported here", r.hash);
            }
        }
        r.use = dds && r.host_id;
    }
    return r.use ? r.host_id : o->id;
}
} // namespace texmod

// Uploads every slot the game changed since the GPU last saw it.
static void gpu_sync(ComObj *o) {
    if (!o)
        return;
    GpuMirror &m = gpu_define(o);
    for (uint32_t slot = 0; slot < m.cpu_dirty.size(); ++slot) {
        if (!m.cpu_dirty[slot])
            continue;
        m.cpu_dirty[slot] = 0;
        ComObj *s = slot_surface(o, slot);
        if (!s || s->blob.empty())
            continue;
        HostD9Surface r = surface_ref(s);
        if (o->kind == K_D3D9VOLUMETEXTURE) {
            const uint32_t depth = volumes().at(s->id).depth;
            for (uint32_t z = 0; z < depth; ++z)
                host_d9_texture_upload(o->id, z, slot, resource_bytes(s) + z * s->pitch * s->height,
                                       s->pitch);
        } else {
            host_d9_texture_upload(o->id, r.face, r.level, resource_bytes(s), s->pitch);
            if (r.face == 0 && r.level == 0 && o->kind == K_D3D9TEXTURE && !o->caps && !m.cpu_dirty.empty() &&
                !(resource_usage()[o->id] & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && texmod::active())
                texmod::hashed(o, resource_bytes(s), s->blob.size());
        }
    }
}

// The surface is about to be read or written by the game.
static void gpu_before_lock(ComObj *s) {
    if (!s || s->kind != K_D3D9SURFACE)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    auto it = gpu_mirrors().find(id);
    if (it == gpu_mirrors().end() || slot >= it->second.gpu_newer.size() ||
        !it->second.gpu_newer[slot])
        return;
    it->second.gpu_newer[slot] = 0;
    if (s->bpp == 32 && !s->blob.empty()) {
        HostD9Surface r = surface_ref(s);
        host_d9_texture_read(id, r.face, r.level, s->blob.data(), s->pitch);
    }
}
// The game wrote the resource.
// `appended`: a buffer range written only through D3DLOCK_NOOVERWRITE locks.
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi, bool appended) {
    if (!o)
        return;
    if (o->kind == K_D3D9VB || o->kind == K_D3D9IB) {
        GpuMirror &m = mirror_of(o);
        if (m.dirty_hi <= m.dirty_lo) {
            m.dirty_lo = lo;
            m.dirty_hi = hi;
            m.dirty_overwrites = !appended;
        } else {
            m.dirty_lo = std::min(m.dirty_lo, lo);
            m.dirty_hi = std::max(m.dirty_hi, hi);
            m.dirty_overwrites |= !appended;
        }
        m.defined = true;
        return;
    }
    uint32_t slot;
    uint32_t id = mirror_slot(o, &slot);
    auto it = gpu_mirrors().find(id);
    if (it == gpu_mirrors().end())
        return; // not mirrored yet: its first use uploads everything
    if (slot < it->second.cpu_dirty.size())
        it->second.cpu_dirty[slot] = 1;
}
static void gpu_drawn_into(ComObj *s) {
    if (!s)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    ComObj *owner = com_get(id);
    if (!owner)
        return;
    GpuMirror &m = gpu_define(owner);
    if (slot < m.gpu_newer.size()) {
        m.gpu_newer[slot] = 1;
        m.cpu_dirty[slot] = 0;
    }
}
static void gpu_sync_buffer(ComObj *b) {
    if (!b)
        return;
    GpuMirror &m = mirror_of(b);
    if (!m.defined) {
        m.defined = true;
        m.dirty_lo = 0;
        m.dirty_hi = (uint32_t)b->blob.size();
        m.dirty_overwrites = true;
    }
    // Autosculpt locks the part's vertex buffer, writes the morphed verts
    // through the pointer it kept, and never calls Unlock. On D3D9 that
    // pointer is the buffer. Here Lock staged a copy, so a draw that still
    // sees the lock must upload that copy or the mesh stays at the pose
    // from the last Unlock (the stock bumper, while the zone line moves).
    if (b->lock_count > 0 && b->pixels && !b->blob.empty()) {
        uint8_t *live = gm_ptr(b->pixels);
        if (memcmp(live, b->blob.data(), b->blob.size()) != 0) {
            memcpy(b->blob.data(), live, b->blob.size());
            m.dirty_lo = 0;
            m.dirty_hi = (uint32_t)b->blob.size();
            m.dirty_overwrites = true;
        }
    }
    if (m.dirty_hi > m.dirty_lo && !b->blob.empty()) {
        uint32_t hi = std::min<uint32_t>(m.dirty_hi, (uint32_t)b->blob.size());
        (m.dirty_overwrites ? host_d9_buffer_upload : host_d9_buffer_append)(
            b->id, (uint32_t)b->blob.size(), m.dirty_lo, b->blob.data() + m.dirty_lo, hi - m.dirty_lo);
    }
    m.dirty_lo = m.dirty_hi = 0;
    m.dirty_overwrites = false;
}
static void gpu_forget(ComObj *o) {
    o->gpu_mirror = nullptr;
    auto it = gpu_mirrors().find(o->id);
    if (it != gpu_mirrors().end()) {
        if (o->kind == K_D3D9VB || o->kind == K_D3D9IB)
            host_d9_buffer_drop(o->id);
        else if (it->second.defined)
            host_d9_texture_drop(o->id);
        gpu_mirrors().erase(it);
    }
    resource_usage().erase(o->id);
}

// (this, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle)
void Dev_CreateVertexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *vb = com_new(K_D3D9VB);
    if (!vb || !resource_alloc(vb, bytes)) {
        if (vb)
            com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    vb->dev_d3d = dev->id;
    vb->buf_bytes = bytes;
    uint32_t view_ = com_view(vb, IF_D3DVERTEXBUFFER9);
    if (!view_) {
        com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle)
void Dev_CreateIndexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *ib = com_new(K_D3D9IB);
    if (!ib || !resource_alloc(ib, bytes)) {
        if (ib)
            com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    ib->dev_d3d = dev->id;
    ib->buf_bytes = bytes;
    ib->rmask = arg(c, 3) == 102 ? 4 : 2; // D3DFMT_INDEX32 or D3DFMT_INDEX16
    uint32_t view_ = com_view(ib, IF_D3DINDEXBUFFER9);
    if (!view_) {
        com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
void Dev_CreateTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 5), out = arg(c, 7);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = w;
    tex->height = h;
    tex->bpp = format_bytes(fmt) * 8;
    // Every level gets its own surface. Levels = 0 asks for the full chain;
    // a game uploads each level with its own LockRect.
    uint32_t levels = arg(c, 3);
    if (!levels) {
        levels = 1;
        for (uint32_t d = std::max(w, h); d > 1; d >>= 1)
            ++levels;
    }
    tex->rmask = fmt;
    uint32_t usage = arg(c, 4);
    resource_usage()[tex->id] = usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    for (uint32_t l = 0; l < levels && l < 16; ++l) {
        ComObj *s = make_surface(dev, std::max(w >> l, 1u), std::max(h >> l, 1u), fmt);
        if (!s)
            break;
        s->front_obj = tex->id;
        tex->surfaces.push_back(s->id);
    }
    tex->back_obj = tex->surfaces.empty() ? 0 : tex->surfaces[0];
    uint32_t view_ = com_view(tex, IF_D3DTEXTURE9);
    if (!view_) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
void Dev_CreateCubeTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t edge = arg(c, 1), fmt = arg(c, 4), out = arg(c, 6);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = tex->height = edge;
    tex->bpp = format_bytes(fmt) * 8;
    tex->rmask = fmt;
    tex->caps = 1;                              // a cube map
    tex->pal_flags = arg(c, 2) ? arg(c, 2) : 1; // levels, reusing a field cubes do not use
    resource_usage()[tex->id] = arg(c, 3) & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    uint32_t view = com_view(tex, IF_D3DCUBETEXTURE9);
    if (!view) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    LOGV("d3d9: CreateCubeTexture %u -> %08x into %08x", edge, view, out);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static void create_surface_method(X86 *c, uint32_t out_index, uint32_t fmt_index, uint32_t usage) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, fmt_index), out = arg(c, out_index);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = make_surface(dev, w, h, fmt);
    if (!s) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    // CreateRenderTarget and CreateDepthStencilSurface: MultiSample, MultisampleQuality.
    if (out_index == 7)
        s->samples = multisample_count(arg(c, 4), arg(c, 5));
    resource_usage()[s->id] = usage;
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Discard,
//  ppSurface, pSharedHandle)
void Dev_CreateDepthStencilSurface(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_DEPTHSTENCIL);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
//  ppSurface, pSharedHandle)
void Dev_CreateRenderTarget(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_RENDERTARGET);
}
// (this, Width, Height, Format, Pool, ppSurface, pSharedHandle)
void Dev_CreateOffscreenPlainSurface(X86 *c) {
    create_surface_method(c, 5, 3, 0);
}

// The device's own back buffer and depth buffer, made on first ask and kept.
// The back buffer lives in its own field. It used to share render_target,
// which SetRenderTarget overwrites, so after the game switched targets the
// "back buffer" was whatever it had last drawn into.
static ComObj *device_backbuffer(ComObj *dev) {
    ComObj *s = com_get(dev->palette_obj);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480, 22);
        dev->palette_obj = s ? s->id : 0;
        if (s)
            s->samples = dev->samples;
        if (s)
            resource_usage()[s->id] = USAGE_RENDERTARGET;
        if (s && !com_get(dev->render_target))
            dev->render_target = s->id;
    }
    return s;
}

// (this, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter): a
// nearest-neighbour copy between 32-bit surfaces, rectangles honoured. This is
// how a game moves an off-screen target onto the back buffer.
void Dev_StretchRect(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 3));
    if (!src || !dst || src->kind != K_D3D9SURFACE || dst->kind != K_D3D9SURFACE) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        uint32_t slot;
        ComObj *sowner = com_get(mirror_slot(src, &slot));
        ComObj *downer = com_get(mirror_slot(dst, &slot));
        if (sowner)
            gpu_sync(sowner);
        if (downer)
            gpu_sync(downer);
        int32_t sr[4] = {0, 0, (int32_t)src->width, (int32_t)src->height};
        int32_t dr[4] = {0, 0, (int32_t)dst->width, (int32_t)dst->height};
        if (arg(c, 2))
            for (int i = 0; i < 4; ++i)
                sr[i] = (int32_t)rd32(arg(c, 2) + 4u * (uint32_t)i);
        if (arg(c, 4))
            for (int i = 0; i < 4; ++i)
                dr[i] = (int32_t)rd32(arg(c, 4) + 4u * (uint32_t)i);
        host_d9_stretch(surface_ref(src), sr, surface_ref(dst), dr, arg(c, 5));
        gpu_drawn_into(dst);
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *sbytes = resource_bytes(src), *dbytes = resource_bytes(dst);
    if (!sbytes || !dbytes || src->bpp != 32 || dst->bpp != 32) {
        com_ret(c, D3D_OK9);
        return;
    }
    auto rect = [](uint32_t p, ComObj *s, int32_t r[4]) {
        r[0] = 0;
        r[1] = 0;
        r[2] = (int32_t)s->width;
        r[3] = (int32_t)s->height;
        if (p)
            for (int i = 0; i < 4; ++i)
                r[i] = (int32_t)rd32(p + 4u * (uint32_t)i);
        r[0] = std::max(r[0], 0);
        r[1] = std::max(r[1], 0);
        r[2] = std::min(r[2], (int32_t)s->width);
        r[3] = std::min(r[3], (int32_t)s->height);
    };
    ComObj *dev_ = com_get(dst->dev_d3d);
    static uint32_t g_stretches = 0;
    if (++g_stretches <= 24)
        LOGW("d3d9: StretchRect %u (%ux%u, %u bpp) -> %u (%ux%u, %u bpp)%s", src->id, src->width,
             src->height, src->bpp, dst->id, dst->width, dst->height, dst->bpp,
             (dev_ && dst->id == dev_->palette_obj) ? ", onto the back buffer" : "");
    int32_t sr[4], dr[4];
    rect(arg(c, 2), src, sr);
    rect(arg(c, 4), dst, dr);
    int32_t sw = sr[2] - sr[0], sh = sr[3] - sr[1], dw = dr[2] - dr[0], dh = dr[3] - dr[1];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        com_ret(c, D3D_OK9);
        return;
    }
    for (int32_t y = 0; y < dh; ++y) {
        int32_t sy = sr[1] + y * sh / dh;
        uint8_t *drow = dbytes + (size_t)(dr[1] + y) * dst->pitch;
        const uint8_t *srow = sbytes + (size_t)sy * src->pitch;
        for (int32_t x = 0; x < dw; ++x)
            memcpy(drow + 4 * (dr[0] + x), srow + 4 * (sr[0] + x * sw / dw), 4);
    }
    d9_raster_invalidate(dst->id);
    com_ret(c, D3D_OK9);
}
// Test157c (audit after the rain incident): depth-stencil ownership as Direct3D 9. The device keeps two
// references of its own: one to its automatic depth buffer (CreateDevice/Reset with EnableAutoDepthStencil,
// tracked here per device) and one to the bound depth-stencil surface (dev->zbuffer_obj; SetDepthStencilSurface
// AddRefs the new one and releases the old one). Before, Set took no reference, so a surface the game released
// while bound was destroyed at once, and Reset released the bound surface even when it was the game's own,
// destroying a surface the game still held.
static std::unordered_map<uint32_t, uint32_t> &auto_depth_surfaces() {
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
// Test159 (20-reset measurement, 4 Oct): a level or face of a 2D/cube texture gets container-coupled lifetime that
// keeps Direct3D 9's documented observable behaviour (a getter returns a reference the caller releases; the texture
// does not outlive its final release). Not checked against Windows' internal counting. The surface object holds one internal reference (its container's); while anything outside holds
// it, it also holds one reference to the container, taken with the first outside reference and given back with the
// last. Before, GetSurfaceLevel / GetCubeMapSurface / GetRenderTarget took a texture reference on every call and
// releasing the surface never gave it back, so every texture whose level was ever asked for (render targets, the
// game's and the mods') outlived its own release: each renderer Reset left its old targets behind.
static ComObj *surface_container(ComObj *s) {
    if (!s || s->kind != K_D3D9SURFACE || !s->front_obj)
        return nullptr;
    ComObj *t = com_get(s->front_obj);
    if (!t || t->kind != K_D3D9TEXTURE)
        return nullptr;
    for (uint32_t id : t->surfaces)
        if (id == s->id)
            return t;
    return nullptr;
}
static void surface_addref(ComObj *s) {
    if (!s)
        return;
    if (ComObj *t = surface_container(s); t && s->refs == 1)
        com_addref(t);
    com_addref(s);
}
static int32_t surface_release(ComObj *s) {
    if (!s)
        return 0;
    ComObj *t = surface_container(s);
    if (!t)
        return com_release(s);
    if (s->refs <= 1)
        return 0; // only the container's own reference: nothing outside to drop
    const int32_t outside = --s->refs - 1;
    if (!outside)
        com_release(t); // may destroy this level together with its container
    return outside;
}
static void bind_depth_stencil(ComObj *dev, ComObj *s) {
    surface_addref(s);
    ComObj *old = com_get(dev->zbuffer_obj);
    dev->zbuffer_obj = s ? s->id : 0;
    if (old)
        surface_release(old);
}
// Test159: the device holds the surfaces bound as render targets, as Direct3D 9 does (SetRenderTarget AddRefs the
// new one and releases the old one), so a target the game releases while it is bound stays valid until unbound.
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    surface_addref(s);
    ComObj *old = com_get(pl.color_target[index]);
    pl.color_target[index] = s ? s->id : 0;
    if (index == 0)
        dev->render_target = pl.color_target[0];
    if (old)
        surface_release(old);
}
// The device's automatic depth buffer, made on first use, bound.
static ComObj *device_depthbuffer(ComObj *dev) {
    auto &autos = auto_depth_surfaces();
    ComObj *s = com_get(autos[dev->id]);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480, 75);
        if (!s) {
            autos.erase(dev->id);
            return nullptr;
        }
        s->samples = dev->samples;
        resource_usage()[s->id] = USAGE_DEPTHSTENCIL;
        autos[dev->id] = s->id; // the device's own reference (make_surface's)
    }
    if (dev->zbuffer_obj != s->id)
        bind_depth_stencil(dev, s);
    return s;
}

// Reset: drops the device's own back buffer, its binding of the depth-stencil surface and its automatic depth
// buffer; the game's references keep those objects alive, and the device makes new ones on next use.
static void device_forget_buffers(ComObj *dev) {
    if (ComObj *s = com_get(dev->palette_obj))
        com_release(s);
    dev->palette_obj = 0;
    bind_depth_stencil(dev, nullptr);
    auto it = auto_depth_surfaces().find(dev->id);
    if (it != auto_depth_surfaces().end()) {
        if (ComObj *a = com_get(it->second))
            com_release(a);
        auto_depth_surfaces().erase(it);
    }
}

// (this, iSwapChain, iBackBuffer, Type, ppBackBuffer)
void Dev_GetBackBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 4);
    ComObj *s = dev ? device_backbuffer(dev) : nullptr;
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    surface_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        surface_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// Test155 (Metal allocation trace): GetDepthStencilSurface returns the depth-stencil
// surface that is set (SetDepthStencilSurface; the automatic one from CreateDevice/Reset until the game sets
// another). With none set, Direct3D 9 returns D3DERR_NOTFOUND. This used to make a new device-sized depth buffer
// instead (device_depthbuffer), which the game never released: at rain onset that was one 3840x2160 4x-MSAA
// D24S8 surface (~204 MB) per frame, ~13 GB/s, until macOS killed the game.
void Dev_GetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = com_get(dev->zbuffer_obj);
    if (!s) {
        static uint64_t none = 0;
        ++none;
        static const bool notfound_log = recomp_env("D3D9_STATS") != nullptr; // expected in rain: debug-only (review 4 Oct)
        if (notfound_log && (none <= 8 || (none & (none - 1)) == 0))
            LOGW("d3d9: GetDepthStencilSurface with no depth-stencil surface set: D3DERR_NOTFOUND, as Direct3D 9 "
                 "(caller %08x, %llu so far)",
                 rd32(c->r[R_ESP]), (unsigned long long)none);
        wr32(out, 0);
        com_ret(c, D3DERR_NOTFOUND9);
        return;
    }
    surface_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        surface_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 2));
    uint32_t index = arg(c, 1);
    if (!dev || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (index == 0 && !(s && s->kind == K_D3D9SURFACE)) {
        com_ret(c, D3DERR_INVALIDCALL); // target 0 cannot be unset
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    bind_color_target(dev, index, (s && s->kind == K_D3D9SURFACE) ? s : nullptr); // Test159: held while bound
    if (index == 0) {
        // Setting render target 0 resets the viewport and scissor to the target.
        pl.viewport_set = false;
        pl.scissor[0] = pl.scissor[1] = 0;
        pl.scissor[2] = (int32_t)s->width;
        pl.scissor[3] = (int32_t)s->height;
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 1));
    if (dev)
        bind_depth_stencil(dev, (s && s->kind == K_D3D9SURFACE) ? s : nullptr); // the device holds the bound one
    com_ret(c, D3D_OK9);
}

// (this, pVertexElements, ppDecl): the element array is kept verbatim, since
// only the eventual renderer can interpret it.
void Dev_CreateVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t elems = arg(c, 1), out = arg(c, 2);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *d = com_new(K_D3D9DECL);
    if (!d) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    d->dev_d3d = dev->id;
    // D3DVERTEXELEMENT9 is 8 bytes and the array ends with D3DDECL_END, whose
    // Stream is 0xff. Copy until that, so the declaration survives the call.
    for (uint32_t i = 0; i < 64 && elems; ++i) {
        uint32_t e = elems + i * 8u;
        uint16_t stream = rd16(e);
        for (int b = 0; b < 8; ++b)
            d->blob.push_back(gm_ptr(e)[b]);
        if (stream == 0xff)
            break;
    }
    uint32_t view_ = com_view(d, IF_D3DVERTEXDECL9);
    if (!view_) {
        com_release(d);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d = com_this(arg(c, 1));
    if (dev) {
        dev->current_viewport = (d && d->kind == K_D3D9DECL) ? d->id : 0;
        d9_pipeline(dev->id).declaration = dev->current_viewport;
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, ppQuery). A query with no GPU behind it answers immediately.
void Dev_CreateQuery(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t type = arg(c, 1), out = arg(c, 2);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (!out) {
        com_ret(c, D3D_OK9); // a null out pointer only asks whether the type exists
        return;
    }
    ComObj *q = com_new(K_D3D9QUERY);
    if (!q) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    q->dev_d3d = dev ? dev->id : 0;
    q->dev_type = type;
    uint32_t view_ = com_view(q, IF_D3DQUERY9);
    if (!view_) {
        com_release(q);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, StreamNumber, pStreamData, OffsetInBytes, Stride)
void Dev_SetStreamSource(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *vb = com_this(arg(c, 2));
    uint32_t stream = arg(c, 1);
    if (dev && stream < 8) {
        D9Pipeline::Stream &st = d9_pipeline(dev->id).stream[stream];
        st.vb = (vb && vb->kind == K_D3D9VB) ? vb->id : 0;
        st.offset = arg(c, 3);
        st.stride = arg(c, 4);
    }
    if (dev && stream == 0 && vb && vb->kind == K_D3D9VB) {
        dev->back_obj = vb->id;
        dev->lock_off = arg(c, 3);
        dev->rate = arg(c, 4); // the stride, reusing a spare field
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetIndices(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *ib = com_this(arg(c, 1));
    if (dev) {
        dev->front_obj = (ib && ib->kind == K_D3D9IB) ? ib->id : 0;
        d9_pipeline(dev->id).index_buffer = dev->front_obj;
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, pTexture): the texture behind a sampler, for the renderer.
void Dev_SetTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *tex = com_this(arg(c, 2));
    uint32_t stage = arg(c, 1);
    if (!dev || stage >= 16 || (arg(c, 2) && !tex) ||
        (tex && tex->kind == K_D3D9VOLUMETEXTURE && volumes().at(tex->id).pool > 1)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *old = com_get(d9_pipeline(dev->id).sampler_tex[stage]);
    if (tex && tex->kind == K_D3D9VOLUMETEXTURE)
        com_addref(tex);
    dev->texture_handle = tex ? tex->id : 0;
    d9_pipeline(dev->id).sampler_tex[stage] = tex ? tex->id : 0;
    if (old && old->kind == K_D3D9VOLUMETEXTURE)
        com_release(old);
    com_ret(c, D3D_OK9);
}
void Dev_GetTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stage = arg(c, 1), out = arg(c, 2);
    if (!dev || stage >= 16 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *o = com_get(d9_pipeline(dev->id).sampler_tex[stage]);
    wr32(out, o ? o->identity : 0);
    com_addref(o);
    com_ret(c, D3D_OK9);
}
static void device_volume_destroy(ComObj *dev) {
    for (uint32_t &id : d9_pipeline(dev->id).sampler_tex) {
        ComObj *o = com_get(id);
        id = 0;
        if (o && o->kind == K_D3D9VOLUMETEXTURE)
            com_release(o);
    }
}

// The draws. Nothing is rasterized yet, so each one reports what it was asked
// to draw: the geometry a renderer would need is all in these arguments, and
// a run that names them is the specification for the work that comes next.
// The first few are reported in full and then every 256th, because a game at
// speed issues thousands.
static uint32_t g_draws = 0;

static const char *prim_name(uint32_t type) {
    switch (type) {
    case 1:
        return "point list";
    case 2:
        return "line list";
    case 3:
        return "line strip";
    case 4:
        return "triangle list";
    case 5:
        return "triangle strip";
    case 6:
        return "triangle fan";
    default:
        return "unknown primitive";
    }
}

// The first few draws, then (with verbose logging) every 256th: a race makes
// thousands a frame, and writing even one in 256 cost a percent of the frame.
static bool draw_worth_reporting() {
    ++g_draws;
    return g_draws <= 8 || ((g_draws % 256) == 0 && log_level() >= 2);
}

static uint32_t primitive_vertices(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1:
        return count; // points
    case 2:
        return count * 2; // lines
    case 3:
        return count + 1; // line strip
    case 4:
        return count * 3; // triangles
    case 5:
    case 6:
        return count + 2;
    default:
        return 0;
    }
}

// The render target and depth buffer a draw or clear goes to, mirrored.
static HostD9Target gpu_target(ComObj *dev) {
    HostD9Target t{};
    D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 4; ++i) {
        ComObj *s = com_get(pl.color_target[i]);
        if (i == 0 && !s)
            s = com_get(dev->render_target) ? com_get(dev->render_target) : device_backbuffer(dev);
        if (!s)
            continue;
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(s, &slot));
        if (owner)
            gpu_sync(owner);
        t.color[i] = surface_ref(s);
    }
    if (ComObj *z = com_get(dev->zbuffer_obj)) {
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(z, &slot));
        if (owner)
            gpu_sync(owner);
        t.depth = surface_ref(z);
    }
    return t;
}
static void gpu_target_drawn(ComObj *dev, bool color, bool depth) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (color)
        for (int i = 0; i < 4; ++i) {
            ComObj *s = com_get(pl.color_target[i]);
            if (i == 0 && !s)
                s = com_get(dev->render_target);
            gpu_drawn_into(s);
        }
    if (depth)
        gpu_drawn_into(com_get(dev->zbuffer_obj));
}
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.viewport_set) {
        for (int i = 0; i < 4; ++i)
            vp[i] = (int32_t)pl.viewport[i];
        return;
    }
    ComObj *rt =
        com_get(pl.color_target[0]) ? com_get(pl.color_target[0]) : com_get(dev->render_target);
    (void)t;
    vp[0] = vp[1] = 0;
    vp[2] = rt ? (int32_t)rt->width : (int32_t)dev->width;
    vp[3] = rt ? (int32_t)rt->height : (int32_t)dev->height;
}

// Only observes calls already routed to the GPU. Does not infer guest visibility
// decisions or model ownership. Correlate serial + target + resources within one run.
static void observe_draw(X86 *c, ComObj *dev, HostD9Draw &d, uint32_t api) {
    if (!host_d9_admission_enabled())
        return;
    static uint64_t serial = 0; // guest execution is serialized by the scheduler baton
    d.admission.serial = ++serial;
    host_d9_admission_entry(serial);
    d.admission.api = api;
    d.admission.caller = rd32(c->r[R_ESP]);
    d.admission.device = dev->identity;
    if (api >= 83) {
        d.admission.inline_vertices = arg(c, api == 83 ? 3 : 7);
        d.admission.inline_indices = api == 84 ? arg(c, 5) : 0;
        d.stream[0].stride = arg(c, api == 83 ? 4 : 8);
        return; // UP calls do not use the device's bound buffers
    }
    const D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 9; ++i)
        if (ComObj *o = com_get(i < 8 ? pl.stream[i].vb : pl.index_buffer))
            d.admission.resource_identity[i] = o->identity;
}
static void reject_draw(ComObj *dev, const HostD9Draw &attempt, uint32_t reason) {
    if (!attempt.admission.serial)
        return;
    // Metadata only: never gpu_target()/gpu_sync(), never touch invalid UP pointers.
    HostD9Draw d{};
    d.admission = attempt.admission;
    d.admission.reason = reason;
    d.primitive = attempt.primitive;
    d.primitive_count = attempt.primitive_count;
    d.start = attempt.start;
    d.base_vertex = attempt.base_vertex;
    const D9Pipeline &pl = d9_pipeline(dev->id);
    d.label = pl.label;
    d.decl_id = pl.declaration;
    d.vs_key = pl.vs_key;
    d.ps_key = pl.ps_key;
    for (int i = 0; i < 8; ++i)
        d.stream[i] = {pl.stream[i].vb, pl.stream[i].offset, pl.stream[i].stride, pl.stream[i].freq};
    d.index_buffer = pl.index_buffer;
    if (d.admission.api >= 83) {
        memset(d.stream, 0, sizeof d.stream);
        d.stream[0].stride = attempt.stream[0].stride;
        d.index_buffer = 0;
    }
    for (int i = 0; i < 4; ++i) {
        ComObj *s = com_get(pl.color_target[i]);
        if (!s && i == 0)
            s = com_get(dev->render_target);
        if (s)
            d.target.color[i] = surface_ref(s);
    }
    if (ComObj *z = com_get(dev->zbuffer_obj))
        d.target.depth = surface_ref(z);
    target_viewport(dev, d.target, d.viewport);
    host_d9_rejected_draw(&d);
}

// One draw, handed to the GPU renderer with everything the device has bound.
static void gpu_draw(ComObj *dev, HostD9Draw &d) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.vs.empty() || pl.ps.empty()) {
        reject_draw(dev, d, 1);
        log_once("d3d9.gpu.ffp",
                 "d3d9: a draw with no shaders bound is skipped; the fixed-function "
                 "pipeline is not rendered on the GPU yet");
        return;
    }
    d.target = gpu_target(dev);
    target_viewport(dev, d.target, d.viewport);
    d.depth_range[0] = pl.viewport_z[0];
    d.depth_range[1] = pl.viewport_z[1];
    for (int i = 0; i < 4; ++i)
        d.scissor[i] = pl.scissor[i];
    if (!pl.vs_key)
        pl.vs_key = d9sh::code_key(pl.vs.data(), pl.vs.size());
    if (!pl.ps_key)
        pl.ps_key = d9sh::code_key(pl.ps.data(), pl.ps.size());
    const bool lookup_always = recomp_ab::old(recomp_ab::MIRRORS);
    if (!pl.vs_prog || pl.vs_prog_key != pl.vs_key || lookup_always) {
        pl.vs_prog = &d9sh::program_for_key(pl.vs_key, pl.vs.data(), pl.vs.size());
        pl.vs_prog_key = pl.vs_key;
    }
    if (!pl.ps_prog || pl.ps_prog_key != pl.ps_key || lookup_always) {
        pl.ps_prog = &d9sh::program_for_key(pl.ps_key, pl.ps.data(), pl.ps.size());
        pl.ps_prog_key = pl.ps_key;
    }
    const d9sh::Program &vs = *static_cast<const d9sh::Program *>(pl.vs_prog);
    const d9sh::Program &ps = *static_cast<const d9sh::Program *>(pl.ps_prog);
    d.vs_key = pl.vs_key;
    d.ps_key = pl.ps_key;
    d.vs = pl.vs.data();
    d.vs_size = (uint32_t)pl.vs.size();
    d.ps = pl.ps.data();
    d.ps_size = (uint32_t)pl.ps.size();
    d.vconst = &pl.vconst[0][0];
    d.vconst_count = std::min<uint32_t>(vs.max_const, 256);
    d.pconst = &pl.pconst[0][0];
    d.pconst_count = std::min<uint32_t>(ps.max_const, 224);
    // RECOMP_D3D9_PS_CONST_SCALE="name:factor,..." (2026-11-17): a pixel shader's constant of that name (from its own
    // CTAB) has its x, y and z multiplied by the factor for each draw (NFS Undercover: cvWorldSpecularColour, the
    // world's specular glare that made the road look wet). The guest's own values stay as set; the draw gets a copy.
    {
        // "name:factor@setting" (2026-11-18): that rule follows its own core setting instead of PS_CONST_SETTING.
        // "name.x:factor" (2026-11-18): only those components (x, y, z, w) are scaled; x, y and z otherwise.
        struct Rule {
            std::string name;
            float factor;
            std::string setting;
            uint8_t mask = 7;
        };
        static const std::vector<Rule> rules = [] {
            std::vector<Rule> r;
            if (const char *e = recomp_env("D3D9_PS_CONST_SCALE"))
                for (const char *p = e; *p;) {
                    const char *colon = strchr(p, ':'), *comma = strchr(p, ',');
                    if (!colon || (comma && comma < colon))
                        break;
                    const float f = (float)atof(colon + 1);
                    const char *end = comma ? comma : p + strlen(p);
                    const char *at = (const char *)memchr(colon, '@', size_t(end - colon));
                    if (f >= 0.0f && f <= 4.0f) {
                        Rule rule{std::string(p, colon), f, at ? std::string(at + 1, end) : std::string()};
                        const size_t dot = rule.name.find('.');
                        if (dot != std::string::npos) {
                            uint8_t m = 0;
                            for (char ch : rule.name.substr(dot + 1))
                                m |= ch == 'x' ? 1 : ch == 'y' ? 2 : ch == 'z' ? 4 : ch == 'w' ? 8 : 0;
                            rule.name.resize(dot);
                            rule.mask = m ? m : 7;
                        }
                        r.push_back(rule);
                    }
                    if (!comma)
                        break;
                    p = comma + 1;
                }
            return r;
        }();
        if (!rules.empty() && pl.ps_key && !pl.ps.empty()) {
            struct Reg {
                uint32_t reg;
                float factor;
                size_t rule;
            };
            static std::unordered_map<uint64_t, std::vector<Reg>> by_shader;
            static std::mutex mu;
            std::vector<Reg> *regs;
            {
                std::lock_guard lock(mu);
                auto it = by_shader.find(pl.ps_key);
                if (it == by_shader.end()) {
                    std::vector<Reg> found;
                    for (size_t ri = 0; ri < rules.size(); ++ri) {
                        const Rule &rule = rules[ri];
                        uint32_t reg = 0, count = 0;
                        if (d3dx9_ctab_float_register(pl.ps.data(), pl.ps.size(), pl.ps_key, rule.name.c_str(), &reg,
                                                      &count))
                            for (uint32_t k = 0; k < count && reg + k < 224; ++k)
                                found.push_back({reg + k, rule.factor, ri});
                    }
                    // RECOMP_D3D9_PS_CONST_LOG=1 (diagnostic): each matched shader's constant names, once.
                    static const char *names_env = recomp_env("D3D9_PS_CONST_LOG");
                    static const bool names_log = names_env != nullptr, names_all = names_env && !strcmp(names_env, "all");
                    if (names_log && (!found.empty() || names_all)) {
                        static char buf[4096];
                        d3dx9_ctab_names(pl.ps.data(), pl.ps.size(), pl.ps_key, buf, sizeof buf);
                        LOGW("d3d9: PS_CONST_SCALE: ps %016llx: %s", (unsigned long long)pl.ps_key, buf);
                    }
                    static int shaders = 0, matched = 0;
                    ++shaders;
                    matched += !found.empty();
                    if (shaders <= 400 && (shaders % 50 == 0 || (!found.empty() && matched <= 3)))
                        LOGW("d3d9: PS_CONST_SCALE: %d of %d pixel shaders have a named constant (this one: %zu "
                             "registers)", matched, shaders, found.size());
                    it = by_shader.emplace(pl.ps_key, std::move(found)).first;
                }
                regs = &it->second;
            }
            // RECOMP_D3D9_PS_CONST_SETTING=<core setting key> (live, 2026-11-17): the factors above are its strongest
            // level; level L of a setting with levels 0..N applies 1 - (1 - factor) x L / N (0 = the game's own).
            // Read about twice a second; RECOMP_D3D9_PS_CONST_LEVELS=N (default 2).
            static const char *setting = recomp_env("D3D9_PS_CONST_SETTING");
            static const int levels = [] {
                const char *e = recomp_env("D3D9_PS_CONST_LEVELS");
                return e && atoi(e) > 0 ? atoi(e) : 2;
            }();
            static std::vector<float> strengths(rules.size(), -1.0f);
            static uint32_t draws_since = 0;
            static bool strengths_read = false;
            if (!strengths_read || ++draws_since >= 100000) {
                strengths_read = true;
                draws_since = 0;
                for (size_t ri = 0; ri < rules.size(); ++ri) {
                    if (rules[ri].setting == "roof")
                        continue; // follows the roof over the camera's car, per draw below
                    const char *key = !rules[ri].setting.empty() ? rules[ri].setting.c_str() : setting;
                    if (!key) {
                        strengths[ri] = 1.0f;
                        continue;
                    }
                    const int64_t l = std::clamp<int64_t>(mods_core_setting(key, levels / 2), 0, levels);
                    strengths[ri] = float(l) / float(levels);
                }
            }
            // "name:factor@roof" (2026-11-19): the factor applies as far as the camera's car is under a roof - the
            // game's own ambient light dimmed in tunnels and garages, the sunlit world outside left as it is.
            {
                // RECOMP_D3D9_ROOF_FORCE=<0..1> (tests): as if the camera's car were that far under a roof
                static const float forced = [] {
                    const char *e = recomp_env("D3D9_ROOF_FORCE");
                    return e && *e ? (float)atof(e) : -1.0f;
                }();
                const float roof = forced >= 0.0f ? forced : g_d9_roof.load(std::memory_order_relaxed);
                for (size_t ri = 0; ri < rules.size(); ++ri)
                    if (rules[ri].setting == "roof")
                        strengths[ri] = roof;
            }
            bool any = false;
            for (const Reg &rf : *regs)
                any |= strengths[rf.rule] > 0.0f;
            if (any) {
                static thread_local float scaled[224][4];
                memcpy(scaled, pl.pconst, sizeof scaled);
                uint32_t top = d.pconst_count;
                for (const Reg &rf : *regs) {
                    const float f = 1.0f - (1.0f - rf.factor) * strengths[rf.rule];
                    for (int k = 0; k < 4; ++k)
                        if (rules[rf.rule].mask >> k & 1)
                            scaled[rf.reg][k] *= f;
                    top = std::max(top, rf.reg + 1);
                }
                d.pconst = &scaled[0][0];
                d.pconst_count = std::min<uint32_t>(std::max(d.pconst_count, 0u), 224);
                (void)top;
            }
        }
    }
    // RECOMP_D3D9_PS_SETTING_REG="<ps code key>:<register>:<setting>,..." (2026-11-19): draws with that pixel shader get
    // the core mod's <setting> as c<register>.x (y, z, w 0), re-read about twice a second - a live switch for a shader
    // patch (NFS Undercover: the Photorealistic grade in the scene composite).
    {
        struct SettingReg {
            uint64_t ps;
            uint32_t reg;
            std::string setting;
            int64_t value = -1;
        };
        static std::vector<SettingReg> regs = [] {
            std::vector<SettingReg> v;
            if (const char *e = recomp_env("D3D9_PS_SETTING_REG"))
                for (const char *p = e; *p;) {
                    const char *comma = strchr(p, ',');
                    const std::string item(p, comma ? comma : p + strlen(p));
                    const size_t c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
                    if (c2 != std::string::npos) {
                        const uint32_t reg = (uint32_t)atoi(item.substr(c1 + 1, c2 - c1 - 1).c_str());
                        if (reg < 224)
                            v.push_back({strtoull(item.substr(0, c1).c_str(), nullptr, 16), reg, item.substr(c2 + 1)});
                    }
                    if (!comma)
                        break;
                    p = comma + 1;
                }
            return v;
        }();
        if (!regs.empty()) {
            static uint32_t since = 0;
            if (++since >= 50000 || regs[0].value < 0) {
                since = 0;
                for (SettingReg &r : regs)
                    r.value = mods_core_setting(r.setting.c_str(), 0);
            }
            for (const SettingReg &r : regs)
                if (r.ps == pl.ps_key) {
                    static thread_local float withreg[224][4];
                    memcpy(withreg, d.pconst, sizeof(float) * 4 * std::min<uint32_t>(d.pconst_count, 224));
                    for (uint32_t k = d.pconst_count; k < 224; ++k)
                        withreg[k][0] = withreg[k][1] = withreg[k][2] = withreg[k][3] = 0.0f;
                    withreg[r.reg][0] = float(r.value);
                    withreg[r.reg][1] = withreg[r.reg][2] = withreg[r.reg][3] = 0.0f;
                    d.pconst = &withreg[0][0];
                    d.pconst_count = std::max(d.pconst_count, r.reg + 1);
                    break;
                }
        }
    }
    {   // recomp_d3d9_ps_reg_set (below): a core mod's live value for one register of one pixel shader
        std::lock_guard<std::mutex> lock(g_mod_ps_regs_m);
        for (const ModPsReg &r : g_mod_ps_regs)
            if (r.ps == pl.ps_key) {
                static thread_local float withreg2[224][4];
                if (d.pconst != &withreg2[0][0]) {
                    memcpy(withreg2, d.pconst, sizeof(float) * 4 * std::min<uint32_t>(d.pconst_count, 224));
                    for (uint32_t k = d.pconst_count; k < 224; ++k)
                        withreg2[k][0] = withreg2[k][1] = withreg2[k][2] = withreg2[k][3] = 0.0f;
                }
                memcpy(withreg2[r.reg], r.v, sizeof r.v);
                d.pconst = &withreg2[0][0];
                d.pconst_count = std::max(d.pconst_count, r.reg + 1);
            }
    }
    {   // RECOMP_D3D9_CAR_LIGHTS (see carlights above)
        struct Cfg {
            bool on = false;
            uint64_t ps = 0;
            std::string car, env = "cmLocalColour";
            uint32_t first = 14;
            float gain = 1.0f;                                  // "gain=<x>": lamp strength x (tests)
            float front = 2.05f, side = 0.62f, height = 0.62f;  // lamp offsets in the car's frame, metres
            float dark = 0.45f, bright = 0.7f;                  // "dark=", "bright=": the car's light, on / off
            bool always = false;                                // "always=1": lamps on whatever the light (tests)
        };
        static const Cfg cfg = [] {
            Cfg c;
            const char *e = recomp_env("D3D9_CAR_LIGHTS");
            if (!e || !*e)
                return c;
            for (const char *p = e; *p;) {
                const char *comma = strchr(p, ',');
                const std::string kv(p, comma ? comma : p + strlen(p));
                const size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                    const float f = (float)atof(v.c_str());
                    if (k == "ps")
                        c.ps = strtoull(v.c_str(), nullptr, 16);
                    else if (k == "car")
                        c.car = v;
                    else if (k == "env")
                        c.env = v;
                    else if (k == "first")
                        c.first = (uint32_t)atoi(v.c_str());
                    else if (k == "gain")
                        c.gain = f;
                    else if (k == "front")
                        c.front = f;
                    else if (k == "side")
                        c.side = f;
                    else if (k == "height")
                        c.height = f;
                    else if (k == "dark")
                        c.dark = f;
                    else if (k == "bright")
                        c.bright = f;
                    else if (k == "always")
                        c.always = f != 0.0f;
                }
                if (!comma)
                    break;
                p = comma + 1;
            }
            c.on = c.ps && !c.car.empty() && c.first >= 12 && c.first <= 20;
            return c;
        }();
        if (cfg.on && pl.ps_key && !pl.ps.empty() && !pl.vs.empty()) {
            carlights::State &st = carlights::state();
            const uint64_t frame = g_d9_presents.load(std::memory_order_relaxed);
            if (st.frame != frame) {
                st.frame = frame;
                st.cars.clear();
                st.have_vp = false;
                st.sun = -1.0f;
            }
            struct Regs {
                int world = -1, wvp = -1, env = -1, sun = -1;
                bool car = false;
            };
            static std::unordered_map<uint64_t, Regs> regs_of; // by (vs key, ps key)
            const uint64_t rk = pl.vs_key * 0x9e3779b97f4a7c15ull ^ pl.ps_key;
            auto it = regs_of.find(rk);
            if (it == regs_of.end()) {
                Regs r;
                uint32_t reg = 0, count = 0;
                if (d3dx9_ctab_float_register(pl.vs.data(), pl.vs.size(), pl.vs_key, "cmWorldMat", &reg, &count) &&
                    reg + 4 <= 256)
                    r.world = (int)reg;
                if (d3dx9_ctab_float_register(pl.vs.data(), pl.vs.size(), pl.vs_key, "cmWorldViewProj", &reg,
                                              &count) &&
                    reg + 4 <= 256)
                    r.wvp = (int)reg;
                r.car = d3dx9_ctab_float_register(pl.ps.data(), pl.ps.size(), pl.ps_key, cfg.car.c_str(), &reg, &count);
                if (d3dx9_ctab_float_register(pl.ps.data(), pl.ps.size(), pl.ps_key, "cvWorldSunColour", &reg, &count) &&
                    reg < 224)
                    r.sun = (int)reg;
                if (r.car && d3dx9_ctab_float_register(pl.ps.data(), pl.ps.size(), pl.ps_key, cfg.env.c_str(), &reg,
                                                       &count) &&
                    reg < 224)
                    r.env = (int)reg;
                it = regs_of.emplace(rk, r).first;
            }
            const Regs &r = it->second;
            if (r.sun >= 0 && st.sun < 0) {
                const float *e = pl.pconst[r.sun];
                st.sun = 0.299f * e[0] + 0.587f * e[1] + 0.114f * e[2];
            }
            if (r.car && r.world >= 0) {
                const float(*m)[4] = &pl.vconst[r.world];
                // one car per 2.5 m: its body parts (bonnet, spoiler, wheels) come with matrices of their own
                carlights::Car *same = nullptr;
                for (carlights::Car &cc : st.cars) {
                    const float dx = cc.m[3][0] - m[3][0], dy = cc.m[3][1] - m[3][1], dz = cc.m[3][2] - m[3][2];
                    if (dx * dx + dy * dy + dz * dz < 2.5f * 2.5f)
                        same = &cc;
                }
                float env = -1.0f;
                if (r.env >= 0) {
                    const float *e = pl.pconst[r.env];
                    env = 0.299f * e[0] + 0.587f * e[1] + 0.114f * e[2];
                }
                if (!same && st.cars.size() < 64) {
                    carlights::Car cc;
                    memcpy(cc.m, m, sizeof cc.m);
                    cc.dist2 = 0;
                    cc.env = env;
                    cc.prims = d.primitive_count;
                    st.cars.push_back(cc);
                } else if (same) {
                    if (env >= 0 && (same->env < 0 || env < same->env))
                        same->env = env;
                    // the body, not a wheel: the part drawn with the most triangles. (The most upright part was a
                    // front wheel when the car stood still, so the lamps turned with the steering - 2026-11-18.)
                    if (d.primitive_count > same->prims) {
                        memcpy(same->m, m, sizeof same->m);
                        same->prims = d.primitive_count;
                    }
                }
            }
            if (!st.have_vp && r.wvp >= 0 && r.world >= 0 && carlights::identity3(&pl.vconst[r.world])) {
                memcpy(st.vp, &pl.vconst[r.wvp], sizeof st.vp);
                st.have_vp = true;
            }
            static int64_t setting = -1;
            static uint32_t since = 0;
            if (setting < 0 || ++since >= 100000) {
                since = 0;
                setting = mods_core_setting("headlights", 1);
                if (setting <= 0)
                    g_d9_roof.store(0.0f, std::memory_order_relaxed);
            }
            if (pl.ps_key == cfg.ps) {
                // Registers from `first` (f): f..f+3 inverse view projection, f+4 camera + lamp count, f+5..f+8 view
                // projection, f+9 = (lamps with shadows, 0, 0, 0), then 2 per lamp (position +
                // strength, direction + cos of the cone's edge) for up to 32 lamps, then their colour.
                constexpr uint32_t kLamps = 32;
                const uint32_t f = cfg.first, lamps0 = f + 10, colour = lamps0 + 2 * kLamps, total = colour + 1;
                static thread_local float lit[96][4];
                memset(lit, 0, sizeof lit);
                memcpy(lit, d.pconst, sizeof(float) * 4 * std::min<uint32_t>(d.pconst_count, f));
                float inv[4][4];
                uint32_t n = 0, shadowed = 0;
                // Cars are followed from frame to frame (the nearest one within 6 m of where it was): a car's lamps
                // come on once its own light (the game's local light colour for it) has stayed below `dark` for 1.5 s
                // - a tunnel, the night - and go out after 2 s above `bright`, fading in and out over half a second.
                // Cars are followed from frame to frame (the nearest one within 6 m of where it was). A car's lamps come
                // on after 1.5 s in the dark and go out after 2 s in the light, fading in and out over half a second; the
                // dark is the night, or a roof over the car (a tunnel) - seen in the probe grid: the point 5 m above it,
                // on screen, lands on something more than 2 m above it - or the game's own light for it low. Its lamp
                // frame is smoothed over a few frames, so the lamps do not jump.
                struct Track {
                    float pos[3];
                    float m[3][3] = {};  // smoothed forward, side, up
                    bool have_m = false;
                    float dark_s = 0, bright_s = 0, level = 0;
                    int covered = -1;    // roof above: 1, open sky: 0, unknown (off screen): -1
                    uint64_t seen = 0;
                };
                static std::vector<Track> tracks;
                // The tunnel's own lamps: the probe grid's brightest spots (brighter than all 8 neighbours and > 0.8,
                // not sky) put back into the world while the camera's car is under a roof or it is night, kept 12 s
                // after last seen and merged within 1.5 m. They light their surroundings in every direction.
                struct Lamp {
                    float pos[3], rgb[3];
                    float lum;
                    uint64_t seen_ns;
                };
                static std::vector<Lamp> lamps;
                static uint64_t grid_serial = 0;
                static std::vector<float> g_depth, g_hd, g_hl, g_hc;
                static int gw = 0, gh = 0;
                static uint64_t last_ns = 0;
                const uint64_t now_ns = os_monotonic_ns();
                const float dt = last_ns ? std::min(0.25f, float(now_ns - last_ns) * 1e-9f) : 0.0f;
                last_ns = now_ns;
                if (setting > 0 && st.have_vp && carlights::invert4(st.vp, inv)) {
                    memcpy(lit[f], inv, sizeof inv);
                    memcpy(lit[f + 5], st.vp, sizeof st.vp);
                    float eye[3] = {0, 0, 0};
                    if (std::fabs(inv[2][3]) > 1e-9f)
                        for (int k = 0; k < 3; ++k)
                            eye[k] = inv[2][k] / inv[2][3];
                    for (carlights::Car &cc : st.cars) {
                        const float dx = cc.m[3][0] - eye[0], dy = cc.m[3][1] - eye[1], dz = cc.m[3][2] - eye[2];
                        cc.dist2 = dx * dx + dy * dy + dz * dz;
                    }
                    std::sort(st.cars.begin(), st.cars.end(),
                              [](const carlights::Car &a, const carlights::Car &b) { return a.dist2 < b.dist2; });
                    // the probe grid (a few frames old at most)
                    {
                        int w = 0, h = 0;
                        host_d9_probe_grid(&w, &h, nullptr, nullptr, nullptr, nullptr);
                        if (w > 0 && h > 0) {
                            g_depth.resize(size_t(w) * h), g_hd.resize(size_t(w) * h), g_hl.resize(size_t(w) * h),
                                g_hc.resize(size_t(w) * h * 3);
                            const uint64_t ser =
                                host_d9_probe_grid(&w, &h, g_depth.data(), g_hd.data(), g_hl.data(), g_hc.data());
                            if (ser)
                                grid_serial = ser, gw = w, gh = h;
                        }
                    }
                    // world point -> grid cell (-1: off screen or behind)
                    auto to_cell = [&](const float p[3], int *cx, int *cy, float uv[2]) {
                        float c4[4];
                        for (int k = 0; k < 4; ++k)
                            c4[k] = p[0] * st.vp[0][k] + p[1] * st.vp[1][k] + p[2] * st.vp[2][k] + st.vp[3][k];
                        if (c4[3] <= 0.1f || !gw)
                            return false;
                        const float u = c4[0] / c4[3] * 0.5f + 0.5f, v = 0.5f - c4[1] / c4[3] * 0.5f;
                        if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
                            return false;
                        *cx = std::min(gw - 1, int(u * gw));
                        *cy = std::min(gh - 1, int(v * gh));
                        if (uv)
                            uv[0] = u, uv[1] = v;
                        return true;
                    };
                    // grid cell (uv, depth) -> world point
                    auto to_world = [&](float u, float v, float dep, float out[3]) {
                        const float nx = u * 2.0f - 1.0f, ny = 1.0f - v * 2.0f;
                        float w4[4];
                        for (int k = 0; k < 4; ++k)
                            w4[k] = nx * inv[0][k] + ny * inv[1][k] + dep * inv[2][k] + inv[3][k];
                        if (std::fabs(w4[3]) < 1e-8f)
                            return false;
                        for (int k = 0; k < 3; ++k)
                            out[k] = w4[k] / w4[3];
                        return true;
                    };
                    float sky_share = -1.0f, sky_lum = -1.0f;
                    if (host_d9_sky)
                        host_d9_sky(&sky_share, &sky_lum);
                    // a blank frame (all far depth, black: the first frames of a scene) says nothing
                    const bool sky_known = sky_share >= 0.0f && !(sky_share > 0.95f && sky_lum < 0.02f);
                    const bool night = sky_known && sky_share > 0.01f && sky_lum < 0.18f;
                    auto norm = [](float *v) {
                        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                        if (l > 1e-6f)
                            for (int k = 0; k < 3; ++k)
                                v[k] /= l;
                    };
                    std::vector<Track *> track_of(st.cars.size(), nullptr);
                    for (size_t ci = 0; ci < st.cars.size(); ++ci) {
                        carlights::Car &cc = st.cars[ci];
                        Track *best = nullptr;
                        float bd = 36.0f;
                        for (Track &t : tracks) {
                            if (t.seen == frame)
                                continue;
                            const float dx = t.pos[0] - cc.m[3][0], dy = t.pos[1] - cc.m[3][1], dz = t.pos[2] - cc.m[3][2];
                            const float d2 = dx * dx + dy * dy + dz * dz;
                            if (d2 < bd)
                                bd = d2, best = &t;
                        }
                        if (!best) {
                            tracks.push_back(Track{});
                            best = &tracks.back();
                            best->level = cfg.always ? 1.0f : 0.0f;
                        }
                        memcpy(best->pos, cc.m[3], sizeof best->pos);
                        best->seen = frame;
                        // smoothed lamp frame
                        for (int a = 0; a < 3; ++a) {
                            float v[3] = {cc.m[a][0], cc.m[a][1], cc.m[a][2]};
                            norm(v);
                            for (int k = 0; k < 3; ++k)
                                best->m[a][k] = best->have_m ? best->m[a][k] * 0.6f + v[k] * 0.4f : v[k];
                            norm(best->m[a]);
                        }
                        best->have_m = true;
                        // a roof over this car? (probe grid) Seven points over it - 5 m and 9 m up, and 5 m up 10 m
                        // ahead, behind and 5 m to each side: the sky seen at any of them means the open (a tree's crown
                        // over the road has gaps, a tunnel has none); a surface 2-15 m above the car (within 40 m across:
                        // the view toward a point over a car far down a tunnel meets the ceiling well before it) at three
                        // or more, and the sky at none, means a roof. Anything else (off screen, a car or a sign in the
                        // way) leaves it as it was. 2026-11-18: one point took trees for roofs. 2026-11-19: from 1.4 m up
                        // (a parking garage's ceiling is low).
                        if (grid_serial) {
                            static const float offs[7][3] = {{0, 0, 5}, {0, 0, 9}, {10, 0, 5}, {-10, 0, 5},
                                                             {0, 5, 5}, {0, -5, 5}, {5, 0, 7}};
                            int sky = 0, roofs = 0;
                            for (const auto &o : offs) {
                                float q[3];
                                for (int k = 0; k < 3; ++k)
                                    q[k] = cc.m[3][k] + best->m[0][k] * o[0] + best->m[1][k] * o[1] + best->m[2][k] * o[2];
                                int cx, cy;
                                float uv[2];
                                if (!to_cell(q, &cx, &cy, uv))
                                    continue;
                                const float dep = g_depth[size_t(cy) * gw + cx];
                                float sp[3];
                                if (dep >= 0.9999f) {
                                    ++sky;
                                } else if (to_world(uv[0], uv[1], dep, sp)) {
                                    const float hx = sp[0] - cc.m[3][0], hy = sp[1] - cc.m[3][1], hz = sp[2] - cc.m[3][2];
                                    if (hx * hx + hy * hy < 40.0f * 40.0f && hz > 1.4f && hz < 15.0f)
                                        ++roofs;
                                }
                            }
                            if (sky > 0)
                                best->covered = 0;
                            else if (roofs >= 3)
                                best->covered = 1;
                        }
                        const bool roof = best->covered == 1;
                        const bool dark = cfg.always || night || roof || (cc.env >= 0 && cc.env < cfg.dark);
                        const bool light = !cfg.always && !night && best->covered == 0 && (cc.env < 0 || cc.env > cfg.bright);
                        // a frame or two that cannot tell (the point above off screen, a sign in the way) does not
                        // start the count again: the time runs back down at half speed instead (2026-11-18 - lamps came
                        // on too seldom in tunnels)
                        best->dark_s = dark ? best->dark_s + dt : std::max(0.0f, best->dark_s - 0.5f * dt);
                        best->bright_s = light ? best->bright_s + dt : std::max(0.0f, best->bright_s - 0.5f * dt);
                        if (dark)
                            best->bright_s = 0.0f;
                        if (light)
                            best->dark_s = std::max(0.0f, best->dark_s - dt);
                        float target = best->level > 0.5f ? 1.0f : 0.0f;
                        if (best->dark_s > 1.0f)
                            target = 1.0f;
                        if (best->bright_s > 2.0f)
                            target = 0.0f;
                        best->level += std::clamp(target - best->level, -2.0f * dt, 2.0f * dt);
                        cc.level = best->level;
                        track_of[ci] = best;
                    }
                    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                                [&](const Track &t) { return t.seen + 120 < frame; }),
                                 tracks.end());
                    // track pointers are stable until the erase above only for tracks seen this frame: re-find them
                    for (size_t ci = 0; ci < st.cars.size(); ++ci) {
                        track_of[ci] = nullptr;
                        for (Track &t : tracks)
                            if (t.seen == frame && std::fabs(t.pos[0] - st.cars[ci].m[3][0]) < 1e-3f &&
                                std::fabs(t.pos[1] - st.cars[ci].m[3][1]) < 1e-3f)
                                track_of[ci] = &t;
                    }
                    // under a roof with the camera's car (yours, in the chase view): the tunnel's lamps. (The tunnel
                    // darkening filter that went with it was taken out, 2026-11-18: the person wanted the game's own
                    // shade made deeper instead - core.nfsu shade_depth, mods/core/nfsu/shaders/all-vertex.patch.)
                    const bool cam_roof = !st.cars.empty() && track_of[0] && track_of[0]->covered == 1;
                    // the roof level: up after 0.7 s under a roof, down after 0.7 s in the open, over a second
                    {
                        static float roof_s = 0.0f, open_s = 0.0f, level = 0.0f;
                        const bool open = !st.cars.empty() && track_of[0] && track_of[0]->covered == 0;
                        roof_s = cam_roof ? roof_s + dt : 0.0f;
                        open_s = open ? open_s + dt : 0.0f;
                        float target = level > 0.5f ? 1.0f : 0.0f;
                        if (roof_s > 0.7f)
                            target = 1.0f;
                        if (open_s > 0.7f || st.cars.empty())
                            target = 0.0f;
                        level += std::clamp(target - level, -dt, dt);
                        g_d9_roof.store(night ? 0.0f : level, std::memory_order_relaxed);
                        // one line in the ordinary log each time it changes (what the roof test saw, for reports)
                        static int told = -1;
                        const int now_state = cam_roof ? 1 : open ? 0 : 2;
                        if (now_state != told && (now_state != 2 || told == -1)) {
                            told = now_state;
                            LOGW("d3d9: car lights: your car %s (sky %.2f, %zu cars, %zu tunnel lamps known)",
                                 now_state == 1 ? "under a roof" : now_state == 0 ? "in the open" : "undecided",
                                 sky_share, st.cars.size(), lamps.size());
                        }
                    }
                    // lamps are looked for once your car has been under a roof for a moment (2026-11-19: not only after
                    // its own lamps came on - in a garage they never did, so nothing lit)
                    const bool lamp_mode = g_d9_roof.load(std::memory_order_relaxed) > 0.3f || night;
                    if (lamp_mode && grid_serial && gw > 2 && gh > 2) {
                        static uint64_t used_serial = 0;
                        if (used_serial != grid_serial) {
                            used_serial = grid_serial;
                            for (int y = 1; y + 1 < gh; ++y)
                                for (int x = 1; x + 1 < gw; ++x) {
                                    // a lamp: a cell clearly brighter than what is around it (tunnel lamps are pale
                                    // blue-green strips, not white: an absolute 0.8 missed them)
                                    const float l = g_hl[size_t(y) * gw + x];
                                    if (l < 0.42f)
                                        continue;
                                    bool peak = true;
                                    float around = 0.0f;
                                    for (int yy = -1; yy <= 1 && peak; ++yy)
                                        for (int xx = -1; xx <= 1; ++xx) {
                                            if (!xx && !yy)
                                                continue;
                                            const float o = g_hl[size_t(y + yy) * gw + x + xx];
                                            around += o;
                                            if (o > l) {
                                                peak = false;
                                                break;
                                            }
                                        }
                                    if (!peak || l < 1.45f * (around / 8.0f))
                                        continue;
                                    float wp[3];
                                    const float u = (x + 0.5f) / gw, v = (y + 0.5f) / gh;
                                    if (!to_world(u, v, g_hd[size_t(y) * gw + x], wp))
                                        continue;
                                    const float ex = wp[0] - eye[0], ey = wp[1] - eye[1], ez = wp[2] - eye[2];
                                    if (ex * ex + ey * ey + ez * ez > 80.0f * 80.0f)
                                        continue;
                                    // not a lamp: anything at road height (the headlights' own pools of light) or on a
                                    // car (its tail and brake lights)
                                    if (wp[2] < st.cars[0].m[3][2] + 1.2f)
                                        continue;
                                    bool on_car = false;
                                    for (const carlights::Car &oc : st.cars) {
                                        const float dx = oc.m[3][0] - wp[0], dy = oc.m[3][1] - wp[1], dz = oc.m[3][2] - wp[2];
                                        on_car |= dx * dx + dy * dy + dz * dz < 3.5f * 3.5f;
                                    }
                                    if (on_car)
                                        continue;
                                    Lamp *same = nullptr;
                                    for (Lamp &lp : lamps) {
                                        const float dx = lp.pos[0] - wp[0], dy = lp.pos[1] - wp[1], dz = lp.pos[2] - wp[2];
                                        if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f)
                                            same = &lp;
                                    }
                                    const float *c3 = &g_hc[(size_t(y) * gw + x) * 3];
                                    if (same) {
                                        for (int k = 0; k < 3; ++k)
                                            same->pos[k] = same->pos[k] * 0.8f + wp[k] * 0.2f;
                                        same->lum = std::max(same->lum * 0.9f, l);
                                        same->seen_ns = now_ns;
                                    } else if (lamps.size() < 96) {
                                        Lamp lp{{wp[0], wp[1], wp[2]}, {c3[0], c3[1], c3[2]}, l, now_ns};
                                        lamps.push_back(lp);
                                    }
                                }
                        }
                    }
                    lamps.erase(std::remove_if(lamps.begin(), lamps.end(),
                                               [&](const Lamp &lp) { return now_ns - lp.seen_ns > 12000000000ull; }),
                                lamps.end());
                    static uint64_t sky_told = 0;
                    if (recomp_env("D3D9_CAR_LIGHTS_LOG") && frame >= sky_told + 120) {
                        sky_told = frame;
                        LOGW("d3d9: car lights: sky share %.3f, sky brightness %.2f%s%s, %zu tunnel lamps known", sky_share,
                             sky_lum, cam_roof ? " (your car under a roof)" : "", night ? " (night)" : "", lamps.size());
                    }
                    // headlights first (the nearest cars' cast shadows), then the tunnel lamps
                    for (size_t ci = 0; ci < st.cars.size(); ++ci) {
                        const carlights::Car &cc = st.cars[ci];
                        if (n + 2 > kLamps - 8 || cc.dist2 > 600.0f * 600.0f)
                            break;
                        if (cc.level <= 0.001f || !track_of[ci])
                            continue;
                        const Track &tr = *track_of[ci];
                        const float *fw = tr.m[0], *sd = tr.m[1], *up = tr.m[2];
                        for (int side = -1; side <= 1; side += 2) {
                            float *pos = lit[lamps0 + 2 * n];
                            float *dir = lit[lamps0 + 2 * n + 1];
                            for (int k = 0; k < 3; ++k) {
                                pos[k] = cc.m[3][k] + fw[k] * cfg.front + sd[k] * (cfg.side * side) + up[k] * cfg.height;
                                dir[k] = fw[k] - up[k] * 0.06f;
                            }
                            norm(dir);
                            pos[3] = 45.0f * cfg.gain * cc.level; // strength: light at 1 m
                            dir[3] = 0.82f;                       // cos of the cone's edge (~35 degrees)
                            ++n;
                        }
                        if (shadowed < 4)
                            shadowed = n; // the nearest two cars' lamps cast shadows
                    }
                    if (lamp_mode) {
                        std::vector<std::pair<float, const Lamp *>> near_l;
                        for (const Lamp &lp : lamps) {
                            const float dx = lp.pos[0] - eye[0], dy = lp.pos[1] - eye[1], dz = lp.pos[2] - eye[2];
                            near_l.push_back({dx * dx + dy * dy + dz * dz, &lp});
                        }
                        std::sort(near_l.begin(), near_l.end(),
                                  [](const auto &a, const auto &b) { return a.first < b.first; });
                        for (const auto &nl : near_l) {
                            if (n + 1 > kLamps)
                                break;
                            float *pos = lit[lamps0 + 2 * n];
                            float *dir = lit[lamps0 + 2 * n + 1];
                            const Lamp &lp = *nl.second;
                            for (int k = 0; k < 3; ++k)
                                pos[k] = lp.pos[k], dir[k] = std::min(1.0f, lp.rgb[k]);
                            pos[3] = 14.0f * cfg.gain * lp.lum; // strength at 1 m
                            dir[3] = -1.0f;                    // a lamp: every direction, colour in xyz
                            ++n;
                        }
                    }
                    lit[f + 4][0] = eye[0];
                    lit[f + 4][1] = eye[1];
                    lit[f + 4][2] = eye[2];
                }
                lit[f + 4][3] = float(n);
                lit[f + 9][0] = float(std::min(shadowed, 4u));
                lit[f + 9][1] = 0.0f;
                lit[colour][0] = 1.0f, lit[colour][1] = 0.93f, lit[colour][2] = 0.80f;
                d.pconst = &lit[0][0];
                d.pconst_count = total;
                static uint64_t told_at = 0;
                if (recomp_env("D3D9_CAR_LIGHTS_LOG") && frame >= told_at + 120) {
                    told_at = frame;
                    char line[1024];
                    int k = snprintf(line, sizeof line, "d3d9: car lights: frame %llu, %u lamps, sun %.2f, cars:",
                                     (unsigned long long)frame, n, st.sun);
                    for (size_t ci = 0; ci < st.cars.size() && ci < 8 && k < 900; ++ci)
                        k += snprintf(line + k, sizeof line - k, " [%.0f m light %.2f on %.2f]",
                                      std::sqrt(st.cars[ci].dist2), st.cars[ci].env, st.cars[ci].level);
                    LOGW("%s", line);
                }
            }
        }
    }
    // RECOMP_D3D9_PS_DUMP_CONSTS=<ps code key> (diagnostic, 2026-11-24): the first 6 draws with that pixel shader
    // print its float registers c0..c41 as the game left them (and which were written since the shader was bound).
    {
        static const uint64_t dump_ps = [] {
            const char *e = recomp_env("D3D9_PS_DUMP_CONSTS");
            return e ? (uint64_t)strtoull(e, nullptr, 16) : 0ull;
        }();
        static int dumped = 0;
        if (dump_ps && pl.ps_key == dump_ps && dumped < 6) {
            ++dumped;
            for (uint32_t r = 0; r < 42; r += 2)
                LOGW("d3d9 psdump %d: c%u [%.4g %.4g %.4g %.4g]  c%u [%.4g %.4g %.4g %.4g]", dumped, r, pl.pconst[r][0],
                     pl.pconst[r][1], pl.pconst[r][2], pl.pconst[r][3], r + 1, pl.pconst[r + 1][0], pl.pconst[r + 1][1],
                     pl.pconst[r + 1][2], pl.pconst[r + 1][3]);
        }
    }
    // RECOMP_D3D9_CONST_PROBE=<present number> (diagnostic, 2026-11-18): in that frame, each draw's shader keys,
    // its vertex shader's constant names (once per shader) and the values of the constants named in
    // RECOMP_D3D9_CONST_PROBE_NAMES ("a,b,..."; vertex and pixel), one line each.
    {
        static const long probe_at = [] {
            const char *e = recomp_env("D3D9_CONST_PROBE");
            return e ? atol(e) : -1L;
        }();
        if (probe_at >= 0 && long(g_d9_presents.load(std::memory_order_relaxed)) == probe_at) {
            static const std::vector<std::string> names = [] {
                std::vector<std::string> v;
                if (const char *e = recomp_env("D3D9_CONST_PROBE_NAMES"))
                    for (const char *p = e; *p;) {
                        const char *comma = strchr(p, ',');
                        v.emplace_back(p, comma ? comma : p + strlen(p));
                        if (!comma)
                            break;
                        p = comma + 1;
                    }
                return v;
            }();
            static std::unordered_map<uint64_t, int> named_vs;
            char line[4096];
            int n = snprintf(line, sizeof line, "d3d9 probe: draw ps %016llx vs %016llx", (unsigned long long)pl.ps_key,
                             (unsigned long long)pl.vs_key);
            for (const std::string &nm : names)
                for (int which = 0; which < 2; ++which) {
                    const auto &code = which ? pl.ps : pl.vs;
                    const uint64_t key = which ? pl.ps_key : pl.vs_key;
                    uint32_t reg = 0, count = 0;
                    if (code.empty() || !d3dx9_ctab_float_register(code.data(), code.size(), key, nm.c_str(), &reg,
                                                                  &count))
                        continue;
                    const float(*cs)[4] = which ? pl.pconst : pl.vconst;
                    const uint32_t lim = which ? 224u : 256u;
                    n += snprintf(line + n, sizeof line - n, " %s.%s=", which ? "ps" : "vs", nm.c_str());
                    for (uint32_t k = 0; k < count && reg + k < lim && n < (int)sizeof line - 80; ++k)
                        n += snprintf(line + n, sizeof line - n, "[%.4g %.4g %.4g %.4g]", cs[reg + k][0],
                                      cs[reg + k][1], cs[reg + k][2], cs[reg + k][3]);
                }
            LOGW("%s", line);
            if (!pl.vs.empty() && named_vs.emplace(pl.vs_key, 1).second) {
                static char buf[4096];
                d3dx9_ctab_names(pl.vs.data(), pl.vs.size(), pl.vs_key, buf, sizeof buf);
                LOGW("d3d9 probe: vs %016llx names: %s", (unsigned long long)pl.vs_key, buf);
            }
            // the pixel shader's names with their registers (name@cN), once per shader
            if (!pl.ps.empty() && named_vs.emplace(pl.ps_key ^ 0x9e3779b97f4a7c15ull, 1).second) {
                static char buf[4096];
                d3dx9_ctab_names(pl.ps.data(), pl.ps.size(), pl.ps_key, buf, sizeof buf);
                std::string out;
                for (const char *p = buf; *p;) {
                    while (*p == ' ')
                        ++p;
                    const char *e = p;
                    while (*e && *e != ' ')
                        ++e;
                    if (e == p)
                        break;
                    const std::string nm(p, e);
                    uint32_t reg = 0, count = 0;
                    char one[160];
                    if (d3dx9_ctab_float_register(pl.ps.data(), pl.ps.size(), pl.ps_key, nm.c_str(), &reg, &count))
                        snprintf(one, sizeof one, " %s@c%u", nm.c_str(), reg);
                    else
                        snprintf(one, sizeof one, " %s", nm.c_str());
                    out += one;
                    p = e;
                }
                LOGW("d3d9 probe: ps %016llx names:%s", (unsigned long long)pl.ps_key, out.c_str());
            }
        }
    }
    memcpy(d.vbool, pl.vbool, sizeof d.vbool);
    memcpy(d.pbool, pl.pbool, sizeof d.pbool);
    ComObj *decl = com_get(pl.declaration);
    if (!decl || decl->kind != K_D3D9DECL) {
        reject_draw(dev, d, 2);
        log_once("d3d9.gpu.nodecl", "d3d9: a draw with no vertex declaration is skipped");
        return;
    }
    d.decl = decl->blob.data();
    d.decl_size = (uint32_t)decl->blob.size();
    d.decl_id = decl->id;
    for (int i = 0; i < 8; ++i) {
        if (d.stream[i].stride && !d.stream[i].buffer)
            continue; // the draw's own inline vertices
        ComObj *vb = com_get(pl.stream[i].vb);
        if (!vb)
            continue;
        gpu_sync_buffer(vb);
        d.stream[i].buffer = vb->id;
        d.stream[i].offset = pl.stream[i].offset;
        d.stream[i].stride = pl.stream[i].stride;
        // Instancing applies to indexed draws from buffers only (D3D9 ignores it
        // for the UP calls, which never reach this loop with a buffer).
        d.stream[i].freq = pl.stream[i].freq;
    }
    uint32_t sampler_state[16][14];
    bool volume_lod = false;
    for (int s = 0; s < 16; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex || (tex->kind != K_D3D9TEXTURE && tex->kind != K_D3D9SURFACE &&
                     tex->kind != K_D3D9VOLUMETEXTURE))
            continue;
        gpu_sync(tex);
        d.sampler_texture[s] = tex->kind == K_D3D9TEXTURE && texmod::active() ? texmod::bound(tex) : tex->id;
        // RECOMP_D3D9_TEXMOD_LOG=1: each replaced texture's role - the pixel shader's sampler name at that stage
        if (d.sampler_texture[s] != tex->id) {
            static const bool log_roles = recomp_env("D3D9_TEXMOD_LOG") != nullptr;
            static std::unordered_map<uint64_t, int> told;
            if (log_roles && told.emplace((uint64_t)tex->id << 8 | (uint64_t)s, 1).second) {
                static char names[4096];
                std::string role = "?";
                if (!pl.ps.empty()) {
                    // CTAB samplers: register set 3, index in the high half
                    const uint8_t *code = pl.ps.data();
                    const size_t n = pl.ps.size();
                    for (size_t i = 4; i + 8 <= n;) {
                        uint32_t t;
                        memcpy(&t, code + i, 4);
                        if ((t & 0xffff) != 0xfffe)
                            break;
                        const size_t len = (t >> 16) & 0x7fff;
                        uint32_t tag;
                        memcpy(&tag, code + i + 4, 4);
                        if (tag == 0x42415443u) {
                            const size_t base = i + 8, end = i + 4 + len * 4;
                            auto u32 = [&](size_t o) -> uint32_t {
                                uint32_t v = 0;
                                if (o + 4 <= end && o + 4 <= n)
                                    memcpy(&v, code + o, 4);
                                return v;
                            };
                            const uint32_t count = u32(base + 12), info = u32(base + 16);
                            for (uint32_t k = 0; k < count && k < 256; ++k) {
                                const size_t e = base + info + 20u * k;
                                const uint32_t name = u32(e), set_index = u32(e + 4);
                                if ((set_index & 0xffff) == 3 && (set_index >> 16) == (uint32_t)s && base + name < n)
                                    role = std::string((const char *)code + base + name,
                                                       strnlen((const char *)code + base + name, n - base - name));
                            }
                            break;
                        }
                        i += 4 + len * 4;
                    }
                }
                (void)names;
                LOGW("d3d9: texmod: texture %ux%u (hash %08x) drawn as %s (stage %d, ps %016llx)", tex->width,
                     tex->height, ~texmod::table()[tex->id].hash, role.c_str(), s, (unsigned long long)pl.ps_key);
            }
        }
        if (tex->kind == K_D3D9VOLUMETEXTURE && volumes().at(tex->id).lod) {
            if (!volume_lod) {
                memcpy(sampler_state, pl.sampler_state, sizeof sampler_state);
                volume_lod = true;
            }
            sampler_state[s][9] = std::max(sampler_state[s][9], volumes().at(tex->id).lod);
        }
    }
    d.sampler_state = volume_lod ? &sampler_state[0][0] : &pl.sampler_state[0][0];
    d.render_state = pl.rs;
    d.render_state_set = (const uint8_t *)pl.rs_set;
    // A volume-LOD draw passes a local copy (sampler [s][9] raised), not the pipeline's tables: version 0 so the queue
    // neither reuses an earlier table for it nor lets the next draw reuse this one.
    d.state_version = volume_lod ? 0 : pl.state_version;
    // Test133: private render state 251 = a guest texture pointer the mod counts as scene content under the HUD.
    if (pl.rs[251] && pl.sampler_tex[0]) {
        ComObj *scene_tex = com_this(pl.rs[251]);
        d.hdr_scene_texture = scene_tex && scene_tex->id == pl.sampler_tex[0];
    }
    for (int st = 0; st < 8; ++st)
        if (pl.tss[st][24] & 0x100) // D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED
            d.projected_mask |= 1u << st;
    d.label = pl.label;
    host_d9_draw(&d);
    bool color_write = !pl.rs_set[168] || (pl.rs[168] & 0xf) != 0;
    bool z_write = (!pl.rs_set[14] || pl.rs[14]) && (!pl.rs_set[7] || pl.rs[7]);
    gpu_target_drawn(dev, color_write, z_write);
}

// Hands one draw to the CPU renderer with what the device has bound.
static void raster(X86 *c, const D9DrawCall &call) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    ComObj *target = com_get(dev->render_target);
    if (!target)
        target = device_backbuffer(dev);
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    static const std::vector<uint8_t> none;
    d9_raster_draw(dev, target, (decl && decl->kind == K_D3D9DECL) ? decl->blob : none, call);
}

// (this, PrimitiveType, StartVertex, PrimitiveCount)
static bool draw_refused(X86 *c, const char *what, uint32_t prim, uint32_t count, uint32_t vertices, uint32_t stride);

void Dev_DrawPrimitive(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives from vertex %u, from the bound stream", g_draws,
             prim_name(arg(c, 1)), arg(c, 3), arg(c, 2));
    if (draw_refused(c, "DrawPrimitive", arg(c, 1), arg(c, 3), 0, 0)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *dev = this_device9(c);
    if (dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 81);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 3);
        d.start = arg(c, 2);
        gpu_draw(dev, d);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    if (vb && resource_bytes(vb) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 3);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.first = arg(c, 2);
        raster(c, call);
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
void Dev_DrawIndexedPrimitive(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices from index %u, indexed", g_draws,
             prim_name(arg(c, 1)), arg(c, 6), arg(c, 4), arg(c, 5));
    if (draw_refused(c, "DrawIndexedPrimitive", arg(c, 1), arg(c, 6), arg(c, 4), 0)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *dev = this_device9(c);
    if (dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 82);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 6);
        d.start = arg(c, 5);
        d.base_vertex = (int32_t)arg(c, 2);
        ComObj *ibo = com_get(d9_pipeline(dev->id).index_buffer);
        if (ibo) {
            gpu_sync_buffer(ibo);
            d.index_buffer = ibo->id;
            d.index_size = ibo->rmask ? ibo->rmask : 2;
            gpu_draw(dev, d);
        } else {
            reject_draw(dev, d, 3);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    ComObj *ib = dev ? com_get(dev->front_obj) : nullptr;
    if (vb && resource_bytes(vb) && ib && resource_bytes(ib) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 6);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.index_data = resource_bytes(ib);
        call.index_bytes = ib->blob.size();
        call.index_size = ib->rmask ? ib->rmask : 2;
        call.base_vertex = (int32_t)arg(c, 2);
        call.first = arg(c, 5);
        raster(c, call);
    }
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, Stride): the
// vertices travel with the call rather than in a buffer, which is how this
// game draws.
// What a rasterizer will need from a draw, spelled out for the first few: the
// bound vertex declaration decoded, the raw vertices as floats, the bound
// texture, and the surface being drawn into.
static void describe_draw_inputs(X86 *c, uint32_t vertices, uint32_t stride, uint32_t nverts) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    if (decl && decl->kind == K_D3D9DECL) {
        static const char *types[] = {"FLOAT1",    "FLOAT2",   "FLOAT3",   "FLOAT4",  "D3DCOLOR",
                                      "UBYTE4",    "SHORT2",   "SHORT4",   "UBYTE4N", "SHORT2N",
                                      "SHORT4N",   "USHORT2N", "USHORT4N", "UDEC3",   "DEC3N",
                                      "FLOAT16_2", "FLOAT16_4"};
        static const char *usages[] = {
            "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
            "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",   "DEPTH",    "SAMPLE"};
        char line[512];
        size_t used = 0;
        for (size_t i = 0; i + 8 <= decl->blob.size(); i += 8) {
            uint16_t stream = (uint16_t)(decl->blob[i] | (decl->blob[i + 1] << 8));
            if (stream == 0xff)
                break;
            uint16_t off = (uint16_t)(decl->blob[i + 2] | (decl->blob[i + 3] << 8));
            uint8_t type = decl->blob[i + 4], usage = decl->blob[i + 6], index = decl->blob[i + 7];
            used += (size_t)snprintf(line + used, sizeof line - used, " %s%u@%u:%s",
                                     usage < 14 ? usages[usage] : "?", index, off,
                                     type < 17 ? types[type] : "?");
            if (used >= sizeof line)
                break;
        }
        LOGW("d3d9:   declaration %u:%s", decl->id, line);
    } else {
        LOGW("d3d9:   no vertex declaration bound (FVF path)");
    }
    for (uint32_t v = 0; v < nverts && v < 4; ++v) {
        uint32_t base = vertices + v * stride;
        char line[256];
        size_t used = 0;
        for (uint32_t o = 0; o + 4 <= stride && o < 40; o += 4)
            used += (size_t)snprintf(
                line + used, sizeof line - used, " %g", (double)[&] {
                    float f;
                    uint32_t bits = rd32(base + o);
                    memcpy(&f, &bits, 4);
                    return f;
                }());
        LOGW("d3d9:   vertex %u:%s", v, line);
    }
    ComObj *tex = com_get(dev->texture_handle);
    ComObj *rt = com_get(dev->render_target);
    LOGW("d3d9:   texture %u (%ux%u), target %u (%ux%u)%s", tex ? tex->id : 0, tex ? tex->width : 0,
         tex ? tex->height : 0, rt ? rt->id : 0, rt ? rt->width : 0, rt ? rt->height : 0,
         (rt && rt->id == dev->palette_obj) ? ", the back buffer" : "");
}


// Test155 (test session: the game's memory footprint went from 3.1 GB to 361 GB within a minute in
// rain and macOS killed it for low swap; 361 GB / 2^31 = ~180 bytes per primitive). The UP draws computed their
// vertex bytes in 32 bits, so a garbage primitive count (e.g. 0x80000000 + n) wrapped to a size that still fit the
// guest arena and was accepted. Direct3D 9 limits every draw to the device's MaxPrimitiveCount (and indexed draws
// to MaxVertexIndex) and an invalid draw returns D3DERR_INVALIDCALL. All four draw calls check the caps values
// this device reports, before any size arithmetic or allocation, and log the raw parameters and the caller.
static uint64_t primitive_vertices64(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1: return count;
    case 2: return (uint64_t)count * 2;
    case 3: return (uint64_t)count + 1;
    case 4: return (uint64_t)count * 3;
    case 5:
    case 6: return (uint64_t)count + 2;
    default: return 0;
    }
}
static bool draw_refused(X86 *c, const char *what, uint32_t prim, uint32_t count, uint32_t vertices, uint32_t stride) {
    if (count <= D9_MAX_PRIMITIVE_COUNT && vertices <= (uint64_t)D9_MAX_VERTEX_INDEX + 1)
        return false;
    static uint64_t refused = 0;
    ++refused;
    if (refused <= 16 || (refused & (refused - 1)) == 0)
        LOGW("d3d9: %s refused (D3DERR_INVALIDCALL, as Direct3D 9): primitive count %u (0x%08x, signed %d), type %u, "
             "%u vertices, stride %u, caller %08x; device limits %u primitives / %u vertex index (%llu refused so far)",
             what, count, count, (int32_t)count, prim, vertices, stride, rd32(c->r[R_ESP]), D9_MAX_PRIMITIVE_COUNT,
             D9_MAX_VERTEX_INDEX, (unsigned long long)refused);
    return true;
}

void Dev_DrawPrimitiveUP(X86 *c) {
    if (draw_worth_reporting()) {
        LOGW("d3d9: draw %u: %s, %u primitives, %u-byte vertices inline at %08x", g_draws,
             prim_name(arg(c, 1)), arg(c, 2), arg(c, 4), arg(c, 3));
        // A fan of n primitives has n + 2 vertices.
        if (g_draws <= 8)
            describe_draw_inputs(c, arg(c, 3), arg(c, 4), arg(c, 2) + 2);
    }
    if (draw_refused(c, "DrawPrimitiveUP", arg(c, 1), arg(c, 2), 0, arg(c, 4))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 83);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 2);
        const uint64_t bytes64 = primitive_vertices64(d.primitive, d.primitive_count) * (uint64_t)arg(c, 4);
        uint32_t bytes = bytes64 <= 0xffffffffull ? (uint32_t)bytes64 : 0;
        if (bytes && gm_fits(arg(c, 3), bytes)) {
            d.stream[0].stride = arg(c, 4);
            d.inline_vertices = gm_ptr(arg(c, 3));
            d.inline_bytes = bytes;
            gpu_draw(dev, d);
        } else {
            reject_draw(dev, d, 4);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 2);
    call.vertices = arg(c, 3);
    call.stride = arg(c, 4);
    raster(c, call);
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
//  pIndexData, IndexDataFormat, pVertexStreamZeroData, Stride)
void Dev_DrawIndexedPrimitiveUP(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices of %u bytes inline at %08x, "
             "indices at %08x",
             g_draws, prim_name(arg(c, 1)), arg(c, 4), arg(c, 3), arg(c, 8), arg(c, 7), arg(c, 5));
    if (draw_refused(c, "DrawIndexedPrimitiveUP", arg(c, 1), arg(c, 4), arg(c, 3), arg(c, 8))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 84);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 4);
        uint32_t isize = arg(c, 6) == 102 ? 4 : 2;
        const uint64_t ib64 = primitive_vertices64(d.primitive, d.primitive_count) * isize;
        const uint64_t vb64 = ((uint64_t)arg(c, 2) + arg(c, 3)) * (uint64_t)arg(c, 8);
        uint32_t ibytes = ib64 <= 0xffffffffull ? (uint32_t)ib64 : 0;
        uint32_t vbytes = vb64 <= 0xffffffffull ? (uint32_t)vb64 : 0;
        if (ibytes && vbytes && gm_fits(arg(c, 5), ibytes) && gm_fits(arg(c, 7), vbytes)) {
            d.stream[0].stride = arg(c, 8);
            d.inline_vertices = gm_ptr(arg(c, 7));
            d.inline_bytes = vbytes;
            d.inline_indices = gm_ptr(arg(c, 5));
            d.inline_index_bytes = ibytes;
            d.index_size = isize;
            gpu_draw(dev, d);
        } else {
            reject_draw(dev, d, 4);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 4);
    call.vertices = arg(c, 7);
    call.stride = arg(c, 8);
    call.indices = arg(c, 5);
    call.index_size = arg(c, 6) == 102 ? 4 : 2;
    raster(c, call);
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// The resource interfaces themselves
// ---------------------------------------------------------------------------
#define RES_STUB(iface, name)                                                                      \
    void iface##_##name(X86 *c) {                                                                  \
        log_once("d3d9." #iface "." #name, "d3d9: " #iface "::" #name " is not implemented");      \
        com_ret(c, D3D_OK9);                                                                       \
    }

RES_STUB(Res, SetPrivateData)
RES_STUB(Res, GetPrivateData)
RES_STUB(Res, FreePrivateData)
RES_STUB(Res, SetPriority)
RES_STUB(Res, GetPriority)
RES_STUB(Res, PreLoad)
RES_STUB(Res, GetType)
RES_STUB(Tex, SetLOD)
RES_STUB(Tex, GetLOD)
RES_STUB(Tex, SetAutoGenFilterType)
RES_STUB(Tex, GetAutoGenFilterType)
RES_STUB(Tex, GenerateMipSubLevels)
RES_STUB(Tex, AddDirtyRect)
RES_STUB(Tex, AddDirtyRectCube)
RES_STUB(Surf, GetDC)
RES_STUB(Surf, ReleaseDC)
RES_STUB(Decl, GetDeclaration)

// (this, riid, ppContainer): the texture a level or face belongs to, or the
// device when the surface stands alone.
void Surf_GetContainer(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *parent = com_get(s->front_obj);
    if (parent) {
        com_addref(parent);
        com_out_ptr(out, com_view(parent, parent->caps ? IF_D3DCUBETEXTURE9 : IF_D3DTEXTURE9));
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *dev = com_get(s->dev_d3d);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, com_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

void Res_GetDevice(X86 *c) {
    ComObj *o = com_this_arg(c);
    ComObj *dev = o ? com_get(o->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, com_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

// Lock hands the guest a staged copy. Unlock copies it back and marks the
// GPU buffer dirty. A buffer still locked at draw time is uploaded from
// that copy in gpu_sync_buffer (autosculpt never Unlock()s).
//
// A buffer locked a second time keeps its guest copy from then on, which always holds the whole buffer, and its
// Unlock writes back and re-uploads only the range the locks named (OffsetToLock, SizeToLock; 0 is the rest of the
// buffer; D3DLOCK_READONLY none). Staging the whole buffer at every Lock and uploading it at every Unlock made NFS
// Undercover's dynamic vertex buffers (particles, debris: many small appends per frame into one large buffer) a
// third of its render thread, and breaking a lamp post or a pipe dropped it to 8 fps (2026-11-15).
static uint32_t buffer_stage(ComObj *o) {
    if (o->pixels) // kept from an earlier lock: already identical to blob outside what is being written
        return o->pixels;
    o->pixels = heap_alloc((uint32_t)o->blob.size(), false, 16);
    if (o->pixels)
        memcpy(gm_ptr(o->pixels), o->blob.data(), o->blob.size());
    return o->pixels;
}
// RECOMP_TRACE_LOCKS=1 (diagnostic): every 5 s, writable buffer locks per second by flags (none, NOOVERWRITE,
// DISCARD) and the busiest buffers with their sizes.
static void lock_trace(const ComObj *o, uint32_t flags, uint32_t bytes) {
    static const bool on = recomp_env("TRACE_LOCKS") != nullptr;
    if (!on || (flags & 0x10u))
        return;
    struct Row {
        uint64_t n = 0, plain = 0, noov = 0, disc = 0, bytes = 0;
        uint32_t total = 0;
    };
    static auto *rows = new std::unordered_map<uint32_t, Row>();
    static auto last = std::chrono::steady_clock::now();
    Row &r = (*rows)[o->id];
    ++r.n;
    r.bytes += bytes;
    r.total = (uint32_t)o->blob.size();
    if (flags & 0x2000u)
        ++r.disc;
    else if (flags & 0x1000u)
        ++r.noov;
    else
        ++r.plain;
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - last).count();
    if (s < 5.0)
        return;
    std::vector<std::pair<uint32_t, Row>> v(rows->begin(), rows->end());
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.n > b.second.n; });
    Row all;
    for (auto &e : v) {
        all.n += e.second.n, all.plain += e.second.plain, all.noov += e.second.noov, all.disc += e.second.disc;
        all.bytes += e.second.bytes;
    }
    LOGW("d3d9: buffer locks/s %.0f (plain %.0f, nooverwrite %.0f, discard %.0f), %.1f MB/s locked, %zu buffers",
         all.n / s, all.plain / s, all.noov / s, all.disc / s, all.bytes / s / 1e6, v.size());
    for (size_t i = 0; i < v.size() && i < 6; ++i)
        LOGW("d3d9:   buffer %u (%u bytes): %.0f/s plain %.0f noov %.0f discard %.0f, %.1f MB/s", v[i].first,
             v[i].second.total, v[i].second.n / s, v[i].second.plain / s, v[i].second.noov / s,
             v[i].second.disc / s, v[i].second.bytes / s / 1e6);
    rows->clear();
    last = now;
}
static void buffer_lock(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t offset = arg(c, 1), size = arg(c, 2), out = arg(c, 3), flags = arg(c, 4);
    if (o && !o->blob.empty())
        lock_trace(o, flags, size ? size : (uint32_t)o->blob.size());
    if (!o || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t total = (uint32_t)o->blob.size();
    if (offset >= total || o->blob.empty()) {
        com_out_ptr(out, 0);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    const uint32_t staged = buffer_stage(o);
    if (!staged) {
        com_out_ptr(out, 0);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    uint32_t end = size == 0 || size > total - offset ? total : offset + size;
    if (!(flags & 0x10u)) { // not D3DLOCK_READONLY: the range comes back at Unlock
        if (!(flags & 0x1000u)) // without D3DLOCK_NOOVERWRITE (DISCARD included) it may rewrite what draws read
            o->lock_overwrites = true;
        if (o->lock_hi <= o->lock_lo) {
            o->lock_lo = offset;
            o->lock_hi = end;
        } else {
            o->lock_lo = std::min(o->lock_lo, offset);
            o->lock_hi = std::max(o->lock_hi, end);
        }
    }
    ++o->lock_count;
    ++o->lock_total;
    com_out_ptr(out, staged + offset);
    com_ret(c, D3D_OK9);
}
void VB_Lock(X86 *c) {
    buffer_lock(c);
}
void IB_Lock(X86 *c) {
    buffer_lock(c);
}
void Buf_Unlock(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (o && o->lock_count > 0 && --o->lock_count == 0 && o->pixels) {
        const uint32_t lo = o->lock_lo, hi = std::min<uint32_t>(o->lock_hi, (uint32_t)o->blob.size());
        o->lock_lo = o->lock_hi = 0;
        const bool appended = !o->lock_overwrites;
        o->lock_overwrites = false;
        if (hi > lo) {
            memcpy(o->blob.data() + lo, gm_ptr(o->pixels) + lo, hi - lo);
            d9_raster_invalidate(o->id);
            gpu_after_unlock(o, lo, hi, appended);
        }
        if (o->lock_total < 2) { // locked once so far (most static geometry): give the guest heap back
            heap_free(o->pixels);
            o->pixels = 0;
        }
    }
    com_ret(c, D3D_OK9);
}
// D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size.
void Buf_GetDesc(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!o || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 20);
    wr32(d + 16, o->buf_bytes);
    com_ret(c, D3D_OK9);
}

// D3DLOCKED_RECT is { INT Pitch; void *pBits; }.
static bool readback_settle(ComObj *s, uint32_t flags);
void Surf_LockRect(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 1);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // (this, pLockedRect, pRect, Flags): a GetRenderTargetData copy still on the GPU answers
    // D3DERR_WASSTILLDRAWING to a D3DLOCK_DONOTWAIT lock, as a driver does.
    if (!readback_settle(s, arg(c, 3))) {
        com_ret(c, D3DERR_WASSTILLDRAWING);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Surf_UnlockRect(X86 *c) {
    stage_unlock(com_this_arg(c));
    com_ret(c, D3D_OK9);
}
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, Quality,
// Width, Height.
void Surf_GetDesc(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!s || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 32);
    wr32(d + 0, s->rmask ? s->rmask : 22);
    wr32(d + 24, s->width);
    wr32(d + 28, s->height);
    com_ret(c, D3D_OK9);
}

// A texture level is the surface made with the texture.
// A cube face, made on first ask and kept, so the game gets the same surface
// back every time. Returning success without writing the pointer - which is
// what the stub did - had the game store nothing and throw the cube away.
// The surface for one face and level of a cube, made on first ask and kept on
// the texture, indexed face + 6 * level.
static ComObj *cube_face(ComObj *tex, uint32_t face, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || !tex->caps || face > 5 || level > 15)
        return nullptr;
    size_t slot = face + 6u * level;
    if (tex->surfaces.size() <= slot)
        tex->surfaces.resize(slot + 1, 0);
    ComObj *s = com_get(tex->surfaces[slot]);
    if (!s) {
        uint32_t edge = std::max(tex->width >> level, 1u);
        s = make_surface(com_get(tex->dev_d3d), edge, edge, tex->rmask ? tex->rmask : 22);
        if (!s)
            return nullptr;
        s->front_obj = tex->id;
        tex->surfaces[slot] = s->id;
    }
    return s;
}

// (this, FaceType, Level, ppCubeMapSurface). A face is a view into the
// texture, so handing one out references the texture too: the game takes
// face zero, releases the cube, and goes on using it for the other five.
void Tex_GetCubeMapSurface(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 3);
    ComObj *s = cube_face(tex, arg(c, 1), arg(c, 2));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = com_view(s, IF_D3DSURFACE9);
    if (!view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    surface_addref(s); // a face shares its cube texture's lifetime (Test159)
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

// Returns a count, not an HRESULT. The stub returned zero, which told the
// game its textures had no levels at all.
// (this, FaceType, Level, pLockedRect, pRect, Flags)
void Tex_LockRectCube(X86 *c) {
    ComObj *s = cube_face(com_this_arg(c), arg(c, 1), arg(c, 2));
    uint32_t out = arg(c, 3);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
// (this, FaceType, Level)
void Tex_UnlockRectCube(X86 *c) {
    stage_unlock(cube_face(com_this_arg(c), arg(c, 1), arg(c, 2)));
    com_ret(c, D3D_OK9);
}

void Tex_GetLevelCount(X86 *c) {
    ComObj *tex = com_this_arg(c);
    set_eax(c, (tex && !tex->caps && !tex->surfaces.empty()) ? (uint32_t)tex->surfaces.size() : 1);
}

// (this, Level, pDesc): the same D3DSURFACE_DESC a surface describes itself
// with, for the level the texture was created at.
void Tex_GetLevelDesc(X86 *c) {
    ComObj *tex = com_this_arg(c);
    ComObj *s = texture_level(tex, arg(c, 1));
    uint32_t d = arg(c, 2);
    if (!tex || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 32);
    wr32(d + 0, tex->rmask ? tex->rmask : 22);
    wr32(d + 24, s ? s->width : tex->width);
    wr32(d + 28, s ? s->height : tex->height);
    com_ret(c, D3D_OK9);
}

void Tex_GetSurfaceLevel(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 2);
    ComObj *s = texture_level(tex, arg(c, 1));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // A level is a view into the texture and references it, the same way a
    // cube face does.
    s->front_obj = tex->id;
    surface_addref(s); // Test159: the container reference goes with the last outside one
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        surface_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
void Tex_LockRect(X86 *c) {
    ComObj *s = texture_level(com_this_arg(c), arg(c, 1));
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Tex_UnlockRect(X86 *c) {
    stage_unlock(texture_level(com_this_arg(c), arg(c, 1)));
    com_ret(c, D3D_OK9);
}

// Volume textures use the same guest ABI and canonical host storage as 2D
// resources. Each mip owns a whole 3D box; uploads preserve the Z slices.
static const uint8_t IID_VolumeTexture[16] =
    IID_BYTES(0x2518526c, 0xe789, 0x4111, 0xa7, 0xb9, 0x47, 0xef, 0x32, 0x8d, 0x13, 0xe6);
static const uint8_t IID_Volume[16] =
    IID_BYTES(0x24f416e6, 0x1f67, 0x4aa7, 0xb8, 0x8e, 0xd3, 0x3f, 0x6f, 0x31, 0x28, 0xa1);
static const uint8_t IID_BaseTexture[16] =
    IID_BYTES(0x580ca87e, 0x1d3c, 0x4d54, 0x99, 0x1d, 0xb7, 0xd3, 0xe3, 0xc2, 0x98, 0xce);
static const uint8_t IID_Resource[16] =
    IID_BYTES(0x05eec05d, 0x8f7d, 0x4362, 0xb9, 0x99, 0xd1, 0xba, 0xf3, 0x57, 0xc7, 0x04);
static bool volume_base_iid(uint32_t p) {
    return p && gm_valid(p, 16) &&
           (!memcmp(gm_ptr(p), IID_BaseTexture, 16) || !memcmp(gm_ptr(p), IID_Resource, 16));
}
static void VolTex_QueryInterface(X86 *c) {
    ComObj *o = com_this_arg(c);
    const uint32_t out = arg(c, 2);
    if (o && out && gm_valid(out, 4) && volume_base_iid(arg(c, 1))) {
        wr32(out, o->identity); // prefix interfaces have the same controlling identity
        com_addref(o);
        com_ret(c, S_OK);
    } else {
        com_QueryInterface(c);
    }
}
// A volume texture and its levels, without the COM call around it: shared by
// CreateVolumeTexture and D3DXCreateVolumeTextureFromFileInMemory.
static ComObj *volume_create(ComObj *dev, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                             uint32_t usage, uint32_t fmt, uint32_t pool) {
    levels = levels ? levels : d9volume::levels(w, h, depth);
    ComObj *tex = com_new(K_D3D9VOLUMETEXTURE);
    if (!tex)
        return nullptr;
    volumes()[tex->id] = {depth, pool};
    tex->dev_d3d = dev->id;
    tex->width = w;
    tex->height = h;
    tex->bpp = d9volume::pixel_bytes(fmt) * 8;
    tex->rmask = fmt;
    resource_usage()[tex->id] = usage;
    bool complete = true;
    for (uint32_t l = 0; l < levels; ++l) {
        ComObj *v = com_new(K_D3D9VOLUME);
        if (!v) {
            complete = false;
            break;
        }
        tex->surfaces.push_back(v->id);
        v->dev_d3d = dev->id;
        v->front_obj = tex->id;
        v->width = std::max(w >> l, 1u);
        v->height = std::max(h >> l, 1u);
        v->rmask = fmt;
        v->bpp = tex->bpp;
        v->pitch = v->width * (v->bpp / 8);
        volumes()[v->id] = {std::max(depth >> l, 1u), pool};
        resource_usage()[v->id] = usage;
        resource_alloc(v, v->pitch * v->height * volumes().at(v->id).depth);
    }
    if (!complete) {
        com_release(tex);
        return nullptr;
    }
    return tex;
}
void Dev_CreateVolumeTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t w = arg(c, 1), h = arg(c, 2), depth = arg(c, 3), usage = arg(c, 5),
                   fmt = arg(c, 6), pool = arg(c, 7), out = arg(c, 8);
    uint32_t levels = arg(c, 4);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    if (!dev || !d9volume::extent(w, h, depth, levels) || !d9volume::pixel_bytes(fmt) || pool > 3 ||
        (usage & ~0x210u) || ((usage & 0x200) && pool != 0) || arg(c, 9)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = volume_create(dev, w, h, depth, levels, usage, fmt, pool);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    const uint32_t view = com_view(tex, IF_D3DVOLUMETEXTURE9);
    if (!view) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    com_ret(c, D3D_OK9);
}
static bool volume_box(ComObj *v, uint32_t box, uint32_t &offset) {
    offset = 0;
    if (!v)
        return false;
    if (!box)
        return true;
    if (!gm_valid(box, 24))
        return false;
    const uint32_t x = rd32(box), y = rd32(box + 4), r = rd32(box + 8), b = rd32(box + 12),
                   z = rd32(box + 16), back = rd32(box + 20);
    if (x >= r || r > v->width || y >= b || b > v->height || z >= back ||
        back > volumes().at(v->id).depth)
        return false;
    offset = z * v->pitch * v->height + y * v->pitch + x * (v->bpp / 8);
    return true;
}
static void volume_lock(X86 *c, ComObj *v, uint32_t out, uint32_t box, uint32_t flags) {
    uint32_t offset;
    if (!v || !out || !gm_valid(out, 12) || v->lock_count || !volume_box(v, box, offset) ||
        (flags & ~0xa810u) ||
        ((flags & 0x2000) && ((flags & 0x10) || box || !(resource_usage()[v->id] & 0x200))) ||
        (volumes().at(v->id).pool == 0 && !(resource_usage()[v->id] & 0x200))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t data = stage_lock(v);
    if (!data) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    volumes().at(v->id).lock_flags = flags;
    wr32(out, v->pitch);
    wr32(out + 4, v->pitch * v->height);
    wr32(out + 8, data + offset);
    com_ret(c, D3D_OK9);
}
static void volume_unlock(X86 *c, ComObj *v) {
    if (!v || !v->lock_count || !v->pixels) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t flags = volumes().at(v->id).lock_flags;
    if (!(flags & 0x10))
        memcpy(v->blob.data(), gm_ptr(v->pixels), v->blob.size());
    heap_free(v->pixels);
    v->pixels = 0;
    v->lock_count = 0;
    if (!(flags & (0x10 | 0x8000)))
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
    com_ret(c, D3D_OK9);
}
// D3DXFillVolumeTexture's work, here where volume storage lives: for every
// texel of every level, the guest's LPD3DXFILL3D (out float4, texel-centre
// coordinate, texel size, user data) gives a colour, stored in the level's
// format. Need for Speed Carbon builds its colour-grading lookup this way
// (0073c1a0 -> 0073c100); left unfilled, the lookup is zero and every 3D
// pixel that passes through the visual treatment comes out black.
bool d3d9_fill_volume(X86 *c, uint32_t view, uint32_t fn, uint32_t data) {
    ComObj *tex = view ? com_this(view) : nullptr;
    if (!tex || tex->kind != K_D3D9VOLUMETEXTURE || !fn)
        return false;
    const uint32_t scratch = heap_alloc(48, true);
    if (!scratch)
        return false;
    const uint32_t out = scratch, coord = scratch + 16, size = scratch + 32;
    auto wrf = [](uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); };
    auto rdf = [](uint32_t a) { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; };
    auto u8 = [](float f) {
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        return uint32_t(f * 255.f + 0.5f);
    };
    for (uint32_t l = 0; l < tex->surfaces.size(); ++l) {
        ComObj *v = volume_level(tex, l);
        if (!v || v->lock_count)
            continue;
        const uint32_t w = v->width, h = v->height, d = volumes().at(v->id).depth;
        const uint32_t bpp = v->bpp / 8, fmt = v->rmask;
        if (v->blob.size() < size_t(v->pitch) * h * d)
            continue;
        wrf(size, 1.f / w);
        wrf(size + 4, 1.f / h);
        wrf(size + 8, 1.f / d);
        for (uint32_t z = 0; z < d; ++z)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    wrf(coord, (x + 0.5f) / w);
                    wrf(coord + 4, (y + 0.5f) / h);
                    wrf(coord + 8, (z + 0.5f) / d);
                    for (int i = 0; i < 4; ++i)
                        wrf(out + 4 * i, 0.f);
                    const uint32_t args[4] = {out, coord, size, data};
                    guest_call(c, fn, args, 4);
                    const uint32_t r = u8(rdf(out)), g = u8(rdf(out + 4)),
                                   b = u8(rdf(out + 8)), a = u8(rdf(out + 12));
                    uint8_t *px = v->blob.data() + size_t(z) * v->pitch * h + size_t(y) * v->pitch +
                                  size_t(x) * bpp;
                    if (bpp == 4) {
                        const uint32_t argb = (fmt == 22 ? 0xffu : a) << 24 | r << 16 | g << 8 | b;
                        memcpy(px, &argb, 4);
                    } else {
                        *px = uint8_t(fmt == 28 ? a : r); // A8 keeps alpha, L8 the first channel
                    }
                }
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
    }
    heap_free(scratch);
    return true;
}
// ---- DDS files handed to D3DXCreate{Cube,Volume}TextureFromFileInMemory ----
// NFSC keeps both in GLOBALB.BUN chunks (0x30300201 cube, 0x30300200 volume):
// five 256^2 DXT1 environment cubes and a 32^3 A8R8G8B8 volume with 6 mips.
struct DdsInfo {
    uint32_t w = 0, h = 0, d = 1, levels = 1, fmt = 0, caps2 = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
};
static bool dds_parse(const uint8_t *p, size_t n, DdsInfo &o) {
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, p + at, 4); return v; };
    if (n < 128 || memcmp(p, "DDS ", 4) || u32(4) != 124)
        return false;
    const uint32_t flags = u32(8), pf_flags = u32(80), fourcc = u32(84), bits = u32(88),
                   rmask = u32(92), amask = u32(104);
    o.h = u32(12);
    o.w = u32(16);
    o.d = (flags & 0x800000) && u32(24) ? u32(24) : 1; // DDSD_DEPTH
    o.levels = (flags & 0x20000) && u32(28) ? u32(28) : 1; // DDSD_MIPMAPCOUNT
    o.caps2 = u32(112);
    if (pf_flags & 0x4) { // DDPF_FOURCC
        if (!dxt_block_bytes(fourcc))
            return false;
        o.fmt = fourcc;
    } else if ((pf_flags & 0x40) && bits == 32 && rmask == 0x00ff0000u) { // DDPF_RGB
        o.fmt = (pf_flags & 0x1) && amask == 0xff000000u ? 21u : 22u;      // A8R8G8B8 : X8R8G8B8
    } else if (bits == 8 && (pf_flags & 0x2)) {                               // DDPF_ALPHA
        o.fmt = 28;                                                           // A8
    } else if (bits == 8 && (pf_flags & 0x20000)) {                           // DDPF_LUMINANCE
        o.fmt = 50;                                                           // L8
    } else {
        return false;
    }
    if (!o.w || !o.h || o.levels > 16)
        return false;
    o.data = p + 128;
    o.size = n - 128;
    return true;
}
// Bytes in one level's 2D slice, and in one row as the file packs it.
static size_t dds_row_bytes(uint32_t fmt, uint32_t w) {
    if (uint32_t b = dxt_block_bytes(fmt))
        return size_t(std::max((w + 3) / 4, 1u)) * b;
    return size_t(w) * format_bytes(fmt);
}
static uint32_t dds_rows(uint32_t fmt, uint32_t h) {
    return dxt_block_bytes(fmt) ? std::max((h + 3) / 4, 1u) : h;
}

// Returns the new cube texture's IDirect3DCubeTexture9 view, or 0.
uint32_t d3d9_cube_from_dds(uint32_t device_view, const uint8_t *p, size_t n) {
    ComObj *dev = device_view ? com_this(device_view) : nullptr;
    DdsInfo dds;
    if (!dev || !dds_parse(p, n, dds) || !(dds.caps2 & 0x200) || dds.w != dds.h) {
        LOGW("d3dx9: cube DDS not loaded (%zu bytes, unsupported or not a full cube)", n);
        return 0;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex)
        return 0;
    tex->dev_d3d = dev->id;
    tex->width = tex->height = dds.w;
    tex->bpp = format_bytes(dds.fmt) * 8;
    tex->rmask = dds.fmt;
    tex->caps = 1;
    tex->pal_flags = dds.levels;
    resource_usage()[tex->id] = 0;
    size_t at = 0;
    for (uint32_t face = 0; face < 6; ++face) // file order: +X -X +Y -Y +Z -Z, each with its mips
        for (uint32_t l = 0; l < dds.levels; ++l) {
            const uint32_t edge = std::max(dds.w >> l, 1u);
            const size_t row = dds_row_bytes(dds.fmt, edge);
            const uint32_t rows = dds_rows(dds.fmt, edge);
            if (at + row * rows > dds.size) {
                LOGW("d3dx9: cube DDS truncated at face %u level %u", face, l);
                com_release(tex);
                return 0;
            }
            ComObj *s = cube_face(tex, face, l);
            const uint32_t staged = s ? stage_lock(s) : 0;
            if (!staged) {
                com_release(tex);
                return 0;
            }
            for (uint32_t r = 0; r < rows; ++r)
                memcpy(gm_ptr(staged + r * s->pitch), dds.data + at + r * row,
                       std::min<size_t>(row, s->pitch));
            stage_unlock(s);
            at += row * rows;
        }
    const uint32_t view = com_view(tex, IF_D3DCUBETEXTURE9);
    if (!view)
        com_release(tex);
    return view;
}

// Returns the new volume texture's IDirect3DVolumeTexture9 view, or 0.
uint32_t d3d9_volume_from_dds(uint32_t device_view, const uint8_t *p, size_t n) {
    ComObj *dev = device_view ? com_this(device_view) : nullptr;
    DdsInfo dds;
    if (!dev || !dds_parse(p, n, dds) || !(dds.caps2 & 0x200000) || !d9volume::pixel_bytes(dds.fmt) ||
        !d9volume::extent(dds.w, dds.h, dds.d, dds.levels)) {
        LOGW("d3dx9: volume DDS not loaded (%zu bytes, unsupported format or size)", n);
        return 0;
    }
    ComObj *tex = volume_create(dev, dds.w, dds.h, dds.d, dds.levels, 0, dds.fmt, 1);
    if (!tex)
        return 0;
    size_t at = 0;
    for (uint32_t l = 0; l < tex->surfaces.size(); ++l) {
        ComObj *v = volume_level(tex, l);
        const uint32_t d = volumes().at(v->id).depth;
        const size_t row = size_t(v->width) * (v->bpp / 8);
        const size_t bytes = row * v->height * d;
        if (at + bytes > dds.size || v->blob.size() < bytes) {
            LOGW("d3dx9: volume DDS truncated at level %u", l);
            com_release(tex);
            return 0;
        }
        for (uint32_t z = 0; z < d; ++z)
            for (uint32_t y = 0; y < v->height; ++y)
                memcpy(v->blob.data() + (size_t(z) * v->height + y) * v->pitch,
                       dds.data + at + (size_t(z) * v->height + y) * row, row);
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
        at += bytes;
    }
    const uint32_t view = com_view(tex, IF_D3DVOLUMETEXTURE9);
    if (!view)
        com_release(tex);
    return view;
}
// ---- D3DXLoadSurfaceFromSurface: whole-surface copy with format conversion ----
// NFSC copies textures into one another at run time (007d7580 when formats
// differ, 00759040 per mip level, 00732ca0 once at boot). Pixels are read from
// the surface's canonical CPU copy, so a GPU-rendered source (a render target)
// is only as current as its last write-back.
static void px_to_rgba(uint32_t fmt, const uint8_t *p, uint8_t *o) {
    uint32_t v = 0;
    switch (fmt) {
    case 21: o[2] = p[0]; o[1] = p[1]; o[0] = p[2]; o[3] = p[3]; return;      // A8R8G8B8 (BGRA bytes)
    case 22: o[2] = p[0]; o[1] = p[1]; o[0] = p[2]; o[3] = 255; return;       // X8R8G8B8
    case 23: v = p[0] | p[1] << 8;                                             // R5G6B5
        o[0] = uint8_t((v >> 11 & 31) * 255 / 31); o[1] = uint8_t((v >> 5 & 63) * 255 / 63);
        o[2] = uint8_t((v & 31) * 255 / 31); o[3] = 255; return;
    case 24: case 25: v = p[0] | p[1] << 8;                                    // X1R5G5B5 / A1R5G5B5
        o[0] = uint8_t((v >> 10 & 31) * 255 / 31); o[1] = uint8_t((v >> 5 & 31) * 255 / 31);
        o[2] = uint8_t((v & 31) * 255 / 31); o[3] = fmt == 25 ? (v & 0x8000 ? 255 : 0) : 255; return;
    case 26: v = p[0] | p[1] << 8;                                             // A4R4G4B4
        o[0] = uint8_t((v >> 8 & 15) * 17); o[1] = uint8_t((v >> 4 & 15) * 17);
        o[2] = uint8_t((v & 15) * 17); o[3] = uint8_t((v >> 12 & 15) * 17); return;
    case 28: o[0] = o[1] = o[2] = 0; o[3] = p[0]; return;                      // A8
    case 50: o[0] = o[1] = o[2] = p[0]; o[3] = 255; return;                    // L8
    default: o[0] = o[1] = o[2] = 0; o[3] = 255; return;
    }
}
static void rgba_to_px(uint32_t fmt, const uint8_t *o, uint8_t *p) {
    uint32_t v;
    switch (fmt) {
    case 21: p[0] = o[2]; p[1] = o[1]; p[2] = o[0]; p[3] = o[3]; return;
    case 22: p[0] = o[2]; p[1] = o[1]; p[2] = o[0]; p[3] = 255; return;
    case 23: v = (o[0] >> 3) << 11 | (o[1] >> 2) << 5 | o[2] >> 3; p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); return;
    case 24: case 25: v = (o[0] >> 3) << 10 | (o[1] >> 3) << 5 | o[2] >> 3 | (fmt == 25 && o[3] < 128 ? 0 : 0x8000);
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); return;
    case 26: v = (o[3] >> 4) << 12 | (o[0] >> 4) << 8 | (o[1] >> 4) << 4 | o[2] >> 4;
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); return;
    case 28: p[0] = o[3]; return;
    case 50: p[0] = uint8_t((o[0] * 77 + o[1] * 150 + o[2] * 29) >> 8); return;
    default: return;
    }
}
static bool rgba_format_known(uint32_t fmt) {
    switch (fmt) {
    case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 50:
        return true;
    default:
        return dxt_block_bytes(fmt) != 0;
    }
}
static void dxt_color_565(const uint8_t *o, uint16_t &c) { // rounded, not truncated
    c = uint16_t((o[0] * 31 + 127) / 255 << 11 | (o[1] * 63 + 127) / 255 << 5 | (o[2] * 31 + 127) / 255);
}
static void unpack_565(uint16_t c, uint8_t *o) {
    o[0] = uint8_t((c >> 11 & 31) * 255 / 31); o[1] = uint8_t((c >> 5 & 63) * 255 / 63);
    o[2] = uint8_t((c & 31) * 255 / 31); o[3] = 255;
}
// The two colours of a DXT colour block: the texels' principal axis, its
// extremes as a first guess, then least-squares passes against the palette
// the 565 endpoints actually give (three-colour mode when `three`). `use`
// picks the texels that count (the opaque ones of a punch-through block).
static void dxt_fit(const uint8_t px[16][4], const bool use[16], bool three, uint8_t hi[3], uint8_t lo[3]) {
    float mean[3] = {0, 0, 0};
    int n = 0;
    for (int k = 0; k < 16; ++k)
        if (use[k]) {
            for (int c = 0; c < 3; ++c)
                mean[c] += px[k][c];
            ++n;
        }
    if (!n) {
        memset(hi, 0, 3);
        memset(lo, 0, 3);
        return;
    }
    for (int c = 0; c < 3; ++c)
        mean[c] /= n;
    float cov[3][3] = {};
    for (int k = 0; k < 16; ++k)
        if (use[k])
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    cov[i][j] += (px[k][i] - mean[i]) * (px[k][j] - mean[j]);
    // Power iteration from the covariance row of the widest channel: a fixed
    // start such as grey (1,1,1) is exactly at right angles to the axis of a
    // block whose channels sum to a constant (a red-to-green ramp), and the
    // iteration would stop there with both endpoints at the mean.
    int wide = 0;
    for (int i = 1; i < 3; ++i)
        if (cov[i][i] > cov[wide][wide])
            wide = i;
    float axis[3] = {cov[wide][0], cov[wide][1], cov[wide][2]};
    if (cov[wide][wide] < 1e-6f) // one colour: any axis will do
        axis[0] = axis[1] = axis[2] = 0.577f;
    for (int it = 0; it < 8; ++it) {
        float v[3], m = 0;
        for (int i = 0; i < 3; ++i) {
            v[i] = cov[i][0] * axis[0] + cov[i][1] * axis[1] + cov[i][2] * axis[2];
            m = std::max(m, std::fabs(v[i]));
        }
        if (m < 1e-6f)
            break;
        for (int i = 0; i < 3; ++i)
            axis[i] = v[i] / m;
    }
    const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    for (int i = 0; i < 3; ++i)
        axis[i] /= len;
    float tmin = 0, tmax = 0;
    for (int k = 0; k < 16; ++k)
        if (use[k]) {
            const float t = (px[k][0] - mean[0]) * axis[0] + (px[k][1] - mean[1]) * axis[1] +
                            (px[k][2] - mean[2]) * axis[2];
            tmin = std::min(tmin, t);
            tmax = std::max(tmax, t);
        }
    float e[2][3];
    for (int c = 0; c < 3; ++c) {
        e[0][c] = mean[c] + axis[c] * tmax;
        e[1][c] = mean[c] + axis[c] * tmin;
    }
    const float steps4[4] = {0, 1, 1.0f / 3, 2.0f / 3}, steps3[3] = {0, 1, 0.5f};
    const float *steps = three ? steps3 : steps4;
    const int nsteps = three ? 3 : 4;
    for (int pass = 0; pass < 2; ++pass) {
        float q[2][3]; // the endpoints as the decoder will see them
        for (int i = 0; i < 2; ++i) {
            uint8_t b[3], u[4];
            for (int c = 0; c < 3; ++c)
                b[c] = uint8_t(std::min(255.0f, std::max(0.0f, e[i][c] + 0.5f)));
            uint16_t c565;
            dxt_color_565(b, c565);
            unpack_565(c565, u);
            for (int c = 0; c < 3; ++c)
                q[i][c] = u[c];
        }
        float aa = 0, bb = 0, ab = 0, ax[3] = {}, bx[3] = {};
        for (int k = 0; k < 16; ++k) {
            if (!use[k])
                continue;
            float best = 1e30f, t = 0;
            for (int s = 0; s < nsteps; ++s) {
                float d = 0;
                for (int c = 0; c < 3; ++c) {
                    const float p = q[0][c] + (q[1][c] - q[0][c]) * steps[s] - px[k][c];
                    d += p * p;
                }
                if (d < best) {
                    best = d;
                    t = steps[s];
                }
            }
            const float a = 1 - t, b = t;
            aa += a * a;
            bb += b * b;
            ab += a * b;
            for (int c = 0; c < 3; ++c) {
                ax[c] += a * px[k][c];
                bx[c] += b * px[k][c];
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f)
            break;
        for (int c = 0; c < 3; ++c) {
            e[0][c] = std::min(255.0f, std::max(0.0f, (ax[c] * bb - bx[c] * ab) / det));
            e[1][c] = std::min(255.0f, std::max(0.0f, (bx[c] * aa - ax[c] * ab) / det));
        }
    }
    for (int c = 0; c < 3; ++c) {
        hi[c] = uint8_t(e[0][c] + 0.5f);
        lo[c] = uint8_t(e[1][c] + 0.5f);
    }
}
static void surface_to_rgba(ComObj *s, std::vector<uint8_t> &out) {
    const uint32_t w = s->width, h = s->height, fmt = s->rmask;
    out.assign(size_t(w) * h * 4, 0);
    const uint8_t *src = s->blob.data();
    if (uint32_t block = dxt_block_bytes(fmt)) {
        const uint32_t bw = std::max((w + 3) / 4, 1u), bh = std::max((h + 3) / 4, 1u);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const uint8_t *b = src + size_t(by) * s->pitch + size_t(bx) * block;
                if (size_t(b - src) + block > s->blob.size())
                    return;
                const uint8_t *cb = block == 8 ? b : b + 8;
                const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
                uint8_t pal[4][4];
                unpack_565(c0, pal[0]);
                unpack_565(c1, pal[1]);
                const bool four = block == 16 || c0 > c1;
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = uint8_t(four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = uint8_t(four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0);
                }
                pal[2][3] = 255;
                pal[3][3] = four ? 255 : 0;
                const uint32_t bits = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
                uint8_t alpha[8] = {};
                uint64_t abits = 0;
                if (fmt == 0x35545844u) { // DXT5
                    alpha[0] = b[0];
                    alpha[1] = b[1];
                    for (int k = 1; k < 7; ++k)
                        alpha[k + 1] = alpha[0] > alpha[1]
                                           ? uint8_t(((7 - k) * alpha[0] + k * alpha[1]) / 7)
                                           : k < 5 ? uint8_t(((5 - k) * alpha[0] + k * alpha[1]) / 5)
                                                   : uint8_t(k == 5 ? 0 : 255);
                    for (int k = 0; k < 6; ++k)
                        abits |= uint64_t(b[2 + k]) << (8 * k);
                }
                for (uint32_t k = 0; k < 16; ++k) {
                    const uint32_t x = bx * 4 + k % 4, y = by * 4 + k / 4;
                    if (x >= w || y >= h)
                        continue;
                    uint8_t *o = &out[(size_t(y) * w + x) * 4];
                    memcpy(o, pal[bits >> (2 * k) & 3], 4);
                    if (fmt == 0x33545844u) // DXT3
                        o[3] = uint8_t((b[k / 2] >> ((k & 1) * 4) & 15) * 17);
                    else if (fmt == 0x35545844u)
                        o[3] = alpha[abits >> (3 * k) & 7];
                }
            }
        return;
    }
    const uint32_t bpp = format_bytes(fmt);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t at = size_t(y) * s->pitch + size_t(x) * bpp;
            if (at + bpp <= s->blob.size())
                px_to_rgba(fmt, src + at, &out[(size_t(y) * w + x) * 4]);
        }
}
// Encode RGBA (w x h) into a staged copy of `d` (pitch d->pitch). Returns how
// many DXT1 blocks kept transparent pixels.
static uint32_t rgba_to_surface(const std::vector<uint8_t> &in, ComObj *d, uint8_t *dst) {
    const uint32_t w = d->width, h = d->height, fmt = d->rmask;
    uint32_t clear_blocks = 0;
    if (uint32_t block = dxt_block_bytes(fmt)) {
        // Colour endpoints from the texels' principal axis, refined by least
        // squares against the 565 palette the decoder will actually use
        // (dxt_fit); the bounding-box corners picked colours no texel had, so
        // vinyl edges and gradients came out banded or tinted.
        // DXT1 keeps 1-bit alpha the way D3DX does: a pixel under half alpha is
        // the block's transparent colour (3-colour mode), and the endpoints come
        // from the other pixels. NFSC draws a car's vinyls into an A8R8G8B8 skin
        // that is transparent where there is no vinyl and copies it into a DXT1
        // texture (007d7580 -> 00732ca0); written opaque, the paint under the
        // vinyl came out black.
        const bool dxt1 = block == 8;
        const uint32_t bw = std::max((w + 3) / 4, 1u), bh = std::max((h + 3) / 4, 1u);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t px[16][4], lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                bool use[16], clear = false;
                for (uint32_t k = 0; k < 16; ++k) {
                    const uint32_t x = std::min(bx * 4 + k % 4, w - 1), y = std::min(by * 4 + k / 4, h - 1);
                    memcpy(px[k], &in[(size_t(y) * w + x) * 4], 4);
                    lo[3] = std::min(lo[3], px[k][3]); // alpha range for DXT5
                    hi[3] = std::max(hi[3], px[k][3]);
                    use[k] = !dxt1 || px[k][3] >= 128;
                    clear |= !use[k];
                }
                uint8_t *b = dst + size_t(by) * d->pitch + size_t(bx) * block;
                uint8_t *cb = block == 8 ? b : b + 8;
                if (clear) // DXT1 3-colour mode: c0 <= c1, index 3 = transparent black
                    ++clear_blocks;
                dxt_fit(px, use, clear, hi, lo);
                uint16_t c0, c1;
                dxt_color_565(hi, c0);
                dxt_color_565(lo, c1);
                if (clear ? c0 > c1 : c0 < c1)
                    std::swap(c0, c1);
                uint8_t pal[4][4];
                unpack_565(c0, pal[0]);
                unpack_565(c1, pal[1]);
                for (int c = 0; c < 3; ++c) {
                    pal[2][c] = clear ? uint8_t((pal[0][c] + pal[1][c]) / 2)
                                      : uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
                    pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
                }
                uint32_t bits = 0;
                for (uint32_t k = 0; k < 16; ++k) {
                    if (!use[k]) {
                        bits |= 3u << (2 * k);
                        continue;
                    }
                    int best = 0, bestd = 1 << 30;
                    for (int i = 0; i < (clear ? 3 : 4); ++i) {
                        int dd = 0;
                        for (int c = 0; c < 3; ++c)
                            dd += (px[k][c] - pal[i][c]) * (px[k][c] - pal[i][c]);
                        if (dd < bestd) { bestd = dd; best = i; }
                    }
                    if (c0 == c1)
                        best = 0;
                    bits |= uint32_t(best) << (2 * k);
                }
                cb[0] = uint8_t(c0); cb[1] = uint8_t(c0 >> 8); cb[2] = uint8_t(c1); cb[3] = uint8_t(c1 >> 8);
                memcpy(cb + 4, &bits, 4);
                if (fmt == 0x33545844u) { // DXT3: explicit 4-bit alpha
                    for (uint32_t k = 0; k < 16; k += 2)
                        b[k / 2] = uint8_t((px[k][3] >> 4) | (px[k + 1][3] >> 4) << 4);
                } else if (block == 16) { // DXT5 (DXT2/4 treated alike): 8-step ramp
                    const uint8_t a0 = hi[3], a1 = lo[3];
                    b[0] = a0;
                    b[1] = a1;
                    uint64_t abits = 0;
                    for (uint32_t k = 0; k < 16; ++k) {
                        int idx = 0;
                        if (a0 != a1) {
                            const int t = ((a0 - px[k][3]) * 7 + (a0 - a1) / 2) / (a0 - a1); // 0..7 along a0->a1
                            idx = t <= 0 ? 0 : t >= 7 ? 1 : t + 1;
                        }
                        abits |= uint64_t(idx) << (3 * k);
                    }
                    for (int k = 0; k < 6; ++k)
                        b[2 + k] = uint8_t(abits >> (8 * k));
                }
            }
        return clear_blocks;
    }
    const uint32_t bpp = format_bytes(fmt);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            rgba_to_px(fmt, &in[(size_t(y) * w + x) * 4], dst + size_t(y) * d->pitch + size_t(x) * bpp);
    return 0;
}
// "DXT1" for a FOURCC format, the D3DFMT number otherwise.
static const char *surface_format_name(uint32_t fmt, char (&out)[12]) {
    if (fmt > 0xff && fmt >> 24 >= ' ' && (fmt >> 24) < 127)
        snprintf(out, sizeof out, "%c%c%c%c", char(fmt), char(fmt >> 8), char(fmt >> 16), char(fmt >> 24));
    else
        snprintf(out, sizeof out, "format %u", fmt);
    return out;
}
// Returns 0 (D3D_OK) or an HRESULT.
uint32_t d3d9_load_surface(uint32_t dst_view, uint32_t src_view, uint32_t color_key) {
    ComObj *d = dst_view ? com_this(dst_view) : nullptr, *s = src_view ? com_this(src_view) : nullptr;
    if (!d || !s || d->kind != K_D3D9SURFACE || s->kind != K_D3D9SURFACE)
        return D3DERR_INVALIDCALL;
    if (!rgba_format_known(d->rmask) || !rgba_format_known(s->rmask) || s->blob.empty()) {
        log_once("d3dx9.loadsurface.fmt", "d3dx9: D3DXLoadSurfaceFromSurface: format %08x -> %08x not supported",
                 s->rmask, d->rmask);
        return 0x8876086au; // D3DERR_NOTAVAILABLE
    }
    const uint32_t staged = stage_lock(d);
    if (!staged)
        return E_OUTOFMEMORY;
    uint8_t *dst = gm_ptr(staged);
    if (s->rmask == d->rmask && s->width == d->width && s->height == d->height && !color_key) {
        const uint32_t rows = dds_rows(d->rmask, d->height);
        const size_t row = std::min<size_t>(dds_row_bytes(d->rmask, d->width), std::min(s->pitch, d->pitch));
        for (uint32_t r = 0; r < rows; ++r)
            if (size_t(r) * s->pitch + row <= s->blob.size())
                memcpy(dst + size_t(r) * d->pitch, s->blob.data() + size_t(r) * s->pitch, row);
    } else {
        std::vector<uint8_t> rgba, scaled;
        surface_to_rgba(s, rgba);
        if (color_key)
            for (size_t i = 0; i < rgba.size(); i += 4) {
                const uint32_t argb = uint32_t(rgba[i + 3]) << 24 | rgba[i] << 16 | rgba[i + 1] << 8 | rgba[i + 2];
                if (argb == color_key)
                    rgba[i] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 0;
            }
        if (s->width != d->width || s->height != d->height) { // nearest neighbour
            scaled.resize(size_t(d->width) * d->height * 4);
            for (uint32_t y = 0; y < d->height; ++y)
                for (uint32_t x = 0; x < d->width; ++x) {
                    const uint32_t sx = uint32_t(uint64_t(x) * s->width / d->width),
                                   sy = uint32_t(uint64_t(y) * s->height / d->height);
                    memcpy(&scaled[(size_t(y) * d->width + x) * 4], &rgba[(size_t(sy) * s->width + sx) * 4], 4);
                }
            rgba.swap(scaled);
        }
        const uint32_t clear = rgba_to_surface(rgba, d, dst);
        // Each kind of conversion once, so a log shows which copies a run made.
        char key[96], sf[12], df[12];
        snprintf(key, sizeof key, "d3dx9.loadsurface.%ux%u.%08x.%ux%u.%08x", s->width, s->height, s->rmask,
                 d->width, d->height, d->rmask);
        log_once(key, "d3dx9: D3DXLoadSurfaceFromSurface %ux%u %s -> %ux%u %s (%u DXT1 blocks with transparent pixels)",
                 s->width, s->height, surface_format_name(s->rmask, sf), d->width, d->height,
                 surface_format_name(d->rmask, df), clear);
    }
    stage_unlock(d);
    return 0;
}
static void VolTex_LockBox(X86 *c) {
    volume_lock(c, volume_level(com_this_arg(c), arg(c, 1)), arg(c, 2), arg(c, 3), arg(c, 4));
}
static void VolTex_UnlockBox(X86 *c) {
    volume_unlock(c, volume_level(com_this_arg(c), arg(c, 1)));
}
static void Vol_LockBox(X86 *c) {
    volume_lock(c, com_this_arg(c), arg(c, 1), arg(c, 2), arg(c, 3));
}
static void Vol_UnlockBox(X86 *c) {
    volume_unlock(c, com_this_arg(c));
}
static void volume_desc(X86 *c, ComObj *v, uint32_t out) {
    if (!v || !out || !gm_valid(out, 28)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, v->rmask);
    wr32(out + 4, 2); // D3DRTYPE_VOLUME
    wr32(out + 8, resource_usage()[v->id]);
    wr32(out + 12, volumes().at(v->id).pool);
    wr32(out + 16, v->width);
    wr32(out + 20, v->height);
    wr32(out + 24, volumes().at(v->id).depth);
    com_ret(c, D3D_OK9);
}
static void VolTex_GetLevelDesc(X86 *c) {
    volume_desc(c, volume_level(com_this_arg(c), arg(c, 1)), arg(c, 2));
}
static void Vol_GetDesc(X86 *c) {
    volume_desc(c, com_this_arg(c), arg(c, 1));
}
static void VolTex_GetVolumeLevel(X86 *c) {
    ComObj *v = volume_level(com_this_arg(c), arg(c, 1));
    const uint32_t out = arg(c, 2);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    if (!v) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t view = com_view(v, IF_D3DVOLUME9);
    if (!view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    volume_addref(v);
    wr32(out, view);
    com_ret(c, D3D_OK9);
}
static void Vol_GetContainer(X86 *c) {
    ComObj *v = com_this_arg(c), *tex = v ? com_get(v->front_obj) : nullptr;
    const uint32_t iid = arg(c, 1), out = arg(c, 2);
    if (!tex || !iid || !gm_valid(iid, 16) || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    static const uint8_t unknown[16] = IID_BYTES(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
    if (!volume_base_iid(iid) && memcmp(gm_ptr(iid), IID_VolumeTexture, 16) &&
        memcmp(gm_ptr(iid), unknown, 16)) {
        com_ret(c, E_NOINTERFACE);
        return;
    }
    wr32(out, tex->identity);
    com_addref(tex);
    com_ret(c, S_OK);
}
static void Vol_GetDevice(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    Res_GetDevice(c);
}
static void VolTex_GetType(X86 *c) {
    com_ret(c, 4);
}
static void VolTex_GetLevelCount(X86 *c) {
    ComObj *o = com_this_arg(c);
    com_ret(c, o ? uint32_t(o->surfaces.size()) : 0);
}
static void VolTex_PreLoad(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (o && volumes().at(o->id).pool <= 1 && gpu_on())
        gpu_sync(o);
    com_ret(c, 0);
}
static void VolTex_SetLOD(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o || volumes().at(o->id).pool != 1) {
        com_ret(c, 0);
        return;
    }
    auto &v = volumes().at(o->id);
    const uint32_t old = v.lod;
    v.lod = std::min(arg(c, 1), uint32_t(o->surfaces.size() - 1));
    com_ret(c, old);
}
static void VolTex_GetLOD(X86 *c) {
    ComObj *o = com_this_arg(c);
    com_ret(c, o ? volumes().at(o->id).lod : 0);
}
static void VolTex_SetPriority(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o) {
        com_ret(c, 0);
        return;
    }
    auto &v = volumes().at(o->id);
    const uint32_t old = v.priority;
    v.priority = arg(c, 1);
    com_ret(c, old);
}
static void VolTex_GetPriority(X86 *c) {
    ComObj *o = com_this_arg(c);
    com_ret(c, o ? volumes().at(o->id).priority : 0);
}
static void VolTex_AddDirtyBox(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t offset;
    if (!volume_box(volume_level(o, 0), arg(c, 1), offset)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    auto it = gpu_mirrors().find(o->id);
    if (it != gpu_mirrors().end())
        std::fill(it->second.cpu_dirty.begin(), it->second.cpu_dirty.end(), 1);
    com_ret(c, D3D_OK9);
}
static void Vol_Unavailable(X86 *c) {
    com_ret(c, D3DERR_NOTAVAILABLE);
}
static void Vol_NoAutoFilter(X86 *c) {
    com_ret(c, 0);
}
// No autogeneration is advertised or admitted at creation. This void method
// cannot fabricate a mip chain; each allocated level must be uploaded by the caller.
static void Vol_NoGenerate(X86 *c) {
    com_ret(c, 0);
}
void Dev_UpdateTexture(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 2));
    if ((!src || src->kind != K_D3D9VOLUMETEXTURE) && (!dst || dst->kind != K_D3D9VOLUMETEXTURE)) {
        log_once("d3d9.UpdateTexture", "d3d9: non-volume UpdateTexture is not implemented");
        com_ret(c, D3D_OK9);
        return;
    }
    if (!this_device9(c) || !src || !dst || src->kind != K_D3D9VOLUMETEXTURE ||
        dst->kind != K_D3D9VOLUMETEXTURE || src->dev_d3d != this_device9(c)->id ||
        dst->dev_d3d != src->dev_d3d || volumes().at(src->id).pool != 2 ||
        volumes().at(dst->id).pool != 0 || src->rmask != dst->rmask || src->width != dst->width ||
        src->height != dst->height || volumes().at(src->id).depth != volumes().at(dst->id).depth ||
        src->surfaces.size() != dst->surfaces.size()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    for (uint32_t l = 0; l < src->surfaces.size(); ++l)
        if (volume_level(src, l)->lock_count || volume_level(dst, l)->lock_count) {
            com_ret(c, D3DERR_INVALIDCALL);
            return;
        }
    for (uint32_t l = 0; l < src->surfaces.size(); ++l) {
        ComObj *v = volume_level(dst, l);
        v->blob = volume_level(src, l)->blob;
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
    }
    com_ret(c, D3D_OK9);
}

// D3DQUERYTYPE: 8 is EVENT, 9 OCCLUSION. An occlusion query with a GPU
// renderer counts real samples; without one, or for any other type, the
// query is finished at once, and an occlusion count is reported as one
// sample so that nothing the game gates on visibility disappears.
static const uint32_t D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9;
static const uint32_t S_FALSE9 = 1;
static std::unordered_map<uint32_t, uint32_t> &query_state() { // id -> 1 begun, 2 ended
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static void query_destroy(ComObj *o) {
    if (query_state().erase(o->id))
        host_d9_query_drop(o->id);
}
void Query_GetType9(X86 *c) {
    ComObj *q = com_this_arg(c);
    set_eax(c, q ? q->dev_type : 0);
}
void Query_GetDataSize(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t type = q ? q->dev_type : 0;
    set_eax(c, (type == D3DQUERYTYPE_OCCLUSION || type == D3DQUERYTYPE_EVENT) ? 4 : 0);
}
// RECOMP_TRACE_QUERY=1 (diagnostic): occlusion query calls per second, every 5 s.
static void query_trace(int kind) {
    static const bool on = recomp_env("TRACE_QUERY") != nullptr;
    if (!on)
        return;
    static uint64_t n[4];
    static auto last = std::chrono::steady_clock::now();
    ++n[kind];
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - last).count();
    if (s >= 5.0) {
        LOGW("d3d9: queries per second: GetData %.0f (not ready %.0f), Issue begin %.0f end %.0f", n[0] / s,
             n[3] / s, n[1] / s, n[2] / s);
        n[0] = n[1] = n[2] = n[3] = 0;
        last = now;
    }
}
// (this, dwIssueFlags): D3DISSUE_END 1, D3DISSUE_BEGIN 2.
void Query_Issue(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t flags = arg(c, 1);
    if (q && q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        if (flags & 2) {
            query_trace(1);
            host_d9_query_begin(q->id);
            query_state()[q->id] = 1;
        } else if (flags & 1) {
            query_trace(2);
            host_d9_query_end(q->id);
            query_state()[q->id] = 2;
        }
    }
    com_ret(c, q ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// (this, pData, dwSize, dwGetDataFlags)
void Query_GetData(X86 *c) {
    query_trace(0);
    ComObj *q = com_this_arg(c);
    uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!q) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t value = 1;
    if (q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        auto it = query_state().find(q->id);
        if (it != query_state().end()) {
            uint32_t count = 0;
            int r = it->second == 2 ? host_d9_query_result(q->id, &count) : 0;
            // A count still on the GPU answers with this query's last count (visible the first time) instead of
            // S_FALSE: the GPU only finishes a frame after Present, and NFS Undercover's render thread polled
            // GetData until then, 4-5% of its time at 120 fps (2026-11-15). The light-flare test is a frame late.
            static std::unordered_map<uint32_t, uint32_t> *last = new std::unordered_map<uint32_t, uint32_t>();
            static const bool wait = recomp_env("D3D9_QUERY_WAIT") != nullptr; // the old answer, for A/B runs
            if (r == 0 && it->second == 2 && !wait) {
                auto l = last->find(q->id);
                value = l != last->end() ? l->second : 1;
            } else if (r == 0) {
                query_trace(3);
                com_ret(c, S_FALSE9); // not yet
                return;
            } else {
                value = r > 0 ? count : 1;
                (*last)[q->id] = value;
            }
        }
    }
    if (data && size >= 4)
        wr32(data, value);
    com_ret(c, D3D_OK9);
}

#define RESOURCE_HEAD                                                                              \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},                          \
        {"Release", 1, com_Release}, {"GetDevice", 2, Res_GetDevice},                              \
        {"SetPrivateData", 5, Res_SetPrivateData}, {"GetPrivateData", 4, Res_GetPrivateData},      \
        {"FreePrivateData", 2, Res_FreePrivateData}, {"SetPriority", 2, Res_SetPriority},          \
        {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad}, {                        \
        "GetType", 1, Res_GetType                                                                  \
    }

static const ComMethod g_vb9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, VB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static const ComMethod g_ib9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, IB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static void Surf_AddRef(X86 *c) {
    ComObj *s = com_this_arg(c);
    surface_addref(s);
    com_ret(c, s ? (uint32_t)(surface_container(s) ? s->refs - 1 : s->refs) : 0);
}
static void Surf_Release(X86 *c) {
    com_ret(c, (uint32_t)surface_release(com_this_arg(c)));
}
static void Surf_QueryInterface(X86 *c) {
    ComObj *s = com_this_arg(c);
    const bool first_outside = s && surface_container(s) && s->refs == 1;
    com_QueryInterface(c);
    if (first_outside && c->r[0] == S_OK)
        com_addref(com_get(s->front_obj));
}
static const ComMethod g_surface9[] = {
    {"QueryInterface", 3, Surf_QueryInterface}, {"AddRef", 1, Surf_AddRef}, {"Release", 1, Surf_Release},
    {"GetDevice", 2, Res_GetDevice}, {"SetPrivateData", 5, Res_SetPrivateData},
    {"GetPrivateData", 4, Res_GetPrivateData}, {"FreePrivateData", 2, Res_FreePrivateData},
    {"SetPriority", 2, Res_SetPriority}, {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad},
    {"GetType", 1, Res_GetType},
    {"GetContainer", 3, Surf_GetContainer},
    {"GetDesc", 2, Surf_GetDesc},
    {"LockRect", 4, Surf_LockRect},
    {"UnlockRect", 1, Surf_UnlockRect},
    {"GetDC", 2, Surf_GetDC},
    {"ReleaseDC", 2, Surf_ReleaseDC},
};

static const ComMethod g_texture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetSurfaceLevel", 3, Tex_GetSurfaceLevel},
    {"LockRect", 5, Tex_LockRect},
    {"UnlockRect", 2, Tex_UnlockRect},
    {"AddDirtyRect", 2, Tex_AddDirtyRect},
};

static const ComMethod g_volume_texture9[] = {
    {"QueryInterface", 3, VolTex_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Vol_GetDevice},
    {"SetPrivateData", 5, Vol_Unavailable},
    {"GetPrivateData", 4, Vol_Unavailable},
    {"FreePrivateData", 2, Vol_Unavailable},
    {"SetPriority", 2, VolTex_SetPriority},
    {"GetPriority", 1, VolTex_GetPriority},
    {"PreLoad", 1, VolTex_PreLoad},
    {"GetType", 1, VolTex_GetType},
    {"SetLOD", 2, VolTex_SetLOD},
    {"GetLOD", 1, VolTex_GetLOD},
    {"GetLevelCount", 1, VolTex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Vol_Unavailable},
    {"GetAutoGenFilterType", 1, Vol_NoAutoFilter},
    {"GenerateMipSubLevels", 1, Vol_NoGenerate},
    {"GetLevelDesc", 3, VolTex_GetLevelDesc},
    {"GetVolumeLevel", 3, VolTex_GetVolumeLevel},
    {"LockBox", 5, VolTex_LockBox},
    {"UnlockBox", 2, VolTex_UnlockBox},
    {"AddDirtyBox", 2, VolTex_AddDirtyBox},
};
static const ComMethod g_volume9[] = {
    {"QueryInterface", 3, Vol_QueryInterface},
    {"AddRef", 1, Vol_AddRef},
    {"Release", 1, Vol_Release},
    {"GetDevice", 2, Vol_GetDevice},
    {"SetPrivateData", 5, Vol_Unavailable},
    {"GetPrivateData", 4, Vol_Unavailable},
    {"FreePrivateData", 2, Vol_Unavailable},
    {"GetContainer", 3, Vol_GetContainer},
    {"GetDesc", 2, Vol_GetDesc},
    {"LockBox", 4, Vol_LockBox},
    {"UnlockBox", 1, Vol_UnlockBox},
};

static const ComMethod g_cubetexture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetCubeMapSurface", 4, Tex_GetCubeMapSurface},
    {"LockRect", 6, Tex_LockRectCube},
    {"UnlockRect", 3, Tex_UnlockRectCube},
    {"AddDirtyRect", 3, Tex_AddDirtyRectCube},
};

static const ComMethod g_decl9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetDeclaration", 3, Decl_GetDeclaration},
};

// IDirect3DVertexShader9 and IDirect3DPixelShader9 have the same three methods past IUnknown.
static void Shader_GetFunction(X86 *c);
static const ComMethod g_shader9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetFunction", 3, Shader_GetFunction},
};

static const ComMethod g_query9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetType", 1, Query_GetType9},
    {"GetDataSize", 1, Query_GetDataSize},
    {"Issue", 2, Query_Issue},
    {"GetData", 4, Query_GetData},
};

// ---------------------------------------------------------------------------
// The pipeline record, and the device setters that write it
// ---------------------------------------------------------------------------
D9Pipeline &d9_pipeline(uint32_t device_id) {
    // Records are never erased and map nodes do not move, so the last one
    // asked for can be handed out again without a lookup.
    static auto *records = new std::map<uint32_t, D9Pipeline>();
    static thread_local uint32_t last_id = 0;
    static thread_local D9Pipeline *last = nullptr;
    if (last && last_id == device_id)
        return *last;
    last = &(*records)[device_id];
    last_id = device_id;
    return *last;
}

// ---- Getters for state a caller saves and puts back (Direct3D 9 semantics) ----
// (this, State, pValue)
void Dev_GetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || state >= 256) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).rs[state]);
    com_ret(c, D3D_OK9);
}
// (this, Sampler, Type, pValue)
void Dev_GetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t sampler = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!dev || !out || sampler >= 16 || type >= 14) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).sampler_state[sampler][type]);
    com_ret(c, D3D_OK9);
}
// (this, RenderTargetIndex, ppRenderTarget): referenced, as D3D hands it out; NOTFOUND for an
// unset target above zero.
void Dev_GetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = com_get(index == 0 ? dev->render_target : d9_pipeline(dev->id).color_target[index]);
    if (index == 0 && !s)
        s = device_backbuffer(dev);
    if (!s) {
        wr32(out, 0);
        com_ret(c, D3DERR_NOTFOUND9);
        return;
    }
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    surface_addref(s); // a level shares its texture's lifetime (Test159)
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
// (this, ppDecl): referenced; null when none is set.
void Dev_GetVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *d = com_get(d9_pipeline(dev->id).declaration);
    uint32_t view_ = d && d->kind == K_D3D9DECL ? com_view(d, IF_D3DVERTEXDECL9) : 0;
    if (view_)
        com_addref(d);
    wr32(out, view_);
    com_ret(c, D3D_OK9);
}

// GetRenderTargetData and its LockRect, bounded: the copy is queued behind the draws that made
// the image and delivered when the GPU has run it (host_d9_texture_read_async); a
// D3DLOCK_DONOTWAIT lock before then answers D3DERR_WASSTILLDRAWING instead of waiting. A lock
// without that flag waits, through the synchronous read, as Direct3D 9 does. The destination is
// the ticket.
static std::unordered_map<uint32_t, HostD9Surface> &readbacks_pending() {
    static std::unordered_map<uint32_t, HostD9Surface> r;
    return r;
}
static void readback_forget(ComObj *o) {
    if (o && readbacks_pending().erase(o->id))
        host_d9_texture_read_forget(o->id);
}
static bool readback_settle(ComObj *s, uint32_t flags) {
    if (!s)
        return true;
    auto it = readbacks_pending().find(s->id);
    if (it == readbacks_pending().end() || s->blob.empty())
        return true;
    int r = host_d9_texture_read_poll(s->id, s->blob.data(), s->pitch, s->width, s->height);
    if (r == 0 && (flags & D3DLOCK_DONOTWAIT))
        return false;
    if (r == 0) // wait: the synchronous read of the same image
        host_d9_texture_read(it->second.id, it->second.face, it->second.level, s->blob.data(),
                             s->pitch);
    host_d9_texture_read_forget(s->id);
    readbacks_pending().erase(it);
    d9_raster_invalidate(s->id);
    return true;
}
// (this, pRenderTarget, pDestSurface)
void Dev_GetRenderTargetData(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 2));
    if (!src || !dst || src->kind != K_D3D9SURFACE || dst->kind != K_D3D9SURFACE ||
        src->width != dst->width || src->height != dst->height || dst->bpp != 32 ||
        dst->blob.empty()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        uint32_t slot;
        if (ComObj *owner = com_get(mirror_slot(src, &slot)))
            gpu_sync(owner);
        const HostD9Surface r = surface_ref(src);
        readbacks_pending()[dst->id] = r;
        host_d9_texture_read_async(r.id, r.face, r.level, dst->id);
        com_ret(c, D3D_OK9);
        return;
    }
    const uint8_t *sb = resource_bytes(src);
    uint8_t *db = resource_bytes(dst);
    if (sb && db && src->bpp == 32)
        for (uint32_t y = 0; y < dst->height; ++y)
            memcpy(db + (size_t)y * dst->pitch, sb + (size_t)y * src->pitch, (size_t)dst->width * 4);
    d9_raster_invalidate(dst->id);
    com_ret(c, D3D_OK9);
}

// (this, State, Value)
// (this, pViewport): D3DVIEWPORT9 is X, Y, Width, Height, MinZ, MaxZ.
void Dev_SetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        for (int i = 0; i < 4; ++i)
            p.viewport[i] = rd32(v + 4u * (uint32_t)i);
        uint32_t b0 = rd32(v + 16), b1 = rd32(v + 20);
        memcpy(&p.viewport_z[0], &b0, 4);
        memcpy(&p.viewport_z[1], &b1, 4);
        p.viewport_set = true;
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        ComObj *rt = com_get(dev->render_target);
        uint32_t whole[4] = {0, 0, rt ? rt->width : 640, rt ? rt->height : 480};
        for (int i = 0; i < 4; ++i)
            wr32(v + 4u * (uint32_t)i, p.viewport_set ? p.viewport[i] : whole[i]);
        float z0 = p.viewport_set ? p.viewport_z[0] : 0.0f,
              z1 = p.viewport_set ? p.viewport_z[1] : 1.0f;
        uint32_t b0, b1;
        memcpy(&b0, &z0, 4);
        memcpy(&b1, &z1, 4);
        wr32(v + 16, b0);
        wr32(v + 20, b1);
    }
    com_ret(c, D3D_OK9);
}

// (this, State, pMatrix): world, view, projection and texture transforms,
// kept for the fixed-function path.
void Dev_SetTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), m = arg(c, 2);
    if (dev && m) {
        uint32_t slot =
            state < 256 ? state : (state >= 256 && state < 260 ? state - 256 + 24 : 0xffffffffu);
        if (slot < 32)
            for (int i = 0; i < 16; ++i) {
                uint32_t b = rd32(m + 4u * (uint32_t)i);
                memcpy(&d9_pipeline(dev->id).transform[slot][i], &b, 4);
            }
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, Type, Value)
void Dev_SetTextureStageState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t stage = arg(c, 1), type = arg(c, 2);
    if (dev && stage < 8 && type < 33)
        d9_pipeline(dev->id).tss[stage][type] = arg(c, 3);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1);
    if (dev && state < 256) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.rs[state] != arg(c, 2) || !p.rs_set[state]) {
            p.rs[state] = arg(c, 2);
            p.rs_set[state] = true;
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Sampler, Type, Value)
void Dev_SetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t sampler = arg(c, 1), type = arg(c, 2);
    if (dev && sampler < 16 && type < 14) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.sampler_state[sampler][type] != arg(c, 3)) {
            p.sampler_state[sampler][type] = arg(c, 3);
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// Copies Count four-float vectors from guest memory into a register table,
// clipped to the table.
static void write_registers(float (*table)[4], uint32_t limit, uint32_t start, uint32_t data,
                            uint32_t count) {
    for (uint32_t i = 0; i < count && start + i < limit && data; ++i)
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t bits = rd32(data + (i * 4 + k) * 4);
            memcpy(&table[start + i][k], &bits, 4);
        }
}

// (this, StartRegister, pConstantData, Vector4fCount)
void Dev_SetVertexShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        D9Pipeline &pl = d9_pipeline(dev->id);
        pl.vbinder = {}; // the registers no longer hold only what an effect bound
        write_registers(pl.vconst, 256, arg(c, 1), arg(c, 2), arg(c, 3));
    }
    com_ret(c, D3D_OK9);
}
void Dev_SetPixelShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        D9Pipeline &pl = d9_pipeline(dev->id);
        pl.pbinder = {};
        write_registers(pl.pconst, 224, arg(c, 1), arg(c, 2), arg(c, 3));
        // RECOMP_D3D9_PS_NAN_CALLER=1 (diagnostic, 2026-11-24): name the guest code that sets a NaN or infinity into a
        // pixel-shader constant (return address and the next stack words), the first 16 distinct callers.
        static const bool nan_caller = getenv("RECOMP_D3D9_PS_NAN_CALLER") != nullptr;
        if (nan_caller) {
            const uint32_t start = arg(c, 1), count = arg(c, 3);
            for (uint32_t i = 0; i < count && start + i < 224; ++i) {
                bool bad = false;
                for (int k = 0; k < 4; ++k)
                    bad = bad || !std::isfinite(pl.pconst[start + i][k]);
                if (!bad)
                    continue;
                static std::mutex m;
                static std::vector<uint32_t> seen;
                const uint32_t esp = c->r[R_ESP], ret = rd32(esp);
                std::lock_guard lock(m);
                if (seen.size() < 16 && std::find(seen.begin(), seen.end(), ret) == seen.end()) {
                    seen.push_back(ret);
                    std::string st;
                    char b[16];
                    for (uint32_t k = 1; k <= 24; ++k) {
                        snprintf(b, sizeof b, " %08x", rd32(esp + 4 * k));
                        st += b;
                    }
                    LOGW("d3d9: NaN pixel constant c%u (start %u count %u) set from %08x; stack:%s", start + i, start, count,
                         ret, st.c_str());
                }
                break;
            }
        }
    }
    com_ret(c, D3D_OK9);
}

// ---- Raw shaders --------------------------------------------------------------------------------------
// CreateVertexShader / CreatePixelShader take SM 1-3 bytecode the game made itself (NFS Undercover creates
// every shader this way; Carbon only used D3DX effects, so these were D9_STUBs that returned D3D_OK without
// writing the out pointer, and Undercover's RenderThread then faulted at 0077c09f, 2026-11-13). The object keeps
// the bytecode; SetVertexShader / SetPixelShader bind it into the device's D9Pipeline exactly as an effect
// pass does (shared bytecode + code key), so the same decoder (d3d9_shader.cpp) and Metal translator
// (d3d9_msl.h) draw with it.

// The bytecode's length in bytes, through its END token: comments are skipped by their own length and, from
// SM 2.0 on, each instruction by the length in its token, so no operand or literal is read as END. 0 when no
// END is found within the guest memory that is there.
static uint32_t shader_code_length(uint32_t code) {
    if (!code || !gm_fits(code, 4))
        return 0;
    const uint32_t version = rd32(code);
    const uint32_t kind = version >> 16;
    if (kind != 0xfffe && kind != 0xffff)
        return 0;
    const bool sized = ((version >> 8) & 0xff) >= 2;
    uint32_t at = 4;
    for (uint32_t steps = 0; steps < (1u << 20); ++steps) {
        if (!gm_fits(code + at, 4))
            return 0;
        const uint32_t tok = rd32(code + at);
        if (tok == 0x0000ffffu)
            return at + 4;
        if ((tok & 0xffff) == 0xfffe) {
            at += 4 + ((tok >> 16) & 0x7fff) * 4;
            continue;
        }
        at += sized ? 4 + ((tok >> 24) & 0xf) * 4 : 4;
    }
    return 0;
}

// For D3DXGetShaderConstantTable (d3dx9.cpp), which is handed the same bytecode without its length.
uint32_t d3d9_shader_code_length(uint32_t code) {
    return shader_code_length(code);
}

// The bytecode of each live shader object, shared with every pipeline that binds it, and its code key.
struct D9RawShader {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    uint64_t key = 0;
    bool pixel = false;
};
static std::unordered_map<uint32_t, D9RawShader> &raw_shaders() {
    static auto *m = new std::unordered_map<uint32_t, D9RawShader>();
    return *m;
}
// What each device has bound, so GetVertexShader / GetPixelShader can answer (object ids, 0 for none).
static std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> &bound_shaders() {
    static auto *m = new std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>>();
    return *m;
}
static void raw_shader_destroy(ComObj *o) {
    raw_shaders().erase(o->id); // a pipeline that still binds it keeps its own reference to the bytes
}

// (this, const DWORD *pFunction, IDirect3D{Vertex,Pixel}Shader9 **ppShader)
static void create_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    const uint32_t code = arg(c, 1), out = arg(c, 2);
    const uint32_t len = shader_code_length(code);
    const uint32_t want = pixel ? 0xffffu : 0xfffeu;
    if (!dev || !out || !len || (rd32(code) >> 16) != want) {
        log_once(pixel ? "d3d9.ps.bad" : "d3d9.vs.bad", "d3d9: Create%sShader refused bytecode it cannot read",
                 pixel ? "Pixel" : "Vertex");
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = com_new(K_D3D9SHADER);
    if (!s) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    s->dev_d3d = dev->id;
    s->blob.assign(gm_ptr(code), gm_ptr(code) + len);
    D9RawShader &r = raw_shaders()[s->id];
    r.bytes = std::make_shared<const std::vector<uint8_t>>(s->blob);
    r.key = d9sh::code_key(s->blob.data(), s->blob.size());
    r.pixel = pixel;
    const uint32_t view_ = com_view(s, pixel ? IF_D3DPIXELSHADER9 : IF_D3DVERTEXSHADER9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
void Dev_CreateVertexShader(X86 *c) {
    create_shader(c, false);
}
void Dev_CreatePixelShader(X86 *c) {
    create_shader(c, true);
}

// (this, shader or NULL): NULL unbinds that stage.
static void set_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = arg(c, 1) ? com_this(arg(c, 1)) : nullptr;
    auto it = s && s->kind == K_D3D9SHADER ? raw_shaders().find(s->id) : raw_shaders().end();
    if (it != raw_shaders().end() && it->second.pixel != pixel)
        it = raw_shaders().end();
    D9Pipeline &pl = d9_pipeline(dev->id);
    D9ShaderBytes &slot = pixel ? pl.ps : pl.vs;
    uint64_t &key = pixel ? pl.ps_key : pl.vs_key;
    if (it == raw_shaders().end()) {
        slot.bytes.reset();
        key = 0;
    } else {
        slot.bytes = it->second.bytes;
        key = it->second.key;
    }
    (pixel ? pl.pbinder : pl.vbinder) = {}; // an effect's constant bind no longer describes the stage
    if (pixel) { // the decoded-program cache of the last draw belongs to the old program
        pl.ps_prog = nullptr;
        pl.ps_prog_key = 0;
    } else {
        pl.vs_prog = nullptr;
        pl.vs_prog_key = 0;
    }
    pl.label = nullptr;
    auto &b = bound_shaders()[dev->id];
    (pixel ? b.second : b.first) = it == raw_shaders().end() ? 0 : s->id;
    com_ret(c, D3D_OK9);
}
void Dev_SetVertexShader(X86 *c) {
    set_shader(c, false);
}
void Dev_SetPixelShader(X86 *c) {
    set_shader(c, true);
}

// (this, IDirect3D{Vertex,Pixel}Shader9 **): AddRef'd, NULL when none is bound.
static void get_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    auto it = bound_shaders().find(dev->id);
    ComObj *s = it == bound_shaders().end() ? nullptr : com_get(pixel ? it->second.second : it->second.first);
    if (s && s->kind == K_D3D9SHADER) {
        com_addref(s);
        com_out_ptr(out, com_view(s, pixel ? IF_D3DPIXELSHADER9 : IF_D3DVERTEXSHADER9));
    } else {
        wr32(out, 0);
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetVertexShader(X86 *c) {
    get_shader(c, false);
}
void Dev_GetPixelShader(X86 *c) {
    get_shader(c, true);
}

// IDirect3D{Vertex,Pixel}Shader9::GetFunction(this, void *pData, UINT *pSizeOfData): the bytecode, or with
// pData NULL only its size.
static void Shader_GetFunction(X86 *c) {
    ComObj *s = com_this_arg(c);
    const uint32_t data = arg(c, 1), psize = arg(c, 2);
    if (!s || s->kind != K_D3D9SHADER || !psize) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t n = (uint32_t)s->blob.size();
    if (data) {
        if (rd32(psize) < n || !gm_fits(data, n)) {
            com_ret(c, D3DERR_INVALIDCALL);
            return;
        }
        memcpy(gm_ptr(data), s->blob.data(), n);
    }
    wr32(psize, n);
    com_ret(c, D3D_OK9);
}

// BOOL is a 32-bit guest value, not a C++ bool. Validate the entire range
// before reading or writing anything so an invalid call cannot partly apply.
static void shader_booleans(X86 *c, bool pixel, bool get) {
    ComObj *dev = this_device9(c);
    const uint32_t start = arg(c, 1), data = arg(c, 2), count = arg(c, 3);
    if (!dev || start > 16 || count > 16 - start ||
        (count && (!data || !gm_fits(data, count * 4)))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    int32_t *table = pixel ? pl.pbool : pl.vbool;
    if (!get)
        (pixel ? pl.pbinder : pl.vbinder) = {};
    for (uint32_t i = 0; i < count; ++i) {
        if (get)
            wr32(data + i * 4, (uint32_t)table[start + i]);
        else
            table[start + i] = (int32_t)rd32(data + i * 4);
    }
    com_ret(c, D3D_OK9);
}
void Dev_SetVertexShaderConstantB(X86 *c) {
    shader_booleans(c, false, false);
}
void Dev_GetVertexShaderConstantB(X86 *c) {
    shader_booleans(c, false, true);
}
void Dev_SetPixelShaderConstantB(X86 *c) {
    shader_booleans(c, true, false);
}
void Dev_GetPixelShaderConstantB(X86 *c) {
    shader_booleans(c, true, true);
}

static const ComMethod g_d3d9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"RegisterSoftwareDevice", 2, D9_RegisterSoftwareDevice},
    {"GetAdapterCount", 1, D9_GetAdapterCount},
    {"GetAdapterIdentifier", 4, D9_GetAdapterIdentifier},
    {"GetAdapterModeCount", 3, D9_GetAdapterModeCount},
    {"EnumAdapterModes", 5, D9_EnumAdapterModes},
    {"GetAdapterDisplayMode", 3, D9_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, D9_CheckDeviceType},
    {"CheckDeviceFormat", 7, D9_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 7, D9_CheckDeviceMultiSampleType},
    {"CheckDepthStencilMatch", 6, D9_CheckDepthStencilMatch},
    {"CheckDeviceFormatConversion", 5, D9_CheckDeviceFormatConversion},
    {"GetDeviceCaps", 4, D9_GetDeviceCaps},
    {"GetAdapterMonitor", 2, D9_GetAdapterMonitor},
    {"CreateDevice", 7, D9_CreateDevice},
};

static const ComMethod g_device9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"TestCooperativeLevel", 1, Dev_TestCooperativeLevel},
    {"GetAvailableTextureMem", 1, Dev_GetAvailableTextureMem},
    {"EvictManagedResources", 1, Dev_EvictManagedResources},
    {"GetDirect3D", 2, Dev_GetDirect3D},
    {"GetDeviceCaps", 2, Dev_GetDeviceCaps},
    {"GetDisplayMode", 3, Dev_GetDisplayMode},
    {"GetCreationParameters", 2, Dev_GetCreationParameters},
    {"SetCursorProperties", 4, Dev_SetCursorProperties},
    {"SetCursorPosition", 4, Dev_SetCursorPosition},
    {"ShowCursor", 2, Dev_ShowCursor},
    {"CreateAdditionalSwapChain", 3, Dev_CreateAdditionalSwapChain},
    {"GetSwapChain", 3, Dev_GetSwapChain},
    {"GetNumberOfSwapChains", 1, Dev_GetNumberOfSwapChains},
    {"Reset", 2, Dev_Reset},
    {"Present", 5, Dev_Present},
    {"GetBackBuffer", 5, Dev_GetBackBuffer},
    {"GetRasterStatus", 3, Dev_GetRasterStatus},
    {"SetDialogBoxMode", 2, Dev_SetDialogBoxMode},
    {"SetGammaRamp", 4, Dev_SetGammaRamp},
    {"GetGammaRamp", 3, Dev_GetGammaRamp},
    {"CreateTexture", 9, Dev_CreateTexture},
    {"CreateVolumeTexture", 10, Dev_CreateVolumeTexture},
    {"CreateCubeTexture", 8, Dev_CreateCubeTexture},
    {"CreateVertexBuffer", 7, Dev_CreateVertexBuffer},
    {"CreateIndexBuffer", 7, Dev_CreateIndexBuffer},
    {"CreateRenderTarget", 9, Dev_CreateRenderTarget},
    {"CreateDepthStencilSurface", 9, Dev_CreateDepthStencilSurface},
    {"UpdateSurface", 5, Dev_UpdateSurface},
    {"UpdateTexture", 3, Dev_UpdateTexture},
    {"GetRenderTargetData", 3, Dev_GetRenderTargetData},
    {"GetFrontBufferData", 3, Dev_GetFrontBufferData},
    {"StretchRect", 6, Dev_StretchRect},
    {"ColorFill", 4, Dev_ColorFill},
    {"CreateOffscreenPlainSurface", 7, Dev_CreateOffscreenPlainSurface},
    {"SetRenderTarget", 3, Dev_SetRenderTarget},
    {"GetRenderTarget", 3, Dev_GetRenderTarget},
    {"SetDepthStencilSurface", 2, Dev_SetDepthStencilSurface},
    {"GetDepthStencilSurface", 2, Dev_GetDepthStencilSurface},
    {"BeginScene", 1, Dev_BeginScene},
    {"EndScene", 1, Dev_EndScene},
    {"Clear", 7, Dev_Clear},
    {"SetTransform", 3, Dev_SetTransform},
    {"GetTransform", 3, Dev_GetTransform},
    {"MultiplyTransform", 3, Dev_MultiplyTransform},
    {"SetViewport", 2, Dev_SetViewport},
    {"GetViewport", 2, Dev_GetViewport},
    {"SetMaterial", 2, Dev_SetMaterial},
    {"GetMaterial", 2, Dev_GetMaterial},
    {"SetLight", 3, Dev_SetLight},
    {"GetLight", 3, Dev_GetLight},
    {"LightEnable", 3, Dev_LightEnable},
    {"GetLightEnable", 3, Dev_GetLightEnable},
    {"SetClipPlane", 3, Dev_SetClipPlane},
    {"GetClipPlane", 3, Dev_GetClipPlane},
    {"SetRenderState", 3, Dev_SetRenderState},
    {"GetRenderState", 3, Dev_GetRenderState},
    {"CreateStateBlock", 3, Dev_CreateStateBlock},
    {"BeginStateBlock", 1, Dev_BeginStateBlock},
    {"EndStateBlock", 2, Dev_EndStateBlock},
    {"SetClipStatus", 2, Dev_SetClipStatus},
    {"GetClipStatus", 2, Dev_GetClipStatus},
    {"GetTexture", 3, Dev_GetTexture},
    {"SetTexture", 3, Dev_SetTexture},
    {"GetTextureStageState", 4, Dev_GetTextureStageState},
    {"SetTextureStageState", 4, Dev_SetTextureStageState},
    {"GetSamplerState", 4, Dev_GetSamplerState},
    {"SetSamplerState", 4, Dev_SetSamplerState},
    {"ValidateDevice", 2, Dev_ValidateDevice},
    {"SetPaletteEntries", 3, Dev_SetPaletteEntries},
    {"GetPaletteEntries", 3, Dev_GetPaletteEntries},
    {"SetCurrentTexturePalette", 2, Dev_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", 2, Dev_GetCurrentTexturePalette},
    {"SetScissorRect", 2, Dev_SetScissorRect},
    {"GetScissorRect", 2, Dev_GetScissorRect},
    {"SetSoftwareVertexProcessing", 2, Dev_SetSoftwareVertexProcessing},
    {"GetSoftwareVertexProcessing", 1, Dev_GetSoftwareVertexProcessing},
    {"SetNPatchMode", 2, Dev_SetNPatchMode},
    {"GetNPatchMode", 1, Dev_GetNPatchMode},
    {"DrawPrimitive", 4, Dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 7, Dev_DrawIndexedPrimitive},
    {"DrawPrimitiveUP", 5, Dev_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, Dev_DrawIndexedPrimitiveUP},
    {"ProcessVertices", 7, Dev_ProcessVertices},
    {"CreateVertexDeclaration", 3, Dev_CreateVertexDeclaration},
    {"SetVertexDeclaration", 2, Dev_SetVertexDeclaration},
    {"GetVertexDeclaration", 2, Dev_GetVertexDeclaration},
    {"SetFVF", 2, Dev_SetFVF},
    {"GetFVF", 2, Dev_GetFVF},
    {"CreateVertexShader", 3, Dev_CreateVertexShader},
    {"SetVertexShader", 2, Dev_SetVertexShader},
    {"GetVertexShader", 2, Dev_GetVertexShader},
    {"SetVertexShaderConstantF", 4, Dev_SetVertexShaderConstantF},
    {"GetVertexShaderConstantF", 4, Dev_GetVertexShaderConstantF},
    {"SetVertexShaderConstantI", 4, Dev_SetVertexShaderConstantI},
    {"GetVertexShaderConstantI", 4, Dev_GetVertexShaderConstantI},
    {"SetVertexShaderConstantB", 4, Dev_SetVertexShaderConstantB},
    {"GetVertexShaderConstantB", 4, Dev_GetVertexShaderConstantB},
    {"SetStreamSource", 5, Dev_SetStreamSource},
    {"GetStreamSource", 5, Dev_GetStreamSource},
    {"SetStreamSourceFreq", 3, Dev_SetStreamSourceFreq},
    {"GetStreamSourceFreq", 3, Dev_GetStreamSourceFreq},
    {"SetIndices", 2, Dev_SetIndices},
    {"GetIndices", 2, Dev_GetIndices},
    {"CreatePixelShader", 3, Dev_CreatePixelShader},
    {"SetPixelShader", 2, Dev_SetPixelShader},
    {"GetPixelShader", 2, Dev_GetPixelShader},
    {"SetPixelShaderConstantF", 4, Dev_SetPixelShaderConstantF},
    {"GetPixelShaderConstantF", 4, Dev_GetPixelShaderConstantF},
    {"SetPixelShaderConstantI", 4, Dev_SetPixelShaderConstantI},
    {"GetPixelShaderConstantI", 4, Dev_GetPixelShaderConstantI},
    {"SetPixelShaderConstantB", 4, Dev_SetPixelShaderConstantB},
    {"GetPixelShaderConstantB", 4, Dev_GetPixelShaderConstantB},
    {"DrawRectPatch", 4, Dev_DrawRectPatch},
    {"DrawTriPatch", 4, Dev_DrawTriPatch},
    {"DeletePatch", 2, Dev_DeletePatch},
    {"CreateQuery", 3, Dev_CreateQuery},
};

// The DLL's one export. Not a COM method: the guest calls it directly.
void d3d9_Direct3DCreate9(X86 *c) {
    ComObj *o = com_new(K_D3D9);
    uint32_t view = o ? com_view(o, IF_D3D9) : 0;
    if (!view) {
        if (o)
            com_release(o);
        set_eax(c, 0);
        return;
    }
    LOGW("d3d9: Direct3DCreate9(SDK %u) -> %08x", arg(c, 0), view);
    set_eax(c, view);
}

static const ImportShim g_d3d9_exports[] = {
    {"d3d9.dll", "Direct3DCreate9", 1, d3d9_Direct3DCreate9},
};

void d3d9_reset() {
    g_scratch = 0;
    g_scratch_size = 0;
}

void d3d9_register() {
    static bool done = false;
    if (done)
        return;
    done = true;

    com_define(IF_D3D9, "d3d9.dll", "IDirect3D9", g_d3d9, std::size(g_d3d9));
    com_define(IF_D3DDEVICE9, "d3d9.dll", "IDirect3DDevice9", g_device9, std::size(g_device9));
    com_bind(IF_D3D9, K_D3D9);
    com_bind(IF_D3DDEVICE9, K_D3D9DEVICE);
    com_register_iid(IF_D3D9, IID_IDirect3D9_);
    com_register_iid(IF_D3DDEVICE9, IID_IDirect3DDevice9_);
    com_define(IF_D3DTEXTURE9, "d3d9.dll", "IDirect3DTexture9", g_texture9, std::size(g_texture9));
    com_define(IF_D3DCUBETEXTURE9, "d3d9.dll", "IDirect3DCubeTexture9", g_cubetexture9,
               std::size(g_cubetexture9));
    com_define(IF_D3DSURFACE9, "d3d9.dll", "IDirect3DSurface9", g_surface9, std::size(g_surface9));
    com_define(IF_D3DVERTEXBUFFER9, "d3d9.dll", "IDirect3DVertexBuffer9", g_vb9, std::size(g_vb9));
    com_define(IF_D3DINDEXBUFFER9, "d3d9.dll", "IDirect3DIndexBuffer9", g_ib9, std::size(g_ib9));
    com_define(IF_D3DVERTEXDECL9, "d3d9.dll", "IDirect3DVertexDeclaration9", g_decl9,
               std::size(g_decl9));
    com_define(IF_D3DQUERY9, "d3d9.dll", "IDirect3DQuery9", g_query9, std::size(g_query9));
    com_bind(IF_D3DTEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DCUBETEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DSURFACE9, K_D3D9SURFACE);
    com_bind(IF_D3DVERTEXBUFFER9, K_D3D9VB);
    com_bind(IF_D3DINDEXBUFFER9, K_D3D9IB);
    com_bind(IF_D3DVERTEXDECL9, K_D3D9DECL);
    com_bind(IF_D3DQUERY9, K_D3D9QUERY);
    com_define(IF_D3DVERTEXSHADER9, "d3d9.dll", "IDirect3DVertexShader9", g_shader9, std::size(g_shader9));
    com_define(IF_D3DPIXELSHADER9, "d3d9.dll", "IDirect3DPixelShader9", g_shader9, std::size(g_shader9));
    com_bind(IF_D3DVERTEXSHADER9, K_D3D9SHADER);
    com_bind(IF_D3DPIXELSHADER9, K_D3D9SHADER);
    com_set_destructor(K_D3D9SHADER, raw_shader_destroy);
    for (ComKind k : {K_D3D9TEXTURE, K_D3D9SURFACE, K_D3D9VB, K_D3D9IB})
        com_set_destructor(k, d3d9_resource_destroy);
    com_define(IF_D3DVOLUMETEXTURE9, "d3d9.dll", "IDirect3DVolumeTexture9", g_volume_texture9,
               std::size(g_volume_texture9));
    com_define(IF_D3DVOLUME9, "d3d9.dll", "IDirect3DVolume9", g_volume9, std::size(g_volume9));
    com_bind(IF_D3DVOLUMETEXTURE9, K_D3D9VOLUMETEXTURE);
    com_bind(IF_D3DVOLUME9, K_D3D9VOLUME);
    com_register_iid(IF_D3DVOLUMETEXTURE9, IID_VolumeTexture);
    com_register_iid(IF_D3DVOLUME9, IID_Volume);
    com_set_destructor(K_D3D9TEXTURE, texture_destroy); // Test159: with its levels and faces
    com_set_destructor(K_D3D9VOLUMETEXTURE, volume_destroy);
    com_set_destructor(K_D3D9VOLUME, volume_destroy);
    com_set_destructor(K_D3D9DEVICE, device_volume_destroy);
    com_set_destructor(K_D3D9QUERY, query_destroy);
    imports_register(g_d3d9_exports, std::size(g_d3d9_exports));
}
