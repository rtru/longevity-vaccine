// Movie frame: the cinematics play inside a picture instead of filling the screen with black bars.
//
// The movies are small (400x192 project clips, 320x240 faction clips, 592x288 opening); fitted to a modern
// screen they are blown up 4-5x. A frame picture -- e.g. the Planetary Monument's near view with the movie on its
// screen -- keeps them near 2x and gives the rest of the screen something to show. The picture marks its screen with
// pure magenta; the movie is fitted into that area (movie.cpp), and the post-movie caption follows it (its layout is
// relative to the movie's rectangle).
//
// Files: <game>\movies\frame\*.png / *.jpg / *.bmp, any size. The one closest to the screen's shape is used, scaled
// to cover the screen (centred crop) with WIC's high-quality scaler. True colour: the movie player composes 32-bit,
// so the frame is not bound to the game's palette. [UIMod] MovieFrame=0 turns it off.

#include "movieframe.h"

#include <wincodec.h>
#include <cmath>
#include <cstdio>
#include <cstring>

void UimodLog(const char* msg);

namespace {

bool FrameEnabled(const char* dir)
{
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%sAlpha Centauri.ini", dir);
    return GetPrivateProfileIntA("UIMod", "MovieFrame", 1, ini) != 0;
}

void GameDir(char* out, size_t n)
{
    GetModuleFileNameA(nullptr, out, static_cast<DWORD>(n));
    char* slash = strrchr(out, '\\');
    if (slash) slash[1] = '\0';
}

struct ComInit {
    bool undo = false;
    ComInit() { const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); undo = SUCCEEDED(hr); }
    ~ComInit() { if (undo) CoUninitialize(); }
};

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// The image file's size (frame 0), or false.
bool ImageSize(IWICImagingFactory* f, const wchar_t* path, UINT* w, UINT* h)
{
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> fr;
    return SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
           SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(w, h)) && *w > 0 && *h > 0;
}

// movies\frame\* closest to w:h in shape (ties: the larger).
bool PickFrame(IWICImagingFactory* f, const char* dir, int w, int h, wchar_t* out, size_t n)
{
    wchar_t wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);
    bool found = false;
    double best = 1e9;
    unsigned long long bestArea = 0;
    for (const wchar_t* ext : { L"png", L"jpg", L"bmp" }) {
        wchar_t pattern[MAX_PATH];
        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%lsmovies\\frame\\*.%ls", wdir, ext);
        WIN32_FIND_DATAW fd;
        const HANDLE hf = FindFirstFileW(pattern, &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t path[MAX_PATH];
            UINT iw = 0, ih = 0;
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmovies\\frame\\%ls", wdir, fd.cFileName);
            if (!ImageSize(f, path, &iw, &ih)) continue;
            const double d = std::fabs(std::log(static_cast<double>(iw) / ih * h / w));
            const unsigned long long area = static_cast<unsigned long long>(iw) * ih;
            if (d < best - 1e-6 || (d < best + 1e-6 && area > bestArea)) {
                best = d;
                bestArea = area;
                wcsncpy_s(out, n, path, _TRUNCATE);
                found = true;
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    return found;
}

// The magenta screen: the bounding box of the pure-magenta pixels (which are painted black). False if none.
bool FindScreen(std::vector<uint32_t>& px, int w, int h, RECT* r)
{
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        uint32_t* row = &px[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
            const uint32_t c = row[x];                  // BGRA in memory: 0xAARRGGBB
            const int R = c >> 16 & 0xFF, G = c >> 8 & 0xFF, B = c & 0xFF;
            if (R >= 200 && B >= 200 && G <= 60) {
                row[x] = 0xFF000000;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0 || y1 < y0 || (x1 - x0) < 16 || (y1 - y0) < 16) return false;
    *r = RECT{ x0, y0, x1 + 1, y1 + 1 };
    // A scaled picture blends the magenta into the bezel: a few pixels around the screen that are still magenta-
    // tinted (red and blue well above green) go black too, so no pink line rims the screen.
    constexpr int kBand = 4;
    for (int y = (y0 - kBand < 0 ? 0 : y0 - kBand); y <= y1 + kBand && y < h; ++y) {
        uint32_t* row = &px[static_cast<size_t>(y) * w];
        for (int x = (x0 - kBand < 0 ? 0 : x0 - kBand); x <= x1 + kBand && x < w; ++x) {
            const int R = row[x] >> 16 & 0xFF, G = row[x] >> 8 & 0xFF, B = row[x] & 0xFF;
            if (R - G > 40 && B - G > 40) row[x] = 0xFF000000;
        }
    }
    return true;
}

} // namespace

bool LoadMovieFrame(int w, int h, MovieFrame* out)
{
    char dir[MAX_PATH];
    GameDir(dir, sizeof dir);
    if (w <= 0 || h <= 0 || !FrameEnabled(dir)) return false;
    ComInit com;
    Com<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    wchar_t path[MAX_PATH];
    if (!PickFrame(f.p, dir, w, h, path, MAX_PATH)) return false;

    const DWORD t0 = GetTickCount();
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> fr;
    Com<IWICFormatConverter> conv;
    Com<IWICBitmapScaler> sc;
    UINT iw = 0, ih = 0;
    if (FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(fr->GetSize(&iw, &ih)) ||
        FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) {
        UimodLog("[uimod] movie frame: could not decode the picture -> black bars\n");
        return false;
    }
    // Cover the screen: scale so both sides reach it, then copy the centred w x h out of the scaled picture.
    const double s = static_cast<double>(w) / iw > static_cast<double>(h) / ih ? static_cast<double>(w) / iw
                                                                                : static_cast<double>(h) / ih;
    const UINT sw = static_cast<UINT>(iw * s + 0.5) < static_cast<UINT>(w) ? w : static_cast<UINT>(iw * s + 0.5);
    const UINT sh = static_cast<UINT>(ih * s + 0.5) < static_cast<UINT>(h) ? h : static_cast<UINT>(ih * s + 0.5);
    // Nearest-neighbour keeps the magenta edge exact; the frame pictures are made at screen sizes, so any scale is mild.
    if (FAILED(f->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(conv.p, sw, sh, (sw == iw && sh == ih) ? WICBitmapInterpolationModeNearestNeighbor
                                                                       : WICBitmapInterpolationModeFant))) {
        UimodLog("[uimod] movie frame: could not scale the picture -> black bars\n");
        return false;
    }
    out->w = w;
    out->h = h;
    out->px.assign(static_cast<size_t>(w) * h, 0);
    const WICRect crop{ static_cast<INT>((sw - w) / 2), static_cast<INT>((sh - h) / 2), w, h };
    if (FAILED(sc->CopyPixels(&crop, static_cast<UINT>(w) * 4, static_cast<UINT>(out->px.size() * 4),
                              reinterpret_cast<BYTE*>(out->px.data())))) {
        UimodLog("[uimod] movie frame: could not read the picture -> black bars\n");
        return false;
    }
    if (!FindScreen(out->px, w, h, &out->screen)) {
        UimodLog("[uimod] movie frame: the picture has no magenta (255,0,255) screen area -> black bars\n");
        return false;
    }
    char line[MAX_PATH + 128];
    _snprintf_s(line, sizeof line, _TRUNCATE, "[uimod] movie frame: %ls at %dx%d, screen %ldx%ld at (%ld,%ld), %lu ms\n",
                path, w, h, out->screen.right - out->screen.left, out->screen.bottom - out->screen.top,
                out->screen.left, out->screen.top, GetTickCount() - t0);
    UimodLog(line);
    return true;
}

RECT MovieFrameFit(const MovieFrame& f, int clipW, int clipH)
{
    const int sw = f.screen.right - f.screen.left, sh = f.screen.bottom - f.screen.top;
    double s = static_cast<double>(sw) / clipW;
    if (static_cast<double>(sh) / clipH < s) s = static_cast<double>(sh) / clipH;
    const int dw = static_cast<int>(clipW * s), dh = static_cast<int>(clipH * s);
    const int dx = f.screen.left + (sw - dw) / 2, dy = f.screen.top + (sh - dh) / 2;
    return RECT{ dx, dy, dx + dw, dy + dh };
}
