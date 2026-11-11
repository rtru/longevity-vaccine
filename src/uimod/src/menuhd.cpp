// menuhd.cpp -- drop-in HD opening pictures: the main menu background (xopeninga.pcx / openinga.pcx) at any size.
//
// A pack is one or more <game>\menu\<name>_<W>x<H>.pcx (e.g. xopeninga_3840x1080.pcx and xopeninga_3840x2160.pcx):
// the stock picture redrawn at any size and shape, 8-bit, in the STOCK file's palette indices (the game loads it with
// the stock remap, so another palette shows wrong colours). For each opening, the file whose shape is closest to the
// window is used.
//
// Engine (PRACX 2d0b1438 and the vanilla exe alike):
//  * change_opening 0x589A30 (cdecl, the pcx name) loads the picture into a local Buffer with
//    Buffer_load_pcx(name, 0, 10, 236) @0x589AA1 (loaded index = file index + 10, 255 -> 9; the Buffer takes the
//    FILE's size) and stretches it over the whole TopMenuWin (0x945824: +0x444 canvas, +0x4C4 width, +0x4C8 negative
//    height) with Buffer_copy_3 0x5D9610 @0x589AE8 = GDI StretchBlt(SRCCOPY).
//  * StretchBlt in the DC's default stretch mode ANDs the palette INDICES of the rows and columns it drops when it
//    shrinks: a 2048x1536 test picture on a 3840x1080 window came out covered in black / red / green dashes. And the
//    stock 4:3 picture is stretched anamorphically on a wide window.
//
// Levers, two byte-checked E8 retargets, cave-free:
//  * @0x589AA1 -> LoadHook: when menu\<name>_*.pcx exist, the one closest to the window's shape is loaded instead
//    (its palette read from the file for the resample below); else the stock call, unchanged.
//  * @0x589AE8 -> CopyHook: for a pack picture whose size is not the window's, the picture is resampled here to
//    exactly the window's size -- scaled to COVER it when that crops at most 12% (no distortion, centred crop),
//    else to FIT it (whole picture centred, black around) -- each pixel the average colour of the source pixels it
//    covers, snapped back to the stock palette with Floyd-Steinberg -- and
//    StretchBlt then copies it 1:1. Cached per picture and window size. A pack picture at the window's own size is
//    copied 1:1 as it is.
// Nothing changes without a file in menu\; [UIMod] MenuHD=0 turns it off with one.

#include "menuhd.h"

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

void UimodLog(const char* msg);

namespace {

constexpr uintptr_t kVA_LoadPcx      = 0x5D7DE0;   // thiscall (Buffer*, name, pal, a4, a5) RET 0x10; resizes self
constexpr uintptr_t kVA_Copy3        = 0x5D9610;   // thiscall (Buffer* src, dst, sx, sy, sw, sh, dx, dy, dw, dh) RET 0x24
constexpr uintptr_t kVA_BufferCtor   = 0x5D7210;
constexpr uintptr_t kVA_BufferResize = 0x5D7670;
constexpr uintptr_t kVA_TopMenuWin   = 0x945824;   // TopMenuWin*; +0x4C4 width, +0x4C8 height (negative)
constexpr uint32_t kSiteLoad = 0x589AA1;
constexpr uint32_t kSiteCopy = 0x589AE8;

constexpr int kBufData = 0x50, kBufBits = 0x54, kBufW = 0x80, kBufH = 0x84, kBufPitch = 0x4A8, kBufSize = 0x588;
constexpr int kRemapBase = 10, kRemapCount = 236, kKeyLoaded = 9;   // Buffer_load_pcx(name, 0, 10, 236)

typedef int (__thiscall* FLoadPcx)(void*, const char*, void*, int, int);
typedef int (__thiscall* FCopy3)(void*, void*, int, int, int, int, int, int, int, int);
typedef void (__thiscall* FBufferCtor)(void*);
typedef int (__thiscall* FBufferResize)(void*, int, int, int, int);

FLoadPcx g_load = nullptr;
FCopy3 g_copy = nullptr;
char g_dir[MAX_PATH];
bool g_active = false;                               // the current change_opening loaded a pack picture
char g_path[MAX_PATH];                               // ... this one
uint8_t g_rgb[256][3];                               // loaded index -> RGB, from that file's palette
alignas(16) uint8_t g_scratch[kBufSize + 0x80];
bool g_scratchReady = false;
char g_cachePath[MAX_PATH];                          // what g_scratch holds
int g_cacheW = 0, g_cacheH = 0;
std::vector<uint8_t> g_lut;                          // 5-bit RGB -> nearest loaded index (palette of g_lutPath)
char g_lutPath[MAX_PATH];

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

bool CallsTo(uint32_t site, uintptr_t target)
{
    const BYTE* p = At(site);
    if (p[0] != 0xE8) return false;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return reinterpret_cast<uintptr_t>(p) + 5 + rel == reinterpret_cast<uintptr_t>(At(target));
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

// Size and 256-colour palette of an 8-bit PCX, read from the file.
bool PcxInfo(const char* path, int* w, int* h, uint8_t pal[768])
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    uint8_t hdr[128];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && hdr[0] == 10 && hdr[3] == 8 && hdr[65] == 1;
    if (ok) {
        *w = (hdr[8] | hdr[9] << 8) - (hdr[4] | hdr[5] << 8) + 1;
        *h = (hdr[10] | hdr[11] << 8) - (hdr[6] | hdr[7] << 8) + 1;
        uint8_t marker = 0;
        ok = fseek(f, -769, SEEK_END) == 0 && fread(&marker, 1, 1, f) == 1 && marker == 0x0C &&
             fread(pal, 1, 768, f) == 768;
    }
    fclose(f);
    return ok;
}

// The window's size (change_opening draws to the whole TopMenuWin).
bool WindowSize(int* w, int* h)
{
    const uint8_t* win = *reinterpret_cast<uint8_t**>(At(kVA_TopMenuWin));
    if (!win) return false;
    *w = *reinterpret_cast<const int*>(win + 0x4C4);
    *h = -*reinterpret_cast<const int*>(win + 0x4C8);
    return *w > 0 && *h > 0;
}

// menu\<stem>_*.pcx closest in shape to the window (ties: the larger). False if none.
bool PickPicture(const char* name, int ww, int wh, char* out, size_t n)
{
    const char* base = name;
    for (const char* p = name; *p; ++p) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    char stem[64];
    strncpy_s(stem, base, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) *dot = 0;
    char pattern[MAX_PATH];
    if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%smenu\\%s_*.pcx", g_dir, stem) <= 0) return false;
    WIN32_FIND_DATAA fd;
    const HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    double best = 1e9;
    long long bestArea = 0;
    bool found = false;
    do {
        char path[MAX_PATH];
        int w = 0, ht = 0;
        uint8_t pal[768];
        if (_snprintf_s(path, sizeof path, _TRUNCATE, "%smenu\\%s", g_dir, fd.cFileName) <= 0) continue;
        if (!PcxInfo(path, &w, &ht, pal) || w <= 0 || ht <= 0) {
            Log("[menuhd] %s is not an 8-bit PCX with a palette -> ignored\n", path);
            continue;
        }
        const double d = std::fabs(std::log(static_cast<double>(w) / ht * wh / ww));
        const long long area = static_cast<long long>(w) * ht;
        if (d < best - 1e-6 || (d < best + 1e-6 && area > bestArea)) {
            best = d;
            bestArea = area;
            strncpy_s(out, n, path, _TRUNCATE);
            found = true;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

void BuildLut()
{
    if (!g_lut.empty() && strcmp(g_lutPath, g_path) == 0) return;
    g_lut.assign(32 * 32 * 32, 0);
    for (int i = 0; i < 32 * 32 * 32; ++i) {
        const int r = (i >> 10) * 255 / 31, g = (i >> 5 & 31) * 255 / 31, b = (i & 31) * 255 / 31;
        int best = kRemapBase, bestD = 1 << 30;
        for (int v = kRemapBase; v < kRemapBase + kRemapCount; ++v) {
            const int dr = r - g_rgb[v][0], dg = g - g_rgb[v][1], db = b - g_rgb[v][2];
            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (d < bestD) { bestD = d; best = v; }
        }
        g_lut[i] = static_cast<uint8_t>(best);
    }
    strncpy_s(g_lutPath, g_path, _TRUNCATE);
}

// The most a picture is cropped to fill the window (the share of its width or height cut away). A picture of a
// shape further from the window's is shown whole and centred instead, the rest black: the pictures are space
// scenes, and filling a 32:9 window with a 16:9 picture cut away its whole top and bottom (a giant logo, in game).
constexpr double kMaxCrop = 0.12;

// src (sw x sh, loaded indices) -> dst (dw x dh): scaled to cover dst (centred crop) when that crops at most kMaxCrop,
// else scaled to fit it (centred, black around); area average + Floyd-Steinberg.
void Resample(const uint8_t* src, int sw, int sh, int spitch, uint8_t* dst, int dw, int dh, int dpitch)
{
    BuildLut();
    const double fx = sw / static_cast<double>(dw), fy = sh / static_cast<double>(dh);
    const double cover = fx < fy ? fx : fy, fit = fx < fy ? fy : fx;
    const double s = 1 - cover / fit <= kMaxCrop ? cover : fit;          // source pixels per window pixel
    const double ox = (sw - dw * s) / 2, oy = (sh - dh * s) / 2;          // negative = black margin (fit)
    const uint8_t black = g_lut[0];
    std::vector<int> x0(dw + 1);
    for (int x = 0; x <= dw; ++x) {
        const double v = ox + x * s;
        x0[x] = v < 0 ? -1 : (v >= sw ? sw : static_cast<int>(v));
    }
    std::vector<float> err(2 * 3 * (dw + 2), 0.f);
    for (int ty = 0; ty < dh; ++ty) {
        const double fy0 = oy + ty * s;
        int y0 = static_cast<int>(fy0), y1 = static_cast<int>(oy + (ty + 1) * s);
        float* cur = &err[(ty & 1) * 3 * (dw + 2)];
        float* nxt = &err[((ty + 1) & 1) * 3 * (dw + 2)];
        memset(nxt, 0, 3 * (dw + 2) * sizeof(float));
        if (fy0 < 0 || y0 >= sh) {                                         // a black margin row
            memset(dst + ty * dpitch, black, dw);
            continue;
        }
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int tx = 0; tx < dw; ++tx) {
            int a = x0[tx], b = x0[tx + 1];
            if (a < 0 || a >= sw) {                                        // a black margin column
                dst[ty * dpitch + tx] = black;
                continue;
            }
            if (b <= a) b = a + 1;
            if (b > sw) b = sw;
            unsigned r = 0, g = 0, bl = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* row = src + y * spitch;
                for (int x = a; x < b; ++x) {
                    const uint8_t* c = g_rgb[row[x]];
                    r += c[0]; g += c[1]; bl += c[2]; ++n;
                }
            }
            float want[3] = { static_cast<float>(r) / n + cur[3 * (tx + 1)], static_cast<float>(g) / n + cur[3 * (tx + 1) + 1],
                              static_cast<float>(bl) / n + cur[3 * (tx + 1) + 2] };
            int q[3];
            for (int c = 0; c < 3; ++c) q[c] = want[c] < 0 ? 0 : (want[c] > 255 ? 255 : static_cast<int>(want[c] + 0.5f));
            const uint8_t v = g_lut[(q[0] >> 3) << 10 | (q[1] >> 3) << 5 | (q[2] >> 3)];
            dst[ty * dpitch + tx] = v;
            for (int c = 0; c < 3; ++c) {
                const float e = want[c] - g_rgb[v][c];
                cur[3 * (tx + 2) + c] += e * 7 / 16;
                nxt[3 * tx + c] += e * 3 / 16;
                nxt[3 * (tx + 1) + c] += e * 5 / 16;
                nxt[3 * (tx + 2) + c] += e * 1 / 16;
            }
        }
    }
}

// --- hooks ------------------------------------------------------------------------------------------------------

// Stands in for Buffer_load_pcx at change_opening's site: same contract (thiscall, RET 0x10).
int __fastcall LoadHook(void* buf, void* /*edx*/, const char* name, void* pal, int a4, int a5)
{
    g_active = false;
    int ww = 0, wh = 0;
    char path[MAX_PATH];
    if (name && a4 == kRemapBase && a5 == kRemapCount && WindowSize(&ww, &wh) &&
        PickPicture(name, ww, wh, path, sizeof path)) {
        int w = 0, h = 0;
        uint8_t filePal[768];
        if (PcxInfo(path, &w, &h, filePal)) {
            const int rc = g_load(buf, path, pal, a4, a5);
            const int bh = Field(buf, kBufH) < 0 ? -Field(buf, kBufH) : Field(buf, kBufH);
            if (Field(buf, kBufW) == w && bh == h && Pixels(buf)) {
                memset(g_rgb, 0, sizeof g_rgb);
                for (int i = 0; i < kRemapCount; ++i) memcpy(g_rgb[kRemapBase + i], filePal + 3 * i, 3);
                memcpy(g_rgb[kKeyLoaded], filePal + 3 * 255, 3);
                if (strcmp(g_path, path) != 0) Log("[menuhd] %s for a %dx%d window\n", path, ww, wh);
                strncpy_s(g_path, path, _TRUNCATE);
                g_active = true;
                return rc;
            }
            Log("[menuhd] %s did not load as %dx%d -> the stock picture\n", path, w, h);
        }
    }
    return g_load(buf, name, pal, a4, a5);
}

// Stands in for Buffer_copy_3 at change_opening's site: same contract (thiscall, RET 0x24).
int __fastcall CopyHook(void* src, void* /*edx*/, void* dst, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh)
{
    const bool active = g_active;
    g_active = false;
    if (!active || dw <= 0 || dh <= 0 || (sw == dw && sh == dh))
        return g_copy(src, dst, sx, sy, sw, sh, dx, dy, dw, dh);
    if (!g_scratchReady) {
        reinterpret_cast<FBufferCtor>(At(kVA_BufferCtor))(g_scratch);
        g_scratchReady = true;
    }
    if (g_cacheW != dw || g_cacheH != dh || strcmp(g_cachePath, g_path) != 0) {
        g_cacheW = g_cacheH = 0;
        reinterpret_cast<FBufferResize>(At(kVA_BufferResize))(g_scratch, dw, dh, 0, 0);
        uint8_t* d = Pixels(g_scratch);
        const int pitch = Field(g_scratch, kBufPitch);
        const int bh = Field(g_scratch, kBufH) < 0 ? -Field(g_scratch, kBufH) : Field(g_scratch, kBufH);
        if (!d || Field(g_scratch, kBufW) != dw || bh != dh || pitch < dw) {
            Log("[menuhd] could not size a %dx%d buffer -> stretched as stock\n", dw, dh);
            return g_copy(src, dst, sx, sy, sw, sh, dx, dy, dw, dh);
        }
        const DWORD t0 = GetTickCount();
        const int srcH = Field(src, kBufH) < 0 ? -Field(src, kBufH) : Field(src, kBufH);
        Resample(Pixels(src), Field(src, kBufW), srcH, Field(src, kBufPitch), d, dw, dh, pitch);
        g_cacheW = dw;
        g_cacheH = dh;
        strncpy_s(g_cachePath, g_path, _TRUNCATE);
        Log("[menuhd] resampled to %dx%d in %lu ms\n", dw, dh, GetTickCount() - t0);
    }
    return g_copy(g_scratch, dst, 0, 0, dw, dh, dx, dy, dw, dh);
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

void InstallMenuHd()
{
    if (!GameDir(g_dir, sizeof g_dir)) return;
    char ini[MAX_PATH];
    if (_snprintf_s(ini, sizeof ini, _TRUNCATE, "%sAlpha Centauri.ini", g_dir) > 0 &&
        GetPrivateProfileIntA("UIMod", "MenuHD", 1, ini) == 0) {
        UimodLog("[menuhd] off ([UIMod] MenuHD=0)\n");
        return;
    }
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%smenu\\*.pcx", g_dir) <= 0) return;
    const HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;           // no pack: nothing patched
    FindClose(h);

    if (!CallsTo(kSiteLoad, kVA_LoadPcx)) {
        Log("[menuhd] unexpected bytes at load call %X -> HD menu NOT installed\n", kSiteLoad);
        return;
    }
    if (!CallsTo(kSiteCopy, kVA_Copy3)) {
        Log("[menuhd] unexpected bytes at copy call %X -> HD menu NOT installed\n", kSiteCopy);
        return;
    }
    g_load = reinterpret_cast<FLoadPcx>(At(kVA_LoadPcx));
    g_copy = reinterpret_cast<FCopy3>(At(kVA_Copy3));
    Retarget(kSiteLoad, reinterpret_cast<const void*>(&LoadHook));
    Retarget(kSiteCopy, reinterpret_cast<const void*>(&CopyHook));
    Log("[menuhd] installed: opening pictures from menu\\<name>_<W>x<H>.pcx (load + copy sites)\n");
}
