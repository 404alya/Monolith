// PNG files for --shot and the request bundle images (png.c)
#ifndef MONOLITH_PNG_H
#define MONOLITH_PNG_H

#include <raylib.h>
#include <stdbool.h>

// Writes img to path as a PNG with the same pixels ExportImage writes, ~4x faster (zlib level 1
// with the Sub filter instead of stb_image_write's level 8 and per-row filter search; the files
// are also smaller). 8-bit grey, grey + alpha, RGB and RGBA images; any other format, or a path
// that doesn't end in .png, goes to ExportImage.
bool PngWrite(Image img, const char *path);

#endif
