---
name: project-smac-menu-hd
description: Drop-in HD opening pictures (the main menu background) — why a bigger xopeninga.pcx alone breaks (StretchBlt ANDs dropped rows), and the closest-shape pick + cover resample in menuhd.cpp
metadata:
  type: project
---
**What:** `<game>\menu\<name>_<W>x<H>.pcx` (e.g. `xopeninga_3840x1080.pcx`, `xopeninga_3840x2160.pcx`), any size, in
the stock picture's palette. Per opening, the file closest to the window's shape is used, scaled to cover the window
(no distortion, centred crop). Module: `src/uimod/src/menuhd.cpp`. Nothing is patched without a file in `menu\`;
`[UIMod] MenuHD=0` turns it off. Same family as [[project-smac-terrain-hd]] and [[project-smac-base-hd]].

**Engine facts** (PRACX 2d0b1438; the same bytes in the vanilla exe):
- `change_opening` 0x589A30 loads the picture into a local Buffer with `Buffer_load_pcx(name, 0, 10, 236)` @0x589AA1
  (the Buffer takes the FILE's size) and draws it over the whole TopMenuWin (0x945824: +0x444 canvas, +0x4C4 width,
  +0x4C8 negative height) with `Buffer_copy_3` 0x5D9610 @0x589AE8 = GDI StretchBlt(SRCCOPY). So a bigger stock-named
  file is picked up with no code at all...
- ...but StretchBlt in the DC's default stretch mode ANDs the palette INDICES of the rows/columns it drops when it
  SHRINKS: a 2048x1536 test picture on a 3840x1080 window was covered in black/red/green dashes (the stock 1024x768
  only ever grows, so it never showed). And a 4:3 picture is stretched anamorphically on a wide window.

**Lever:** two byte-checked E8 retargets, cave-free. @0x589AA1 -> the closest-shape `menu\<stem>_*.pcx` is loaded
instead (its palette read from the file). @0x589AE8 -> a pack picture not at the window's size is resampled to
exactly the window's size here: scaled to cover the window when that crops at most 12%, else to fit it (whole picture
centred, black around: cover-filling a 32:9 window with a 16:9 picture cut away its top and bottom and gave a giant
logo in game), each pixel the average colour of the source pixels it covers, back to the
stock palette (loaded 10..245) through a 5-bit RGB LUT with Floyd-Steinberg; StretchBlt then copies it 1:1. Cached
per picture and window size. A picture at the window's own size goes straight through.

**Verified in game** (PRACX + Thinker, 3840x1080, 2026-11-11): the 32:9 picture 1:1; the 16:9 one alone (forcing
the resample path, 78 ms), first cover (giant logo -> the 12% rule) then fit (whole, centred, black sides). Not done:
openinga.pcx (SMAC), tests.
