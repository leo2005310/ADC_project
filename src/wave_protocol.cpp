#include "wave_protocol.h"

namespace WaveProtocol {
size_t encode(const DisplayFrame &frame, uint32_t nowMs, uint8_t *output, size_t capacity) {
  const size_t count = frame.waveCount;
  if (count > AppConfig::WaveColumns) return 0;
  const size_t payloadSize = MetadataBytes + count * 4;
  if (!output || capacity < payloadSize + 8) return 0;
  size_t pos = 0;
  auto put8 = [&](uint8_t value) { output[pos++] = value; };
  auto put16 = [&](uint16_t value) { put8(value & 0xff); put8(value >> 8); };
  auto put32 = [&](uint32_t value) { put16(value & 0xffff); put16(value >> 16); };
  put8('A'); put8('D'); put8('C'); put8('W');
  put16(payloadSize);
  put8(1);  // Protocol version.
  const bool stale = static_cast<uint32_t>(nowMs - frame.timestampMs) > AppConfig::StaleDataMs;
  put8((frame.valid ? 1 : 0) | (frame.nearLimit ? 2 : 0) | (stale ? 4 : 0));
  put8(static_cast<uint8_t>(frame.state));
  put8(0);  // Reserved.
  put32(frame.sequence);
  put32(frame.timestampMs);
  put16(frame.rawMean);
  put16(frame.milliVolts);
  put16(count);
  put16(AppConfig::WaveColumnMs);
  put16(AppConfig::WaveColumns);
  put16(AppConfig::PlotMaxMv);
  for (size_t i = 0; i < count; ++i) {
    put16(frame.waveMin[i]);
    put16(frame.waveMax[i]);
  }
  // CRC-16/CCITT-FALSE covers the length and payload, excludes magic.
  uint16_t crc = 0xffff;
  for (size_t i = 4; i < pos; ++i) {
    crc ^= static_cast<uint16_t>(output[i]) << 8;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  put16(crc);
  return pos;
}
}  // namespace WaveProtocol
