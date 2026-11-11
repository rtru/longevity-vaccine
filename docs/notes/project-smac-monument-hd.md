---
name: project-smac-monument-hd
description: Planetary Monument across the whole screen — wide near/far backdrops painted over the mod's 4:3 resample, aligned via the resampler's own state; the failed canvas read-back, the floored-scale edge gap, the far stage = milestone count
metadata:
  type: project
---
**What:** `<game>\monument\monzoom1_<W>x<H>.pcx`, `monzoom2_<W>x<H>.pcx` (near view, before / from 5 milestones) and
`farNN_<W>x<H>.pcx`, NN = 00..12 (far view, one per monument.flc stage), any size, stock monzoom palette (= the
FLC's), the screen where the milestone picture goes painted index 255. Closest shape is used. Module
`src/uimod/src/monumenthd.cpp`; `[UIMod] MonumentHD=0` turns it off; nothing patched without files.
Builds on [[project-smac-monument-screen]] (mon_scale): the 4:3 layout, text, icons and buttons stay the mod's.

**Engine facts:** load_monicons 0x472230 advances monument.flc once per milestone ([0x7FD634], 14 counted as 13,
loop @0x4723FB) and loads monzoom1/2 @0x47244B (Buffer_load_pcx(…, 10, 236)). MonumentWin_draw draws the near
backdrop @0x4737E9 and the far one @0x4760E2 — Buffer_copy_4 in the vanilla exe, mon_scale's resamplers
0xA37470 / 0xA373C0 in the patched one. They fill only the centred 4:3 area; the rest of the full-screen canvas keeps
whatever was there (black at first, then the OTHER view's art: in game the far view showed the near scene at its sides).

**Lever:** three E8 retargets (load + both draws), cave-free. After the original draw, the pack picture is painted
over the whole canvas except the screen rectangle, scaled so its screen lands where the backdrop's screen went:
x' = off + ((x*num*2 + 15) >> 5) from the resampler's state (num 0xA39380, on 0xA39384, off 0xA39388/0xA3938C).
Lessons: (1) reading the canvas back for the screen failed — the resampled screen is not one flat index; use the
resampler's law. (2) num is floored (22/16 at 1080 px), so art made for 22.5/16 falls ~60 px short of the edges:
clamping smeared the last column into streaks — mirror past the edge. Mode filter, no blending.

**Verified in game** (PRACX + Thinker, 3840x1080, 2026-11-11): near + far, both views, edges clean; "perfect" (rtru).
