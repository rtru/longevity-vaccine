// monumenthd.cpp -- the Planetary Monument across the whole screen instead of a 4:3 box between black bars.
//
// A pack, in <game>\monument\:
//  * monzoom1_<W>x<H>.pcx, monzoom2_<W>x<H>.pcx -- the near view (one milestone large) before / from 5 milestones;
//  * farNN_<W>x<H>.pcx, NN = 00..12 -- the far view (the pyramid), one per stage of monument.flc.
// Any size and shape (one 32:9 file covers every screen up to 32:9: it is lined up on the screen, a narrower canvas
// shows its middle), 8-bit in the stock monzoom palette (= monument.flc's), the screen where the game draws the
// milestone picture painted index 255. The file closest to the canvas's shape is used.
//
// Engine (PRACX 2d0b1438 and the vanilla exe alike; see [[project-smac-monument-screen]]):
//  * load_monicons 0x472230 decodes monument.flc into MonumentWin+0x20FC, advancing it once per milestone
//    ([0x7FD634], 14 counted as 13: the loop @0x4723FB), and loads monzoom1.pcx (fewer than 5 milestones) or
//    monzoom2.pcx into +0x2684 with Buffer_load_pcx(name, 0, 10, 236) @0x47244B (file index 255 -> loaded 9).
//  * MonumentWin_draw 0x4736E0 draws the near backdrop into the canvas (+0x444) @0x4737E9 and the far one @0x4760E2:
//    Buffer_copy_4 1:1 in the vanilla exe; in the patched exe the mod's mon_scale bake calls its Lanczos resamplers
//    there (0xA37470 / 0xA373C0), which put the 1024x768 backdrop in a centred 4:3 area of a canvas the size of the
//    whole screen (the rest stays as it was: black at first, then whatever the other view left there). The pictures,
//    text, icons and buttons are all drawn after it at that same 4:3 layout.
//
// Levers, three byte-checked E8 retargets, cave-free:
//  * @0x47244B -> LoadHook: remembers which monzoom was loaded (then the stock call, unchanged).
//  * @0x4737E9 / @0x4760E2 -> NearHook / FarHook: the original call first (the 4:3 layout stays the mod's), then the
//    pack picture is painted over the whole canvas, scaled so its screen lands exactly on the rectangle the backdrop's
//    screen went to -- from the resampler's own state and law (scale numerator 0xA39380, 4:3 offset
//    0xA39388/0xA3938C, x' = off + ((x*num*2 + 15) >> 5)), or 1:1 at (dx, dy) in the vanilla exe. (Reading the canvas
//    back for it failed in game: the resampled screen is not one flat index.) Each pixel is the most frequent index of
//    the picture pixels it covers (no blending, the palette stays exact); past the picture's edge it is mirrored (the
//    game's scale is floored, e.g. 22/16 at 1080 px, so a picture made for 22.5/16 falls ~60 px short: clamping
//    smeared the last column into streaks, in game). The screen rectangle itself is left as the game drew it.
//    Cached per view.
// Nothing changes without a file in monument\; [UIMod] MonumentHD=0 turns it off.

#include "monumenthd.h"

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

void UimodLog(const char* msg);

namespace {

constexpr uintptr_t kVA_LoadPcx    = 0x5D7DE0;   // thiscall (Buffer*, name, pal, a4, a5) RET 0x10
constexpr uintptr_t kVA_Copy4      = 0x5DFF00;   // thiscall (src, dst, sx, sy, dx, dy, w, h) RET 0x1C (vanilla sites)
constexpr uintptr_t kVA_ResampNear = 0xA37470;   // the patched exe's mon_scale resamplers at the same sites, same contract
constexpr uintptr_t kVA_ResampFar  = 0xA373C0;
constexpr uintptr_t kVA_BufferCtor = 0x5D7210;
constexpr uintptr_t kVA_Milestones = 0x7FD634;   // what load_monicons advances monument.flc by (14 counted as 13)
constexpr uint32_t kSiteLoad = 0x47244B;
constexpr uint32_t kSiteNear = 0x4737E9;
constexpr uint32_t kSiteFar  = 0x4760E2;
// The mon_scale resampler's state (patched exe): on/off, scale numerator (/16), the 4:3 area's offset on the canvas.
constexpr uintptr_t kVA_MonNum = 0xA39380, kVA_MonOn = 0xA39384, kVA_MonOffX = 0xA39388, kVA_MonOffY = 0xA3938C;
constexpr RECT kNearScreen{ 213, 202, 813, 562 };   // where the game draws the milestone picture (1024x768 space)
constexpr RECT kFarScreen{ 432, 287, 591, 380 };
constexpr int kFarFrames = 13;

constexpr int kBufData = 0x50, kBufBits = 0x54, kBufW = 0x80, kBufH = 0x84, kBufPitch = 0x4A8, kBufSize = 0x588;
constexpr int kScreenLoaded = 9;                 // file index 255 after Buffer_load_pcx(…, 10, 236)

typedef int (__thiscall* FLoadPcx)(void*, const char*, void*, int, int);
typedef int (__thiscall* FCopy)(void*, void*, int, int, int, int, int, int);
typedef void (__thiscall* FBufferCtor)(void*);

FLoadPcx g_load = nullptr;
FCopy g_near = nullptr, g_far = nullptr;         // what the draw sites called before us
bool g_patched = false;
char g_dir[MAX_PATH];
char g_stem[32];                                 // "monzoom1" / "monzoom2": the last near backdrop loaded

struct Art {
    char path[MAX_PATH];
    alignas(16) uint8_t buf[kBufSize + 0x80];
    bool loaded = false;
    RECT screen{};
};
std::vector<Art*> g_arts;

struct Cache { char path[MAX_PATH]; int w = 0, h = 0; RECT screen{}; std::vector<uint8_t> px; };
Cache g_nearCache, g_farCache;

BYTE* At(uintptr_t va) { return reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr)) + (va - 0x400000); }

void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    UimodLog(buf);
}

bool CallTarget(uint32_t site, uintptr_t* target)
{
    const BYTE* p = At(site);
    if (p[0] != 0xE8) return false;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    *target = reinterpret_cast<uintptr_t>(p) + 5 + rel;
    return true;
}

void Retarget(uint32_t site, const void* fn)
{
    BYTE* p = At(site);
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - (reinterpret_cast<uintptr_t>(p) + 5));
    DWORD old;
    VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

uint8_t* Pixels(void* buf)
{
    uint8_t* d = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(buf) + kBufData);
    return d ? d : *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(buf) + kBufBits);
}

int Field(void* buf, int off) { return *reinterpret_cast<int*>(static_cast<uint8_t*>(buf) + off); }
int Height(void* buf) { const int h = Field(buf, kBufH); return h < 0 ? -h : h; }

// The bounding box of `v` in the whole image. False if there is none or it is too small to be a screen.
bool BoxOf(const uint8_t* px, int w, int h, int pitch, uint8_t v, RECT* r)
{
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = px + y * pitch;
        for (int x = 0; x < w; ++x) {
            if (row[x] != v) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 - x0 < 16 || y1 - y0 < 16) return false;
    *r = RECT{ x0, y0, x1 + 1, y1 + 1 };
    return true;
}

bool PcxSize(const char* path, int* w, int* h)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    uint8_t hdr[128];
    const bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && hdr[0] == 10 && hdr[3] == 8 && hdr[65] == 1;
    fclose(f);
    if (!ok) return false;
    *w = (hdr[8] | hdr[9] << 8) - (hdr[4] | hdr[5] << 8) + 1;
    *h = (hdr[10] | hdr[11] << 8) - (hdr[6] | hdr[7] << 8) + 1;
    return true;
}

// monument\<stem>_*.pcx closest in shape to w:h (ties: the larger).
bool Pick(const char* stem, int w, int h, char* out, size_t n)
{
    char pattern[MAX_PATH];
    if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%smonument\\%s_*.pcx", g_dir, stem) <= 0) return false;
    WIN32_FIND_DATAA fd;
    const HANDLE hf = FindFirstFileA(pattern, &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    double best = 1e9;
    long long bestArea = 0;
    bool found = false;
    do {
        char path[MAX_PATH];
        int iw = 0, ih = 0;
        if (_snprintf_s(path, sizeof path, _TRUNCATE, "%smonument\\%s", g_dir, fd.cFileName) <= 0) continue;
        if (!PcxSize(path, &iw, &ih) || iw <= 0 || ih <= 0) continue;
        const double d = std::fabs(std::log(static_cast<double>(iw) / ih * h / w));
        const long long area = static_cast<long long>(iw) * ih;
        if (d < best - 1e-6 || (d < best + 1e-6 && area > bestArea)) {
            best = d;
            bestArea = area;
            strncpy_s(out, n, path, _TRUNCATE);
            found = true;
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    return found;
}

Art* LoadArt(const char* path)
{
    for (Art* a : g_arts) if (strcmp(a->path, path) == 0) return a->loaded ? a : nullptr;
    Art* a = new Art{};
    strncpy_s(a->path, path, _TRUNCATE);
    g_arts.push_back(a);
    reinterpret_cast<FBufferCtor>(At(kVA_BufferCtor))(a->buf);
    g_load(a->buf, path, nullptr, 10, 0xEC);         // load_monicons' own arguments: the same index remap
    int w = 0, h = 0;
    PcxSize(path, &w, &h);
    if (Field(a->buf, kBufW) != w || Height(a->buf) != h || !Pixels(a->buf) ||
        !BoxOf(Pixels(a->buf), w, h, Field(a->buf, kBufPitch), kScreenLoaded, &a->screen)) {
        Log("[monumenthd] %s did not load as %dx%d with a screen (index 255) -> ignored\n", path, w, h);
        return nullptr;
    }
    a->loaded = true;
    Log("[monumenthd] loaded %s, screen %ldx%ld at (%ld,%ld)\n", path, a->screen.right - a->screen.left,
        a->screen.bottom - a->screen.top, a->screen.left, a->screen.top);
    return a;
}

// Mirror an out-of-range coordinate back into 0..n-1.
int Reflect(int v, int n)
{
    if (n <= 1) return 0;
    while (v < 0 || v >= n) v = v < 0 ? -v - 1 : 2 * n - v - 1;
    return v;
}

// The picture over the whole canvas, its screen on `rc`.
void Paint(const Art* a, const RECT& rc, int w, int h, std::vector<uint8_t>& out)
{
    void* buf = const_cast<uint8_t*>(a->buf);
    const uint8_t* src = Pixels(buf);
    const int sw = Field(buf, kBufW), sh = Height(buf), sp = Field(buf, kBufPitch);
    const double fx = static_cast<double>(a->screen.right - a->screen.left) / (rc.right - rc.left);   // src px per dst px
    const double fy = static_cast<double>(a->screen.bottom - a->screen.top) / (rc.bottom - rc.top);
    const int spanX = fx < 1 ? 1 : static_cast<int>(fx + 0.5), spanY = fy < 1 ? 1 : static_cast<int>(fy + 0.5);
    std::vector<int> col(w);
    for (int x = 0; x < w; ++x) col[x] = static_cast<int>(std::floor(a->screen.left + (x - rc.left) * fx));
    out.assign(static_cast<size_t>(w) * h, 0);
    unsigned count[256] = {};
    uint8_t seen[256];
    for (int ty = 0; ty < h; ++ty) {
        const int row0 = static_cast<int>(std::floor(a->screen.top + (ty - rc.top) * fy));
        for (int tx = 0; tx < w; ++tx) {
            int nseen = 0, best = 0;
            unsigned bestCount = 0;
            for (int dy = 0; dy < spanY; ++dy) {
                const uint8_t* line = src + Reflect(row0 + dy, sh) * sp;
                for (int dx = 0; dx < spanX; ++dx) {
                    const uint8_t v = line[Reflect(col[tx] + dx, sw)];
                    if (count[v]++ == 0) seen[nseen++] = v;
                }
            }
            for (int i = 0; i < nseen; ++i) if (count[seen[i]] > bestCount) { bestCount = count[seen[i]]; best = seen[i]; }
            for (int i = 0; i < nseen; ++i) count[seen[i]] = 0;
            out[static_cast<size_t>(ty) * w + tx] = static_cast<uint8_t>(best);
        }
    }
}

// After the original draw: the pack picture `stem` over the whole canvas, lined up on `native`'s screen.
void Overpaint(Cache& cache, const char* stem, const RECT& native, void* dst, int sx, int sy, int dx, int dy)
{
    if (!dst || !stem[0]) return;
    uint8_t* px = Pixels(dst);
    const int cw = Field(dst, kBufW), ch = Height(dst), pitch = Field(dst, kBufPitch);
    char path[MAX_PATH];
    if (!px || cw <= 0 || ch <= 0 || !Pick(stem, cw, ch, path, sizeof path)) return;
    // Where the backdrop's screen went: the resampler's own law (it is off when [0xA39384] is 0), or 1:1 at (dx, dy).
    int num = 16, offX = 0, offY = 0;
    if (g_patched && *reinterpret_cast<int*>(At(kVA_MonOn)) != 0) {
        num = *reinterpret_cast<int*>(At(kVA_MonNum));
        offX = *reinterpret_cast<int*>(At(kVA_MonOffX));
        offY = *reinterpret_cast<int*>(At(kVA_MonOffY));
    }
    auto map = [&](int v, int off) { return off + ((v * num * 2 + 15) >> 5); };
    const RECT rc{ map(dx + native.left - sx, offX), map(dy + native.top - sy, offY),
                   map(dx + native.right - sx, offX), map(dy + native.bottom - sy, offY) };
    if (num <= 0 || rc.left < 0 || rc.top < 0 || rc.right > cw || rc.bottom > ch || rc.right - rc.left < 16) {
        Log("[monumenthd] the backdrop's screen maps outside the %dx%d canvas (num %d) -> stock backdrop\n", cw, ch, num);
        return;
    }
    if (cache.w != cw || cache.h != ch || memcmp(&cache.screen, &rc, sizeof rc) != 0 || strcmp(cache.path, path) != 0) {
        const Art* a = LoadArt(path);
        if (!a) return;
        const DWORD t0 = GetTickCount();
        Paint(a, rc, cw, ch, cache.px);
        cache.w = cw;
        cache.h = ch;
        cache.screen = rc;
        strncpy_s(cache.path, path, _TRUNCATE);
        Log("[monumenthd] %s over the %dx%d canvas, screen at (%ld,%ld)-(%ld,%ld), %lu ms\n", path, cw, ch,
            rc.left, rc.top, rc.right, rc.bottom, GetTickCount() - t0);
    }
    // Everything but the screen itself, which stays as the game drew it (the milestone picture goes there next).
    for (int y = 0; y < ch; ++y) {
        uint8_t* row = px + y * pitch;
        const uint8_t* art = &cache.px[static_cast<size_t>(y) * cw];
        if (y < rc.top || y >= rc.bottom) {
            memcpy(row, art, cw);
        } else {
            memcpy(row, art, rc.left);
            memcpy(row + rc.right, art + rc.right, cw - rc.right);
        }
    }
}

// --- hooks ------------------------------------------------------------------------------------------------------

// Stands in for Buffer_load_pcx at load_monicons' monzoom site: same contract (thiscall, RET 0x10).
int __fastcall LoadHook(void* buf, void* /*edx*/, const char* name, void* pal, int a4, int a5)
{
    g_stem[0] = 0;
    if (name) {
        strncpy_s(g_stem, name, _TRUNCATE);
        if (char* dot = strrchr(g_stem, '.')) *dot = 0;
    }
    return g_load(buf, name, pal, a4, a5);
}

// Stand in for the near / far backdrop draws in MonumentWin_draw: same contract (thiscall, RET 0x1C).
int __fastcall NearHook(void* src, void* /*edx*/, void* dst, int sx, int sy, int dx, int dy, int w, int h)
{
    const int rc = g_near(src, dst, sx, sy, dx, dy, w, h);
    Overpaint(g_nearCache, g_stem, kNearScreen, dst, sx, sy, dx, dy);
    return rc;
}

int __fastcall FarHook(void* src, void* /*edx*/, void* dst, int sx, int sy, int dx, int dy, int w, int h)
{
    const int rc = g_far(src, dst, sx, sy, dx, dy, w, h);
    // The stage on show: load_monicons advanced monument.flc once per milestone (14 counted as 13).
    int count = *reinterpret_cast<int*>(At(kVA_Milestones));
    if (count == 14) count = 13;
    int frame = count - 1;
    frame = frame < 0 ? 0 : (frame >= kFarFrames ? kFarFrames - 1 : frame);
    char stem[16];
    _snprintf_s(stem, sizeof stem, _TRUNCATE, "far%02d", frame);
    Overpaint(g_farCache, stem, kFarScreen, dst, sx, sy, dx, dy);
    return rc;
}

bool GameDir(char* out, size_t n)
{
    const DWORD len = GetModuleFileNameA(nullptr, out, static_cast<DWORD>(n));
    if (len == 0 || len >= n) return false;
    char* slash = strrchr(out, '\\');
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

} // namespace

void InstallMonumentHd()
{
    if (!GameDir(g_dir, sizeof g_dir)) return;
    char ini[MAX_PATH];
    if (_snprintf_s(ini, sizeof ini, _TRUNCATE, "%sAlpha Centauri.ini", g_dir) > 0 &&
        GetPrivateProfileIntA("UIMod", "MonumentHD", 1, ini) == 0) {
        UimodLog("[monumenthd] off ([UIMod] MonumentHD=0)\n");
        return;
    }
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%smonument\\*.pcx", g_dir) <= 0) return;
    const HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;           // no pack: nothing patched
    FindClose(h);

    const uintptr_t copy4 = reinterpret_cast<uintptr_t>(At(kVA_Copy4));
    uintptr_t loadT = 0, nearT = 0, farT = 0;
    if (!CallTarget(kSiteLoad, &loadT) || loadT != reinterpret_cast<uintptr_t>(At(kVA_LoadPcx))) {
        Log("[monumenthd] unexpected bytes at load call %X -> HD Monument NOT installed\n", kSiteLoad);
        return;
    }
    if (!CallTarget(kSiteNear, &nearT) || (nearT != copy4 && nearT != reinterpret_cast<uintptr_t>(At(kVA_ResampNear))) ||
        !CallTarget(kSiteFar, &farT) || (farT != copy4 && farT != reinterpret_cast<uintptr_t>(At(kVA_ResampFar)))) {
        Log("[monumenthd] unexpected bytes at a backdrop draw (%X / %X) -> HD Monument NOT installed\n", kSiteNear, kSiteFar);
        return;
    }
    g_load = reinterpret_cast<FLoadPcx>(At(kVA_LoadPcx));
    g_near = reinterpret_cast<FCopy>(nearT);
    g_far = reinterpret_cast<FCopy>(farT);
    g_patched = nearT != copy4;
    Retarget(kSiteLoad, reinterpret_cast<const void*>(&LoadHook));
    Retarget(kSiteNear, reinterpret_cast<const void*>(&NearHook));
    Retarget(kSiteFar, reinterpret_cast<const void*>(&FarHook));
    Log("[monumenthd] installed: near + far backdrops from monument\\ (%s)\n",
        g_patched ? "over the mod's 4:3 resample" : "vanilla copy");
}
