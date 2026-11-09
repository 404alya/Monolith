// Text for people (HUD lines, panel footers, the log) without fixed-size buffers: a MonoMsg holds
// either a static literal (never copied or freed) or a heap string sized to fit (vsnprintf in two
// passes). The result of a function is its status code (an unsigned char, 0 = success); these
// strings only say it in words. docs/architecture.md "Errors and reports"; editor only (src/).
#ifndef MONOLITH_MSG_H
#define MONOLITH_MSG_H

#include <stdarg.h>
#include <stdbool.h>

#if defined(__GNUC__) || defined(__clang__)
#define MSG_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define MSG_PRINTF(f, a)
#endif

// A stored text: `text` is what to show ("" when nothing was set); `owned` is the heap copy it
// points to, or NULL when `text` is a literal. Zero-initialised it is empty and needs no free.
typedef struct {
    const char *text;
    char *owned;
} MonoMsg;

// A literal (or any string that outlives the message): not copied; the previous owned text is freed
void MsgSetStatic(MonoMsg *m, const char *literal);
// printf into a new heap string sized to fit; the previous text is freed after formatting, so the
// arguments may quote it (MsgSetf(m, "auto save: %s", MsgText(m))). Out of memory: the static
// "out of memory".
void MsgSetf(MonoMsg *m, const char *fmt, ...) MSG_PRINTF(2, 3);
void MsgSetv(MonoMsg *m, const char *fmt, va_list ap);
// Appends printf output to the current text (a static text is copied first). Out of memory: the
// static "out of memory".
void MsgAppendf(MonoMsg *m, const char *fmt, ...) MSG_PRINTF(2, 3);
// The text ("" when nothing was set)
const char *MsgText(const MonoMsg *m);
// Frees the owned text; the message is empty again
void MsgFree(MonoMsg *m);

// A heap printf for one-off texts (sized to fit; free it). NULL when out of memory.
char *MonoStrf(const char *fmt, ...) MSG_PRINTF(1, 2);
char *MonoStrv(const char *fmt, va_list ap);

#endif
