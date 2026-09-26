# PIC-2000 speaker playback

Reconstructed from the PIC-2000 ROM's register accesses and interrupt
handlers. The audio ASIC has not been identified. This is a separate 68k
device model, not the DataRover's UCB1100 codec (see [AUDIO.md](AUDIO.md)).
HIX-300 and Envoy use the same engine with board-specific differences
noted in [HIX300.md](HIX300.md) and [ENVOY.md](ENVOY.md).

## Observed protocol

| dev21 offset | Playback behavior / ROM evidence |
| --- | --- |
| `4E` | Start bit 0: `0E0AA05A` writes `1006`, `0E0AA09C` sets bit 0; stop at `0E0AA0E8` writes `0008`. Recording uses a different configuration and is not implemented. |
| `50` | DMA buffer address divided by 4096; written at `0E0AA020` from RAM global `0734`. |
| `52` | Sample-rate encoding selected at `0E0A9FBE..0E0AA024`. |
| `54` | Bit 10 is set during speaker shutdown and cleared during startup. The model gates output with it while DMA continues. Other gain/input functions are not implemented. |
| `56` | Attenuation fields at bits 11:8 and 7:4. The ROM maps volume 0..100 to codes 15..0 at `0E0AB38A..0E0AB3B8`. The hardware transfer curve is unknown; the intermediate curve is an explicitly unverified approximation. |
| `B0/B4` | Pending/enabled DMA sources 18 and 19. `0E0AA13E` acknowledges both by writing `000C` to the high word of B0 and requests more sound from the mixer. |

The rate encodings are `D20=7350`, `C30=8820`, `B40=11025`, `A50=14700`,
`950=17640`, `960=22050` Hz. These match the SDK's `Utilities.h` constants
`k7kHzSampleRate` through `k22kHzSampleRate`. The ROM compares the integer
part of the requested 16.16 rate and selects the encoding. This establishes
the supported selections, not the physical oscillator or divisor formula.
Unknown encodings do not generate speculative samples or interrupts, except
under the inferred divider that is HIX-300's default and is otherwise
enabled with `--experimental-audio-divider`.

The mixer builds signed 16-bit linear samples: `0E0AC55A` constructs a
table beginning with -32768 and saturating at +32767. `0E0ADCCC` copies
words through that table into the buffer and wraps the write offset with
`0FFF`. The refill routine at `0E0ADCAE` calls the mixer four times; each
call writes 256 words (two 128-word passes or one 256-word pass). Each
refill therefore supplies 2048 bytes of a 4096-byte ring. The model reads
big-endian signed words and raises a DMA source at each half-ring boundary.
Assignment of source 18 to half and 19 to full is inferred; the ROM uses
the same handler and acknowledges both together.

## Implementation and limits

- `src/machines/pic2000/audio.cpp` advances DMA from emulated CPU cycles,
  including while no host audio sink exists. Only actual buffer
  consumption produces the modeled IRQ sources.
- The runtime provides mono PCM at a fixed 44.1 kHz to SDL or WAV
  capture. Source samples are held between DMA reads, then filtered using
  the unverified response described below.
- SDL opens audio by default and bounds queued latency. `--no-audio`
  disables host output while guest playback and interrupts continue.
- `--wav` (also `--audio-wav`) captures headless output. GUI and WAV
  capture together are rejected rather than silently replacing one sink.
- Hardware-verified intermediate attenuation, analog filtering,
  stereo/headphone behavior, microphone recording, clipping detection, and
  additional DMA modes are not implemented. The zero-volume endpoint and
  the observed shutdown mute work; this is usable speaker playback, not
  complete codec fidelity.

No ROM instructions, RAM shutdown cookies or OS callbacks are patched or
invoked by the host. The power button (F4) gates speaker output.

## Validation

A temporary session using retained PIC RAM was in the Storeroom. An
ordinary tap on a close button requested a guest sound. Before the audio
model, the ROM enabled playback (`4E=1007`) but never received a refill
interrupt and never stopped. With the model, the ROM reached `0E0AA13E`
twice and stopped playback normally (`4E=0008`). The WAV contains about
0.186 seconds of nonzero output, peak absolute sample 24218, with no
ignored writes or undecoded bus accesses. An SDL mouse event repeated this
result using dummy SDL video and audio drivers, which validates the guest
and SDL queue path.

Tests cover signed conversion, rate selection, half/full refill events and
write-one-to-clear, mute while DMA continues, and power-off:

```sh
ctest --test-dir build -R 'pic2000_audio|pic2000_runtime' --output-on-failure
```

## Unverified output response

Volume attenuation and filtering are enabled by default and identified as
unverified at startup. Use `--no-pic-audio-approx` for raw PCM comparison.
The ROM establishes two reversed 4-bit volume fields, not their channel
routing. The model assumes linear amplitude `(15-code)/15` for each field
and averages their gains for mono output. It does not assume the UCB1100's
3 dB steps or 12-bit input.

Two cascaded one-pole low-pass filters run at 44100 Hz with each pole at
0.4 times the selected source rate. Each uses `y += alpha*(x-y)`, with
`alpha = 1-exp(-2*pi*0.4*source_rate/44100)`. This is a deliberately
chosen smoothing response, not a recovered analog circuit. Attenuation and
mute follow the filter. Filtering advances while muted and without a host
sink; playback stop and power-off clear its history, and source-rate
changes update the coefficient. Output processing changes neither DMA nor
IRQ timing. Tests measure all 16 equal-field volume settings, low and high
tone response, DMA timing, and silence/reset on power-off. These validate
the approximation, not hardware fidelity.

## Host servicing schedule

The shared 68k machine services audio on the next 44.1 kHz output-sample
boundary, rounded to an execution slot, and immediately after
audio-register or power changes. Each deadline derives from the existing
sample phase, which avoids per-instruction bookkeeping without changing
sample rate, DMA consumption, or interrupt deadlines. Changing the CPI or
divider setting invalidates the deadline. HIX (200 million slots) and Envoy
(80 million slots) before/after comparisons produced byte-identical WAV and
RAM dumps.

## Cross-device validation

Fresh PIC-2000, Envoy and HIX calibration replays each captured nonzero
PCM through this shared engine; see [68k parity](68K_PARITY.md). These
runs validate delivery and DMA progress, not analog accuracy.
