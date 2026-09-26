# Shared SDL display

Both the DataRover (MIPS) and the 68k frontends use
`src/frontend/sdl/display.c` and `display.h` for host presentation.

The component owns the window, renderer, streaming texture, and ARGB pixel
buffer. It requests an accelerated vsync renderer, falls back to software,
and reports the actual renderer. Texture resizing allocates the replacement
before releasing existing resources; unchanged dimensions reuse the buffer.

`mh_sdl_display_gray` converts either runtime frame format (0..255
grayscale or 0..3 inverted LCD levels) to identical ARGB gray values.
Presentation uses the shared aspect-ratio/letterbox calculation, minus the
width of the control rail. Each frontend supplies its panel dimensions,
integer-scaling preference, and background shade. The optional dot
structure and backlight tint (`--lcd`, `--tint`, `src/frontend/sdl/lcd.c`)
prepare pixels in the shared buffer before presentation.

The frontends still own event handling, execution pacing, audio,
screenshot keys and device switching. Device framebuffer decoding and LCD
register behaviour remain in the machine models.

This component does not make guest drawing atomic. Vsync synchronizes host
presentation; a framebuffer snapshot may still contain a guest redraw in
progress. No frame-completion heuristic or new display timing is
introduced.

Tests run under SDL's dummy driver with pixel readback for both frame
formats, texture reuse, invalid-size preservation, repeated LCD cell
resizes, and cleanup (`tests/frontend/test_display.c`,
`tests/frontend/test_lcd.c`). Builds without SDL retain unavailable-GUI
stubs.
