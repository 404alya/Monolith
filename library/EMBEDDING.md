# The embedding model

The model catalog's vectors (`library/library.db`, `docs/model-library.md` §5) are made by one
model file. It is not in the repo: it lives outside it, in `~/.cache/monolith/embed/` (or the
folder `$MONOLITH_EMBED_DIR` names), and `build/tools/library` checks it against the sha256
below (`meta.embed_sha256` in the database) the first time a run needs it.

| | |
|---|---|
| Model | Qwen3-Embedding-0.6B (Qwen team, Alibaba Cloud) |
| File | `Qwen3-Embedding-0.6B-Q8_0.gguf` |
| Source | https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF (file `Qwen3-Embedding-0.6B-Q8_0.gguf`; repository revision `370f27d7550e0def9b39c1f16d3fbaa13aa67728` on 2026-11-20) |
| Download URL | https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF/resolve/main/Qwen3-Embedding-0.6B-Q8_0.gguf |
| Hosts | `huggingface.co` answers with a 302 redirect to `us.aws.cdn.hf.co` (Hugging Face's Xet CDN, a signed URL), which serves the file (seen on 2026-11-20 with `curl -sIL`; the CDN host can differ by region) |
| Licence | Apache 2.0 (the model card; https://www.apache.org/licenses/LICENSE-2.0) |
| Size | 639,150,592 bytes (610 MiB) |
| sha256 | `06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439` (the repository's LFS hash for the file, checked on download) |
| Quantisation | Q8_0 (8-bit weights) |
| Dimensions | 1024; the catalog keeps the first 512 (Matryoshka), L2-normalised, int8 per vector |
| Pooling | last token (`<|endoftext|>`, which the GGUF's tokenizer appends itself) |
| Runs on | the vendored llama.cpp v1.0.0 (`src/vendor/llama.cpp/`), CPU only, 4 threads |

## Getting it

```
python3 -I tools/fetch_asset.py huggingface get Qwen/Qwen3-Embedding-0.6B-GGUF Qwen3-Embedding-0.6B-Q8_0.gguf \
    --expect-sha256 06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439
```

then move the file into `~/.cache/monolith/embed/` (or point `MONOLITH_EMBED_DIR` at its
folder). `fetch_asset.py` refuses a file whose sha256 differs. Without the file, the commands
that need it (`embed`, `search` and `eval` with query texts, `check`) stop with exit 3 and say
so; `--embedder stub` runs without it (tests only: its vectors mean nothing).

## Changing the model

Every vector in the catalog belongs to this file: another model, quantisation or llama.cpp
release gives other vectors. Change it only together with the catalog: record the new file
here and in `tools/library.c` (`EMBED_*`), embed every row again (`library embed --all` on a
database whose `meta` names the new file), compare `library eval` before and after, and commit
the database with its dump.
