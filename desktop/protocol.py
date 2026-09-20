"""ADCW v1 stream decoder; independent of serial ports and GUI libraries."""
from dataclasses import dataclass
import binascii
import struct

MAGIC = b"ADCW"
HEADER = struct.Struct("<BBBBIIHHHHHH")
MAX_COLUMNS = 1024


@dataclass(frozen=True)
class Frame:
    flags: int
    state: int
    sequence: int
    timestamp_ms: int
    raw_mean: int
    millivolts: int
    column_ms: int
    columns: int
    max_mv: int
    wave: tuple[tuple[int, int], ...]

    @property
    def usable(self):
        return bool(self.flags & 1) and not (self.flags & 4) and self.state == 1


def decode_payload(payload):
    if len(payload) < HEADER.size:
        raise ValueError("short metadata")
    version, flags, state, reserved, seq, ms, raw, mv, count, step, columns, maximum = HEADER.unpack_from(payload)
    if version != 1 or reserved or flags & ~7 or state > 4:
        raise ValueError("unsupported metadata")
    if not (0 < columns <= MAX_COLUMNS and count <= columns and step and maximum):
        raise ValueError("invalid waveform dimensions")
    if raw > 4095 or len(payload) != HEADER.size + 4 * count:
        raise ValueError("invalid payload")
    wave = tuple(struct.iter_unpack("<HH", payload[HEADER.size:]))
    if any(low > high for low, high in wave):
        raise ValueError("inverted envelope")
    return Frame(flags, state, seq, ms, raw, mv, step, columns, maximum, wave)


class StreamParser:
    def __init__(self):
        self.buffer = bytearray()
        self.errors = 0

    def feed(self, chunk):
        self.buffer.extend(chunk)
        frames = []
        while True:
            start = self.buffer.find(MAGIC)
            if start < 0:
                # Keep a possible partial magic between reads; discard text logs.
                del self.buffer[:-3]
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 6:
                break
            length = struct.unpack_from("<H", self.buffer, 4)[0]
            if not HEADER.size <= length <= HEADER.size + MAX_COLUMNS * 4:
                self.errors += 1
                del self.buffer[0]
                continue
            total = length + 8
            if len(self.buffer) < total:
                break
            expected = struct.unpack_from("<H", self.buffer, total - 2)[0]
            actual = binascii.crc_hqx(self.buffer[4:total - 2], 0xFFFF)
            try:
                if actual != expected:
                    raise ValueError("CRC mismatch")
                frame = decode_payload(self.buffer[6:total - 2])
            except ValueError:
                self.errors += 1
                del self.buffer[0]
                continue
            frames.append(frame)
            del self.buffer[:total]
        return frames
