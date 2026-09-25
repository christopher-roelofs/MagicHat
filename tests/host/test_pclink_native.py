"""Exercise the native PC Link peer with captured PIC GMTP traffic."""
import ctypes
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def scratch_directory():
    # Windows cannot delete a library this process still has loaded.
    if sys.platform == 'win32':
        return tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
    return tempfile.TemporaryDirectory()


def checksum(data):
    data += bytes(len(data) & 1)
    value = sum(struct.unpack('>' + 'H' * (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return value ^ 65535


def fcs(data):
    value = 65535
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x8408 if value & 1 else 0)
    return value ^ 65535


def frame(pdu):
    udp = bytes(4) + struct.pack('>HH', 8 + len(pdu), 0) + pdu
    ip = bytearray(bytes.fromhex('4500000000004000101100000000000000000000'))
    ip[2:4] = struct.pack('>H', len(ip) + len(udp))
    ip[10:12] = struct.pack('>H', checksum(bytes(ip)))
    body = b'\xff\x03\x00\x21' + ip + udp
    body += struct.pack('<H', fcs(body))
    return b'\x7e' + b''.join(bytes([125, b ^ 32]) if b < 32 or b in (125, 126)
                               else bytes([b]) for b in body) + b'\x7e'


def hix_frame(pdu):
    # Captured HIX MC19 C2 GMTPOverPPPLink packet: four-byte record length,
    # 24-byte link header, then the ordinary checksummed GMTP PDU.
    header = bytes.fromhex('000100000000ffbf000000000000000000000000002b0000')
    record = header + pdu
    body = b'\xff\x03\x00\x21' + struct.pack('>I', 4 + len(record)) + record
    body += struct.pack('<H', fcs(body))
    return b'\x7e' + b''.join(bytes([125, b ^ 32]) if b < 32 or b in (125, 126)
                               else bytes([b]) for b in body) + b'\x7e'


class NativePPPTests(unittest.TestCase):
    def test_distribution_envelope(self):
        with scratch_directory() as directory:
            directory = pathlib.Path(directory)
            harness = directory / 'loader.c'
            library = directory / 'loader.so'
            harness.write_text('''
#include "host/pclink.c"
int check_package(const char *path, const unsigned char *expected,
                  unsigned length, int raw) {
    mrc_pclink *peer = mrc_pclink_open(path);
    if (!peer) return 0;
    int result = peer->package_len == length &&
        !memcmp(peer->package, expected, length) && peer->m68k_package == raw;
    mrc_pclink_close(peer);
    return result ? 1 : -1;
}
''')
            subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra',
                            '-I' + str(ROOT / 'src'), str(harness),
                            '-o', str(library)], check=True)
            lib = ctypes.CDLL(str(library))
            lib.check_package.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                         ctypes.c_uint, ctypes.c_int]
            stream = b'MPkg' + struct.pack('>7I', 1, 105820, 0, 0, 0, 0, 0) + b'\x01\x12'
            name = b'PrestoPPP'
            envelope = b'MCap' + struct.pack('>3I', 0, 105820, len(name)) + name
            raw = bytes(16) + struct.pack('>I', 20)
            cases = [
                (envelope + stream, stream, 0, 1),
                (stream, stream, 0, 1),
                (raw, raw, 1, 1),
                (b'\0SALTCOD', b'\0SALTCOD', 0, 1),
                (b'MCap', b'', 0, 0),
                (b'MCap' + struct.pack('>3I', 1, 0, 0) + stream, b'', 0, 0),
                (b'MCap' + struct.pack('>3I', 0, 0, 0xffffffff) + stream, b'', 0, 0),
                (envelope + stream[:33], b'', 0, 0),
                (envelope + b'junk' + stream, b'', 0, 0),
                (envelope + b'MPkg\0\0\0\2' + stream[8:], b'', 0, 0),
            ]
            for i, (data, expected, is_raw, result) in enumerate(cases):
                with self.subTest(case=i):
                    package = directory / 'Test.cap'
                    package.write_bytes(data)
                    self.assertEqual(lib.check_package(str(package).encode(), expected,
                                                       len(expected), is_raw), result)
                    self.assertEqual(package.read_bytes(), data)

    def test_receive_ack_and_send_window(self):
        self.check_receive_ack_and_send_window(False)

    def test_distribution_stream_transmission(self):
        self.check_receive_ack_and_send_window(True)

    def test_hix_direct_gmtp_over_ppp(self):
        self.check_receive_ack_and_send_window(False, direct_hix=True)

    def check_receive_ack_and_send_window(self, distribution, direct_hix=False):
        with scratch_directory() as directory:
            directory = pathlib.Path(directory)
            library = directory / 'peer.so'
            harness = directory / 'peer.c'
            harness.write_text('''
#include "host/pclink.c"
int test_sequence_wrap(void) {
    mrc_pclink peer = {0};
    peer.ppp_package_sent = true;
    peer.ppp_package_end = 0x10005;
    peer.gmtp_acked = 4;
    peer.gmtp_sent = 0x10005;
    ppp_acked(&peer, 5);
    if (peer.ppp_package_acked) return 1; /* Same wire ID, earlier cycle. */
    peer.gmtp_acked = 0xffff;
    ppp_acked(&peer, 0);
    if (peer.gmtp_acked != 0x10000) return 2;
    ppp_acked(&peer, 0); /* Duplicate after wrap. */
    if (peer.gmtp_acked != 0x10000) return 3;
    for (unsigned i = 1; i <= 5; i++) ppp_acked(&peer, i);
    return peer.ppp_package_acked ? 0 : 4;
}
''')
            subprocess.run(['cc', '-shared', '-fPIC', '-I' + str(ROOT / 'src'),
                            str(harness), '-o', str(library)], check=True)
            lib = ctypes.CDLL(str(library))
            self.assertEqual(lib.test_sequence_wrap(), 0)
            lib.mrc_pclink_open.argtypes = [ctypes.c_char_p]
            lib.mrc_pclink_open.restype = ctypes.c_void_p
            lib.mrc_pclink_close.argtypes = [ctypes.c_void_p]
            lib.mrc_pclink_from_guest.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
            lib.mrc_pclink_to_guest.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8)]
            lib.mrc_pclink_to_guest.restype = ctypes.c_bool
            lib.mrc_pclink_state_of.argtypes = [ctypes.c_void_p]
            lib.mrc_pclink_state_of.restype = ctypes.c_int
            package = directory / 'Test.pkg'
            cluster = bytes(16) + struct.pack('>I', 20)
            expected = (b'MPkg' + struct.pack('>7I', 1, 0, 0, 0, 0, 0, 0)
                        + bytes.fromhex('010075c73e0d') + b'FrozenPackage'
                        + bytes.fromhex('01021614') + cluster + b'\x12')
            if distribution:
                # Nonzero metadata must survive without wrapping a second time.
                expected = expected[:8] + struct.pack('>I', 1234) + expected[12:]
                package.write_bytes(b'MCap' + struct.pack('>3I', 0, 1234, 4)
                                    + b'Test' + expected)
            else:
                package.write_bytes(cluster)
            peer = lib.mrc_pclink_open(str(package).encode())
            self.assertTrue(peer)

            def send(pdu):
                wire = hix_frame(pdu) if direct_hix else frame(pdu)
                for byte in wire:
                    lib.mrc_pclink_from_guest(peer, byte)

            def drain():
                wire = bytearray()
                byte = ctypes.c_uint8()
                while lib.mrc_pclink_to_guest(peer, ctypes.byref(byte)):
                    wire.append(byte.value)
                frames = []
                for encoded in wire.split(b'\x7e'):
                    if not encoded:
                        continue
                    decoded = bytearray()
                    escaped = False
                    for b in encoded:
                        if escaped:
                            decoded.append(b ^ 32)
                            escaped = False
                        elif b == 125:
                            escaped = True
                        else:
                            decoded.append(b)
                    self.assertEqual(fcs(decoded[:-2]), struct.unpack('<H', decoded[-2:])[0])
                    self.assertEqual(decoded[4] >> 4, 4)
                    self.assertEqual(decoded[13], 17)
                    pdu = bytes(decoded[32:-2])
                    self.assertEqual(checksum(pdu), 0)
                    frames.append(pdu)
                return frames

            try:
                if direct_hix:
                    connect = bytes.fromhex(
                        '01c102d70000585100171f20'
                        '436e63740000000f000000010a4669727374204c617374')
                else:
                    connect = bytes.fromhex('01c1041000003418000d1f20436e6374000000050000000200')
                send(connect)
                received = drain()
                if direct_hix:
                    ack = bytes.fromhex('040002d700010000')
                    expected_ack = ack[:6] + struct.pack('>H', checksum(ack))
                else:
                    expected_ack = bytes.fromhex('040004100001f7ee')
                self.assertEqual(received[0], expected_ack)
                self.assertEqual(received[1][12:], b'Cntd' + struct.pack('>I', 1) + b'\0')
                if direct_hix:
                    self.assertEqual(received[2][12:], received[1][12:])
                    self.assertTrue(received[3][12:].startswith(b'SPkg'))
                    self.assertEqual(len(received), 4)
                    # HIX cumulatively ACKs a short run; data must not wait
                    # for an ACK after every packet.
                    cumulative = bytearray(struct.pack('>BBHHH', 4, 0, 0x2d7, 3, 0))
                    cumulative[6:8] = struct.pack('>H', checksum(cumulative))
                    send_command = b'Pong' + bytes(4) + b'Send' + bytes(4)
                    msg = bytearray(struct.pack('>BBHHHHBB', 1, 0x81, 0x2d7, 1,
                                                0, len(send_command), 31, 32) + send_command)
                    msg[6:8] = struct.pack('>H', checksum(msg))
                    # HIX can put a cumulative ACK and multiple commands
                    # inside one UDP datagram.
                    for byte in frame(bytes(cumulative) + bytes(msg)):
                        lib.mrc_pclink_from_guest(peer, byte)
                    data_frames = drain()
                    self.assertEqual(data_frames[0][0], 4)
                    self.assertEqual(len(data_frames), 4)
                    self.assertTrue(data_frames[1][12:].startswith(b'SBuf'))
                    return
                self.assertEqual(len(received), 2)
                send(connect)  # Retransmission is acknowledged without redispatch.
                self.assertEqual(drain(), [received[0]])
                def ack(sequence):
                    association = 0x2D7 if direct_hix else 0x410
                    pdu = struct.pack('>BBHHH', 4, 0, association, sequence, 0)
                    return pdu[:6] + struct.pack('>H', checksum(pdu))

                corrupt_ack = bytearray(ack(1))
                corrupt_ack[-1] ^= 1
                send(bytes(corrupt_ack))  # Corrupt GMTP checksum.
                self.assertEqual(drain(), [])
                send(ack(1))
                self.assertEqual(drain()[0][12:], received[1][12:])
                send(ack(1))  # Duplicate ACK cannot skip a block.
                self.assertEqual(drain(), [])
                send(ack(2))
                self.assertTrue(drain()[0][12:].startswith(b'SPkg'))

                def message(sequence, tag):
                    command = tag + bytes(4)
                    pdu = bytearray(struct.pack('>BBHHHHBB', 1, 0x81,
                                               0x2D7 if direct_hix else 0x410,
                                               sequence, 0, len(command), 31, 32)
                                    + command)
                    pdu[6:8] = struct.pack('>H', checksum(bytes(pdu)))
                    send(bytes(pdu))

                send(ack(3))
                self.assertEqual(drain(), [])
                message(1, b'Send')
                received_send = drain()
                stream = received_send[1][12:]
                self.assertTrue(stream.startswith(b'SBuf'))
                message(2, b'Pong')  # An early Pong cannot finish a transfer.
                drain()
                self.assertEqual(lib.mrc_pclink_state_of(peer), 2)
                sequence = 4
                while True:
                    send(ack(sequence))
                    payload = drain()[0][12:]
                    sequence += 1
                    if payload.startswith(b'SndX'):
                        break
                    stream += payload
                wrapped = stream[8:]
                self.assertEqual(struct.unpack('>I', stream[4:8])[0], len(wrapped))
                self.assertEqual(wrapped, expected)
                send(ack(sequence))
                drain()
                # The guest replies to Ping before acknowledging that Ping.
                byte = ctypes.c_uint8()
                for _ in range(20001):
                    if lib.mrc_pclink_to_guest(peer, ctypes.byref(byte)):
                        break
                else:
                    self.fail('peer did not send its completion Ping')
                drain()
                message(3, b'Pong')
                self.assertEqual(lib.mrc_pclink_state_of(peer), 3)
            finally:
                lib.mrc_pclink_close(peer)


if __name__ == '__main__':
    unittest.main()
