#include <assert.h>
#include <stdio.h>
#include "wave_protocol.h"

int main(int argc, char **argv) {
  DisplayFrame frame;
  frame.sequence = 0x12345678;
  frame.timestampMs = 0xfffffff0;
  frame.rawMean = 2048;
  frame.milliVolts = 1550;
  frame.state = AdcState::Running;
  frame.valid = true;
  frame.nearLimit = true;
  frame.waveCount = AppConfig::WaveColumns;
  for (size_t i = 0; i < frame.waveCount; ++i) {
    frame.waveMin[i] = 1000 + i;
    frame.waveMax[i] = 1100 + i;
  }
  uint8_t packet[WaveProtocol::MaxPacketBytes];
  assert(WaveProtocol::encode(frame, 10, nullptr, sizeof(packet)) == 0);
  assert(WaveProtocol::encode(frame, 10, packet, sizeof(packet) - 1) == 0);
  size_t size = WaveProtocol::encode(frame, 10, packet, sizeof(packet));
  assert(size == sizeof(packet));
  assert(packet[0] == 'A' && packet[3] == 'W' && packet[6] == 1);
  assert(packet[7] == 3);  // millis rollover must not produce a stale flag.
  if (argc == 2) {
    FILE *output = fopen(argv[1], "wb");
    assert(output);
    assert(fwrite(packet, 1, size, output) == size);
    assert(fclose(output) == 0);
  }
  WaveProtocol::encode(frame, 1000, packet, sizeof(packet));
  assert(packet[7] == 7);
  frame.waveCount = 0;
  assert(WaveProtocol::encode(frame, 10, packet, sizeof(packet)) == 32);
  frame.waveCount = AppConfig::WaveColumns + 1;
  assert(WaveProtocol::encode(frame, 10, packet, sizeof(packet)) == 0);
  puts("PASS: packet capacity, full/empty history, invalid count, stale and millis rollover");
}
