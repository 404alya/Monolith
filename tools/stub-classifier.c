// tools/stub-classifier.c -> build/tools/stub-classifier: a fake AI classifier for tests of the editor's
// model choice (src/classifier.c). Used as MONOLITH_CLASSIFIER, it answers like `claude -p --output-format
// json` (or fails in the way MONOLITH_STUB_CLASSIFIER asks), so no test ever starts a real Claude run.
// Reference: docs/code/stub-classifier.md

#include "tool.h"

// The value after `flag` in argv, or "" (the editor always passes --model and --fallback-model)
static const char *ArgAfter(int argc, char **argv, const char *flag)
{
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return "";
}

// One answer in the shape of Claude Code's result object (the fields the editor reads, plus a few it
// skips, so its reader is exercised on the real layout)
static void Answer(const char *tier, const char *model, const char *reason)
{
    printf("{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"duration_ms\":12,\"num_turns\":1,"
           "\"result\":\"{\\\"tier\\\":\\\"%s\\\"}\",\"session_id\":\"stub\",\"total_cost_usd\":0,"
           "\"usage\":{\"input_tokens\":1,\"output_tokens\":9},"
           "\"modelUsage\":{\"claude-haiku-stub\":{\"inputTokens\":3,\"outputTokens\":1,\"costUSD\":0},"
           "\"%s\":{\"inputTokens\":2,\"outputTokens\":9,\"costUSD\":0}},"
           "\"permission_denials\":[],\"structured_output\":{\"tier\":\"%s\",\"reason\":\"%s\"}}\n",
           tier, model, tier, reason);
}

// Whether $MONOLITH_ACCOUNT ("main" when unset, as for a session without a fallback) is in the
// comma-separated list MONOLITH_STUB_LIMITED: that account has "reached its usage limit"
static bool Limited(void)
{
    const char *list = ToolEnv("MONOLITH_STUB_LIMITED", "");
    const char *account = ToolEnv("MONOLITH_ACCOUNT", "main");
    size_t n = strlen(account);
    for (const char *p = list; *p;) {
        size_t len = strcspn(p, ",");
        if (len == n && strncmp(p, account, n) == 0) return true;
        p += len;
        if (*p == ',') p++;
    }
    return false;
}

// Claude Code's result object for a run stopped by the usage limit (is_error, the limit text), exit 1
static int LimitAnswer(void)
{
    printf("{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true,\"duration_ms\":3,\"num_turns\":1,"
           "\"result\":\"You\u2019ve hit your session limit \u00b7 resets 3:45pm\",\"session_id\":\"stub\",\"total_cost_usd\":0,"
           "\"usage\":{\"input_tokens\":0,\"output_tokens\":0},\"modelUsage\":{},\"permission_denials\":[]}\n");
    return 1;
}

int main(int argc, char **argv)
{
    ToolInit(argv, "stub-classifier", "stub-classifier");

    // Read the whole input first (the editor's classifier-input.txt), as claude -p does
    Nob_String_Builder in = {0};
    char buf[4096];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        nob_sb_append_buf(&in, buf, (size_t)n);
    }

    const char *log = ToolEnv("MONOLITH_STUB_CLASSIFIER_LOG", ToolEnv("MONOLITH_STUB_LOG", NULL));
    if (log) {
        FILE *f = fopen(log, "ab");
        if (f) {
            fprintf(f, "stub-classifier account: %s config: %s\n", ToolEnv("MONOLITH_ACCOUNT", "-"), ToolEnv("CLAUDE_CONFIG_DIR", "-"));
            fprintf(f, "stub-classifier argv:\n");
            for (int i = 0; i < argc; i++) fprintf(f, "  arg: %s\n", argv[i]);
            fprintf(f, "stub-classifier stdin:\n");
            fwrite(in.items, 1, in.count, f);
            if (fclose(f) != 0) ToolErr("%s: %s", log, strerror(errno));
        } else {
            ToolErr("%s: %s", log, strerror(errno));
        }
    }
    nob_sb_free(in);

    const char *mode = ToolEnv("MONOLITH_STUB_CLASSIFIER", "default");
    const char *model = ArgAfter(argc, argv, "--model");
    if (strcmp(mode, "limit") == 0 || Limited()) return LimitAnswer();
    if (strcmp(mode, "fast") == 0 || strcmp(mode, "default") == 0 || strcmp(mode, "heavy") == 0) {
        Answer(mode, model, "stub answer: caf\\u00e9\\nnext line");
    } else if (strcmp(mode, "fallback") == 0) {
        Answer("heavy", ArgAfter(argc, argv, "--fallback-model"), "stub answer from the fallback model");
    } else if (strcmp(mode, "garbage") == 0) {
        printf("I think this should be the heavy tier, use opus.\n");
    } else if (strcmp(mode, "outside") == 0) {
        Answer("opus", model, "a tier outside the schema");
    } else if (strcmp(mode, "fail") == 0) {
        ToolDie(1, "failing on purpose (MONOLITH_STUB_CLASSIFIER=fail)");
    } else if (strcmp(mode, "slow") == 0) {
        sleep(600);
        Answer("default", model, "too late");
    } else {
        ToolDie(1, "unknown MONOLITH_STUB_CLASSIFIER '%s'", mode);
    }
    return 0;
}
