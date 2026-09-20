#pragma once
#include <stddef.h>
#include <stdint.h>

namespace AppConfig {
constexpr uint8_t PotPin = 1;
constexpr uint8_t AdcChannel = 0;
constexpr uint32_t SampleRateHz = 4000;
constexpr size_t BlockSamples = 128;
constexpr size_t SampleBytes = 4;
constexpr size_t ReadBufferBytes = BlockSamples * SampleBytes;
constexpr size_t DriverBufferBytes = 4096;
constexpr uint32_t ReadTimeoutMs = 200;
constexpr uint16_t AdcMax = 4095;
constexpr uint16_t PlotMaxMv = 3100;
constexpr float VoltageFilterAlpha = 0.2f;
constexpr int8_t TftClock = 12;
constexpr int8_t TftMosi = 11;
constexpr int8_t TftReset = 10;
constexpr int8_t TftDc = 9;
constexpr int8_t TftCs = 8;
constexpr int8_t TftBacklight = 7;
constexpr uint8_t TftRotation = 0;
constexpr uint32_t SpiHz = 20000000;
constexpr int16_t PlotX = 12;
constexpr int16_t PlotY = 102;
constexpr size_t WaveColumns = 216;
constexpr int16_t PlotHeight = 90;
constexpr uint32_t WaveColumnMs = 10;
constexpr size_t SamplesPerColumn = SampleRateHz * WaveColumnMs / 1000;
constexpr uint32_t DisplayPeriodMs = 50;
constexpr uint32_t StatusPeriodMs = 1000;
constexpr uint32_t SerialBaud = 115200;
constexpr uint32_t TelemetryPeriodMs = 100;
constexpr uint32_t StaleDataMs = 500;
constexpr unsigned AdcPriority = 4;
constexpr unsigned DisplayPriority = 2;
constexpr unsigned StatusPriority = 1;
constexpr unsigned AdcStackBytes = 4096;
constexpr unsigned DisplayStackBytes = 6144;
constexpr unsigned StatusStackBytes = 3072;
constexpr int AdcCore = 0;
constexpr int DisplayCore = 1;
constexpr int StatusCore = 1;
static_assert(SampleRateHz * WaveColumnMs % 1000 == 0,
              "Each waveform column must contain a whole number of samples.");
static_assert(SamplesPerColumn > 0 && BlockSamples > 0, "Invalid sample count");
static_assert(VoltageFilterAlpha > 0 && VoltageFilterAlpha <= 1, "Invalid IIR alpha");
}  // namespace AppConfig
