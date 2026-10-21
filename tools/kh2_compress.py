#!/usr/bin/env python3
"""KH2 archive (IMG) compression: backwards LZ with an escape key byte.

Port of OpenKh.Kh2/Img.Decompress (MIT, OpenKH project).
Layout: data is read from the END. Trailing zero padding, then a key byte,
then the decompressed size (big-endian when read backwards), then the stream.
A key byte followed by n>0 and len copies len+3 bytes from n bytes ahead
(output is also written back to front); key followed by 0 is a literal key.
"""
import sys

def decompress(src: bytes) -> bytes:
    i = len(src) - 1
    key = 0
    while i > 0:
        key = src[i]; i -= 1
        if key != 0:
            break
    if i <= 0:
        return b""
    size = src[i] | (src[i-1] << 8) | (src[i-2] << 16) | (src[i-3] << 24)
    i -= 4
    dst = bytearray(size)
    o = size - 1
    while o >= 0 and i >= 0:
        b = src[i]; i -= 1
        if b == key and i >= 0:
            dist = src[i]; i -= 1
            if dist > 0 and i >= 0:
                n = src[i] + 3; i -= 1
                for _ in range(n):
                    if o < 0:
                        break
                    dst[o] = dst[o + dist]  # C#: dst[o--] = dst[o_after + dist + 1]
                    o -= 1
            else:
                dst[o] = b; o -= 1
        else:
            dst[o] = b; o -= 1
    return bytes(dst)

def stored_size(block: int) -> int:
    """Bytes occupied on disc by a compressed IDX entry."""
    return ((block & 0x3FFF) + 1) * 0x800

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    out = decompress(data)
    open(sys.argv[2], "wb").write(out)
    print(f"{len(data)} -> {len(out)} bytes")
