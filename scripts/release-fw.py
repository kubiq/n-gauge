#!/usr/bin/env python3
"""Release the gauge's car firmware to the upload server, signed.

The gauge looks for a new firmware there before each upload (upload.cpp),
downloads it to its card, checks the signature against the public key built
into it (FW_SIGN_PUB, from scripts/build.sh) and installs it before it next
sleeps. Only an image signed with the private key here is ever installed, and
only one built later than the firmware it replaces.

  scripts/release-fw.py --init
      Once: makes the signing key - FW_KEY, default
      ~/.config/n-gauge/fw-signing.key, 32 random bytes, not in the repo - and
      its public half beside it (.pub), which scripts/build.sh builds into the
      firmware as FW_SIGN_PUB. Each builder their own key; the repo has none.
  scripts/release-fw.py --pub
      Writes the .pub again from the key (wherever it is - see below).
  scripts/release-fw.py --to server:/path/to/data/firmware
      Builds the car firmware (scripts/build.sh), signs it, and copies the
      image, its signature and "latest" there with scp - "latest" last, so
      the gauge never sees it before the image.

The key is looked for first in the keyring (the Secret Service - GNOME
Keyring, KWallet, or KeePassXC with its Secret Service integration on): the
entry whose attributes are service=n-gauge and key=fw-signing, the key as 64
hex digits in its password. Then in the FW_KEY file (raw 32 bytes, or the 64
hex digits). Once it is in the keyring, the file can go; the .pub stays.

The signature is standard Ed25519 (RFC 8032 - as minisign, signify and
OpenSSL make it) over 40 bytes: the image's SHA-256, then its build time
(FW_BUILD_TIME, Unix seconds) as 8 bytes little-endian. "latest" is
"<app hash> <size> <build time>". The gauge refuses a build time not after
its own, so an old signed release cannot be served as an update.
"""

import argparse
import hashlib
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(REPO, "firmware", "car_gauge_v1")
IMAGE = os.path.join(REPO, ".build", "car_gauge_v1", "car_gauge_v1.ino.bin")
BUILD_INFO = os.path.join(FW, "build_info.h")
KEY = os.environ.get("FW_KEY", os.path.expanduser("~/.config/n-gauge/fw-signing.key"))
PUB = os.path.splitext(KEY)[0] + ".pub"
KEYRING = ["service", "n-gauge", "key", "fw-signing"]
# esp_app_desc_t follows the image header (24 bytes) and the first segment
# header (8): its magic word, then app_elf_sha256 at 144 into it.
APP_DESC = 24 + 8
APP_DESC_MAGIC = 0xABCD5432
ELF_SHA_AT = APP_DESC + 144


def from_keyring() -> bytes | None:
    if not shutil.which("secret-tool"):
        return None
    r = subprocess.run(["secret-tool", "lookup", *KEYRING], capture_output=True, text=True)
    s = r.stdout.strip()
    return bytes.fromhex(s) if re.fullmatch(r"[0-9a-fA-F]{64}", s) else None


def from_file() -> bytes | None:
    if not os.path.exists(KEY):
        return None
    with open(KEY, "rb") as f:
        raw = f.read()
    if len(raw) == 32:
        return raw
    s = raw.decode(errors="replace").strip()
    if re.fullmatch(r"[0-9a-fA-F]{64}", s):
        return bytes.fromhex(s)
    sys.exit(f"{KEY} is neither 32 bytes nor 64 hex digits")


def load_key() -> Ed25519PrivateKey:
    seed = from_keyring() or from_file()
    if not seed:
        sys.exit(f"no signing key: not in the keyring ({' '.join(KEYRING)}), no {KEY} - make one with --init")
    return Ed25519PrivateKey.from_private_bytes(seed)


def pub_hex(key: Ed25519PrivateKey) -> str:
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


def write_pub(key: Ed25519PrivateKey) -> str:
    pub = pub_hex(key)
    os.makedirs(os.path.dirname(PUB), mode=0o700, exist_ok=True)
    with open(PUB, "w") as f:
        f.write(pub + "\n")
    return pub


def init() -> None:
    if from_keyring() or os.path.exists(KEY):
        sys.exit("a signing key exists already (keyring or file) - not replacing it")
    os.makedirs(os.path.dirname(KEY), mode=0o700, exist_ok=True)
    fd = os.open(KEY, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(secrets.token_bytes(32))
    pub = write_pub(load_key())
    print(f"made {KEY} - put it in your password manager (see --help): without it no update can be signed")
    print(f"and {PUB}, which scripts/build.sh builds into the firmware: {pub}")


def build_time() -> int:
    with open(BUILD_INFO) as f:
        m = re.search(r"#define FW_BUILD_TIME (\d+)UL", f.read())
    if not m:
        sys.exit(f"no FW_BUILD_TIME in {BUILD_INFO} - built without scripts/build.sh?")
    return int(m.group(1))


def release(dest: str) -> None:
    key = load_key()
    pub = pub_hex(key)
    # What the build embeds: a .pub from another key would make a firmware
    # that refuses every update signed here. A missing one (the key moved
    # to the keyring, the folder cleared with it) is made again from the key.
    if "FW_SIGN_PUB" not in os.environ and not os.path.exists(PUB):
        print(f"no {PUB} - made again from the key")
        write_pub(key)
    on_file = open(PUB).read().strip() if os.path.exists(PUB) else ""
    if os.environ.get("FW_SIGN_PUB", on_file) != pub:
        sys.exit(f"the build's public key is not this key's ({pub}) - run --pub, or fix FW_SIGN_PUB")
    subprocess.run([os.path.join(REPO, "scripts", "build.sh"), "gauge"], check=True)
    with open(IMAGE, "rb") as f:
        image = f.read()
    if int.from_bytes(image[APP_DESC:APP_DESC + 4], "little") != APP_DESC_MAGIC:
        sys.exit(f"{IMAGE} has no app description - not an ESP32 app image?")
    app_hash = image[ELF_SHA_AT:ELF_SHA_AT + 32].hex()
    built = build_time()
    sig = key.sign(hashlib.sha256(image).digest() + built.to_bytes(8, "little"))
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in (("car_gauge_v1.bin", image), ("car_gauge_v1.bin.sig", sig),
                           ("latest", f"{app_hash} {len(image)} {built}\n".encode())):
            with open(os.path.join(tmp, name), "wb") as f:
                f.write(data)
        files = [os.path.join(tmp, n) for n in ("car_gauge_v1.bin", "car_gauge_v1.bin.sig")]
        subprocess.run(["scp", "-q", *files, dest + "/"], check=True)
        subprocess.run(["scp", "-q", os.path.join(tmp, "latest"), dest + "/"], check=True)
    print(f"released {app_hash[:8]} ({len(image)} bytes, built {built}) to {dest}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--init", action="store_true", help="make the signing key")
    p.add_argument("--pub", action="store_true", help="write the .pub again from the key")
    p.add_argument("--to", help="scp destination folder, e.g. server:/path/to/data/firmware")
    a = p.parse_args()
    if a.init:
        init()
    elif a.pub:
        print(f"{PUB}: {write_pub(load_key())}")
    elif a.to:
        release(a.to)
    else:
        p.print_help()


if __name__ == "__main__":
    main()
