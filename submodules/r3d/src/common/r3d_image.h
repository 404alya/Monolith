/* r3d_image.h -- Common R3D Image Functions
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_COMMON_IMAGE_H
#define R3D_COMMON_IMAGE_H

#include <raylib.h>

#include "./r3d_math.h"

/**
 * Copy or resample a rectangular region from `src` into `dst`.
 *
 * Both rectangles are clipped to their image bounds. Automatically picks
 * the fastest strategy: raw memcpy (same size/format), per-pixel format
 * conversion (same size, different format), or bilinear resampling
 * (different size). Only mip level 0 of `dst` is affected. Compressed
 * pixel formats are not supported.
 */
void r3d_image_blit(Image* dst, const Image* src, r3d_rect_t dstRect, r3d_rect_t srcRect);

/**
 * Build an RGB image from up to 3 grayscale sources (R,G,B).
 * Missing channels use defaultColor.
 * Output size = max width/height of non-NULL inputs.
 * Channels are resampled with nearest-neighbor (16.16 fixed-point).
 */
Image r3d_image_compose_rgb(const Image* sources[3], Color defaultColor);

/**
 * Uploads the given image to a 2D texture, setting wrap and filter.
 * The 'isColor' flag tries to load the texture in an sRGB format when supported.
 */
Texture2D r3d_image_upload(const Image* image, TextureWrap wrap, TextureFilter filter, bool isColor);

#endif // R3D_COMMON_IMAGE_H
