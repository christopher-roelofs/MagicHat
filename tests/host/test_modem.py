"""Regressions for the diagnostic Hayes peer, not a PPP/Internet test."""
import pathlib
import runpy
import unittest
from unittest.mock import Mock, patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
Modem = runpy.run_path(str(ROOT / 'scripts/modem'))['Modem']


class HayesTests(unittest.TestCase):
    def setUp(self):
        self.modem = Modem.__new__(Modem)
        self.modem.quiet = True
        self.modem.echo = True
        self.modem.verbose = True
        self.modem.data_mode = False
        self.modem.write = Mock()

    def test_pic_initialization_is_not_a_dial(self):
        self.modem.command(b'ATE0V0')
        self.assertFalse(self.modem.echo)
        self.assertFalse(self.modem.verbose)
        self.modem.command(b'ATS11=100H0\\N0&Q6N1S37=0&C1&D2&K3%E0')
        self.assertFalse(self.modem.data_mode)
        self.modem.write.assert_called_with(b'\r\n0\r')

    def test_extended_commands_do_not_change_basic_settings(self):
        self.modem.command(b'AT&D2%E0\\N0')
        self.assertTrue(self.modem.echo)
        self.assertFalse(self.modem.data_mode)
        self.modem.write.assert_called_once_with(b'\r\nOK\r\n')

    def test_only_unprefixed_d_enters_data_mode(self):
        with patch('time.sleep'):
            self.modem.command(b'ATE0V0&D2DT5550100')
        self.assertTrue(self.modem.data_mode)
        self.modem.write.assert_called_once_with(b'\r\n1\r')

    def test_voice_dial_does_not_claim_a_data_carrier(self):
        self.modem.command(b'ATD1;')
        self.assertFalse(self.modem.data_mode)
        self.modem.write.assert_called_once_with(b'\r\nERROR\r\n')


if __name__ == '__main__':
    unittest.main()
