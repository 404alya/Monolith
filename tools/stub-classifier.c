// tools/stub-classifier.c: a fake AI classifier for tests of the editor's model choice (src/classifier.c,
// docs/request-queue.md "Models"). Built by `./build.sh --tools stub-classifier` into
// build/tools/stub-classifier.
//
// Use it as MONOLITH_CLASSIFIER=build/tools/stub-classifier: the editor starts it exactly as it starts
// `claude -p` (the same arguments, the request on stdin, stdout into the bundle's classifier.json), and it
// answers the way Claude Code's --output-format json does, so no test ever starts a real Claude run.
//
// Environment (each one is optional):
//   MONOLITH_STUB_CLASSIFIER=<mode>   what to answer (default: default):
//       fast | default | heavy   a valid answer on that tier, from the --model it was given (modelUsage
//                                names it); the reason has a line break and a non-ASCII letter, which the
//                                editor must turn into plain ASCII
//       fallback                 a valid heavy answer from the --fallback-model (as if Fable was overloaded)
//       garbage                  text that is not JSON, exit 0
//       outside                  valid JSON whose tier is "opus" (outside the schema), exit 0
//       fail                     a message on stderr, exit 1
//       slow                     sleeps 600 s before answering (the editor's timeout must kill it)
//   MONOLITH_STUB_CLASSIFIER_LOG=<file>, else MONOLITH_STUB_LOG=<file>
//                                appends what it was given to <file>: a line "stub-classifier argv:", one
//                                argument per line (each prefixed with "  arg: "), then "stub-classifier stdin:"
//                                and the input as it came. Appends, so a stub terminal that replaces
//                                MONOLITH_STUB_LOG later in the same test doesn't hide it: give the classifier
//                                its own file with MONOLITH_STUB_CLASSIFIER_LOG when both run.
//
// Exit status: 0 (answers, garbage, outside, slow when it is not killed); 1 (fail, or an unknown mode).
//
// Called by: the editor (src/classifier.c) in launcher tests, build/tools/leakcheck, build/tools/test-coordinator.

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
            fprintf(f, "stub-classifier argv:\n");
            for (int i = 0; i < argc; i++) fprintf(f, "  arg: %s\n", argv[i]);
            fprintf(f, "stub-classifier stdin:\n");
            fwrite(in.items, 1, in.count, f);
            fclose(f);
        } else {
            ToolErr("%s: %s", log, strerror(errno));
        }
    }
    nob_sb_free(in);

    const char *mode = ToolEnv("MONOLITH_STUB_CLASSIFIER", "default");
    const char *model = ArgAfter(argc, argv, "--model");
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
