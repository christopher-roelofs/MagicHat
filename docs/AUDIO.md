# DataRover 840 audio

## Output path

The TX39 SIB reads signed big-endian samples from the guest DMA ring. The
UCB1100 output model then decodes the top 12 bits, removes DC, interpolates
and low-pass filters, applies attenuation and mute, and delivers signed
16-bit mono PCM to SDL or `--audio-wav`.

The guest DMA rate remains approximately 11077 Hz for the usual ROM
settings. Host output is **2x** that rate (currently 22152 Hz, due to
integer truncation of the underlying 11076.923 Hz). This represents
reconstruction between guest samples, not faster DMA or a pitch change. The
0.6*Fs stopband lies above the guest Nyquist frequency, which is why
filtering uses an oversampled output.

Implementation:

- `src/soc/tx39/ucb1100_audio.c`: codec output and filter coefficients.
- `src/soc/tx39/tx39_sib.c`: DMA timing, interrupts and codec delivery.
- `src/machines/datarover840/runtime.c`: reconstructed output rate to SDL.
- `src/frontend/sdl/datarover_gui_sdl.c`: host buffering and rate changes.

Codec/filter history is per machine and saved in the snapshot. States older
than snapshot version 5 lack it and are rejected; boot afresh to create new
states.

The 68k machines have a separate audio path; see
[PIC2000_AUDIO.md](PIC2000_AUDIO.md).

## Documented controls and model assumptions

Reference: Philips UCB1100 datasheet v1.2, 1998-05-08, sections 6.1, 6.7
and 8.

- The serial input is left-justified signed 12-bit data. The four low bits
  are ignored, including for negative samples. Output PCM retains the usual
  16-bit full-scale convention; it is not reduced to 1/16 loudness.
- Register 8 bit 15 enables the DAC. Disabled output is zero and the model
  clears filter history. Reset timing/transients are an approximation.
- Register 8 bit 13 mutes the speaker after filtering. Filter state
  continues advancing while muted; DMA and interrupts continue in both
  cases.
- Register 8 bits 4:0 select attenuation. Codes 0..23 use nominal 3 dB steps
  through -69 dB. **Codes 24..31 currently saturate at -69 dB.** The
  preliminary sheet states 24 steps but describes a five-bit field ending at
  31 without a complete encoding table. This saturation policy is not
  hardware-verified.
- Register 7 input gain affects the microphone path, not speaker gain, and
  is not applied here. Loopback, capture and telecom processing are not
  implemented.

## Filtering: response approximation, not exact silicon

The datasheet specifies the output response but does not give its internal
FIR/IIR coefficients. The model therefore uses a response-based
approximation:

1. DC blocker: `y = 0.999875*(x-x_previous) + 0.99975*y_previous` at guest
   Fs. The chosen pole puts the response at 0.00016*Fs within the published
   0.5 dB passband limit. It is not a recovered hardware pole.
2. Twofold zero-insertion followed by a 127-tap Blackman-windowed sinc,
   cutoff 0.51*Fs, normalized to gain 2. Coefficients are fixed in the
   source. For tap i, use x=i-63, `sin(2*pi*0.255*x)/(pi*x)` (0.51 at x=0),
   multiplied by `0.42-0.5*cos(2*pi*i/126)+0.08*cos(4*pi*i/126)`, then
   normalize the sum to 2.
3. Output attenuation, signed 16-bit saturation/rounding, then mute.

The model does not reproduce the 64x sigma-delta modulator, analogue
smoothing circuit, speaker distortion, exact phase response, or the chip's
internal fixed-point rounding. Reconstruction adds 31.5 guest samples of
group delay. It provides an output response approximation, not a
transistor-accurate DAC.

Tests check signed endpoints and discarded bits, attenuation, mute
continuity, DAC disable, idle -768 DC rejection, passband tones and
reconstruction-image rejection. Measured passband gain: -0.261 dB at
0.00016*Fs and approximately 0 dB at 0.01, 0.1, 0.3 and 0.42*Fs. Image
rejection at 0.6*Fs is about 90.5 dB against the specified minimum 70 dB.
These tests do not prove all-frequency hardware equivalence or audible
freedom from crackle.

## Host playback and remaining timing limits

SDL opens on the first sample and follows rate changes. It queues two
512-sample periods of startup silence and limits queued audio to 250 ms at
the actual rate. Guest DMA and codec processing run without a host audio
sink. `--no-audio` silences the window.

Host underruns are separate from codec filtering. Slow emulation, dropped
batches and gaps between short sounds may still crackle. Rate changes
reopen the device, discarding pending output. Partial batches and final
filter tails are not flushed at shutdown. Filters advance only when DMA
supplies a sample; free-running codec output while DMA is stopped is not
modelled.

SIBCLK is assumed rather than derived from all clock controls. The NetBSD
TX39 driver (`sys/arch/hpcmips/tx/tx39sib.c`) and the Philips formula
support Fs=2*SIBCLK/(64*divisor) for the usual configuration, but SCLKDIV,
the codec's independent divisor, serial valid-sample handshakes and
arbitrary DMA modes still need work. Control frames run per machine tick.

`--audio-wav` contains **post-codec output**, not raw DMA samples. Its rate
is fixed at opening: capture from a state with the sound rate already
programmed. Cold-boot and changing-rate WAV timing remain unsupported. GUI
playback replaces the WAV sink; simultaneous GUI/WAV recording is
unsupported.

Both frontends prefer Wayland automatically when `WAYLAND_DISPLAY` is set;
an explicit `SDL_VIDEODRIVER` setting still takes precedence. Dummy-driver
runs cannot verify audible quality or real sound-card scheduling.

## Integration validation

A fresh ROM boot reached calibration and the Desk with the codec model.
Calibration captured 49,152 post-codec samples at 22,152 Hz, with sample
range -12,832..10,869. This validates boot and signal flow; listening on the
real backend is still needed to assess crackle.
