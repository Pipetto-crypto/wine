# Fullscreen hack AMD FSR 1

This port adds EASU upscaling and RCAS sharpening to the existing win32u Vulkan
fullscreen hack. It is spatial FSR 1, not FSR 2/3 or frame generation. It applies
when Wine's fullscreen hack is active; setting these variables alone does not
force every application to use that presentation path. Native OpenGL rendering
is not covered by this Vulkan implementation.

Set the environment before starting Wine, then select a lower fullscreen
resolution in the game. For example:

```sh
WINE_FULLSCREEN_FSR=1 WINE_FULLSCREEN_FSR_MODE=quality wine game.exe
```

Controls inherited from the source patch:

- `WINE_FULLSCREEN_FSR=1`: enable FSR; `0` or unset disables it.
- `WINE_FULLSCREEN_FSR=lite`: use the lighter EASU shader.
- `WINE_FULLSCREEN_FSR_MODE`: `ultra`, `quality`, `balanced`, or `performance`.
  Without a single mode, the corresponding FSR resolutions are exposed together.
- `WINE_FULLSCREEN_FSR_CUSTOM_MODE=1280x720`: request a custom input resolution.
- `WINE_FULLSCREEN_FSR_STRENGTH`: RCAS attenuation in tenths of a stop; default
  `2`. Smaller values give stronger sharpening. This port bounds the input to
  `0`–`10`.
- `WINE_FULLSCREEN_FSR_SATURATION`: SDR saturation in percent, bounded to
  `100`–`150`, default `100`. Try `110`–`115` for a mild boost. RCAS limits the
  chroma gain near the RGB range boundaries to preserve luma and avoid clipping
  individual channels. The setting applies to SDR BGRA8 FSR swapchains.

For more detail, use full FSR with `ultra` mode and select the corresponding
higher input resolution in the game. Try `WINE_FULLSCREEN_FSR_STRENGTH=1` for
slightly stronger sharpening. Higher input resolution increases rendering cost.

The game must actually select an exposed lower resolution for upscaling to take
place. Restart Wine when changing environment controls. The Vulkan debug log
reports the selected scaler (`FSR`, `FSR lite`, `ArtCNN C4F16`, or `blit`) for
each fullscreen-hack swapchain. See [ArtCNN](ARTCNN.md) for the optional neural
scaler.

The intermediate format must support sampled and storage images. If it does
not, Wine uses ordinary fullscreen scaling. A surface without storage-image
usage cannot use this compute fullscreen hack and returns
`VK_ERROR_FORMAT_NOT_SUPPORTED`.

Source: [requested patch](https://github.com/as14725836/termux-glibc-mangohud/blob/main/0001-fshack-Implement-AMD-FSR-upscaler-for-fullscreen-hac.patch),
commit `92fbaabc91a040584dde1c809f1bd980b8147250`, by Stelios Tsampas, with
Georg Lehmann, GloriousEggroll, and Ph42oN. The two EASU shader binaries are
preserved. The RCAS shader includes the SDR saturation control and guards
stores outside the output extent. The AMD shader notice is retained from
[ffx_fsr1.h](https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/ffx-fsr/ffx_fsr1.h).

The embedded RCAS binary can be regenerated with glslangValidator 12.0.0 and
`ffx_a.h` and `ffx_fsr1.h` from the `ffx-fsr` directory of AMD's repository at
revision `a21ffb8f6c13233ba336352bdff293894c706575`.

```sh
glslangValidator -V --target-env vulkan1.0 -Os -I/path/to/ffx-fsr \
  -o fsr_rcas.spv dlls/win32u/fsr_rcas.comp
spirv-val --target-env vulkan1.0 fsr_rcas.spv
```

Embed the output as little-endian 32-bit words in `fsr_rcas_comp_spv`.
Device validation should cover normal/lite/off, SDR saturation at 100 and 115,
changing fullscreen resolution, resize/minimize/restore, and swapchain recreation,
including the right and bottom edges at resolutions that are not multiples of 8.
