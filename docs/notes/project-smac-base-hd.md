---
name: project-smac-base-hd
description: Drop-in HD base sprites (issue #1, after the terrain) — the one extract and one draw site, why the engine's own shrink can't be used, and the exact-size cut from the 8x master in basehd.cpp
metadata:
  type: project
---
**What:** `<game>\bases\<faction>_x8.pcx` (e.g. `univ_x8.pcx`) = the base block of `<faction>.pcx` — its top-left
404x456 pixels: 6 rows (land, land defence 1/2, water, water defence 1/2) x 4 sizes of 100x75 sprites on a 101x76
grid — at 8x (3232x3648), 8-bit, in the faction file's own palette indices. Replaces that faction's base icons on
the map at every zoom. Module: `src/uimod/src/basehd.cpp`. Nothing is patched without a file in `bases\`;
`[UIMod] BaseHD=0` turns it off. Builds on [[project-smac-terrain-hd]] (same Buffer calls, same load arguments).

**Engine facts** (PRACX 2d0b1438; the same bytes in the vanilla exe):
- `load_faction_art` 0x453710 builds `"<faction>.pcx"` at 0x9B86A0 (stem from MFactions+0 = 0x946A54 + f*0x59C,
  fallback `faction.pcx`), loads it into the loading canvas 0x798668 with `Buffer_load_pcx(name, 0, 10, 236)`
  (loaded index = file index + 10, 255 -> 9), and cuts all 24 base sprites with ONE `Sprite_extract_2` 0x5E39A0
  call @0x453836 (100x75, key 9) into `g_faction_sprites` 0x78E978 + f*0x65C, index water*12 + size*3 + defence.
- `base_draw` 0x55AF20 sets SCALE_NUM_G/DEN_G (0x696D18/1C) to (size+16)/16 (or g_map_zoom_num/den) and draws the
  base with ONE `Sprite_draw_dest` 0x5E5833 call @0x55B32B through the pointer table 0x76E590, top-left at (x, y).
  The sprite's size and crop offsets (+0x20/+0x24) are scaled by num/den inside the draw; base_draw itself reads no
  sprite dimension, so swapping the sprite object at that one call moves nothing else.
- `Sprite_draw_dest` scales by nearest-neighbour SKIPPING (run table 0x9B3AD8, 800 entries, one per destination
  pixel when shrinking; scales > 4.0 go to the patched exe's 0xA103A0). Tried first: HD levels 2/4/8 drawn at
  num/(den*k) — fully zoomed in it looked great, but at the mid zooms the non-integer shrink dropped whole rows and
  columns and the base looked jagged.
- `Sprite_extract_2` frees the sprite's old pixels itself (when +0x28 == 0), so a zeroed 0x2C object can be cut
  into again and again.

**Lever:** two byte-checked E8 retargets, cave-free. @0x453836 -> recorder (sprite, rect, faction file). @0x55B32B
-> for a recorded sprite whose faction has a pack: make the sprite at exactly its on-screen size
(100*num/den x 75*num/den) from the 8x master — each pixel = the most frequent index of the master pixels it covers
(never blends: palette and shading stay exact, the key wins where the master is mostly see-through) — cut it with the
engine's Sprite_extract_2 and draw it 1:1 (num = den for that one draw), same (x, y). Each base sprite keeps its last
4 sizes; the master stays loaded (~12 MB per faction with a pack). Sprites cut before uimod's Init are placed at their
first draw by their slot in g_faction_sprites (the same timing gap terrainhd met).

**Verified in game** (PRACX + Thinker, 2026-11-11): University land bases from a hand-made pack, clean at every zoom;
load 16 ms. Not yet: water/walled art (they show the stock sprites enlarged from the pack), other factions, tests.
