#pragma once

// Drop-in HD opening pictures (the main menu background): <game>\menu\<name>_<W>x<H>.pcx, e.g. xopeninga_3840x1080.pcx,
// in the stock file's palette. The one whose shape is closest to the window is used, scaled to cover the window
// without distortion. Byte-checked, cave-free, both exes; [UIMod] MenuHD=0 turns it off. See menuhd.cpp.
void InstallMenuHd();
