#!/usr/bin/env python3
"""
module_sign.py - AuroraOS Module Signing Tool (ECDSA P-256)

FIXED (v4.3.8): SEC-004 — Offline module signing tool using ECDSA on the
secp256r1 (NIST P-256) curve.  Generates signatures compatible with the
kernel's module_sign_verify() in module_sign.c.

Signature format (appended to the .ko file):
  [module ELF data] [module_sign_header]

  module_sign_header:
    uint64_t magic      = 0x4543445341534947 ("ECDSASIG")
    uint32_t version    = 2
    uint32_t sig_size   = 64
    uint8_t  signature[64]  = r (32 bytes) || s (32 bytes, big-endian)
    uint8_t  reserved[32]   = zero padding

The SHA-256 hash is computed over the module ELF data (excluding the
signature header).  The signature is an ECDSA P-256 signature of that
hash, encoded as two 32-byte big-endian integers (r || s).

Usage:
  python module_sign.py --gen-key [--output privkey.pem]
  python module_sign.py --sign module.ko --key privkey.pem
  python module_sign.py --sign module.ko --key privkey.pem --output signed.ko
  python module_sign.py --verify module.ko --key pubkey.pem
  python module_sign.py --extract-pubkey privkey.pem [--output pubkey.pem]

Dependencies:
  pip install ecdsa
"""

import argparse
import hashlib
import struct
import sys
import os

# ================================================================
# Module signature header constants (must match module_sign.c)
# ================================================================
MODULE_SIGN_MAGIC    = 0x4543445341534947  # "ECDSASIG"
MODULE_SIGN_VERSION  = 2
MODULE_SIGN_SIZE     = 64                   # r (32) + s (32)
MODULE_SIGN_RESERVED = 32
HEADER_TOTAL_SIZE    = 8 + 4 + 4 + MODULE_SIGN_SIZE + MODULE_SIGN_RESERVED  # = 112

# ================================================================
# Key generation and ECDSA P-256 signing
# ================================================================

def generate_keypair():
    """Generate a new ECDSA P-256 (secp256r1) key pair.
    Returns (signing_key, verifying_key) from the ecdsa library."""
    try:
        from ecdsa import SigningKey, NIST256p
    except ImportError:
        print("ERROR: 'ecdsa' library not found. Install with: pip install ecdsa",
              file=sys.stderr)
        sys.exit(1)

    sk = SigningKey.generate(curve=NIST256p)
    vk = sk.verifying_key
    return sk, vk


def load_private_key(path):
    """Load an ECDSA P-256 private key from a PEM file."""
    try:
        from ecdsa import SigningKey, NIST256p
    except ImportError:
        print("ERROR: 'ecdsa' library not found. Install with: pip install ecdsa",
              file=sys.stderr)
        sys.exit(1)

    with open(path, 'rb') as f:
        data = f.read()
    try:
        sk = SigningKey.from_pem(data)
    except Exception:
        # Try DER format
        try:
            sk = SigningKey.from_der(data)
        except Exception as e:
            print(f"ERROR: Failed to load private key from {path}: {e}",
                  file=sys.stderr)
            sys.exit(1)
    return sk


def load_public_key(path):
    """Load an ECDSA P-256 public key from a PEM file."""
    try:
        from ecdsa import VerifyingKey, NIST256p
    except ImportError:
        print("ERROR: 'ecdsa' library not found. Install with: pip install ecdsa",
              file=sys.stderr)
        sys.exit(1)

    with open(path, 'rb') as f:
        data = f.read()
    try:
        vk = VerifyingKey.from_pem(data)
    except Exception:
        try:
            vk = VerifyingKey.from_der(data)
        except Exception as e:
            print(f"ERROR: Failed to load public key from {path}: {e}",
                  file=sys.stderr)
            sys.exit(1)
    return vk


def save_private_key(sk, path):
    """Save an ECDSA private key to a PEM file."""
    pem_data = sk.to_pem()
    with open(path, 'wb') as f:
        f.write(pem_data)
    print(f"Private key saved to: {path}")
    # Set restrictive permissions on Unix
    if os.name != 'nt':
        os.chmod(path, 0o600)


def save_public_key(vk, path):
    """Save an ECDSA public key to a PEM file."""
    pem_data = vk.to_pem()
    with open(path, 'wb') as f:
        f.write(pem_data)
    print(f"Public key saved to: {path}")


def sign_module(sk, module_data):
    """Sign module data with ECDSA P-256.
    Returns the 64-byte signature (r || s, big-endian)."""
    # Compute SHA-256 of the module data
    digest = hashlib.sha256(module_data).digest()

    # Sign the digest
    signature_der = sk.sign_digest(digest, sigencode=signature_to_der)

    # Decode DER signature to (r, s) integers
    r, s = decode_der_signature(signature_der)

    # Convert to 32-byte big-endian
    r_bytes = r.to_bytes(32, byteorder='big')
    s_bytes = s.to_bytes(32, byteorder='big')

    return r_bytes + s_bytes, digest


def signature_to_der(r, s, order):
    """Encode (r, s) as a DER signature.  Used as sigencode callback."""
    from ecdsa.util import sigencode_der
    return sigencode_der(r, s, order)


def verify_module(vk, module_data, signature_bytes):
    """Verify an ECDSA P-256 signature over module data.
    Returns True if valid, False otherwise."""
    digest = hashlib.sha256(module_data).digest()

    # Decode signature from 64-byte r||s to DER
    r = int.from_bytes(signature_bytes[:32], byteorder='big')
    s = int.from_bytes(signature_bytes[32:64], byteorder='big')

    try:
        signature_der = encode_der_signature(r, s)
    except ValueError as e:
        print(f"ERROR: Invalid signature values: {e}", file=sys.stderr)
        return False

    try:
        return vk.verify_digest(signature_der, digest)
    except Exception as e:
        print(f"ERROR: Signature verification failed: {e}", file=sys.stderr)
        return False


def decode_der_signature(der_bytes):
    """Decode a DER-encoded ECDSA signature to (r, s) integers."""
    if der_bytes[0] != 0x30:
        raise ValueError("Invalid DER signature: expected 0x30 sequence tag")

    total_len = der_bytes[1]
    pos = 2

    # Read r
    if der_bytes[pos] != 0x02:
        raise ValueError("Invalid DER signature: expected 0x02 integer tag for r")
    r_len = der_bytes[pos + 1]
    pos += 2
    r_bytes = der_bytes[pos:pos + r_len]
    pos += r_len
    r = int.from_bytes(r_bytes, byteorder='big')

    # Read s
    if der_bytes[pos] != 0x02:
        raise ValueError("Invalid DER signature: expected 0x02 integer tag for s")
    s_len = der_bytes[pos + 1]
    pos += 2
    s_bytes = der_bytes[pos:pos + s_len]
    s = int.from_bytes(s_bytes, byteorder='big')

    return r, s


def encode_der_signature(r, s):
    """Encode (r, s) integers as a DER-encoded ECDSA signature."""
    def encode_int(v):
        b = v.to_bytes((v.bit_length() + 7) // 8, byteorder='big') or b'\x00'
        if b[0] & 0x80:
            b = b'\x00' + b
        return b

    r_enc = encode_int(r)
    s_enc = encode_int(s)

    der = b'\x30' + bytes([len(r_enc) + len(s_enc) + 4])
    der += b'\x02' + bytes([len(r_enc)]) + r_enc
    der += b'\x02' + bytes([len(s_enc)]) + s_enc
    return der


# ================================================================
# Signature header I/O
# ================================================================

def build_signature_header(signature_bytes):
    """Build the module_sign_header binary blob."""
    if len(signature_bytes) != MODULE_SIGN_SIZE:
        raise ValueError(f"Signature must be {MODULE_SIGN_SIZE} bytes, "
                         f"got {len(signature_bytes)}")

    header = struct.pack('<Q', MODULE_SIGN_MAGIC)       # uint64_t magic
    header += struct.pack('<I', MODULE_SIGN_VERSION)     # uint32_t version
    header += struct.pack('<I', MODULE_SIGN_SIZE)        # uint32_t sig_size
    header += signature_bytes                            # uint8_t[64]
    header += b'\x00' * MODULE_SIGN_RESERVED             # uint8_t[32] reserved

    assert len(header) == HEADER_TOTAL_SIZE, \
        f"Header size mismatch: {len(header)} != {HEADER_TOTAL_SIZE}"
    return header


def parse_signature_header(data):
    """Parse a module_sign_header from binary data.
    Returns (module_data, signature_bytes) or raises ValueError."""
    if len(data) < HEADER_TOTAL_SIZE:
        raise ValueError(f"File too small for signature header "
                         f"({len(data)} < {HEADER_TOTAL_SIZE})")

    hdr_start = len(data) - HEADER_TOTAL_SIZE
    module_data = data[:hdr_start]
    hdr = data[hdr_start:]

    magic, version, sig_size = struct.unpack_from('<QII', hdr, 0)

    if magic != MODULE_SIGN_MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:016X} (expected 0x{MODULE_SIGN_MAGIC:016X})")
    if version != MODULE_SIGN_VERSION:
        raise ValueError(f"Unsupported version: {version} (expected {MODULE_SIGN_VERSION})")
    if sig_size != MODULE_SIGN_SIZE:
        raise ValueError(f"Bad signature size: {sig_size} (expected {MODULE_SIGN_SIZE})")

    signature_bytes = hdr[16:16 + MODULE_SIGN_SIZE]
    return module_data, signature_bytes


# ================================================================
# Public key extraction for kernel embedding
# ================================================================

def extract_pubkey_hex(vk):
    """Extract the public key as (Qx, Qy) hex strings for kernel embedding.
    The kernel uses little-endian 64-bit limb format (u256)."""
    # Get the raw public key point in uncompressed format (04 || x || y)
    pubkey_bytes = vk.to_string()  # 04 || x (32 bytes) || y (32 bytes)

    x_bytes = pubkey_bytes[1:33]   # 32 bytes, big-endian
    y_bytes = pubkey_bytes[33:65]  # 32 bytes, big-endian

    # Convert to little-endian 64-bit limbs for kernel u256 format
    def to_le_limbs(b):
        # Parse as big-endian 256-bit integer, then split into 4 x 64-bit LE
        val = int.from_bytes(b, byteorder='big')
        limbs = []
        for i in range(4):
            limbs.append(val & 0xFFFFFFFFFFFFFFFF)
            val >>= 64
        return limbs

    qx_limbs = to_le_limbs(x_bytes)
    qy_limbs = to_le_limbs(y_bytes)

    return qx_limbs, qy_limbs


def print_pubkey_header(qx_limbs, qy_limbs):
    """Print public key in a format suitable for kernel C header inclusion."""
    print("\n/* Embedded ECDSA P-256 public key for kernel module_sign.c */")
    print("/* Generated by module_sign.py --extract-pubkey */")
    print()
    print("#define MODULE_PUBKEY_QX0  0x{:016X}ULL".format(qx_limbs[0]))
    print("#define MODULE_PUBKEY_QX1  0x{:016X}ULL".format(qx_limbs[1]))
    print("#define MODULE_PUBKEY_QX2  0x{:016X}ULL".format(qx_limbs[2]))
    print("#define MODULE_PUBKEY_QX3  0x{:016X}ULL".format(qx_limbs[3]))
    print("#define MODULE_PUBKEY_QY0  0x{:016X}ULL".format(qy_limbs[0]))
    print("#define MODULE_PUBKEY_QY1  0x{:016X}ULL".format(qy_limbs[1]))
    print("#define MODULE_PUBKEY_QY2  0x{:016X}ULL".format(qy_limbs[2]))
    print("#define MODULE_PUBKEY_QY3  0x{:016X}ULL".format(qy_limbs[3]))


# ================================================================
# Command handlers
# ================================================================

def cmd_gen_key(args):
    """Generate a new ECDSA P-256 key pair."""
    sk, vk = generate_keypair()

    priv_path = args.output or "module_privkey.pem"
    pub_path = priv_path.replace(".pem", "_pub.pem")
    if pub_path == priv_path:
        pub_path = priv_path + ".pub"

    save_private_key(sk, priv_path)
    save_public_key(vk, pub_path)

    print(f"\nKey pair generated successfully!")
    print(f"  Private key: {priv_path}")
    print(f"  Public key:  {pub_path}")
    print(f"\nTo sign a module:")
    print(f"  python module_sign.py --sign module.ko --key {priv_path}")
    print(f"\nTo embed the public key in the kernel, rebuild with:")
    print(f"  python module_sign.py --extract-pubkey {priv_path}")


def cmd_sign(args):
    """Sign a module file with ECDSA P-256."""
    sk = load_private_key(args.key)

    # Read module data
    with open(args.module, 'rb') as f:
        module_data = f.read()

    if len(module_data) == 0:
        print("ERROR: Module file is empty", file=sys.stderr)
        sys.exit(1)

    # Sign the module
    signature_bytes, digest = sign_module(sk, module_data)

    # Build the signature header
    header = build_signature_header(signature_bytes)

    # Write the signed module
    output_path = args.output or args.module
    with open(output_path, 'wb') as f:
        f.write(module_data)
        f.write(header)

    print(f"Module signed successfully!")
    print(f"  Input:  {args.module}")
    print(f"  Output: {output_path}")
    print(f"  SHA-256: {digest.hex()}")
    print(f"  Signature: {signature_bytes.hex()}")


def cmd_verify(args):
    """Verify a module's ECDSA P-256 signature."""
    vk = load_public_key(args.key)

    with open(args.module, 'rb') as f:
        data = f.read()

    try:
        module_data, signature_bytes = parse_signature_header(data)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    if verify_module(vk, module_data, signature_bytes):
        digest = hashlib.sha256(module_data).hexdigest()
        print(f"Signature VERIFIED successfully!")
        print(f"  Module: {args.module}")
        print(f"  SHA-256: {digest}")
    else:
        print("ERROR: Signature verification FAILED!", file=sys.stderr)
        sys.exit(1)


def cmd_extract_pubkey(args):
    """Extract the public key from a private key PEM file."""
    sk = load_private_key(args.key)
    vk = sk.verifying_key

    qx_limbs, qy_limbs = extract_pubkey_hex(vk)

    if args.output:
        with open(args.output, 'w') as f:
            f.write("/* Embedded ECDSA P-256 public key for AuroraOS kernel */\n")
            f.write("/* Generated by module_sign.py --extract-pubkey */\n\n")
            f.write("#define MODULE_PUBKEY_QX0  0x{:016X}ULL\n".format(qx_limbs[0]))
            f.write("#define MODULE_PUBKEY_QX1  0x{:016X}ULL\n".format(qx_limbs[1]))
            f.write("#define MODULE_PUBKEY_QX2  0x{:016X}ULL\n".format(qx_limbs[2]))
            f.write("#define MODULE_PUBKEY_QX3  0x{:016X}ULL\n".format(qx_limbs[3]))
            f.write("#define MODULE_PUBKEY_QY0  0x{:016X}ULL\n".format(qy_limbs[0]))
            f.write("#define MODULE_PUBKEY_QY1  0x{:016X}ULL\n".format(qy_limbs[1]))
            f.write("#define MODULE_PUBKEY_QY2  0x{:016X}ULL\n".format(qy_limbs[2]))
            f.write("#define MODULE_PUBKEY_QY3  0x{:016X}ULL\n".format(qy_limbs[3]))
        print(f"Public key header written to: {args.output}")
    else:
        print_pubkey_header(qx_limbs, qy_limbs)


# ================================================================
# Main entry point
# ================================================================

def main():
    parser = argparse.ArgumentParser(
        description="AuroraOS Module Signing Tool (ECDSA P-256)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  Generate a new key pair:
    python module_sign.py --gen-key

  Sign a module:
    python module_sign.py --sign mymodule.ko --key privkey.pem

  Verify a signed module:
    python module_sign.py --verify mymodule.ko --key pubkey.pem

  Extract public key for kernel embedding:
    python module_sign.py --extract-pubkey privkey.pem
""")

    subparsers = parser.add_subparsers(dest='command', help='Commands')

    # --gen-key
    gen_parser = subparsers.add_parser('--gen-key', help='Generate a new ECDSA P-256 key pair')
    gen_parser.add_argument('--output', '-o', metavar='FILE',
                            help='Output path for private key (default: module_privkey.pem)')

    # --sign
    sign_parser = subparsers.add_parser('--sign', help='Sign a module file')
    sign_parser.add_argument('--module', '-m', required=True, metavar='FILE',
                             help='Module file to sign (.ko)')
    sign_parser.add_argument('--key', '-k', required=True, metavar='FILE',
                             help='Private key PEM file')
    sign_parser.add_argument('--output', '-o', metavar='FILE',
                             help='Output path for signed module (default: overwrite input)')

    # --verify
    verify_parser = subparsers.add_parser('--verify', help='Verify a signed module')
    verify_parser.add_argument('--module', '-m', required=True, metavar='FILE',
                               help='Signed module file to verify')
    verify_parser.add_argument('--key', '-k', required=True, metavar='FILE',
                               help='Public key PEM file')

    # --extract-pubkey
    extract_parser = subparsers.add_parser('--extract-pubkey',
                                           help='Extract public key from private key for kernel embedding')
    extract_parser.add_argument('--key', '-k', required=True, metavar='FILE',
                                help='Private key PEM file')
    extract_parser.add_argument('--output', '-o', metavar='FILE',
                                help='Output header file (default: print to stdout)')

    args = parser.parse_args()

    if args.command == '--gen-key':
        cmd_gen_key(args)
    elif args.command == '--sign':
        cmd_sign(args)
    elif args.command == '--verify':
        cmd_verify(args)
    elif args.command == '--extract-pubkey':
        cmd_extract_pubkey(args)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()