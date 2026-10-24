// glibc_compat.c -- lets the Linux build run on older distros (glibc >= 2.34).
//
// Linking against a current distribution's glibc (2.44 when this was written) makes the executable require the
// newest version of every symbol it uses, e.g. acosf@GLIBC_2.43, which fails to load
// on Ubuntu 22.04/24.04, Debian 12 etc. build.sh links with -Wl,--wrap=<symbol> for
// each one below, so all references (also those inside the static R3D, raylib, assimp
// and libstdc++ libraries) go to __wrap_<symbol>, which calls an older version.
// The newer libm versions only changed error handling for edge cases (no SVID
// matherr/_LIB_VERSION support); results for normal inputs are the same.
// Check with: objdump -T build/linux/monolith | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1

#include <stdlib.h>   // also defines __GLIBC__

#if defined(__linux__) && defined(__GLIBC__) && defined(__x86_64__)

#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>

#define COMPAT_OLD(ret, name, params, args)                       \
    ret name##_old params;                                        \
    __asm__(".symver " #name "_old, " #name "@GLIBC_2.2.5");      \
    ret __wrap_##name params;                                     \
    ret __wrap_##name params { return name##_old args; }

COMPAT_OLD(float,  acosf,  (float x),           (x))     // GLIBC_2.43
COMPAT_OLD(float,  asinf,  (float x),           (x))     // GLIBC_2.43
COMPAT_OLD(float,  atan2f, (float y, float x),  (y, x))  // GLIBC_2.43
COMPAT_OLD(float,  sqrtf,  (float x),           (x))     // GLIBC_2.43
COMPAT_OLD(double, fmod,   (double x, double y),(x, y))  // GLIBC_2.38
COMPAT_OLD(float,  fmodf,  (float x, float y),  (x, y))  // GLIBC_2.38
COMPAT_OLD(double, hypot,  (double x, double y),(x, y))  // GLIBC_2.35
COMPAT_OLD(float,  hypotf, (float x, float y),  (x, y))  // GLIBC_2.35

// GLIBC_2.38: C23 strtoul (also accepts a "0b" prefix), used by libstdc++. Only
// called on environment variables there, so plain strtoul is equivalent.
unsigned long __wrap___isoc23_strtoul(const char *s, char **end, int base);
unsigned long __wrap___isoc23_strtoul(const char *s, char **end, int base)
{
    return strtoul(s, end, base);
}

// GLIBC_2.36: used by libstdc++'s std::random_device. getrandom() is GLIBC_2.25.
uint32_t __wrap_arc4random(void);
uint32_t __wrap_arc4random(void)
{
    uint32_t v = 0;
    size_t got = 0;
    while (got < sizeof(v)) {
        ssize_t n = getrandom((char *)&v + got, sizeof(v) - got, 0);
        if (n > 0) got += (size_t)n;
    }
    return v;
}

#else
typedef int GlibcCompatUnused;   // ISO C forbids an empty translation unit
#endif
