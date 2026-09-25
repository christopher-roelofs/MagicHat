import pathlib
import runpy
import struct
import tempfile
import unittest
from unittest.mock import Mock, call, patch

pc = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[2] / 'scripts/pclink'))


class ProtocolTests(unittest.TestCase):
    def test_known_connect_reply(self):
        self.assertEqual(pc['frame'](b'Cntd').hex(), '0008436e7464000000001a474c7a')

    def test_connection_confirms_both_guest_waits(self):
        # Drive the CLI's Cnct handler, including its real confirmation
        # method. A single Cntd leaves the ROM's second event wait pending
        # and closes the connection 20 seconds into a large transfer.
        link = Mock()
        link.confirm_connection = lambda: pc['Link'].confirm_connection(link)
        link.pump.side_effect = [None, KeyboardInterrupt]
        link.decoder.next_frame.side_effect = [(b'Cnct', bytes(1028)), None]
        link.pending = b'queued'
        with patch.dict(pc['main'].__globals__, Link=Mock(return_value=link)), \
                patch('sys.argv', ['pclink', '/unused', '--quiet']), \
                patch('builtins.print'):
            self.assertEqual(pc['main'](), 130)
        self.assertEqual(link.send.call_args_list, [call(b'Cntd'), call(b'Cntd')])

    def test_goodbye_matches_reference_shutdown_sequence(self):
        link = Mock()
        link.send = Mock()
        link.pump = Mock()
        link.decoder = Mock()
        link.decoder.next_frame.return_value = (b'GBye', b'')
        self.assertTrue(pc['Link'].goodbye(link))
        self.assertEqual(link.send.call_args_list, [call(b'Abrt'), call(b'GBye')])

    def test_fragmented_and_coalesced_commands(self):
        payload = bytes(range(256)) * 4 + bytes(4)
        wire = b'boot noiseChMa' + pc['frame'](b'Cnct', payload) + pc['frame'](b'Ping')
        d = pc['Decoder']()
        frames = []
        for b in wire:
            d.feed(bytes([b]))
            while (f := d.next_frame()) is not None:
                frames.append(f)
        self.assertEqual(frames, [(b'Cnct', payload), (b'Ping', b'')])
        d = pc['Decoder']()
        d.feed(wire)
        self.assertEqual(d.next_frame(), (b'Cnct', payload))
        self.assertEqual(d.next_frame(), (b'Ping', b''))
        self.assertIsNone(d.next_frame())

    def test_escape_split_across_blocks(self):
        # Eight header bytes + 247 data bytes put the escape at block end.
        data = b'x' * 247 + b'\x0e\x0f\x10'
        d = pc['Decoder'](greeting=False)
        d.feed(pc['frame'](b'SPkg', data))
        self.assertEqual(d.next_frame(), (b'SPkg', data))

    def test_crc_failure_does_not_deliver_command(self):
        wire = bytearray(pc['frame'](b'Cntd'))
        wire[3] ^= 1
        d = pc['Decoder'](greeting=False)
        with self.assertRaises(pc['ProtocolError']):
            d.feed(wire)
        self.assertIsNone(d.next_frame())

    def test_incomplete_payload_waits(self):
        d = pc['Decoder'](greeting=False)
        d.feed(pc['encode'](b'Cnct' + struct.pack('>I', 4) + b'ab'))
        self.assertIsNone(d.next_frame())
        d.feed(pc['encode'](b'cd'))
        self.assertEqual(d.next_frame(), (b'Cnct', b'abcd'))

    def test_invalid_lengths(self):
        for n in (0, 257, 65535):
            with self.assertRaises(pc['ProtocolError']):
                pc['Decoder'](greeting=False).feed(struct.pack('>H', n))
        d = pc['Decoder'](greeting=False)
        d.feed(pc['encode'](b'SPkg' + struct.pack('>I', 1029)))
        with self.assertRaises(pc['ProtocolError']):
            d.next_frame()

    def test_unescaped_stream_control_delimits_incoming_data(self):
        data = b'package data with \x0e quoted inside'
        quoted = pc['encode'](data)
        end = bytes([14])
        control = struct.pack('>H', 1) + end + struct.pack('>I', pc['crc'](end))
        d = pc['Decoder'](greeting=False)
        d.feed(quoted + control + pc['frame'](b'Ping'))
        self.assertEqual(d.take_to_control(), (data, 14))
        self.assertEqual(d.next_frame(), (b'Ping', b''))

    def test_receives_package_to_unique_safe_path(self):
        name = '../Test/Package'.encode('utf-16-be')
        data = b'guest package contents'
        info = (struct.pack('>2I', 0, len(name) // 2) + name).ljust(520, b'\0')
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            first = pc['IncomingPackage'](directory, info)
            first.write(data[:5])
            first.write(data[5:])
            path = first.finish(14)
            self.assertEqual(path.name, '_Test_Package.package')
            self.assertEqual(path.read_bytes(), data)

            second = pc['IncomingPackage'](directory, info)
            self.assertEqual(second.path.name, '_Test_Package (2).package')
            second.abort()
            self.assertFalse(second.partial.exists())

    def test_incoming_package_rejects_bad_metadata_and_empty_data(self):
        name = 'Broken'.encode('utf-16-be')
        with self.assertRaises(pc['ProtocolError']):
            pc['incoming_package_info'](
                (struct.pack('>2I', 1, len(name) // 2) + name).ljust(520, b'\0'))
        info = (struct.pack('>2I', 0, len(name) // 2) + name).ljust(520, b'\0')
        with tempfile.TemporaryDirectory() as directory:
            incoming = pc['IncomingPackage'](pathlib.Path(directory), info)
            with self.assertRaises(pc['ProtocolError']):
                incoming.finish(14)
            incoming.abort()

    def test_reference_noop_notifications_are_accepted(self):
        link = Mock()
        link.pump.side_effect = [None, KeyboardInterrupt]
        link.decoder.next_frame.side_effect = [
            (b'APkg', b'a'), (b'Free', b'b'), (b'Flsh', b'c'),
            (b'Baud', b'd'), None,
        ]
        link.pending = b''
        with patch.dict(pc['main'].__globals__, Link=Mock(return_value=link)), \
                patch('sys.argv', ['pclink', '/unused', '--quiet']), \
                patch('builtins.print'):
            self.assertEqual(pc['main'](), 130)
        link.send.assert_not_called()


if __name__ == '__main__':
    unittest.main()
