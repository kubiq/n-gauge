#!/usr/bin/env python3
"""The gauge's firmware signing key, and the signing of a release.

A release is a v* tag: CI builds the firmware, signs it here (--sign-into)
and puts it in a GitHub release. The upload server passes the latest one on
(server/upload_server.py, FW_GITHUB_REPO); the gauge looks there before each
upload (upload.cpp), downloads it to its card, checks the signature against
the public key built into it (FW_SIGN_PUB, from scripts/build.sh) and
installs it before it next sleeps. The app can install one from GitHub too.
Only an image signed with the private key is ever installed, and only one
built later than the firmware it replaces.

  scripts/release-fw.py --init
      Once: makes the signing key - FW_KEY, default
      ~/.config/n-gauge/fw-signing.key, 32 random bytes, not in the repo - and
      its public half beside it (.pub), which scripts/build.sh builds into the
      firmware as FW_SIGN_PUB. Each builder their own key; the repo has none.
  scripts/release-fw.py --pub
      Writes the .pub again from the key (wherever it is - see below).
  scripts/release-fw.py --sign-into DIR
      CI (.github/workflows/ci.yml): signs the last car build into DIR,
      without building - the files a GitHub release carries.

The key is FW_SIGN_KEY (64 hex digits) when that is set - CI's, from a
GitHub secret. Otherwise it is looked for in the keyring (the Secret Service - GNOME
Keyring, KWallet, or KeePassXC with its Secret Service integration on): the
entry whose attributes are service=n-gauge and key=fw-signing, the key as 64
hex digits in its password. Then in the FW_KEY file (raw 32 bytes, or the 64
hex digits). Once it is in the keyring, the file can go; the .pub stays.

The signature is standard Ed25519 (RFC 8032 - as minisign, signify and
OpenSSL make it) over 40 bytes: the image's SHA-256, then its build time
(FW_BUILD_TIME, Unix seconds) as 8 bytes little-endian. "latest" is
"<app hash> <size> <build time> <commit>". The gauge refuses a build time not after
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


def from_env() -> bytes | None:
    """FW_SIGN_KEY, 64 hex digits - CI's, from a GitHub secret."""
    s = os.environ.get("FW_SIGN_KEY", "").strip()
    if not s:
        return None
    if not re.fullmatch(r"[0-9a-fA-F]{64}", s):
        sys.exit("FW_SIGN_KEY is set but not 64 hex digits")
    return bytes.fromhex(s)


def load_key() -> Ed25519PrivateKey:
    seed = from_env() or from_keyring() or from_file()
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


def build_name() -> str:
    """The build as people see it: the git commit of its stamp ("48cfc3a",
    "+changes" kept) - what the gauge's settings and the app show. The
    image's own hash stays inside, for telling builds apart (2026-11-11:
    two hashes for one build were confusing)."""
    with open(BUILD_INFO) as f:
        m = re.search(r'#define FW_BUILD "\S+ \S+ ([^" ]+)', f.read())
    return m.group(1) if m else "unknown"


def sign_into(key: Ed25519PrivateKey, out: str) -> tuple[str, int]:
    """The last car build (IMAGE, and its build_info.h) signed into `out`:
    car_gauge_v1.bin, car_gauge_v1.bin.sig and latest - what a GitHub
    release carries (.github/workflows/ci.yml), and the server passes on.
    Returns the build's name (its commit) and size."""
    with open(IMAGE, "rb") as f:
        image = f.read()
    if int.from_bytes(image[APP_DESC:APP_DESC + 4], "little") != APP_DESC_MAGIC:
        sys.exit(f"{IMAGE} has no app description - not an ESP32 app image?")
    app_hash = image[ELF_SHA_AT:ELF_SHA_AT + 32].hex()
    built = build_time()
    name = build_name()
    sig = key.sign(hashlib.sha256(image).digest() + built.to_bytes(8, "little"))
    os.makedirs(out, exist_ok=True)
    # The commit fourth: shown by the gauge and the app; gauges from before
    # 2026-11-11 read the first three only.
    for file, data in (("car_gauge_v1.bin", image), ("car_gauge_v1.bin.sig", sig),
                       ("latest", f"{app_hash} {len(image)} {built} {name}\n".encode())):
        with open(os.path.join(out, file), "wb") as f:
            f.write(data)
    return name, len(image)


def sign_existing(out: str) -> None:
    """CI: the build check.sh made, signed - FW_SIGN_KEY from a secret. Its
    public half must be the one built in (FW_SIGN_PUB), or the gauges would
    refuse every release."""
    key = load_key()
    pub = pub_hex(key)
    if os.environ.get("FW_SIGN_PUB", "") != pub:
        sys.exit(f"FW_SIGN_PUB is not this key's public half ({pub})")
    if f'#define FW_SIGN_PUB "{pub}"' not in open(BUILD_INFO).read():
        sys.exit(f"the last build ({BUILD_INFO}) does not carry this key's public half")
    name, size = sign_into(key, out)
    print(f"signed {name} ({size} bytes) into {out}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--init", action="store_true", help="make the signing key")
    p.add_argument("--pub", action="store_true", help="write the .pub again from the key")
    p.add_argument("--sign-into", metavar="DIR",
                   help="CI: sign the last car build into DIR (no build)")
    a = p.parse_args()
    if a.init:
        init()
    elif a.pub:
        print(f"{PUB}: {write_pub(load_key())}")
    elif a.sign_into:
        sign_existing(a.sign_into)
    else:
        p.print_help()


if __name__ == "__main__":
    main()
