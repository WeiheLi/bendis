#!/usr/bin/env python3
"""Generates rtl/bendis_hash.v: the CRC hashes of src/p4_model.h (kCrc) as XOR matrices.

A CRC with fixed polynomial, init, reflection and output xor is affine in its input, so each output
bit is the parity of a fixed subset of the 64 key bits, xored with a constant."""
import sys


def rev(x, bits):
    return int(format(x, f'0{bits}b')[::-1], 2)


def crc32(key, poly, refl, init, xorout):
    c = init
    for i in range(8):
        by = (key >> (56 - 8 * i)) & 0xFF
        if refl:
            by = rev(by, 8)
        c ^= by << 24
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    if refl:
        c = rev(c, 32)
    return c ^ xorout


def crc16(key, poly):
    c = 0
    for i in range(8):
        by = rev((key >> (56 - 8 * i)) & 0xFF, 8)
        c ^= by << 8
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return rev(c, 16)


HASHES = [
    ('h_cm0', 32, lambda k: crc32(k, 0x04C11DB7, True, 0xFFFFFFFF, 0xFFFFFFFF)),
    ('h_cm1', 32, lambda k: crc32(k, 0x1EDC6F41, True, 0xFFFFFFFF, 0xFFFFFFFF)),
    ('h_cm2', 32, lambda k: crc32(k, 0x741B8CD7, True, 0xFFFFFFFF, 0xFFFFFFFF)),
    ('h_tab0', 32, lambda k: crc32(k, 0x814141AB, False, 0, 0)),
    ('h_tab1', 32, lambda k: crc32(k, 0xA833982B, True, 0xFFFFFFFF, 0xFFFFFFFF)),
    ('h_fp', 16, lambda k: crc16(k, 0x8005)),
]


def main(out):
    lines = ['// CRC hashes of p4/bendis.p4 (counting-row and onset-table indexes, fingerprint) as XOR matrices (generated).',
             '`default_nettype none', 'module bendis_hash (', '    input  wire [63:0] key,']
    ports = [f'    output wire [{w - 1}:0] {n}' for n, w, _ in HASHES]
    lines.append(',\n'.join(ports))
    lines.append(');')
    for name, width, f in HASHES:
        base = f(0)
        cols = [f(1 << j) ^ base for j in range(64)]
        for i in range(width):
            mask = sum(1 << j for j in range(64) if (cols[j] >> i) & 1)
            const = (base >> i) & 1
            lines.append(f"    assign {name}[{i}] = ^(key & 64'h{mask:016X}) ^ 1'b{const};")
    lines += ['endmodule', '`default_nettype wire', '']
    open(out, 'w').write('\n'.join(lines))
    # self-check against the reference functions
    import random
    rnd = random.Random(1)
    for _ in range(2000):
        k = rnd.getrandbits(64)
        for name, width, f in HASHES:
            base = f(0)
            v = base
            for j in range(64):
                if (k >> j) & 1:
                    v ^= f(1 << j) ^ base
            assert v == f(k), name


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'rtl/bendis_hash.v')
