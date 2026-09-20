#pragma once
#include "app_types.h"

namespace WaveProtocol {
// Magic + payload length + metadata + min/max pairs + CRC16.
constexpr size_t MetadataBytes = 24;
constexpr size_t MaxPacketBytes = 6 + MetadataBytes + AppConfig::WaveColumns * 4 + 2;
size_t encode(const DisplayFrame &frame, uint32_t nowMs, uint8_t *output, size_t capacity);
}  // namespace WaveProtocol
