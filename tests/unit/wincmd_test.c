// Unit check of the Windows request command line (src/wincmd.c), runs on Linux:
//   cc -std=c11 -Wall -Wextra -Isrc tests/unit/wincmd_test.c src/wincmd.c -o /tmp/wincmd_test && /tmp/wincmd_test
#include "wincmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void Expect(const char *what, const char *got, const char *want)
{
    if (!got || strcmp(got, want) != 0) { printf("FAIL %s\n  got:  %s\n  want: %s\n", what, got ? got : "(null)", want); fails++; }
    else printf("ok   %s\n", what);
}

int main(void)
{
    const char *flags[] = { "--allow-dangerously-skip-permissions", "--dangerously-skip-permissions" };
    const char *prompt = "Map edit request: read requests/x/request.md and carry it out.";
    int skipped = -1;
    char *s = WinClaudeCommand("C:\\bin\\claude.exe", flags, 2, "haiku", NULL, 0, prompt, &skipped);
    Expect("plain", s, "\"\"C:\\bin\\claude.exe\" --allow-dangerously-skip-permissions --dangerously-skip-permissions"
                       " --model \"haiku\" \"Map edit request: read requests/x/request.md and carry it out.\"\"");
    free(s);

    char *extra[] = { "--verbose", "a&b", "x\"y" };
    s = WinClaudeCommand("claude", NULL, 0, NULL, extra, 3, "p", &skipped);
    Expect("extra words quoted, a word with a quote dropped", s, "\"\"claude\" \"--verbose\" \"a&b\" \"p\"\"");
    if (skipped != 1) { printf("FAIL skipped = %d, want 1\n", skipped); fails++; }
    free(s);

    // 20000 extra words (~380 KB, far over the old 4096-byte buffer): the prompt must
    // still be at the end
    enum { N = 20000 };
    char **many = malloc(sizeof(char *) * N);
    for (int i = 0; i < N; i++) many[i] = "--some-long-flag";
    s = WinClaudeCommand("claude", flags, 2, "opus", many, N, prompt, &skipped);
    size_t want = strlen("\"\"claude\"") + strlen(" --allow-dangerously-skip-permissions --dangerously-skip-permissions")
                + strlen(" --model \"opus\"") + (size_t)N * strlen(" \"--some-long-flag\"") + 1 + strlen(prompt) + 3;
    const char *tail = " \"Map edit request: read requests/x/request.md and carry it out.\"\"";
    size_t len = s ? strlen(s) : 0;
    int okTail = len >= strlen(tail) && strcmp(s + len - strlen(tail), tail) == 0;
    if (!okTail || len != want || skipped != 0) { printf("FAIL long: len %zu want %zu, tail ok %d\n", len, want, okTail); fails++; }
    else printf("ok   long (%zu bytes, prompt at the end)\n", len);
    free(s);
    free(many);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
