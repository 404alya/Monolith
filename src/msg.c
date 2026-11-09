// Text for people without fixed-size buffers (msg.h): static literals kept apart from heap strings
// formatted to fit. Plain C11 + libc, so tests/unit/msg_test.c builds it alone.
#include "msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// tests/unit/msg_test.c injects a failing allocator here (-DMSG_MALLOC=...)
#ifndef MSG_MALLOC
#define MSG_MALLOC malloc
#endif

static const char OUT_OF_MEMORY[] = "out of memory";

char *MonoStrv(const char *fmt, va_list ap)
{
    va_list count;
    va_copy(count, ap);
    int n = vsnprintf(NULL, 0, fmt, count);
    va_end(count);
    if (n < 0) return NULL;
    char *s = MSG_MALLOC((size_t)n + 1);
    if (!s) return NULL;
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

char *MonoStrf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = MonoStrv(fmt, ap);
    va_end(ap);
    return s;
}

// Takes `s` (heap, or NULL = out of memory) as the new text; the old one goes after
static void Take(MonoMsg *m, char *s)
{
    char *old = m->owned;
    m->owned = s;
    m->text = s ? s : OUT_OF_MEMORY;
    free(old);
}

void MsgSetStatic(MonoMsg *m, const char *literal)
{
    free(m->owned);
    m->owned = NULL;
    m->text = literal;
}

void MsgSetv(MonoMsg *m, const char *fmt, va_list ap)
{
    Take(m, MonoStrv(fmt, ap));
}

void MsgSetf(MonoMsg *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    MsgSetv(m, fmt, ap);
    va_end(ap);
}

void MsgAppendf(MonoMsg *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *tail = MonoStrv(fmt, ap);
    va_end(ap);
    if (!tail) { Take(m, NULL); return; }
    const char *head = MsgText(m);
    size_t a = strlen(head), b = strlen(tail);
    char *s = MSG_MALLOC(a + b + 1);
    if (s) {
        memcpy(s, head, a);
        memcpy(s + a, tail, b + 1);
    }
    free(tail);
    Take(m, s);
}

const char *MsgText(const MonoMsg *m)
{
    return m->text ? m->text : "";
}

void MsgFree(MonoMsg *m)
{
    free(m->owned);
    m->owned = NULL;
    m->text = NULL;
}
