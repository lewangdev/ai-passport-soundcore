#!/usr/bin/env python3
"""End-to-end compatibility between the firmware protocol code and the D3200 client.

The client side is a line-by-line Python port of the Soundcore Manager (Flutter)
logic that talks to a real D3200: ProtocolFrame.encode/processBuffer,
DeviceCrypto (ECDH P-256 + HKDF + AES-CTR), AudioFileSecretKey.parseFrame and
BleFilePull._writeSlices / OpusPcmDecoder.trimOpusPacket. The device side is
the firmware's own sc_proto/sc_crypto/sc_xfer compiled for the host.

Needs a C compiler, mbedtls 3.x headers/library, and the `cryptography` package;
the test is skipped (not failed) when either dependency is missing.
"""

from __future__ import annotations

import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

try:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
except ImportError:  # pragma: no cover - environment dependent
    ec = None


# ---------------------------------------------------------------- client port

def encode(cmd_type: int, cmd_id: int, payload: bytes = b"") -> bytes:
    """ProtocolFrame.encode"""
    total = 5 + 2 + 2 + len(payload) + 1
    body = bytes([0x08, 0xEE, 0, 0, 0, cmd_type, cmd_id, total & 0xFF, total >> 8]) + payload
    return body + bytes([sum(body) & 0xFF])


def process_buffer(buffer: bytearray) -> list[bytes]:
    """ProtocolFrame.processBuffer (consumes complete frames from buffer)."""
    out = []
    i = 0
    while i + 10 <= len(buffer):
        found = -1
        for j in range(i, len(buffer) - 9):
            if buffer[j:j + 4] == b"\x09\xff\x00\x00":
                found = j
                break
        if found < 0:
            break
        i = found
        total = buffer[i + 7] | (buffer[i + 8] << 8)
        if total < 10:
            i += 1
            continue
        if i + total > len(buffer):
            break
        frame = bytes(buffer[i:i + total])
        if sum(frame[:-1]) & 0xFF == frame[-1]:
            out.append(frame)
        i += total
    del buffer[:i]
    return out


def aes_ctr(key: bytes, iv: bytes, data: bytes) -> bytes:
    dec = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor()
    return dec.update(data) + dec.finalize()


def parse_secret(raw: bytes):
    """AudioFileSecretKey.parseFrame"""
    if len(raw) < 97:
        return None
    return {
        "file_id": int.from_bytes(raw[9:13], "little"),
        "size": int.from_bytes(raw[13:17], "little"),
        "nonce": raw[17:33],
        "key": raw[33:79],
        "session_nonce": raw[79:95],
        "error": raw[95],
    }


def write_slices(frame: bytes, file_key: bytes | None, nonce: bytes | None) -> list[tuple[int, bytes]]:
    """BleFilePull._writeSlices + AesCtrStreamDecrypt + trimOpusPacket"""
    out = []
    i = 9
    while len(frame) - i >= 165:
        seq = int.from_bytes(frame[i:i + 4], "little")
        i += 5
        chunk = frame[i:i + 160]
        i += 160
        if i < len(frame):
            i += 1
        if file_key is not None:
            iv = nonce[:12] + (seq * 10).to_bytes(4, "big")
            chunk = aes_ctr(file_key, iv, chunk)
        n = len(chunk)
        while n > 1 and chunk[n - 1] == 0:
            n -= 1
        out.append((seq, chunk[:n]))
    return out


def run_receiver(frames: list[bytes], priv=None) -> tuple[dict[int, bytes], list[bytes]]:
    """Feed the device stream in random BLE-sized notifications, like BleService._onBytes."""
    stream = b"".join(frames)
    buffer = bytearray()
    received: list[bytes] = []
    pos = 0
    rnd = random.Random(7)
    while pos < len(stream):
        n = rnd.choice([20, 182, 244, 509])
        buffer.extend(stream[pos:pos + n])
        pos += n
        received.extend(process_buffer(buffer))
    return received


# ---------------------------------------------------------------- tests

def find_mbedtls() -> tuple[list[str], list[str]] | None:
    candidates = []
    if shutil.which("pkg-config"):
        r = subprocess.run(["pkg-config", "--cflags", "--libs", "mbedcrypto"], capture_output=True, text=True)
        if r.returncode == 0:
            flags = r.stdout.split()
            candidates.append(([f for f in flags if not f.startswith("-l") and not f.startswith("-L")],
                               [f for f in flags if f.startswith("-l") or f.startswith("-L")]))
    if shutil.which("brew"):
        r = subprocess.run(["brew", "--prefix", "mbedtls"], capture_output=True, text=True)
        if r.returncode == 0:
            prefix = r.stdout.strip()
            candidates.append(([f"-I{prefix}/include"], [f"-L{prefix}/lib", "-lmbedcrypto"]))
    candidates.append(([], ["-lmbedcrypto"]))
    return candidates


@unittest.skipIf(ec is None, "python 'cryptography' package not installed")
class ClientCompatTest(unittest.TestCase):
    harness: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="sc-harness-")
        cls.harness = Path(cls.tmp.name) / "sc_device_harness"
        cc = os.environ.get("CC", "cc")
        sources = [ROOT / "tests/sc_device_harness.c", ROOT / "main/sc_proto.c",
                   ROOT / "main/sc_crypto.c", ROOT / "main/sc_xfer.c"]
        errors = []
        for cflags, libs in find_mbedtls():
            cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'main'}", *cflags,
                   *map(str, sources), "-o", str(cls.harness), *libs]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 0:
                return
            errors.append(r.stderr.strip().splitlines()[-1:] or [""])
        cls.tmp.cleanup()
        raise unittest.SkipTest(f"mbedtls 3.x not available for host build: {errors[-1]}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, *args: str) -> list[tuple[str, bytes]]:
        r = subprocess.run([str(self.harness), *args], capture_output=True, text=True, check=True)
        return [(kind, bytes.fromhex(hexdata)) for kind, hexdata in
                (line.split(" ", 1) for line in r.stdout.splitlines() if " " in line)]

    def test_encrypted_transfer_round_trip(self) -> None:
        priv = ec.generate_private_key(ec.SECP256R1())
        app_pub = priv.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)
        self.assertEqual(len(app_pub), 65)
        out = self.run_harness("session", app_pub.hex())
        plain = [data for kind, data in out if kind == "PLAIN"]
        frames = run_receiver([data for kind, data in out if kind == "FRAME"])

        # RecorderController._onEncryptResponse + DeviceCrypto.completeHandshake
        hs = frames[0]
        self.assertEqual((hs[5], hs[6]), (0x2E, 0x01))
        self.assertGreaterEqual(len(hs), 106)
        dev_pub, dev_shared = hs[9:74], hs[74:106]
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), dev_pub)
        shared = priv.exchange(ec.ECDH(), peer)
        self.assertEqual(shared.hex().upper(), dev_shared.hex().upper(), "ECDH shared secret mismatch")
        session_key = HKDF(algorithm=hashes.SHA256(), length=32, salt=bytes([1, 2, 3]),
                           info=bytes([1, 2, 3])).derive(shared)

        # DeviceCrypto.initFileDecrypt
        head = frames[1]
        self.assertEqual((head[5], head[6]), (0x1A, 0x07))
        secret = parse_secret(head)
        self.assertIsNotNone(secret)
        self.assertEqual(secret["file_id"], 1735689601)
        self.assertEqual(secret["size"], len(plain) * 160)
        self.assertEqual(secret["error"], 0)
        unwrapped = aes_ctr(session_key, secret["session_nonce"], secret["key"])
        self.assertEqual(unwrapped[:14], b"soundcored3200")
        file_key = unwrapped[14:46]

        got: dict[int, bytes] = {}
        for frame in frames[2:-1]:
            self.assertEqual((frame[5], frame[6]), (0x1A, 0x08))
            for seq, data in write_slices(frame, file_key, secret["nonce"]):
                got[seq] = data
        self.assertEqual(sorted(got), list(range(len(plain))))
        for seq, data in enumerate(plain):
            self.assertEqual(got[seq], data, f"frame {seq} differs after decrypt+trim")
        done = frames[-1]
        self.assertEqual((done[5], done[6]), (0x1A, 0x0A))
        self.assertEqual(int.from_bytes(done[9:13], "little"), 1735689601)

    def test_plain_transfer_without_session(self) -> None:
        out = self.run_harness("plain")
        plain = [data for kind, data in out if kind == "PLAIN"]
        frames = run_receiver([data for kind, data in out if kind == "FRAME"])
        head = frames[0]
        self.assertIsNone(parse_secret(head), "short head => client keeps data unencrypted")
        self.assertEqual(int.from_bytes(head[13:17], "little"), len(plain) * 160)
        got = {}
        for frame in frames[1:-1]:
            for seq, data in write_slices(frame, None, None):
                got[seq] = data
        self.assertEqual([got[i] for i in range(len(plain))], plain)

    def test_device_parses_client_commands(self) -> None:
        cases = {
            "getDeviceInfo": encode(0x01, 0x01),
            "syncTime": encode(0x01, 0xA6, (1760000000).to_bytes(4, "little") + bytes([8])),
            "startRecord": encode(0x18, 0x82, b"\x01"),
            "listFilesWithEndTime": encode(0x1B, 0x0E, (2).to_bytes(2, "little")),
            "startTransportAudioFile": encode(0x1A, 0x07, (320).to_bytes(4, "little")
                                              + (1735689601).to_bytes(4, "little") + b"\x01"),
            "sendWifiConfig": encode(0x1A, 0x05, bytes([18]) + b"WiFi-1760000000000" + bytes([13]) + b"1760000000000"),
            "bind": encode(0x0B, 0x87, b"\x01"),
            "notifyEncryptPublicKey": encode(0x2E, 0x01, bytes([4]) + bytes(range(64))),
        }
        stream = b"".join(cases.values())
        out = self.run_harness("parse", stream.hex())
        cmds = [data for kind, data in out if kind == "CMD"]
        self.assertEqual(len(cmds), len(cases))
        for (name, frame), cmd in zip(cases.items(), cmds):
            with self.subTest(name):
                self.assertEqual(cmd[0:2], frame[5:7])
                self.assertEqual(cmd[2], 1, "checksum")
                self.assertEqual(cmd[3:], frame[9:-1])

        # WifiExportService._buildFileHeaderRequestHex: 18 bytes, last byte 0x1B, no checksum
        req = bytes([0x08, 0xEE, 0, 0, 0, 0x1A, 0x07, 0x12, 0x00]) + (0).to_bytes(4, "little") \
            + (1735689601).to_bytes(4, "little") + b"\x1b"
        out = self.run_harness("parse", req.hex(), "lenient")
        cmd = [data for kind, data in out if kind == "CMD"]
        self.assertEqual(len(cmd), 1)
        self.assertEqual(cmd[0][0:2], b"\x1a\x07")
        self.assertEqual(int.from_bytes(cmd[0][7:11], "little"), 1735689601)


if __name__ == "__main__":
    unittest.main()
