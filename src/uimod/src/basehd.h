#pragma once

// Drop-in HD base sprites: <game>\bases\<faction>_x8.pcx (the base block of <faction>.pcx at 8x, in the faction file's
// palette; 4x / 2x are cut down from it) replaces the 24 base icons of that faction at every zoom.
// Byte-checked, cave-free, both exes; [UIMod] BaseHD=0 turns it off. See basehd.cpp.
void InstallBaseHd();
