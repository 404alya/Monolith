/* r3d_image.c -- Common R3D Image Functions
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#include "./r3d_image.h"

#include <r3d_config.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <rlgl.h>
#include <glad.h>

#include "../modules/r3d_driver.h"
#include "../common/r3d_helper.h"

// ========================================
// INTERNAL DECLARATIONS
// ========================================

static void image_blit_copy(
    Image* dst, const Image* src,
    int clipX0, int clipY0, int outW, int outH,
    int srcX, int srcY,
    int bpp, int dstStride, int srcStride);

static void image_blit_convert(
    Image* dst, const Image* src,
    int clipX0, int clipY0, int outW, int outH,
    int srcX, int srcY,
    int bppDst, int bppSrc, int dstStride, int srcStride);

static void image_blit_linear(
    Image* dst, const Image* src,
    r3d_rect_t dstRect, r3d_rect_t srcRect,
    int clipX0, int clipY0, int outW, int outH,
    int bppDst, int bppSrc, int dstStride, int srcStride);

static void get_texture_format(int format, bool isColor, GLenum* glInternalFormat, GLenum* glFormat, GLenum* glType);
static void upload_texture_mipmap(const uint8_t *data, int width, int height, int level, int format, bool isColor);
static void set_texture_swizzle(int format);
static void set_texture_wrap(TextureWrap wrap);
static void set_texture_filter(TextureFilter filter);

// ========================================
// IMAGE FUNCTIONS
// ========================================

void r3d_image_blit(Image* dst, const Image* src, r3d_rect_t dstRect, r3d_rect_t srcRect)
{
    if (!dst || !dst->data || dst->width <= 0 || dst->height <= 0) return;
    if (!src || !src->data || src->width <= 0 || src->height <= 0) return;
    if (dstRect.w <= 0 || dstRect.h <= 0) return;

    if (srcRect.x < 0) { srcRect.w += srcRect.x; srcRect.x = 0; }
    if (srcRect.y < 0) { srcRect.h += srcRect.y; srcRect.y = 0; }
    if (srcRect.x + srcRect.w > src->width)  srcRect.w = src->width  - srcRect.x;
    if (srcRect.y + srcRect.h > src->height) srcRect.h = src->height - srcRect.y;
    if (srcRect.w <= 0 || srcRect.h <= 0) return;

    int bppSrc = GetPixelDataSize(1, 1, src->format);
    int bppDst = GetPixelDataSize(1, 1, dst->format);

    int dstX0 = dstRect.x, dstY0 = dstRect.y;
    int dstX1 = dstRect.x + dstRect.w, dstY1 = dstRect.y + dstRect.h;

    int clipX0 = (dstX0 < 0) ? 0 : dstX0;
    int clipY0 = (dstY0 < 0) ? 0 : dstY0;
    int clipX1 = (dstX1 > dst->width)  ? dst->width  : dstX1;
    int clipY1 = (dstY1 > dst->height) ? dst->height : dstY1;

    if (clipX1 <= clipX0 || clipY1 <= clipY0) return;

    int outW = clipX1 - clipX0;
    int outH = clipY1 - clipY0;

    int dstStride = dst->width * bppDst;
    int srcStride = src->width * bppSrc;

    bool sameSize   = (dstRect.w == srcRect.w) && (dstRect.h == srcRect.h);
    bool sameFormat = (dst->format == src->format);

    if (sameSize)
    {
        int srcX = srcRect.x + (clipX0 - dstX0);
        int srcY = srcRect.y + (clipY0 - dstY0);

        if (sameFormat)
        {
            image_blit_copy(
                dst, src, clipX0, clipY0, outW, outH,
                srcX, srcY, bppDst, dstStride, srcStride);
        }
        else
        {
            image_blit_convert(
                dst, src, clipX0, clipY0, outW, outH,
                srcX, srcY, bppDst, bppSrc, dstStride, srcStride);
        }
    }
    else
    {
        image_blit_linear(
            dst, src, dstRect, srcRect,
            clipX0, clipY0, outW, outH,
            bppDst, bppSrc, dstStride, srcStride);
    }
}

Image r3d_image_compose_rgb(const Image* sources[3], Color defaultColor)
{
    Image image = {0};

    /* --- Determine dimensions and allocate --- */

    int w = 0, h = 0;
    for (int i = 0; i < 3; i++)
    {
        if (sources[i])
        {
            w = (w < sources[i]->width) ? sources[i]->width : w;
            h = (h < sources[i]->height) ? sources[i]->height : h;
        }
    }

    if (w == 0 || h == 0)
    {
        return image;
    }

    uint8_t* pixels = r3d_zalloc(3 * w * h);

    /* --- Calculate scales --- */

    int scaleX[3] = {0};
    int scaleY[3] = {0};
    for (int i = 0; i < 3; i++)
    {
        if (sources[i] && sources[i]->width > 0 && sources[i]->height > 0)
        {
            scaleX[i] = (sources[i]->width << 16) / w;
            scaleY[i] = (sources[i]->height << 16) / h;
        }
    }

    /* --- Calculate bytes per pixels --- */

    int bpp[3] = {
        sources[0] ? GetPixelDataSize(1, 1, sources[0]->format) : 0,
        sources[1] ? GetPixelDataSize(1, 1, sources[1]->format) : 0,
        sources[2] ? GetPixelDataSize(1, 1, sources[2]->format) : 0,
    };

    /* --- Generate optimized loop based on source combination --- */

    int mask = (sources[0] ? 1 : 0) | (sources[1] ? 2 : 0) | (sources[2] ? 4 : 0);

    #define SAMPLE_TEMPLATE(R_CODE, G_CODE, B_CODE) \
        for (int y = 0; y < h; y++) { \
            for (int x = 0; x < w; x++) { \
                Color color = defaultColor; \
                R_CODE; G_CODE; B_CODE; \
                int offset = 3 * (y * w + x); \
                pixels[offset + 0] = color.r; \
                pixels[offset + 1] = color.g; \
                pixels[offset + 2] = color.b; \
            } \
        }

    #define SAMPLE_R do { \
        int srcX = (x * scaleX[0]) >> 16; \
        int srcY = (y * scaleY[0]) >> 16; \
        if (srcX >= sources[0]->width) srcX = sources[0]->width - 1; \
        if (srcY >= sources[0]->height) srcY = sources[0]->height - 1; \
        Color src = GetPixelColor(((uint8_t*)sources[0]->data) + bpp[0] * (srcY * sources[0]->width + srcX), sources[0]->format); \
        color.r = src.r; \
    } while(0)

    #define SAMPLE_G do { \
        int srcX = (x * scaleX[1]) >> 16; \
        int srcY = (y * scaleY[1]) >> 16; \
        if (srcX >= sources[1]->width) srcX = sources[1]->width - 1; \
        if (srcY >= sources[1]->height) srcY = sources[1]->height - 1; \
        Color src = GetPixelColor(((uint8_t*)sources[1]->data) + bpp[1] * (srcY * sources[1]->width + srcX), sources[1]->format); \
        color.g = src.g; \
    } while(0)

    #define SAMPLE_B do { \
        int srcX = (x * scaleX[2]) >> 16; \
        int srcY = (y * scaleY[2]) >> 16; \
        if (srcX >= sources[2]->width) srcX = sources[2]->width - 1; \
        if (srcY >= sources[2]->height) srcY = sources[2]->height - 1; \
        Color src = GetPixelColor(((uint8_t*)sources[2]->data) + bpp[2] * (srcY * sources[2]->width + srcX), sources[2]->format); \
        color.b = src.b; \
    } while(0)

    #define NOOP do {} while(0)

    switch (mask) {
        case 0: break; // No sources
        case 1: SAMPLE_TEMPLATE(SAMPLE_R, NOOP, NOOP); break;
        case 2: SAMPLE_TEMPLATE(NOOP, SAMPLE_G, NOOP); break;
        case 3: SAMPLE_TEMPLATE(SAMPLE_R, SAMPLE_G, NOOP); break;
        case 4: SAMPLE_TEMPLATE(NOOP, NOOP, SAMPLE_B); break;
        case 5: SAMPLE_TEMPLATE(SAMPLE_R, NOOP, SAMPLE_B); break;
        case 6: SAMPLE_TEMPLATE(NOOP, SAMPLE_G, SAMPLE_B); break;
        case 7: SAMPLE_TEMPLATE(SAMPLE_R, SAMPLE_G, SAMPLE_B); break;
    }

    #undef SAMPLE_TEMPLATE
    #undef SAMPLE_R
    #undef SAMPLE_G  
    #undef SAMPLE_B
    #undef NOOP

    image.data    = pixels;
    image.width   = w;
    image.height  = h;
    image.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
    image.mipmaps = 1;

    return image;
}

Texture2D r3d_image_upload(const Image* image, TextureWrap wrap, TextureFilter filter, bool isColor)
{
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const uint8_t *dataPtr = image->data;
    int mipW = image->width, mipH = image->height;

    for (int i = 0; i < image->mipmaps; i++)
    {
        upload_texture_mipmap(dataPtr, mipW, mipH, i, image->format, isColor);
        if (i == 0) set_texture_swizzle(image->format);

        int mipSize = GetPixelDataSize(mipW, mipH, image->format);
        if (dataPtr) dataPtr += mipSize;

        mipW = (mipW > 1) ? mipW / 2 : 1;
        mipH = (mipH > 1) ? mipH / 2 : 1;
    }

    if (image->mipmaps == 1 && filter >= TEXTURE_FILTER_TRILINEAR)
    {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    set_texture_wrap(wrap);
    set_texture_filter(filter);
    glBindTexture(GL_TEXTURE_2D, 0);

    return (Texture2D) {
        .id = id,
        .width = image->width,
        .height = image->height,
        .mipmaps = image->mipmaps,
        .format = image->format
    };
}

// ========================================
// INTERNAL FUNCTIONS
// ========================================

void image_blit_copy(
    Image* dst, const Image* src,
    int clipX0, int clipY0, int outW, int outH,
    int srcX, int srcY,
    int bpp, int dstStride, int srcStride)
{
    uint8_t* dstData = dst->data;
    uint8_t* srcData = src->data;

    size_t rowBytes = (size_t)outW * bpp;

    for (int y = 0; y < outH; y++)
    {
        uint8_t* srow = srcData + (size_t)(srcY + y) * srcStride + (size_t)srcX * bpp;
        uint8_t* drow = dstData + (size_t)(clipY0 + y) * dstStride + (size_t)clipX0 * bpp;
        memcpy(drow, srow, rowBytes);
    }
}

void image_blit_convert(
    Image* dst, const Image* src,
    int clipX0, int clipY0, int outW, int outH,
    int srcX, int srcY,
    int bppDst, int bppSrc, int dstStride, int srcStride)
{
    uint8_t* dstData = dst->data;
    uint8_t* srcData = src->data;

    for (int y = 0; y < outH; y++)
    {
        uint8_t* srow = srcData + (size_t)(srcY + y) * srcStride + (size_t)srcX * bppSrc;
        uint8_t* drow = dstData + (size_t)(clipY0 + y) * dstStride + (size_t)clipX0 * bppDst;

        for (int x = 0; x < outW; x++)
        {
            uint8_t* sp = srow + (size_t)x * bppSrc;
            uint8_t* dp = drow + (size_t)x * bppDst;
            SetPixelColor(dp, GetPixelColor(sp, src->format), dst->format);
        }
    }
}

void image_blit_linear(
    Image* dst, const Image* src,
    r3d_rect_t dstRect, r3d_rect_t srcRect,
    int clipX0, int clipY0, int outW, int outH,
    int bppDst, int bppSrc, int dstStride, int srcStride)
{
    uint8_t* dstData = dst->data;
    uint8_t* srcData = src->data;

    float scaleX = (float)srcRect.w / (float)dstRect.w;
    float scaleY = (float)srcRect.h / (float)dstRect.h;

    int srcXMax = srcRect.x + srcRect.w - 1;
    int srcYMax = srcRect.y + srcRect.h - 1;

    float sYAcc = srcRect.y + ((clipY0 - dstRect.y) + 0.5f) * scaleY - 0.5f;

    for (int y = 0; y < outH; y++, sYAcc += scaleY)
    {
        float sy = (sYAcc < srcRect.y) ? (float)srcRect.y : sYAcc;
        int sY0 = (int)sy;
        int sY1 = (sY0 + 1 > srcYMax) ? srcYMax : sY0 + 1;
        if (sY0 > srcYMax) sY0 = srcYMax;
        float fy = sy - (float)sY0;

        uint8_t* srow0 = srcData + (size_t)sY0 * srcStride;
        uint8_t* srow1 = srcData + (size_t)sY1 * srcStride;
        uint8_t* drow  = dstData + (size_t)(clipY0 + y) * dstStride;

        float sXAcc = srcRect.x + ((clipX0 - dstRect.x) + 0.5f) * scaleX - 0.5f;

        for (int x = 0; x < outW; x++, sXAcc += scaleX)
        {
            float sx = (sXAcc < srcRect.x) ? (float)srcRect.x : sXAcc;
            int sX0 = (int)sx;
            int sX1 = (sX0 + 1 > srcXMax) ? srcXMax : sX0 + 1;
            if (sX0 > srcXMax) sX0 = srcXMax;
            float fx = sx - (float)sX0;

            Color c00 = GetPixelColor(srow0 + (size_t)sX0 * bppSrc, src->format);
            Color c10 = GetPixelColor(srow0 + (size_t)sX1 * bppSrc, src->format);
            Color c01 = GetPixelColor(srow1 + (size_t)sX0 * bppSrc, src->format);
            Color c11 = GetPixelColor(srow1 + (size_t)sX1 * bppSrc, src->format);

            float w00 = (1.0f - fx) * (1.0f - fy);
            float w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy;
            float w11 = fx * fy;

            Color c = {
                (uint8_t)(c00.r*w00 + c10.r*w10 + c01.r*w01 + c11.r*w11),
                (uint8_t)(c00.g*w00 + c10.g*w10 + c01.g*w01 + c11.g*w11),
                (uint8_t)(c00.b*w00 + c10.b*w10 + c01.b*w01 + c11.b*w11),
                (uint8_t)(c00.a*w00 + c10.a*w10 + c01.a*w01 + c11.a*w11)
            };

            uint8_t* dp = drow + (size_t)(clipX0 + x) * bppDst;
            SetPixelColor(dp, c, dst->format);
        }
    }
}

void get_texture_format(int format, bool isColor, GLenum* glInternalFormat, GLenum* glFormat, GLenum* glType)
{
    // TODO: Add checks for support of compressed formats (consider WebGL 2 for later)

    *glInternalFormat = 0;
    *glFormat = 0;
    *glType = 0;

    switch (format)
    {
    case RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE: *glInternalFormat = GL_R8; *glFormat = GL_RED; *glType = GL_UNSIGNED_BYTE; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA: *glInternalFormat = GL_RG8; *glFormat = GL_RG; *glType = GL_UNSIGNED_BYTE; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R5G6B5: *glInternalFormat = GL_RGB565; *glFormat = GL_RGB; *glType = GL_UNSIGNED_SHORT_5_6_5; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8: *glInternalFormat = GL_RGB8; *glFormat = GL_RGB; *glType = GL_UNSIGNED_BYTE; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R5G5B5A1: *glInternalFormat = GL_RGB5_A1; *glFormat = GL_RGBA; *glType = GL_UNSIGNED_SHORT_5_5_5_1; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R4G4B4A4: *glInternalFormat = GL_RGBA4; *glFormat = GL_RGBA; *glType = GL_UNSIGNED_SHORT_4_4_4_4; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8: *glInternalFormat = GL_RGBA8; *glFormat = GL_RGBA; *glType = GL_UNSIGNED_BYTE; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R32: *glInternalFormat = GL_R32F; *glFormat = GL_RED; *glType = GL_FLOAT; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32: *glInternalFormat = GL_RGB32F; *glFormat = GL_RGB; *glType = GL_FLOAT; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32: *glInternalFormat = GL_RGBA32F; *glFormat = GL_RGBA; *glType = GL_FLOAT; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R16: *glInternalFormat = GL_R16F; *glFormat = GL_RED; *glType = GL_HALF_FLOAT; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16: *glInternalFormat = GL_RGB16F; *glFormat = GL_RGB; *glType = GL_HALF_FLOAT; break;
    case RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16: *glInternalFormat = GL_RGBA16F; *glFormat = GL_RGBA; *glType = GL_HALF_FLOAT; break;
    case RL_PIXELFORMAT_COMPRESSED_DXT1_RGB: *glInternalFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT; break;
    case RL_PIXELFORMAT_COMPRESSED_DXT1_RGBA: *glInternalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
    case RL_PIXELFORMAT_COMPRESSED_DXT3_RGBA: *glInternalFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
    case RL_PIXELFORMAT_COMPRESSED_DXT5_RGBA: *glInternalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
    default: R3D_TRACELOG(RL_LOG_WARNING, "Current format not supported (%i)", format); break;
    }

    if (isColor)
    {
        switch (*glInternalFormat)
        {
        case GL_RGBA8: *glInternalFormat = GL_SRGB8_ALPHA8; break;
        case GL_RGB8: *glInternalFormat = GL_SRGB8; break;
        // NOT SUPPORTED FOR NOW
        case GL_COMPRESSED_RGBA_BPTC_UNORM: *glInternalFormat = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM; break;
        case GL_COMPRESSED_RGBA_ASTC_4x4_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_5x4_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_5x5_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_6x5_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_6x6_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_8x5_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_8x6_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_8x8_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_10x5_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_10x6_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_10x8_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_10x10_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_12x10_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR; break;
        case GL_COMPRESSED_RGBA_ASTC_12x12_KHR: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR; break;
        case GL_COMPRESSED_RGB8_ETC2: *glInternalFormat = GL_COMPRESSED_SRGB8_ETC2; break;
        case GL_COMPRESSED_RGBA8_ETC2_EAC: *glInternalFormat = GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC; break;
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2: *glInternalFormat = GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2; break;
        }
    }
}

void upload_texture_mipmap(const uint8_t *data, int width, int height, int level, int format, bool isColor)
{
    GLenum glInternalFormat, glFormat, glType;
    get_texture_format(format, isColor, &glInternalFormat, &glFormat, &glType);
    
    if (glInternalFormat == 0) return;
    
    if (format < RL_PIXELFORMAT_COMPRESSED_DXT1_RGB)
    {
        glTexImage2D(
            GL_TEXTURE_2D, level, glInternalFormat, width, height, 
            0, glFormat, glType, data
        );
    }
    else
    {
        int size = GetPixelDataSize(width, height, format);
        glCompressedTexImage2D(
            GL_TEXTURE_2D, level, glInternalFormat,
            width, height, 0, size, data
        );
    }
}

void set_texture_swizzle(int format)
{
    static const GLint grayscale[]  = {GL_RED, GL_RED, GL_RED, GL_ONE};
    static const GLint gray_alpha[] = {GL_RED, GL_RED, GL_RED, GL_GREEN};
    
    const GLint *mask = NULL;
    if (format == RL_PIXELFORMAT_UNCOMPRESSED_GRAYSCALE) mask = grayscale;
    else if (format == RL_PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA) mask = gray_alpha;
    
    if (mask)
    {
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, mask);
    }
}

void set_texture_wrap(TextureWrap wrap)
{
    static const GLenum wrap_modes[] = {
        [TEXTURE_WRAP_REPEAT] = GL_REPEAT,
        [TEXTURE_WRAP_CLAMP] = GL_CLAMP_TO_EDGE,
        [TEXTURE_WRAP_MIRROR_REPEAT] = GL_MIRRORED_REPEAT,
        [TEXTURE_WRAP_MIRROR_CLAMP] = GL_MIRROR_CLAMP_TO_EDGE
    };
    
    if (wrap < sizeof(wrap_modes) / sizeof(wrap_modes[0]) && wrap_modes[wrap])
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_modes[wrap]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_modes[wrap]);
    }
}

void set_texture_filter(TextureFilter filter)
{
    typedef struct {
        GLenum mag, min;
        float aniso;
    } FilterMode;
    
    static const FilterMode modes[] = {
        [TEXTURE_FILTER_POINT] = {GL_NEAREST, GL_NEAREST, 0},
        [TEXTURE_FILTER_BILINEAR] = {GL_LINEAR, GL_LINEAR, 0},
        [TEXTURE_FILTER_TRILINEAR] = {GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR, 0},
        [TEXTURE_FILTER_ANISOTROPIC_4X] = {GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR, 4.0f},
        [TEXTURE_FILTER_ANISOTROPIC_8X] = {GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR, 8.0f},
        [TEXTURE_FILTER_ANISOTROPIC_16X] = {GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR, 16.0f}
    };
    
    if (filter < sizeof(modes) / sizeof(modes[0]) && modes[filter].mag)
    {
        const FilterMode *m = &modes[filter];
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m->mag);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m->min);
        if (m->aniso > 0)
        {
            float maxAniso = 1.0f;
            if (r3d_driver_has_anisotropy(&maxAniso))
            {
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, fminf(m->aniso, maxAniso));
            }
        }
    }
}
