// Movie frame: a picture around the cinematics instead of black bars (movieframe.cpp).
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

struct MovieFrame {
    std::vector<uint32_t> px;   // BGRA, w x h, top-down: the frame scaled to the screen, its screen area black
    int  w = 0, h = 0;
    RECT screen{};              // the screen area inside it (where the movie plays)
};

// <game>\movies\frame\*.png (or .jpg / .bmp): the one closest to the screen's shape, scaled to cover w x h. Its
// screen is the pure magenta (255, 0, 255) area of the picture. False -- no frame, black bars as before -- when there
// is no such file, it can't be decoded, it has no magenta area, or [UIMod] MovieFrame=0.
bool LoadMovieFrame(int w, int h, MovieFrame* out);

// The movie's place inside the frame's screen: uniform scale, as large as fits, centred.
RECT MovieFrameFit(const MovieFrame& f, int clipW, int clipH);
