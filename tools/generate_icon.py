"""Rasterize the existing five-bar brand into a transparent multi-size ICO."""
from pathlib import Path
import math
import struct
import zlib

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

def inside_round(x, y, size):
    radius = size / 3
    dx = max(abs(x - size / 2) - (size / 2 - radius), 0)
    dy = max(abs(y - size / 2) - (size / 2 - radius), 0)
    return dx * dx + dy * dy <= radius * radius

def inside_bar(x, y, size):
    r = size * .055 / 2
    for i, height in enumerate((.16, .34, .52, .30, .16)):
        center = size * (.25 + .125 * i)
        dy = max(abs(y - size / 2) - size * height / 2, 0)
        if (x - center) ** 2 + dy ** 2 <= r * r:
            return True
    return False

def png(size):
    rows = bytearray()
    samples = 4
    for y in range(size):
        rows.append(0)
        for x in range(size):
            covered = white = 0
            for sy in range(samples):
                for sx in range(samples):
                    px, py = x + (sx + .5) / samples, y + (sy + .5) / samples
                    if inside_round(px, py, size):
                        covered += 1
                        white += inside_bar(px, py, size)
            color = round((36 * (covered - white) + 255 * white) / covered) if covered else 0
            rows.extend((color, color, color, round(255 * covered / samples ** 2)))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')

if __name__ == '__main__':
    destination = Path(__file__).resolve().parents[1] / 'assets'
    destination.mkdir(exist_ok=True)
    sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)
    images = [png(size) for size in sizes]
    offset = 6 + 16 * len(sizes)
    table = bytearray(struct.pack('<HHH', 0, 1, len(sizes)))
    for size, data in zip(sizes, images):
        table.extend(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    (destination / 'aggregate-audio.ico').write_bytes(table + b''.join(images))
    (destination / 'aggregate-audio.png').write_bytes(images[-1])
