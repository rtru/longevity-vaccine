// basehd.cpp -- drop-in HD base sprites (GitHub issue #1, after the terrain: faction cities at the zoomed-in map).
//
// A pack is <game>\bases\<faction>_x8.pcx: the base block of <faction>.pcx -- its top-left 404x456 pixels, 6 rows
// (land, land defence 1/2, water, water defence 1/2) x 4 sizes of 100x75 sprites on a 101x76 grid -- redrawn at 8x
// (3232x3648), 8-bit, in the faction file's own palette indices.
//
// Engine (PRACX 2d0b1438 and the vanilla exe alike):
//  * load_faction_art 0x453710 loads "<faction>.pcx" (name built at 0x9B86A0) into the loading canvas 0x798668 with
//    Buffer_load_pcx(name, 0, 10, 236) and cuts the 24 base sprites with ONE Sprite_extract_2 0x5E39A0 call
//    (@0x453836, 100x75, key 9) into g_faction_sprites 0x78E978 (+faction*0x65C, index water*12 + size*3 + defence).
//  * base_draw 0x55AF20 sets SCALE_NUM_G / SCALE_DEN_G (0x696D18 / 0x696D1C) to the map zoom ((size+16)/16, or
//    g_map_zoom_num/den) and draws the base with ONE Sprite_draw_dest 0x5E5833 call (@0x55B32B) through the
//    pointer table 0x76E590, top-left at (x, y). Sprite_draw_dest scales width, height and the sprite's crop offsets
//    (+0x20 / +0x24) by num/den with nearest-neighbour SKIPPING (run table 0x9B3AD8): shrinking a big sprite by a
//    non-integer factor drops whole rows and columns -- in game, an 8x sprite drawn at 1.5x looked jagged.
//
// So the HD art is never shrunk by the engine. For each scale actually drawn, each base sprite is made at exactly its
// on-screen size (100*num/den x 75*num/den) from the 8x master: every pixel takes the most frequent index of the master
// pixels it covers (DownscaleModeTo -- never blends, so the palette and the shading bands stay exact, and the key
// wins where the master is mostly see-through), cut through the engine's own Sprite_extract_2, and drawn 1:1
// (SCALE_NUM_G = SCALE_DEN_G for that one draw), top-left at the same (x, y). Each base sprite keeps its last 4 sizes.
//
// Levers, both byte-checked E8 retargets, cave-free:
//  * @0x453836 -> ExtractRecorder: records each base sprite, its rect and its faction file. The stock sprites are
//    never changed; everything else that reads them (sizes, other screens) keeps reading stock. Sprites cut before
//    this module was installed are placed by their slot in g_faction_sprites at their first draw (Adopt).
//  * @0x55B32B -> DrawHook: a recorded sprite whose faction has a pack is drawn from its exact-size HD copy.
// Nothing changes without a pack on disk; [UIMod] BaseHD=0 turns it off with one.

#include "basehd.h"

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

void UimodLog(const char* msg);

namespace {

constexpr uintptr_t kVA_Extract2       = 0x5E39A0;   // thiscall (Sprite*, Buffer* src, key, x, y, w, h, a8) RET 0x1C
constexpr uintptr_t kVA_DrawDest       = 0x5E5833;   // thiscall (Sprite*, Buffer* dst, key, x, y, table) RET 0x14
constexpr uintptr_t kVA_SpriteClose    = 0x5E3820;   // thiscall (Sprite*): frees +0x04 when +0x28 == 0
constexpr uintptr_t kVA_BufferCtor     = 0x5D7210;
constexpr uintptr_t kVA_BufferDtor     = 0x5D7410;
constexpr uintptr_t kVA_BufferResize   = 0x5D7670;
constexpr uintptr_t kVA_LoadPcx        = 0x5D7DE0;
constexpr uintptr_t kVA_FactionPcxName = 0x9B86A0;   // "<faction>.pcx", built by load_faction_art before the load
constexpr uintptr_t kVA_FactionSprites = 0x78E978;   // g_faction_sprites: 8 x 0x65C, 37 Sprites of 0x2C each
constexpr uintptr_t kVA_FactionFile    = 0x946A54;   // MFactions[f].filename (stem), stride 0x59C (load_faction_art)
constexpr uintptr_t kVA_ScaleNum       = 0x696D18;   // SCALE_NUM_G
constexpr uintptr_t kVA_ScaleDen       = 0x696D1C;   // SCALE_DEN_G
constexpr uint32_t kSiteExtract = 0x453836;
constexpr uint32_t kSiteDraw    = 0x55B32B;
constexpr int kFactionStride = 0x65C, kFactionFileStride = 0x59C, kSpriteSize = 0x2C;

constexpr int kBufData = 0x50, kBufBits = 0x54, kBufW = 0x80, kBufH = 0x84, kBufPitch = 0x4A8, kBufSize = 0x588;
constexpr int kBlockW = 404, kBlockH = 456;          // the base block of a faction pcx (6 rows x 4 sizes)
constexpr int kMaster = 8;
constexpr int kKey = 9;                               // load_faction_art's key (file 255 -> loaded 9)
constexpr int kSlots = 4;                             // sizes kept per base sprite

struct Sprite { uint8_t raw[0x2C]; };                 // +0x04 data, +0x10/+0x14 w/h, +0x28 owner flag

struct Slot { int w, h; unsigned used; Sprite spr; };

struct Entry {
    void* stock;
    char name[40];                                    // the faction file it was cut from, e.g. "univ.pcx"
    int x, y, w, h;                                   // stock rect in the faction pcx
    Slot slots[kSlots];
};

struct Pack {
    char name[40];
    int state;                                        // 0 = not tried, 1 = master loaded, -1 = no / bad file
    uint8_t* master;                                  // a Buffer (kBufSize) holding <faction>_x8.pcx
};

typedef int (__thiscall* FExtract2)(void*, void*, int, int, int, int, int, void*);
typedef int (__thiscall* FDraw)(void*, void*, int, int, int, void*);
typedef void (__thiscall* FSpriteClose)(void*);
typedef void (__thiscall* FBufferCtor)(void*);
typedef int (__thiscall* FBufferResize)(void*, int, int, int, int);
typedef int (__thiscall* FLoadPcx)(void*, const char*, void*, int, int);
typedef void (__thiscall* FBufferDtor)(void*);

FExtract2 g_extract = nullptr;
FDraw g_draw = nullptr;
std::vector<Entry*> g_entries;
std::vector<Pack*> g_packs;
alignas(16) uint8_t g_scratch[kBufSize + 0x80];
bool g_scratchReady = false;
unsigned g_clock = 0;
char g_dir[MAX_PATH];

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

// Shrink (or enlarge) an index image to any size: each destination pixel takes the most frequent index of the source
// pixels its footprint covers; a tie goes to the index met first (row-major). Never blends. dst is dw x dh.
void DownscaleModeTo(const uint8_t* src, int sw, int sh, int spitch, uint8_t* dst, int dw, int dh, int dpitch)
{
    unsigned count[256] = {};
    uint8_t seen[256];
    for (int ty = 0; ty < dh; ++ty) {
        const int y0 = ty * sh / dh, y1 = (ty + 1) * sh / dh > y0 ? (ty + 1) * sh / dh : y0 + 1;
        for (int tx = 0; tx < dw; ++tx) {
            const int x0 = tx * sw / dw, x1 = (tx + 1) * sw / dw > x0 ? (tx + 1) * sw / dw : x0 + 1;
            int nseen = 0, best = 0;
            unsigned bestCount = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* row = src + y * spitch;
                for (int x = x0; x < x1; ++x) if (count[row[x]]++ == 0) seen[nseen++] = row[x];
            }
            for (int i = 0; i < nseen; ++i) if (count[seen[i]] > bestCount) { bestCount = count[seen[i]]; best = seen[i]; }
            for (int i = 0; i < nseen; ++i) count[seen[i]] = 0;
            dst[ty * dpitch + tx] = static_cast<uint8_t>(best);
        }
    }
}

// "univ.pcx" -> "<game>\bases\univ_x8.pcx"
bool PackPath(const char* name, char* out, size_t n)
{
    char stem[40];
    strncpy_s(stem, name, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) *dot = 0;
    return _snprintf_s(out, n, _TRUNCATE, "%sbases\\%s_x%d.pcx", g_dir, stem, kMaster) > 0;
}

bool PcxSize(const char* path, int* w, int* h)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    uint8_t hdr[128];
    const bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr;
    fclose(f);
    if (!ok || hdr[0] != 10 || hdr[3] != 8 || hdr[65] != 1) return false;   // 8-bit, one plane
    *w = (hdr[8] | hdr[9] << 8) - (hdr[4] | hdr[5] << 8) + 1;
    *h = (hdr[10] | hdr[11] << 8) - (hdr[6] | hdr[7] << 8) + 1;
    return true;
}

Pack* PackFor(const char* name)
{
    for (Pack* p : g_packs) if (_stricmp(p->name, name) == 0) return p;
    Pack* p = new Pack{};
    strncpy_s(p->name, name, _TRUNCATE);
    g_packs.push_back(p);
    return p;
}

// Load the faction's 8x master once; it stays, as every new size is made from it.
bool LoadPack(Pack* pack)
{
    char path[MAX_PATH];
    int w = 0, h = 0;
    pack->state = -1;
    if (!PackPath(pack->name, path, sizeof path) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;
    if (!PcxSize(path, &w, &h) || w != kBlockW * kMaster || h != kBlockH * kMaster) {
        Log("[basehd] %s is %dx%d, not an 8-bit %dx%d PCX -> ignored\n", path, w, h, kBlockW * kMaster, kBlockH * kMaster);
        return false;
    }
    const DWORD t0 = GetTickCount();
    pack->master = static_cast<uint8_t*>(_aligned_malloc(kBufSize + 0x80, 16));
    if (!pack->master) return false;
    memset(pack->master, 0, kBufSize + 0x80);
    reinterpret_cast<FBufferCtor>(At(kVA_BufferCtor))(pack->master);
    reinterpret_cast<FLoadPcx>(At(kVA_LoadPcx))(pack->master, path, nullptr, 10, 0xEC);   // load_faction_art's arguments
    const int bh = Field(pack->master, kBufH);
    if (Field(pack->master, kBufW) != kBlockW * kMaster || (bh != kBlockH * kMaster && bh != -kBlockH * kMaster) ||
        !Pixels(pack->master)) {
        Log("[basehd] %s did not load as %dx%d -> HD bases off for %s\n", path, kBlockW * kMaster, kBlockH * kMaster,
            pack->name);
        reinterpret_cast<FBufferDtor>(At(kVA_BufferDtor))(pack->master);
        _aligned_free(pack->master);
        pack->master = nullptr;
        return false;
    }
    pack->state = 1;
    Log("[basehd] loaded %s in %lu ms\n", path, GetTickCount() - t0);
    return true;
}

void Release(Entry* e)
{
    for (Slot& s : e->slots) {
        if (*reinterpret_cast<void**>(s.spr.raw + 4)) reinterpret_cast<FSpriteClose>(At(kVA_SpriteClose))(&s.spr);
        s = Slot{};
    }
}

// The sprite at w x h from the master, cut by the engine into the least recently used slot. Null if it could not be.
Sprite* SpriteAt(Entry* e, Pack* pack, int w, int h)
{
    for (Slot& s : e->slots) if (s.w == w && s.h == h) { s.used = ++g_clock; return &s.spr; }
    Slot* s = &e->slots[0];
    for (Slot& it : e->slots) if (it.used < s->used) s = &it;
    if (!g_scratchReady) {
        reinterpret_cast<FBufferCtor>(At(kVA_BufferCtor))(g_scratch);
        g_scratchReady = true;
    }
    reinterpret_cast<FBufferResize>(At(kVA_BufferResize))(g_scratch, w, h, 0, 0);
    uint8_t* d = Pixels(g_scratch);
    const int pitch = Field(g_scratch, kBufPitch);
    if (!d || Field(g_scratch, kBufW) != w || pitch < w) return nullptr;
    const int mpitch = Field(pack->master, kBufPitch);
    DownscaleModeTo(Pixels(pack->master) + e->y * kMaster * mpitch + e->x * kMaster, e->w * kMaster, e->h * kMaster,
                    mpitch, d, w, h, pitch);
    s->w = s->h = 0;
    if (g_extract(&s->spr, g_scratch, kKey, 0, 0, w, h, nullptr) != 0) return nullptr;
    s->w = w;
    s->h = h;
    s->used = ++g_clock;
    return &s->spr;
}

// --- hooks ------------------------------------------------------------------------------------------------------

// Stands in for Sprite_extract_2 at load_faction_art's base-sprite site: same contract (thiscall, RET 0x1C).
int __fastcall ExtractRecorder(void* spr, void* /*edx*/, void* src, int key, int x, int y, int w, int h, void* a8)
{
    const int rc = g_extract(spr, src, key, x, y, w, h, a8);
    if (rc != 0 || x < 0 || y < 0 || x + w > kBlockW || y + h > kBlockH) return rc;
    const char* name = reinterpret_cast<const char*>(At(kVA_FactionPcxName));
    Entry* e = nullptr;
    for (Entry* it : g_entries) if (it->stock == spr) { e = it; break; }
    if (!e) {
        e = new Entry{};
        e->stock = spr;
        g_entries.push_back(e);
    }
    if (_stricmp(e->name, name) != 0 || e->x != x || e->y != y || e->w != w || e->h != h) {
        Release(e);                                    // another faction in this slot (a new game)
        strncpy_s(e->name, name, _TRUNCATE);
        e->x = x; e->y = y; e->w = w; e->h = h;
    }
    return rc;
}

// A base sprite load_faction_art cut BEFORE this module was installed (the first game_init can run before uimod's
// Init): placed by its slot in g_faction_sprites -- load_faction_art's own layout (index water*12 + size*3 + defence,
// rect (1 + size*101, 1 + row*76, 100, 75)) -- and its faction's file name from the table load_faction_art reads.
Entry* Adopt(void* spr)
{
    const uintptr_t lo = reinterpret_cast<uintptr_t>(At(kVA_FactionSprites));
    const uintptr_t p = reinterpret_cast<uintptr_t>(spr);
    if (p < lo || p >= lo + 8 * kFactionStride) return nullptr;
    const int f = static_cast<int>((p - lo) / kFactionStride), off = static_cast<int>((p - lo) % kFactionStride);
    const int idx = off / kSpriteSize;
    if (off % kSpriteSize != 0 || idx >= 24) return nullptr;
    const int water = idx / 12, size = idx % 12 / 3, def = idx % 3;
    const char* stem = reinterpret_cast<const char*>(At(kVA_FactionFile + f * kFactionFileStride));
    if (!stem[0]) return nullptr;
    Entry* e = new Entry{};
    e->stock = spr;
    _snprintf_s(e->name, sizeof e->name, _TRUNCATE, "%.30s.pcx", stem);
    e->x = 1 + size * 101;
    e->y = 1 + (water * 3 + def) * 76;
    e->w = 100;
    e->h = 75;
    g_entries.push_back(e);
    return e;
}

// Stands in for Sprite_draw_dest at base_draw's site: same contract (thiscall, RET 0x14).
int __fastcall DrawHook(void* spr, void* /*edx*/, void* dst, int key, int x, int y, void* table)
{
    Entry* e = nullptr;
    for (Entry* it : g_entries) if (it->stock == spr) { e = it; break; }
    if (!e) e = Adopt(spr);
    if (e) {
        Pack* pack = PackFor(e->name);
        if (pack->state == 0) LoadPack(pack);
        int32_t* num = reinterpret_cast<int32_t*>(At(kVA_ScaleNum));
        int32_t* den = reinterpret_cast<int32_t*>(At(kVA_ScaleDen));
        const int32_t n = *num, d = *den;
        if (pack->state == 1 && n > 0 && d > 0) {
            const int w = static_cast<int>((static_cast<long long>(e->w) * n + d / 2) / d);
            const int h = static_cast<int>((static_cast<long long>(e->h) * n + d / 2) / d);
            if (w >= 1 && h >= 1 && w <= e->w * kMaster && h <= e->h * kMaster) {
                if (Sprite* hd = SpriteAt(e, pack, w, h)) {
                    *num = *den = 1;                   // 1:1 -- the sprite is already at its on-screen size
                    const int rc = g_draw(hd, dst, key, x, y, table);
                    *num = n;
                    *den = d;
                    return rc;
                }
            }
        }
    }
    return g_draw(spr, dst, key, x, y, table);
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

void InstallBaseHd()
{
    if (!GameDir(g_dir, sizeof g_dir)) return;
    char ini[MAX_PATH];
    if (_snprintf_s(ini, sizeof ini, _TRUNCATE, "%sAlpha Centauri.ini", g_dir) > 0 &&
        GetPrivateProfileIntA("UIMod", "BaseHD", 1, ini) == 0) {
        UimodLog("[basehd] off ([UIMod] BaseHD=0)\n");
        return;
    }
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%sbases\\*_x%d.pcx", g_dir, kMaster) <= 0) return;
    const HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;             // no pack: nothing patched
    FindClose(h);

    if (!CallsTo(kSiteExtract, kVA_Extract2)) {
        Log("[basehd] unexpected bytes at extract call %X -> HD bases NOT installed\n", kSiteExtract);
        return;
    }
    if (!CallsTo(kSiteDraw, kVA_DrawDest)) {
        Log("[basehd] unexpected bytes at draw call %X -> HD bases NOT installed\n", kSiteDraw);
        return;
    }
    g_extract = reinterpret_cast<FExtract2>(At(kVA_Extract2));
    g_draw = reinterpret_cast<FDraw>(At(kVA_DrawDest));
    g_entries.reserve(8 * 24);
    Retarget(kSiteExtract, reinterpret_cast<const void*>(&ExtractRecorder));
    Retarget(kSiteDraw, reinterpret_cast<const void*>(&DrawHook));
    Log("[basehd] installed: base sprites from bases\\*_x%d.pcx (extract + draw sites)\n", kMaster);
}
