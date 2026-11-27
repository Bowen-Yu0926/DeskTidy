#!/usr/bin/env python3
"""Generate the DeskTidy update-signing keypair and sign release manifests.

WHY: the SHA256 digest and the installer both live in the same GitHub release.
Anyone who can publish a release can replace both, so a digest-only check
cannot stop a compromised-account attacker. The Ed25519 signature does,
because only this private key can author a manifest the client accepts.

PRIVATE KEY HANDLING — read this before running:
  * The private key is written to a file OUTSIDE the repo (default
    ~/.desktidy/update_signing.key) and must never be committed or uploaded.
  * For CI, store it as a repository secret (base64 of the raw 32 bytes).
  * If the key leaks, anyone can sign a malicious update. Rotate: generate a
    new pair, release a build with the new public key, then destroy the old.

Usage:
  # one-time: create the pair (prints the client pubkey to paste into source)
  python scripts/update_signing.py genkey

  # sign an installer before publishing (writes <exe>.manifest + .sig)
  python scripts/update_signing.py sign dist/DeskTidy_Setup_4.0.58.exe --version 4.0.58
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
from pathlib import Path

# Windows' default console encoding (cp1252 in CI) cannot represent the Chinese
# status strings printed below, and `print` raises UnicodeEncodeError — which
# aborted the release AFTER the signature had already been written. Force UTF-8
# on stdout/stderr; no-op on platforms that are already UTF-8.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
        Ed25519PublicKey,
    )
except ImportError:  # pragma: no cover - guidance only
    sys.exit(
        "缺少 cryptography 依赖：pip install cryptography"
    )

DEFAULT_KEY_PATH = Path.home() / ".desktidy" / "update_signing.key"


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def cmd_genkey(args: argparse.Namespace) -> int:
    key_path = Path(args.key).expanduser()
    if key_path.exists() and not args.force:
        sys.exit(f"拒绝覆盖已存在的私钥：{key_path}（如确需重生成，加 --force）")

    private = Ed25519PrivateKey.generate()
    raw_priv = private.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    raw_pub = private.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )

    key_path.parent.mkdir(parents=True, exist_ok=True)
    # 0600: owner-read/write only.
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as fh:
        fh.write(raw_priv)

    print(f"私钥已写入：{key_path}")
    print("  ⚠️  务必备份并离线保管；泄露=任何人可伪造更新。\n")
    print("客户端公钥（粘贴到 native/src/update_verify.cpp 的 kBuiltinSigningKeyB64）：")
    print(f'  constexpr const char* kBuiltinSigningKeyB64 = "{_b64(raw_pub)}";\n')
    print("CI secret（base64 形式，存入 GitHub Secrets，名如 UPDATE_SIGNING_KEY_B64）：")
    print(f"  {_b64(raw_priv)}")
    print("\n公钥指纹(sha256)：", hashlib.sha256(raw_pub).hexdigest())
    return 0


def cmd_sign(args: argparse.Namespace) -> int:
    key_path = Path(args.key).expanduser()
    if not key_path.exists():
        sys.exit(f"找不到私钥：{key_path}（先运行 genkey）")

    raw_priv = key_path.read_bytes()
    if len(raw_priv) != 32:
        sys.exit(f"私钥长度异常（应为 32 字节）：{len(raw_priv)}")
    private = Ed25519PrivateKey.from_private_bytes(raw_priv)

    exe = Path(args.installer)
    if not exe.exists():
        sys.exit(f"找不到安装包：{exe}")

    exe_sha = hashlib.sha256(exe.read_bytes()).hexdigest()
    manifest = json.dumps(
        {
            "schema": 1,
            "version": args.version,
            "asset": exe.name,
            "exe_sha256": exe_sha,
            "exe_size": exe.stat().st_size,
        },
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")

    signature = private.sign(manifest)

    manifest_path = exe.with_suffix(exe.suffix + ".manifest")
    sig_path = exe.with_suffix(exe.suffix + ".sig")
    manifest_path.write_bytes(manifest)
    sig_path.write_text(_b64(signature), encoding="ascii")

    print(f"安装包 sha256 : {exe_sha}")
    print(f"清单           : {manifest_path}")
    print(f"签名           : {sig_path}")
    print("\n发布时把这两个文件一起上传到同一个 release。")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    """Local self-check: verify a .sig against a .manifest and a public key."""
    sig = base64.b64decode(Path(args.sig).read_text().strip())
    manifest = Path(args.manifest).read_bytes()
    pub_raw = base64.b64decode(args.pubkey) if args.pubkey else None
    if pub_raw is None:
        key_path = Path(args.key).expanduser()
        private = Ed25519PrivateKey.from_private_bytes(key_path.read_bytes())
        pub = private.public_key()
    else:
        pub = Ed25519PublicKey.from_public_bytes(pub_raw)
    try:
        pub.verify(sig, manifest)
    except Exception:
        print("签名校验失败")
        return 1
    print("签名校验通过")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="DeskTidy update signing")
    sub = parser.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("genkey", help="生成 Ed25519 密钥对")
    g.add_argument("--key", default=str(DEFAULT_KEY_PATH))
    g.add_argument("--force", action="store_true")
    g.set_defaults(func=cmd_genkey)

    s = sub.add_parser("sign", help="为安装包生成清单和签名")
    s.add_argument("installer")
    s.add_argument("--version", required=True)
    s.add_argument("--key", default=str(DEFAULT_KEY_PATH))
    s.set_defaults(func=cmd_sign)

    v = sub.add_parser("verify", help="本地校验签名")
    v.add_argument("manifest")
    v.add_argument("sig")
    v.add_argument("--key", default=str(DEFAULT_KEY_PATH))
    v.add_argument("--pubkey", default="", help="base64 公钥（优先于 --key）")
    v.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
