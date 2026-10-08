/* r3d_helper.h -- Common R3D Helpers
 *
 * Copyright (c) 2025-2026 Le Juez Victor
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * For conditions of distribution and use, see accompanying LICENSE file.
 */

#ifndef R3D_COMMON_HELPER_H
#define R3D_COMMON_HELPER_H

#include <r3d_config.h>
#include <raylib.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>

#if defined(_MSC_VER)
#   include <intrin.h>
#endif

// ========================================
// HELPER MACROS
// ========================================

#if !defined(NDEBUG)
#   define R3D_ASSERT(x) assert(x)
#else
#   define R3D_ASSERT(x) R3D_UNUSED(x)
#endif

#define R3D_UNUSED(x) ((void)(x))

#define R3D_MIN(x, y) ((x) < (y) ? (x) : (y))
#define R3D_MAX(x, y) ((x) > (y) ? (x) : (y))

#define R3D_CLAMP(v, min, max) ((v) < (min) ? (min) : ((v) > (max) ? (max) : (v)))
#define R3D_SATURATE(x) (R3D_CLAMP(x, 0.0f, 1.0f))

#define R3D_ARRAY_SIZE(arr) (sizeof((arr)) / sizeof((arr)[0]))

#define R3D_SWAP(type, a, b) do { type _tmp = (a); (a) = (b); (b) = _tmp; } while (0)

#define R3D_BIT_SET(v, m) ((v) |= (m))

#define R3D_BIT_CLEAR(v, m) ((v) &= ~(m))

#define R3D_BIT_TOGGLE(v, m) ((v) ^= (m))

#define R3D_BIT_ALL(v, m) (((v) & (m)) == (m))

#define R3D_BIT_ANY(v, m) (((v) & (m)) != 0)

#define R3D_MAXOF(x) _Generic((x), \
    char: CHAR_MAX, \
    signed char: SCHAR_MAX, \
    unsigned char: UCHAR_MAX, \
    short: SHRT_MAX, \
    unsigned short: USHRT_MAX, \
    int: INT_MAX, \
    unsigned int: UINT_MAX, \
    long: LONG_MAX, \
    unsigned long: ULONG_MAX, \
    long long: LLONG_MAX, \
    unsigned long long: ULLONG_MAX \
)

#define R3D_CONCAT_(a, b) a##b
#define R3D_CONCAT(a, b)  R3D_CONCAT_(a, b)

// ========================================
// HELPER FUNCTIONS
// ========================================

/*
 * Returns the number of logical CPUs available to the system.
 * The value is detected once and cached.
 */
int r3d_get_cpu_count(void);

// ========================================
// INLINED FUNCTIONS
// ========================================

static inline void* r3d_zalloc(size_t size)
{
    assert(size <= UINT_MAX); // duplicated to keep the stack with debuggers

    if (size > UINT_MAX)
    {
        R3D_TRACELOG(LOG_FATAL, "Allocation size %zu exceeds UINT_MAX", size);
        abort();
    }

    void* ptr = MemAlloc((unsigned int)size);
    if (ptr == NULL)
    {
        R3D_TRACELOG(LOG_FATAL, "OOM: allocation of %zu bytes failed", size);
        abort();
    }

    return ptr;
}

static inline void* r3d_realloc(void* ptr, size_t size)
{
    assert(size <= UINT_MAX); // duplicated to keep the stack with debuggers

    if (size > UINT_MAX)
    {
        R3D_TRACELOG(LOG_FATAL, "Reallocation size %zu exceeds UINT_MAX", size);
        abort();
    }

    void* newPtr = MemRealloc(ptr, (unsigned int)size);
    if (newPtr == NULL)
    {
        R3D_TRACELOG(LOG_FATAL, "OOM: reallocation of %zu bytes failed", size);
        abort();
    }

    return newPtr;
}

static inline void r3d_free(void* ptr)
{
    MemFree(ptr);
}

static inline void r3d_string_copy(char* dst, size_t dstSize, const char* src, size_t srcLen)
{
    if (dstSize == 0) return;

    size_t len = (srcLen < dstSize - 1) ? srcLen : dstSize - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static inline int r3d_string_format(char* dst, size_t dstSize, const char* fmt, ...)
{
    if (!dst || dstSize == 0) return -1;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(dst, dstSize, fmt, args);
    va_end(args);

    dst[dstSize - 1] = '\0';

    return written;
}

static inline bool r3d_is_absolute_path(const char* path)
{
#ifdef _WIN32
    return (path[1] == ':') || (path[0] == '\\' && path[1] == '\\');
#else
    return path[0] == '/';
#endif
}

static inline int32_t r3d_get_mip_levels_1d(int32_t s)
{
    if (s <= 0) return 0;

#if defined(__GNUC__) || defined(__clang__)
    return 32 - __builtin_clz((unsigned)s);
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse(&index, (unsigned long)s);
    return (int32_t)index + 1;
#else
    int32_t levels = 0;
    while (s > 0)
    {
        levels++;
        s >>= 1;
    }
    return levels;
#endif
}

static inline int32_t r3d_get_mip_levels_2d(int32_t w, int32_t h)
{
    return r3d_get_mip_levels_1d((w > h) ? w : h);
}

static inline int32_t r3d_lsb_index(uint32_t value)
{
    if (value == 0) return -1;

#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(value);
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanForward(&index, (unsigned long)value);
    return (int32_t)index;
#else
    int32_t index = 0;
    while ((value & 1) == 0)
    {
        value >>= 1;
        index++;
    }
    return index;
#endif
}

static inline int r3d_align_offset(int offset, int align)
{
    return (offset + align - 1) & ~(align - 1);
}

#endif // R3D_COMMON_HELPER_H
