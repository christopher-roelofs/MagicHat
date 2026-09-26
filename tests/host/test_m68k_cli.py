"""Synthetic ROMs exercise the loader and diagnostics without archived ROMs."""
import pathlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest

EXE = sys.argv.pop(1)

# The 68k machines are part of the one emulator binary, reached through the
# launcher, so these run the same way a person does.
#
# --headless because the launcher opens a window by default for a 68k board
# and a test must not. BASE_PIC additionally names the machine, for the
# minimal fixture below: it is a reset vector and one branch, with none of the
# strings identification looks for, so nothing can be deduced from it. The
# fixtures that are made into a recognisable Envoy or HIX-300 use BASE and are
# identified the way a real image is.
BASE = [EXE, '--headless']
BASE_PIC = [*BASE, '--device', 'pic2000']


class M68kCli(unittest.TestCase):
    def test_serial_a_is_explicit_and_b_is_not_misrouted(self):
        if sys.platform != 'win32':  # channel A is a pty, which Windows lacks
            result = self.run_guest('--serial', 'a', '-n', '1')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('experimental DUART A', result.stderr)
            self.assertIn('PPP backend unimplemented', result.stderr)
        result = self.run_guest('--serial', 'b', '-n', '1')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn('supports only experimental channel a', result.stderr)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        # The minimal fixture cannot be identified, so the machine is named.
        # A test that rewrites it into a recognisable image clears this and is
        # identified the way a real ROM is.
        self.device = 'pic2000'
        self.rom = pathlib.Path(self.tmp.name) / 'test.rom'
        image = bytearray(4 * 1024 * 1024)
        struct.pack_into('>II', image, 0, 0x100000, 0x02400200)
        image[0x200:0x202] = bytes.fromhex('60fe')  # BRA.S self
        self.rom.write_bytes(image)

    def run_guest(self, *args):
        named = [*BASE, '--device', self.device] if self.device else BASE
        return subprocess.run([*named, '--temporary', '--fresh', '--rom', str(self.rom), '-n', '4', *args],
                              capture_output=True, text=True, timeout=10)

    def test_envoy_reset_window(self):
        r = self.run_guest()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('pc=02400200', r.stderr)
        self.assertIn('0 undecoded reads', r.stderr)

    def test_keyboard_rom_gating(self):
        self.device = None
        for identity, pc, supported in (
            (b',SONY,2,PIC-2000', 0x0e000200, True),
            (b',MOTO,1,Motorola Envoy', 0x02400200, True),
            (b',SONY,1,HIX-300', 0x0e000200, True),
            (b'1,0.31,MOTO,1,Envoy', 0x02400200, False),
        ):
            with self.subTest(identity=identity):
                image = bytearray(4 * 1024 * 1024)
                struct.pack_into('>II', image, 0, 0x100000, pc)
                code = bytes.fromhex('2e7c0010000091c84e6060fe')
                image[0x200:0x200+len(code)] = code
                image[0x300:0x300+len(identity)] = identity
                self.rom.write_bytes(image)
                attached = self.run_guest('--keyboard')
                self.assertEqual(attached.returncode, 0 if supported else 2, attached.stderr)
                if not supported:
                    self.assertIn('unsupported for this ROM', attached.stderr)
                detached = self.run_guest('--no-keyboard')
                self.assertEqual(detached.returncode, 0, detached.stderr)

    def test_stopped_core_fast_path_matches_reference(self):
        """Idle skipping must preserve the event boundary and diagnostics."""
        image = bytearray(self.rom.read_bytes())
        # LPSTOP #$2000 leaves the core stopped until an enabled device event.
        image[0x200:0x206] = bytes.fromhex('f80001c02000')
        self.rom.write_bytes(image)
        command = [*BASE_PIC, '--temporary', '--fresh', '--rom', str(self.rom),
                   '-n', '2000000', '--tap', '100,200,23,24']
        fast = subprocess.run(command, capture_output=True, text=True, timeout=30)
        reference_env = dict(os.environ, MH_68K_IDLE_FAST='0')
        reference = subprocess.run(command, capture_output=True, text=True,
                                   timeout=30, env=reference_env)
        self.assertEqual(fast.returncode, 0, fast.stderr)
        self.assertEqual(reference.returncode, 0, reference.stderr)
        self.assertEqual(fast.stdout, reference.stdout)
        self.assertEqual(fast.stderr, reference.stderr)

    def test_device_resumes_from_its_own_state(self):
        """A device is put down and picked up again.

        There is no power button in this and nothing to confirm: a run that
        ends writes the whole machine beside the firmware, and the next run
        carries on from it. --temporary runs without keeping anything and
        --fresh starts from the ROM, which is what the tests above rely on.
        """
        image = bytearray(self.rom.read_bytes())
        code = bytes.fromhex('2e7c0010000091c84e6060fe')
        image[0x200:0x200+len(code)] = code
        identity = b',MOTO,1,Motorola Envoy'
        image[0x300:0x300+len(identity)] = identity
        self.rom.write_bytes(image)
        state = self.rom.with_suffix('.state')
        command = [*BASE, '--rom', str(self.rom), '-n', '4']

        first = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn('state saved to', first.stderr)
        self.assertTrue(state.exists())
        saved = state.read_bytes()

        # Picked up again: it starts where it stopped, not at the reset vector.
        second = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn('state loaded from', second.stderr)
        self.assertIn('at instruction 4', second.stderr)
        self.assertNotEqual(state.read_bytes(), saved)

        # Kept nothing: the state on disk is the one from before.
        held = state.read_bytes()
        third = subprocess.run(command + ['--temporary'], capture_output=True,
                               text=True, timeout=10)
        self.assertEqual(third.returncode, 0, third.stderr)
        self.assertNotIn('state loaded from', third.stderr)
        self.assertEqual(state.read_bytes(), held)

        # Started over: from the ROM, with the saved state left alone until
        # the run ends.
        fourth = subprocess.run(command + ['--fresh', '--temporary'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(fourth.returncode, 0, fourth.stderr)
        self.assertNotIn('state loaded from', fourth.stderr)
        self.assertEqual(state.read_bytes(), held)

    def test_a_devices_unreadable_state_starts_it_from_the_rom(self):
        """A stale state is a reason to boot the ROM, not to refuse.

        A build that lays its devices out differently cannot read what the
        last one wrote. Refusing took the whole session down: the window is
        opened above this and closes when it returns, so choosing a device
        whose state had gone stale made the emulator vanish instead of
        starting the machine that was asked for.

        A state named on the command line still fails, because that one is
        the point of the run rather than a convenience beside the firmware.
        """
        image = bytearray(self.rom.read_bytes())
        code = bytes.fromhex('2e7c0010000091c84e6060fe')
        image[0x200:0x200+len(code)] = code
        identity = b',MOTO,1,Motorola Envoy'
        image[0x300:0x300+len(identity)] = identity
        self.rom.write_bytes(image)
        state = self.rom.with_suffix('.state')
        command = [*BASE, '--rom', str(self.rom), '-n', '4']

        # A real state, and then one this build cannot read.
        self.assertEqual(subprocess.run(command, capture_output=True,
                                        text=True, timeout=10).returncode, 0)
        self.assertTrue(state.exists())
        stale = bytearray(state.read_bytes())
        stale[8:64] = bytes(56)         # the sizes it checks itself against
        state.write_bytes(stale)

        # Asked for by name, the same file is fatal. First, because the run
        # below writes a readable state back over it.
        named = subprocess.run([*BASE, '--rom', str(self.rom), '-n', '4',
                                '--load-state', str(state)],
                               capture_output=True, text=True, timeout=10)
        self.assertEqual(named.returncode, 1, named.stderr)

        # As the device's own, it starts anyway. Not --temporary: that says
        # to keep nothing, and a device with nothing to keep never looks for
        # a state in the first place.
        started = subprocess.run(command, capture_output=True, text=True,
                                 timeout=10)
        self.assertEqual(started.returncode, 0, started.stderr)
        self.assertIn('from its ROM instead', started.stderr)
        self.assertNotIn('state loaded from', started.stderr)
        self.assertIn('pc=0240020A', started.stderr)   # ran from the reset PC

        # And what it wrote on the way out is readable again, so the device
        # is not stuck refusing its own state for ever.
        again = subprocess.run(command, capture_output=True, text=True,
                               timeout=10)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn('state loaded from', again.stderr)

    def test_hix_checksum_interception_is_guarded(self):
        image = bytearray(4 * 1024 * 1024)
        struct.pack_into('>II', image, 0, 0x100000, 0x0E0000C2)
        struct.pack_into('>I', image, 0x4C, 1)  # actual checked sum is zero
        code = bytes.fromhex('2e7c0010000091c84e607000287c0e00000c4ef90e000a82')
        image[0xC2:0xC2 + len(code)] = code
        identity = b'1,0.19,SONY,1, HIX-300'
        image[0x300:0x300 + len(identity)] = identity
        self.device = None          # a real HIX-300 now, and identified as one
        signature = bytes.fromhex('4ebaf952504fb0ac00406706701660000082487800ac')
        image[0xA7C:0xA7C + len(signature)] = signature
        # Both paths record D0 outside the reset ROM overlay.
        save = bytes.fromhex('23c00400000060fe')
        image[0xA92:0xA92 + len(save)] = save
        image[0xB0E:0xB0E + len(save)] = save
        out = pathlib.Path(self.tmp.name) / 'result.bin'
        for variant, result, intercepted in [('original', 1, True),
                                              ('unknown-code', 0x16, False),
                                              ('matching-sum', 0, False)]:
            with self.subTest(variant=variant):
                rom = bytearray(image)
                if variant == 'unknown-code':
                    rom[0xA7D] ^= 1
                if variant == 'matching-sum':
                    struct.pack_into('>I', rom, 0x4C, 0)
                self.rom.write_bytes(rom)
                r = self.run_guest('-n', '30', '--dump', f'4000000,4,{out}')
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn('inferred divider enabled by default', r.stderr)
                self.assertEqual(struct.unpack('>I', out.read_bytes())[0], result)
                self.assertEqual('intercepted HIX-300 checksum result' in r.stderr,
                                 intercepted)
                self.assertEqual(self.rom.read_bytes(), rom)

    def test_mc31_checksum_interception_is_guarded(self):
        image = bytearray(4 * 1024 * 1024)
        struct.pack_into('>II', image, 0, 0x100000, 0x024000C2)
        struct.pack_into('>I', image, 0x10, 0x0040111E)
        struct.pack_into('>I', image, 0x34, 0x1130)
        struct.pack_into('>I', image, 0x1124, 0x2dfe04)
        struct.pack_into('>I', image, 0x4C, 1)
        code = bytes.fromhex('2e7c0010000091c84e607000267c0040000c4ef900400e88')
        image[0xC2:0xC2+len(code)] = code
        identity = b'1,0.31,MOTO,1, Envoy'
        image[0x300:0x300+len(identity)] = identity
        signature = bytes.fromhex('4ebaf886504fb0ab0040670a487a028e21df00104afa')
        image[0xE82:0xE82+len(signature)] = signature
        save = bytes.fromhex('23c00400000060fe')
        image[0xE98:0xE98+len(save)] = save
        image[0x1000:0x1000+len(save)] = save
        image[0x111E:0x1124] = bytes.fromhex('4ef900401000')
        self.device = None
        out = pathlib.Path(self.tmp.name) / 'mc31-result.bin'
        for engine in ('jit', 'interpreter'):
            for variant in ('original', 'unknown-code', 'unknown-range',
                            'matching-sum', 'wrong-result', 'wrong-base'):
                with self.subTest(engine=engine, variant=variant):
                    rom = bytearray(image)
                    if variant == 'unknown-code': rom[0xE83] ^= 1
                    if variant == 'unknown-range': rom[0x1127] ^= 1
                    if variant == 'matching-sum': struct.pack_into('>I', rom, 0x4C, 0)
                    if variant == 'wrong-result': rom[0xCD] = 2  # MOVEQ #2,D0
                    if variant == 'wrong-base': rom[0xD3] = 8  # A3 != header base
                    self.rom.write_bytes(rom)
                    r = self.run_guest('--cpu-engine', engine, '-n', '30',
                                       '--dump', f'4000000,4,{out}')
                    self.assertEqual(r.returncode, 0, r.stderr)
                    self.assertEqual('intercepted Envoy mc31 checksum result' in r.stderr,
                                     variant == 'original')
                    expected = 1 if variant == 'original' else 2 if variant == 'wrong-result' else 0
                    self.assertEqual(struct.unpack('>I', out.read_bytes())[0], expected)
                    self.assertEqual(self.rom.read_bytes(), rom)

    def test_hix_power_switch_stays_on_until_toggled(self):
        image = bytearray(4 * 1024 * 1024)
        struct.pack_into('>II', image, 0, 0x100000, 0x0E0000C2)
        code = bytes.fromhex('2e7c0010000091c84e60'
                             '3039210000d033c00400000060f2')
        image[0xC2:0xC2 + len(code)] = code
        identity = b'1,0.19,SONY,1, HIX-300'
        image[0x300:0x300 + len(identity)] = identity
        self.rom.write_bytes(image)
        self.device = None
        out = pathlib.Path(self.tmp.name) / 'power.bin'
        for args, level in [((), 4), (('--power-at', '100'), 0),
                            (('--tap', '100,3000000,100,100'), 4)]:
            r = self.run_guest('-n', '2000000', '--dump', f'4000000,2,{out}', *args)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(struct.unpack('>H', out.read_bytes())[0], level)

    def test_help_advertises_interactive_frontend(self):
        r = subprocess.run([*BASE_PIC, '--help'], capture_output=True, text=True,
                           timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('--gui', r.stdout)

    def test_reject_bad_images_and_ram(self):
        for mb in ('0', '17', '4096'):
            with self.subTest(ram=mb):
                self.assertNotEqual(self.run_guest('--ram', mb).returncode, 0)
        self.rom.write_bytes(b'')
        self.assertNotEqual(self.run_guest().returncode, 0)

    def counter_after(self, insns, *args):
        """Run a ROM that stores the free-running counter, and read it back."""
        out = pathlib.Path(self.tmp.name) / 'counter.bin'
        image = bytearray(4 * 1024 * 1024)
        struct.pack_into('>II', image, 0, 0x100000, 0x02400200)
        image[0x200:0x20E] = bytes.fromhex(
            '2039210000d4'   # move.l $210000D4,d0
            '23c004000000'   # move.l d0,$04000000 (the boot overlay still
                             #   covers low RAM; this window does not)
            '60f2')          # bra.s  back to 0x200
        self.rom.write_bytes(image)
        r = subprocess.run([*BASE_PIC, '--temporary', '--fresh', '--rom', str(self.rom), '-n', str(insns),
                            '--dump', f'4000000,4,{out}', *args],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return struct.unpack('>I', out.read_bytes())[0]

    def test_counter_runs_at_128_Hz(self):
        # ROM battery accounting uses 3,600,000 milliseconds/hour. Its
        # 125/16 conversion therefore means 128 Hz, not 128 kHz.
        n = 16777216
        got = self.counter_after(n)
        self.assertLessEqual(abs(got - 128), 1)
        self.assertLessEqual(abs(self.counter_after(n // 2) - 64), 1)
        self.assertLessEqual(abs(self.counter_after(n, '--cpi', '2') - 256), 1)

    def test_mc68349_identification_is_read_only(self):
        image = bytearray(self.rom.read_bytes())
        code = bytes.fromhex(
            '10393c000002'     # move.b IDR,d0
            '13c004000000'     # save initial ID
            '13fc00003c000002' # attempt to clear read-only IDR
            '10393c000002'
            '13c004000001'     # save ID after write
            '60fe')
        image[0x200:0x200 + len(code)] = code
        self.rom.write_bytes(image)
        out = pathlib.Path(self.tmp.name) / 'idr.bin'
        r = self.run_guest('-n', '20', '--dump', f'4000000,2,{out}')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out.read_bytes(), bytes([0x31, 0x31]))

    def test_presets_are_strict(self):
        for preset in ('dev21:EF=1', 'dev21:1000=1', 'dev21:EE=10000',
                       'dev21:EE=100000000', 'dev21:EE=1junk', 'unknown:EE=1'):
            with self.subTest(preset=preset):
                self.assertEqual(self.run_guest('--probe-preset', preset).returncode, 2)
        r = self.run_guest('--probe-preset', 'dev21:EE=00C0')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('DEVIATION: dev21 +0EE preset to 00C0', r.stderr)

    def test_preset_overrides_loaded_state(self):
        image = bytearray(self.rom.read_bytes())
        image[0x200:0x20e] = bytes.fromhex(
            '1039210000d1'   # move.b dev21+D1,d0
            '13c004000000'   # move.b d0,$04000000
            '60f2')          # bra.s back to the input read
        self.rom.write_bytes(image)
        state = pathlib.Path(self.tmp.name) / 'preset.state'
        out = pathlib.Path(self.tmp.name) / 'input.bin'
        base = [*BASE_PIC, '--temporary', '--fresh', '--rom', str(self.rom)]
        first = subprocess.run([*base, '-n', '20', '--save-state', str(state)],
                               capture_output=True, text=True, timeout=10)
        self.assertEqual(first.returncode, 0, first.stderr)
        second = subprocess.run([*base, '--load-state', str(state),
                                 '--probe-preset', 'dev21:D0=0020', '-n', '20',
                                 '--dump', f'4000000,1,{out}'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(out.read_bytes(), b'\x20')

    def battery_samples(self, *args):
        image = bytearray(self.rom.read_bytes())
        code = bytearray()
        for index, channel in enumerate((2, 4, 0)):
            # Start conversion, allow its 64-instruction latency, then store
            # the latched reading in XRAM (outside the boot ROM overlay).
            code += bytes.fromhex('33fc') + struct.pack('>HI', channel << 6,
                                                       0x210000E4)
            code += bytes.fromhex('4e71') * 80
            code += bytes.fromhex('3039210000e4')
            code += bytes.fromhex('33c0') + struct.pack('>I', 0x04000000 + 2*index)
        code += bytes.fromhex('60fe')
        image[0x200:0x200 + len(code)] = code
        self.rom.write_bytes(image)
        out = pathlib.Path(self.tmp.name) / 'adc.bin'
        r = self.run_guest('-n', '1000', '--dump', f'4000000,6,{out}', *args)
        self.assertEqual(r.returncode, 0, r.stderr)
        return struct.unpack('>3H', out.read_bytes())

    def test_simulated_batteries_do_not_supply_pen_pressure(self):
        self.assertEqual(self.battery_samples(), (832, 512, 0))

    def test_battery_readings_can_be_overridden(self):
        self.assertEqual(self.battery_samples('--adc-chan', '2=0,4=4096'),
                         (0, 4096, 0))
        self.assertEqual(self.battery_samples('--adc', '1024'),
                         (1024, 1024, 1024))

    def test_trace_can_start_at_a_named_pc(self):
        r = self.run_guest('--trace-after', '0x02400200,3')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr.count('[68k] 02400200'), 3)
        r = self.run_guest('--trace-after', '0x02400200,2,2')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr.count('[68k] 02400200'), 2)
        self.assertEqual(self.run_guest('--trace-after', '0x02400200').returncode, 2)
        self.assertEqual(self.run_guest('--trace-after', '0x02400200,0,2').returncode, 2)

    def test_cpu_engine_option_is_accepted(self):
        """The Android app passes --cpu-engine to whichever machine a ROM
        selects, so the 68k CLI must answer for it rather than refuse to
        start. Every engine these machines have is accepted by name, and a
        name they do not have is refused rather than quietly ignored.

        The interpreter engine is the project's single-step CPU32 core;
        blocks and jit select its cached and native execution paths."""
        help_text = subprocess.run([*BASE_PIC, '--help'], capture_output=True,
                                   text=True, timeout=30).stdout
        engines = ['auto', 'blocks', 'jit', 'interpreter']
        for engine in engines:
            with self.subTest(engine=engine):
                r = self.run_guest('--cpu-engine', engine)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertNotIn('unknown option', r.stderr)
        r = self.run_guest('--cpu-engine', 'warp')
        self.assertEqual(r.returncode, 2)
        self.assertIn('no warp engine', r.stderr)

    def test_screen_taps_may_be_scheduled(self):
        r = self.run_guest('--tap', '10,20,23,24',
                           '--tap', '40,30,456,297')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('scheduling screen tap (23,24) at +10 for 20', r.stderr)
        self.assertIn('scheduling screen tap (456,297) at +40 for 30', r.stderr)
        for tap in ('0,20,23,24', '10,0,23,24', '10,20,480,24',
                    '10,20,23,320', '10,20,23'):
            with self.subTest(tap=tap):
                self.assertEqual(self.run_guest('--tap', tap).returncode, 2)


if __name__ == '__main__':
    unittest.main()
