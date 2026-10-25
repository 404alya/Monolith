// PNG writer for shots and request bundles. raylib's ExportImage (stb_image_write) spent ~200 ms
// on a 1600 x 900 shot, 85 % of it in stb's deflate at level 8 trying every row filter; zlib's
// level 1 on Sub-filtered rows takes ~45 ms and gives smaller files (measured in
// .claude/skills/benchmarking/measurements.md). zlib is assimp's copy, linked in already.
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void Put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

// One chunk: length, type, data, CRC of type + data
static bool WriteChunk(FILE *f, const char *type, const unsigned char *data, unsigned long size)
{
    unsigned char head[8], tail[4];
    Put32(head, size);
    memcpy(head + 4, type, 4);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, head + 4, 4);
    if (size > 0) crc = crc32(crc, data, (uInt)size);
    Put32(tail, crc);
    return fwrite(head, 1, 8, f) == 8 && (size == 0 || fwrite(data, 1, size, f) == size) && fwrite(tail, 1, 4, f) == 4;
}

bool PngWrite(Image img, const char *path)
{
    int channels = img.format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE ? 1 : img.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA ? 2 :
                   img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8 ? 3 : img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 ? 4 : 0;
    if (channels == 0 || !img.data || img.width <= 0 || img.height <= 0 || !IsFileExtension(path, ".png"))
        return ExportImage(img, path);
    static const unsigned char COLOR_TYPE[5] = { 0, 0, 4, 2, 6 };
    size_t stride = (size_t)img.width * channels, rawSize = (stride + 1) * (size_t)img.height;
    unsigned char *raw = malloc(rawSize);
    uLongf zSize = compressBound((uLong)rawSize);
    unsigned char *z = raw ? malloc(zSize) : NULL;
    bool ok = z != NULL && rawSize <= 0xFFFFFFFFu;
    // Filter Sub (1): each byte minus the same channel of the pixel to its left
    for (int y = 0; ok && y < img.height; y++) {
        const unsigned char *src = (const unsigned char *)img.data + (size_t)y * stride;
        unsigned char *row = raw + (size_t)y * (stride + 1);
        row[0] = 1;
        memcpy(row + 1, src, (size_t)channels);
        for (size_t x = (size_t)channels; x < stride; x++) row[1 + x] = (unsigned char)(src[x] - src[x - channels]);
    }
    ok = ok && compress2(z, &zSize, raw, (uLong)rawSize, 1) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const unsigned char SIGNATURE[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
        unsigned char ihdr[13] = { 0 };
        Put32(ihdr, (unsigned long)img.width);
        Put32(ihdr + 4, (unsigned long)img.height);
        ihdr[8] = 8;                        // bits per channel
        ihdr[9] = COLOR_TYPE[channels];     // compression, filter method, interlace: 0
        ok = fwrite(SIGNATURE, 1, 8, f) == 8 && WriteChunk(f, "IHDR", ihdr, 13) && WriteChunk(f, "IDAT", z, zSize) &&
             WriteChunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0 && ok;
    } else ok = false;
    free(z);
    free(raw);
    if (!ok) TraceLog(LOG_WARNING, "monolith: cannot write the PNG %s", path);
    return ok;
}
