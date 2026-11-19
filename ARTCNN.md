# Fullscreen hack ArtCNN C4F16 (experimental)

This optional spatial upscaler runs inside Wine's win32u Vulkan fullscreen
hack. It uses the original [ArtCNN C4F16 convolution weights][upstream] to
reconstruct luma at 2×, then resamples that luma to the output size and combines
it with bilinearly sampled RGB chroma. It does not apply FSR/RCAS sharpening or
FSR saturation adjustment on top of ArtCNN. This is an SDR game RGB adaptation,
not a pixel-identical replacement for mpv's video color pipeline.

Set the environment before starting Wine and select a lower **fullscreen**
resolution in the game. For a 1920×1080 output, start with 960×540 if available:

```sh
WINE_FULLSCREEN_ARTCNN=1 WINE_FULLSCREEN_FSR=lite WINEDEBUG=+vulkan wine game.exe
```

- `WINE_FULLSCREEN_ARTCNN=1` opts in. Unset or any other value keeps the default
  scaler behavior. The FSR settings above only choose the fallback; if FSR
  is disabled or cannot initialize, Wine tries ordinary fullscreen scaling
  (blit). A lost device is reported to the game rather than retried.
- Both output axes must be strictly greater than 1.3× the input. The FSR
  resolution presets and `WINE_FULLSCREEN_FSR_CUSTOM_MODE` are also available
  when only ArtCNN is enabled. See [FSR settings](FSR.md).
- Look for `scaler ArtCNN C4F16` in the Vulkan log and the reported scratch size.
  An `ArtCNN C4F16 unavailable` line means initialization was skipped or failed;
  check the earlier warning for the cause and the `scaler` line for what ran.
- Requires a single-layer SDR BGRA8 swapchain, a compute-capable present queue,
  sampled/storage support for RGBA16F, and storage writes without a shader image
  format for BGRA8. Wine checks the latter using the host's Vulkan 1.3 format
  properties or `VK_KHR_format_feature_flags2`. All of these scalers use the
  existing fullscreen hack and still require a surface with storage-image
  usage. The option does not force Wine into the fullscreen hack and does not
  apply to native OpenGL, WineD3D's OpenGL path, or VMR7/DirectDraw video
  presentation.
- The model is substantially heavier than FSR. Each swapchain image owns three
  RGBA16F work textures (~84 MiB **per image** at 1280×720 input, before driver
  padding). Wine falls back if combined scratch allocation would exceed 256 MiB
  or if ArtCNN setup fails. At 960×540, the seven convolutions require about
  6.35 billion multiply-accumulates per frame, before the final resampling.
  Start at a low input resolution and compare frame times against FSR on the
  target GPU. With ArtCNN enabled, a smaller explicit mode such as
  `WINE_FULLSCREEN_FSR_CUSTOM_MODE=640x360` is available if 960×540 is too slow.

## Shader regeneration and verification

The [vendored model](dlls/win32u/artcnn_c4f16.glsl) is pinned to the upstream
revision in the [generator](dlls/win32u/artcnn_generate.py). It and the embedded
SPIR-V retain the MIT notice. Build-time regeneration is not required. With
Python ≥3.9, `glslangValidator` 12.0.0 and SPIRV-Tools 2023.1 on `PATH`:

```sh
python3 dlls/win32u/artcnn_generate.py --check
```

The generator validates each Vulkan 1.0 SPIR-V module and compares the header
byte-for-byte; omit `--check` to rewrite it. Convolutions use FP32 arithmetic,
RGBA16F storage, and zero padding as in the upstream ONNX model. The final pass
clamps luma at the frame edges and preserves bilinearly resampled alpha.

The optional [numerical regression](dlls/win32u/artcnn_test.py) runs the
embedded modules on a Vulkan compute device and checks all eight passes
against a CPU reference built from the pinned [upstream ONNX model][onnx]. It
requires Python 3.9+, NumPy 1.20+, `onnx`, and the Python `vulkan` package;
these are not Wine build dependencies. Download the ONNX file, then run:

```sh
python3 dlls/win32u/artcnn_test.py --onnx /path/to/ArtCNN_C4F16.onnx
python3 dlls/win32u/artcnn_test.py --onnx /path/to/ArtCNN_C4F16.onnx --input-size 8x9 --output-size 16x18
```

Mesa Lavapipe passed the default 9×11 → 17×19 case, exact 2× (8×9 → 16×18),
and a single-pixel-wide input (1×3 → 3×7). Across the three cases the largest
intermediate absolute difference was 0.00390625 and the maximum final difference
was one 8-bit channel code value. The reference quantizes to FP16 between passes.
Set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` if the layer is installed.

A separate Lavapipe headless swapchain smoke test exercised Wine's ArtCNN
initialization and command recording at 8×8 → 16×16, with successful Vulkan
acquire, submit and present and no synchronization validation errors. This
isolated test did not run the full Wine client or a game.

## Target-driver test

Use a build from this source and a game that presents through Wine Vulkan
(for example through DXVK). For a 1920×1080 desktop, set
`WINE_FULLSCREEN_FSR_CUSTOM_MODE=960x540` if the game cannot otherwise select
960×540. Compare the same scene with `WINE_FULLSCREEN_ARTCNN=1` and with it
unset, keeping `WINE_FULLSCREEN_FSR=lite` and the input resolution identical.
Check the `scaler` line each time before comparing screenshots, frame times,
and memory. In particular, use frame times rather than average FPS alone;
shader compilation at swapchain creation can also introduce a startup hitch.

To exercise fallback, select an input mode with at least one output/input ratio
at or below 1.3 while still engaging the fullscreen hack (for example 1600×900
→ 1920×1080). Expect the ratio warning and `scaler FSR lite`. Repeat with FSR
unset to verify `scaler blit`. Also test non-multiple-of-8 sizes, both 2× and
non-integer output ratios, and fullscreen exit/re-entry. Shader validation
alone does not verify Wine presentation.

[upstream]: https://github.com/Artoriuz/ArtCNN/blob/a1b6ace035df4a9e9a113713dd7ee4eb92dd67ea/GLSL/ArtCNN_C4F16.glsl
[onnx]: https://github.com/Artoriuz/ArtCNN/blob/a1b6ace035df4a9e9a113713dd7ee4eb92dd67ea/ONNX/ArtCNN_C4F16.onnx
