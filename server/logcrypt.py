"""The gauge's encrypted log format, read on the server (asked 2026-11-14:
the car is the only dangerous place - the server may hold plain copies).

The format and its reasons are in firmware/car_gauge_v1/logcrypt.h (the
gauge's writer) and DECISIONS.md; this is the same reader as the app's
mobile/src/lib/logcrypt.ts, held to the same bytes by the shared test vector
tests/vectors/logcrypt-v1.json:

  file   = block*,  block = type (1) | length (u32 LE) | body
  0x01   header: "NGE1" | 0x01 | eph_pub (32)
  0x02   record: ciphertext | mac (16), XChaCha20-Poly1305, AD = header body
  key    = BLAKE2b-256(X25519(secret, eph_pub) | eph_pub | our_pub | "n-gauge log v1")
  nonce  = record index in its segment, u64 LE, then 16 zero bytes

X25519 and ChaCha20-Poly1305 from the `cryptography` package (the standard
library has neither); XChaCha20 is HChaCha20 - one ChaCha block, here - over
the nonce's first 16 bytes, then ChaCha20-Poly1305 with the rest.
"""

import hashlib
import struct

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

TYPE_HEADER, TYPE_RECORD, VERSION = 0x01, 0x02, 0x01
MAGIC = b"NGE1"
INFO = b"n-gauge log v1"
HEADER_BODY = 4 + 1 + 32
MAC = 16
BLOCK_HEAD = 5
MAX_RECORD = 256 * 1024
SUFFIX = ".nge"
KEY_PREFIX = "ngauge-log-key-v1:"


def key_from_text(text: str) -> bytes | None:
    """The secret from the app's exported line ("ngauge-log-key-v1:<hex>",
    or the bare 64 hex digits), or None if it is not one."""
    t = text.strip()
    if t.lower().startswith(KEY_PREFIX):
        t = t[len(KEY_PREFIX):]
    try:
        b = bytes.fromhex(t)
    except ValueError:
        return None
    return b if len(b) == 32 else None


def _rotl(v: int, n: int) -> int:
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def _hchacha20(key: bytes, nonce16: bytes) -> bytes:
    """RFC draft-irtf-cfrg-xchacha: the subkey for XChaCha20."""
    s = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574,
         *struct.unpack("<8I", key), *struct.unpack("<4I", nonce16)]

    def qr(a, b, c, d):
        s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = _rotl(s[d] ^ s[a], 16)
        s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = _rotl(s[b] ^ s[c], 12)
        s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] = _rotl(s[d] ^ s[a], 8)
        s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] = _rotl(s[b] ^ s[c], 7)

    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<8I", *s[0:4], *s[12:16])


def _derive(shared: bytes, eph_pub: bytes, our_pub: bytes) -> bytes:
    return hashlib.blake2b(shared + eph_pub + our_pub + INFO, digest_size=32).digest()


def _is_header_at(f: bytes, i: int) -> bool:
    return (i + BLOCK_HEAD + 5 <= len(f) and f[i] == TYPE_HEADER
            and struct.unpack_from("<I", f, i + 1)[0] == HEADER_BODY
            and f[i + BLOCK_HEAD:i + BLOCK_HEAD + 4] == MAGIC and f[i + BLOCK_HEAD + 4] == VERSION)


def _next_header(f: bytes, frm: int) -> int:
    i = f.find(MAGIC, frm + BLOCK_HEAD)
    while i >= 0:
        if _is_header_at(f, i - BLOCK_HEAD):
            return i - BLOCK_HEAD
        i = f.find(MAGIC, i + 1)
    return len(f)


def open_log(f: bytes, secret: bytes) -> bytes:
    """A whole file decrypted, without raising. A block cut short or failing
    its MAC is skipped to the next segment header (a card error leaves good
    segments after it); with none after it, it is the end - a power loss
    mid-write, or the upload not finished yet. As openLog() in the app."""
    priv = X25519PrivateKey.from_private_bytes(secret)
    our_pub = priv.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    out = []
    aead = ad = None
    counter = at = 0
    while at + BLOCK_HEAD <= len(f):
        typ = f[at]
        ln = struct.unpack_from("<I", f, at + 1)[0]
        body = f[at + BLOCK_HEAD:at + BLOCK_HEAD + ln]
        ok = ln <= MAX_RECORD and at + BLOCK_HEAD + ln <= len(f)
        if ok and typ == TYPE_HEADER:
            ok = ln == HEADER_BODY and body[:4] == MAGIC and body[4] == VERSION
            if ok:
                eph = body[5:37]
                try:   # a low-order point: refused by X25519, as a broken header
                    shared = priv.exchange(X25519PublicKey.from_public_bytes(eph))
                    key = _derive(shared, eph, our_pub)
                    aead, ad, counter = key, bytes(body), 0
                except ValueError:
                    ok = False
        elif ok and typ == TYPE_RECORD and aead and ln >= MAC:
            nonce24 = struct.pack("<Q", counter) + bytes(16)
            sub = ChaCha20Poly1305(_hchacha20(aead, nonce24[:16]))
            try:
                out.append(sub.decrypt(bytes(4) + nonce24[16:], body, ad))
                counter += 1
            except InvalidTag:
                ok = False
        else:
            ok = False
        if not ok:
            aead = ad = None
            at = _next_header(f, at + 1)
            continue
        at += BLOCK_HEAD + ln
    return b"".join(out)
