# llama.cpp (vendored, CPU only)

ggml and libllama from llama.cpp, which run the embedding model of the model catalog
(Qwen3-Embedding-0.6B, `library/EMBEDDING.md`) on the CPU for `library embed`, `search` and
`eval` (`tools/library.c`, `docs/model-library.md` §5). Only `build/tools/library` and its unit
test link it.

| | |
|---|---|
| Source | https://github.com/ggml-org/llama.cpp |
| Release | tag `v1.0.0` (published 2026-11-13; the latest release on 2026-11-20), commit `b1b770d90d94cad9e9ea5b993bdbc383a522fb28` (the annotated tag object is `a6d1a74dbebce3a16f4f2ceddbf38c21a08d8674`) |
| Archive | https://github.com/ggml-org/llama.cpp/archive/refs/tags/v1.0.0.tar.gz, sha256 `3f5f750e36326aea4d65c7623e7d597a33e395d65f4d76e5e0532171f252532b` |
| Files | from the archive, byte for byte: `LICENSE`, `include/`, `src/` (with `src/models/`), `ggml/include/`, and of `ggml/src/` its top-level files and `ggml-cpu/` (the CPU backend). Left out: the GPU and other backends (`ggml/src/ggml-cuda/`, `-vulkan/`, `-metal/`, `-sycl/`, `-opencl/`, …), `ggml/cmake/`, `common/`, `tools/`, `examples/`, `tests/`, `models/`, `vendor/`, the Python and build scripts |
| Licence | MIT (`LICENSE`, "Copyright (c) 2023-2026 The ggml authors"), sha256 `94f29bbed6a22c35b992c5c6ebf0e7c92f13b836b90f36f461c9cf2f0f1d010d` |
| Downloaded | 2026-11-20 |
| Changes | none. The upstream `CMakeLists.txt` files are kept as they are, unused |

Every file here is byte-identical to the same path in the archive (check: unpack the archive
into an empty folder and `diff -r` each kept folder).

How it is built (`nob.c`, function `LibraryVendor`, run by `./build.sh --tools library` and
`./build.sh test library`; nothing here goes through CMake):

- **Sources:** upstream's CMake lists for a static CPU-only build on x86-64: ggml-base (`ggml.c`,
  `ggml.cpp`, `ggml-alloc.c`, `ggml-backend.cpp`, `ggml-backend-meta.cpp`, `ggml-opt.cpp`,
  `ggml-threading.cpp`, `ggml-quants.c`, `gguf.cpp`), ggml (`ggml-backend-dl.cpp`,
  `ggml-backend-reg.cpp`), the CPU backend (`ggml-cpu/`: `ggml-cpu.c`, `ggml-cpu.cpp`,
  `repack.cpp`, `hbm.cpp`, `quants.c`, `traits.cpp`, `amx/`, `tiled/`, `binary-ops.cpp`,
  `unary-ops.cpp`, `vec.cpp`, `ops.cpp`, `arch/x86/quants.c`, `arch/x86/repack.cpp`,
  `llamafile/sgemm.cpp`), and libllama (every `src/*.cpp` and `src/models/*.cpp`): 239 files with
  SQLite and sqlite-vec, in parallel through nob's pool, into `build/tools-obj/library/ggml-c/`
  (C) and `cxx/` (C++), each folder with its `.flags` stamp, only what is stale; packed with
  SQLite into `build/tools-obj/library/libvendor.a` (`ar rcsD`).
- **Flags:** C as `-std=gnu11`, C++ as `-std=c++17`; `-w` (its warnings are not ours), `-O2`,
  `-DNDEBUG`, `-march=x86-64-v3` (AVX2, FMA, F16C, BMI2) and never `-march=native`, so a text
  gives the same vector on every x86-64-v3 machine with this build (`docs/model-library.md` §5);
  the defines upstream's build sets for that configuration: `_XOPEN_SOURCE=600`, `_GNU_SOURCE`,
  `GGML_SCHED_MAX_COPIES=4`, `GGML_USE_CPU` (the CPU backend registered statically),
  `GGML_USE_LLAMAFILE` and `GGML_USE_CPU_REPACK` (upstream's defaults), `GGML_SSE42`,
  `GGML_F16C`, `GGML_FMA`, `GGML_BMI2`, `GGML_AVX`, `GGML_AVX2`.
- **Not built:** OpenMP (ggml's own thread pool runs with the fixed thread count of
  `tools/library.c`), every GPU backend, `GGML_BACKEND_DL` (no backend is loaded from a file:
  with the CPU backend registered, `llama_backend_init` never searches for one).
- **Generated headers:** CMake would write `ggml-version.h` and `llama-version.h` from the
  `.in` templates; `LibraryVendor` writes them into `build/tools-obj/library/gen/` instead, with
  version `1.0.0` and the commit above (`LLAMA_CPP_VERSION`, `LLAMA_CPP_COMMIT` in `nob.c`).
- **At run time** `tools/library.c` sets `GGML_NO_BACKTRACE=1` before loading the model, so an
  abort inside ggml never starts `gdb` / `lldb` to print a backtrace (ggml's default).

Read for vendoring: the build lists and options (`CMakeLists.txt`, `ggml/CMakeLists.txt`,
`ggml/src/CMakeLists.txt`, `ggml/src/ggml-cpu/CMakeLists.txt`, `src/CMakeLists.txt`), the
public API `include/llama.h`, and the vendored sources searched for network, process and
library-loading calls (`socket`, `connect`, `getaddrinfo`, `popen`, `system`, `exec*`, `fork`,
`dlopen`): none outside the crash backtrace (`ggml.c`, switched off as above) and the dynamic
backend loader (`ggml-backend-dl.cpp`, never reached in this build).

To update: download the archive of a new tag into an empty folder outside the repo, read the
CMake lists for changed source lists or defines, copy the same folders here, update this table,
`LLAMA_CPP_VERSION` / `LLAMA_CPP_COMMIT` and the source lists in `nob.c`, `LLAMA_CPP_RELEASE` in
`tools/library.c` and `docs/architecture.md` §2. A new llama.cpp can change the vectors: embed
the catalog again (`build/tools/library embed --all`) and compare `library eval` before and after.
