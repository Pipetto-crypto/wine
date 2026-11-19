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

The game must actually select an exposed lower resolution for upscaling to take
place. Restart Wine when changing environment controls. The Vulkan debug log
reports whether FSR or lite is enabled for each fullscreen-hack swapchain.

The intermediate format must support sampled and storage images. If it does
not, Wine uses ordinary fullscreen scaling. A surface without storage-image
usage cannot use this compute fullscreen hack and returns
`VK_ERROR_FORMAT_NOT_SUPPORTED`.

Source: [requested patch](https://github.com/as14725836/termux-glibc-mangohud/blob/main/0001-fshack-Implement-AMD-FSR-upscaler-for-fullscreen-hac.patch),
commit `92fbaabc91a040584dde1c809f1bd980b8147250`, by Stelios Tsampas, with
Georg Lehmann, GloriousEggroll, and Ph42oN. Embedded shader binaries are preserved;
the port corrects intermediate-image barriers, presentation layout, RCAS edge
coordinates, and initialization failure cleanup in the host implementation.
The AMD shader notice is retained from
[ffx_fsr1.h](https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/ffx-fsr/ffx_fsr1.h).

Validation before device testing: review the ported diff, check patch whitespace,
and verify the three SPIR-V instruction streams are structurally complete.
These checks do not replace a Wine build, SPIR-V semantic validation, or GPU
runtime tests. Device validation should cover normal/lite/off, changing fullscreen
resolution, resize/minimize/restore, and swapchain recreation, including the
right and bottom image edges.
