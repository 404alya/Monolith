# Benchmark 2026-10-26 09:20:12

> profile tool test

## Setup

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2 |
| Driver | NVIDIA Corporation / 3.3.0 NVIDIA 615.71.09 |
| CPU | AMD Ryzen 7 7435HS |
| System | Linux 7.2.5-1-cachyos (x86_64) |
| Resolution | 1920 x 1006 |
| Map | `/path/to/monolith/maps/monolith/monolith.map` (format 2, 52244 boxes, 106 materials, 291 lights) |
| R3D build | RelWithDebInfo |
| Quality | ultra (graphics settings: preset, or custom; the settings file is not read by --bench) |
| Sun shadow | 8192 px, range 100 m, re-rendered only when the view leaves the cached map |
| Effects | volFog on, fog on, SSAO on (16 samples, radius 1.2), SSIL on (16 samples, radius 5), bloom on (intensity 0.04, levels 0.5), exposure 0.9 |
| Vsync | app off; `__GL_SYNC_TO_VBLANK=0`, `vblank_mode=(unset)` |
| Window | headless: hidden window, offscreen target, glFinish per frame (no presentation) |
| Method | per viewpoint: 60 warmup frames, then 150 timed frames while the camera spins 360 deg |
| GPU profiler | on: per-pass GL_TIME_ELAPSED queries (R3D patch 5) |
| Command | `/path/to/monolith/build/linux-profile/monolith` `--bench` `--headless` `--bench-frames` `150` `--note` `profile tool test` |

## Results

| View | Avg ms | Avg FPS | 1% low FPS | Worst ms | Shadow updates |
|---|---:|---:|---:|---:|---:|
| Canyon | 5.308 | 188.4 | 131.0 | 7.63 | 0 / 150 |
| Hypostyle hall | 5.654 | 176.9 | 122.5 | 8.16 | 0 / 150 |
| Above the roof | 5.599 | 178.6 | 127.5 | 7.84 | 0 / 150 |
| Stairs | 5.556 | 180.0 | 141.0 | 7.09 | 0 / 150 |
| Terrace | 4.800 | 208.3 | 129.8 | 7.71 | 0 / 150 |
| Tower | 4.363 | 229.2 | 147.8 | 6.77 | 0 / 150 |
| Overview | 5.357 | 186.7 | 138.9 | 7.20 | 0 / 150 |
| **All** | **5.234** | **191.1** | | | 0 / 1050 |

## GPU passes

Average GPU time per frame of each R3D pass (passes that didn't run on a frame count as 0, so e.g. shadow maps show their re-render cost spread over all frames). *GPU total* spans `R3D_End()`; *untimed rest* is that minus the passes (G-buffer clear, state changes, gaps). *CPU until finish* is the CPU time from the start of the frame until the `glFinish` that waits for the GPU, *CPU in R3D_End* the part spent inside `R3D_End()` (command submission); *frame time* is the regular average above.

| GPU ms per frame | Canyon | Hypostyle hall | Above the roof | Stairs | Terrace | Tower | Overview | **All** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shadow min/max | 0.001 | 0.001 | 0.001 | 0.001 | 0.001 | 0.001 | 0.001 | **0.001** |
| geometry | 2.225 | 1.980 | 1.913 | 2.003 | 1.318 | 1.118 | 1.805 | **1.766** |
| lights | 0.186 | 0.261 | 0.288 | 0.254 | 0.518 | 0.364 | 0.263 | **0.305** |
| depth pyramid | 0.066 | 0.052 | 0.071 | 0.049 | 0.061 | 0.074 | 0.061 | **0.062** |
| SSAO | 0.257 | 0.184 | 0.158 | 0.176 | 0.103 | 0.092 | 0.146 | **0.160** |
| SSIL | 0.730 | 0.610 | 0.493 | 0.592 | 0.275 | 0.187 | 0.355 | **0.463** |
| ambient | 0.227 | 0.219 | 0.201 | 0.220 | 0.107 | 0.093 | 0.182 | **0.178** |
| compose | 0.131 | 0.148 | 0.144 | 0.144 | 0.043 | 0.040 | 0.091 | **0.106** |
| background | 0.005 | 0.009 | 0.013 | 0.010 | 0.029 | 0.031 | 0.013 | **0.016** |
| fog | 0.061 | 0.068 | 0.075 | 0.067 | 0.050 | 0.053 | 0.064 | **0.062** |
| vfog transmit | 0.036 | 0.036 | 0.036 | 0.036 | 0.036 | 0.036 | 0.036 | **0.036** |
| vfog radiance | 0.170 | 0.649 | 0.852 | 0.673 | 1.018 | 1.071 | 1.072 | **0.786** |
| vfog upsample | 0.096 | 0.149 | 0.165 | 0.130 | 0.139 | 0.152 | 0.169 | **0.143** |
| forward | 0.002 | 0.002 | 0.003 | 0.004 | 0.006 | 0.005 | 0.003 | **0.003** |
| bloom | 0.137 | 0.167 | 0.153 | 0.159 | 0.133 | 0.131 | 0.132 | **0.145** |
| tonemap | 0.064 | 0.066 | 0.069 | 0.064 | 0.065 | 0.064 | 0.064 | **0.065** |
| FXAA/SMAA | 0.104 | 0.114 | 0.099 | 0.112 | 0.090 | 0.089 | 0.098 | **0.101** |
| blit | 0.001 | 0.001 | 0.001 | 0.001 | 0.001 | 0.001 | 0.001 | **0.001** |
| **sum of passes** | 4.496 | 4.717 | 4.735 | 4.693 | 3.990 | 3.602 | 4.557 | **4.399** |
| untimed rest | 0.056 | 0.057 | 0.056 | 0.061 | 0.054 | 0.057 | 0.054 | **0.056** |
| **GPU total (R3D)** | 4.552 | 4.774 | 4.791 | 4.754 | 4.044 | 3.659 | 4.611 | **4.455** |
| CPU until finish | 0.685 | 0.799 | 0.720 | 0.743 | 0.696 | 0.641 | 0.683 | **0.710** |
| CPU in R3D_End | 0.557 | 0.635 | 0.576 | 0.597 | 0.566 | 0.516 | 0.551 | **0.571** |
| **Frame time** | 5.308 | 5.654 | 5.599 | 5.556 | 4.800 | 4.363 | 5.357 | **5.234** |
