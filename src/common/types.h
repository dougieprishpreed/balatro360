// Basic portable types. The Xbox 360 XDK toolchain (VS2010-era) does not
// reliably ship <stdint.h>, so we define our own fixed-width types here.
#ifndef B360_TYPES_H
#define B360_TYPES_H

#include <stddef.h>

#if defined(_MSC_VER)
typedef signed __int8 i8;
typedef unsigned __int8 u8;
typedef signed __int16 i16;
typedef unsigned __int16 u16;
typedef signed __int32 i32;
typedef unsigned __int32 u32;
typedef signed __int64 i64;
typedef unsigned __int64 u64;
#define B360_INLINE __forceinline
#else
typedef signed char i8;
typedef unsigned char u8;
typedef signed short i16;
typedef unsigned short u16;
typedef signed int i32;
typedef unsigned int u32;
typedef signed long long i64;
typedef unsigned long long u64;
#define B360_INLINE inline
#endif

#if defined(_XBOX) || defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define B360_BIG_ENDIAN 1
#else
#define B360_BIG_ENDIAN 0
#endif

// VS2010 lacks C99 snprintf/vsnprintf; provide always-terminating versions.
#include <stdarg.h>
#include <stdio.h>
#if defined(_MSC_VER) && _MSC_VER < 1900
static __inline int b360_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    int r = _vsnprintf(buf, n, fmt, ap);
    if (n > 0 && (r < 0 || (size_t)r >= n)) buf[n - 1] = 0;
    return r;
}
static __inline int b360_snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = b360_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
#define vsnprintf b360_vsnprintf
#define snprintf b360_snprintf
#endif

#endif
