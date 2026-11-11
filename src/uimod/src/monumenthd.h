#pragma once

// Drop-in widescreen Planetary Monument backdrops: <game>\monument\monzoom1/2_<W>x<H>.pcx (near view) and
// farNN_<W>x<H>.pcx (the pyramid stages), in the stock palette, the screen area index 255 like the stock art. The
// closest shape is painted over the whole Monument canvas, lined up on the screen the stock backdrop was drawn at,
// so the side bars show the scene.
// Byte-checked, cave-free, both exes; [UIMod] MonumentHD=0 turns it off. See monumenthd.cpp.
void InstallMonumentHd();
