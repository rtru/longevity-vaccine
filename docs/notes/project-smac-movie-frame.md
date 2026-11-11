---
name: project-smac-movie-frame
description: Movie frame — the cinematics play inside a picture (the Planetary Monument's screen) near 2x instead of being blown up to fill the screen; magenta marks the screen, closest-shape pick, WIC decode, movieframe.cpp
metadata:
  type: project
---
**What:** `<game>\movies\frame\*.png` (or .jpg / .bmp), any size, true colour. The picture closest to the screen's
shape is scaled to cover the screen; its pure magenta (255,0,255) area is the "screen" the movie plays in, uniform
scale, as large as fits, centred; the rest of that area is black glass. No file = black bars as before;
`[UIMod] MovieFrame=0` turns it off. Module: `src/uimod/src/movieframe.cpp`, three lines in `movie.cpp`
(paint the frame instead of zeroing the back buffer, `MovieFrameFit` instead of `FitRect`).

**Why:** the clips are tiny (37 project clips 400x192, 22 faction/ending clips 320x240, merchantExchange 400x208,
openingx 592x288 — read from the HD pack's mp4 headers / 4); fitted to a 4K or 32:9 screen they are blown up 4-5x,
and the AI-upscaled pack only hides that. A frame keeps them near 2x and gives the screen something to show. The
first frame is the Monument's near view (monzoom1) redrawn widescreen, its screen sized so both main clip shapes land
at ~2.1x at 1080 px height (screen ~47% of the height, 5:3 — the original monument screen's shape).

**Details:** the post-movie caption needs nothing: it is laid out on vanilla's 640x480 surface centred on the clip
and scaled by dst width / native width, so it follows the movie into the frame. The movie's rectangle is fitted to
the PLAYED stream's shape, so the HD mp4s land on the same rectangle as the .wve, only sharper. The frame is scaled
with WIC (Fant) when it is not the screen's size; magenta blended into the bezel by that scale is cleared in a 4 px
band around the screen. COM is initialised apartment-threaded for the load only (undone if this call did it).

**Verified in game** (PRACX + Thinker, 3840x1080, 2026-11-11): opening movie on the 32:9 frame, screen 852x511,
frame load 235 ms; "this is how it should be" (rtru). Not done: a frame on the pristine exe's DirectDraw path (same
back buffer, untested), tests.
