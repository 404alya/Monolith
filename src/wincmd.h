// Windows: the command line cmd /k runs for a request terminal (see wincmd.c)
#pragma once

// ""<claude>" <flags...> [--model "<model>"] "<extra>"... "<prompt>"" in a heap string
// (free() it); NULL when out of memory. Words that contain a double quote can't be passed
// through cmd safely and are left out; *skipped (may be NULL) counts them.
char *WinClaudeCommand(const char *claude, const char *const *flags, int flagCount, const char *model,
                       char *const *extra, int extraCount, const char *prompt, int *skipped);
