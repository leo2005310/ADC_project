# ESP32-S3 N16R8：ADC DMA + FreeRTOS + ST7789

使用 Arduino 框架，在 GPIO1 连续采集电位器电压。三个业务任务分别负责 ADC 处理、240×240 ST7789 显示、串口状态统计。

## 任务与数据流

```text
GPIO1 → ADC1_CH0 → DMA → 驱动内部缓存
                            ↓
                    A/B 交替读取缓冲
                            ↓
                    ADCProcessTask ── adcStatsQueue ─────┐
                            │                            │
                      displayQueue                      ▼
                            │                       StatusTask → 串口
                            ▼                            ▲
                       DisplayTask ── displayStatsQueue ─┘
                            ↓
                         ST7789
```

| 任务 | 优先级 | CPU 核心 | 初始栈 | 执行方式 |
| --- | --- | --- | --- | --- |
| ADCProcessTask | 4（高） | Core 0 | 4096 字节 | 阻塞等待读取数据，每 128 个有效样本发布结果 |
| DisplayTask | 2（中） | Core 1 | 6144 字节 | 每 50ms 检查最新结果，目标 20 FPS |
| StatusTask | 1（低） | Core 1 | 3072 字节 | 每 1000ms 打印一次统计 |

Arduino 自带的 `loopTask` 阻塞等待，系统还会运行 FreeRTOS 的 Idle 等任务。表中的栈大小单位为 ESP-IDF FreeRTOS 使用的字节。

所有队列长度为 1。生产者使用 `xQueueOverwrite()` 发布最新值，不等待消费者：

- `displayQueue` 按值复制完整电压和波形快照，只有 DisplayTask 接收。
- 两个独立统计队列供 StatusTask 使用 `xQueuePeek()` 读取，不会抢走显示数据。
- SPI 与所有 `tft.*` 调用由 DisplayTask 独占。
- 正常运行中的应用串口输出由 StatusTask 独占；启动分配失败时由 main.cpp 输出错误，框架自身也可能输出启动/错误日志。
- 所有任务创建成功、句柄就绪后，通过任务通知统一放行。队列或任务分配失败时清理已创建资源并报告错误。

## 采样、双缓冲和处理

| 参数 | 当前值 |
| --- | --- |
| 输入 | GPIO1 / ADC1_CH0 |
| 采样率目标 | 4000 samples/s |
| ADC 精度 | 12 位，0～4095 |
| DMA 输出格式 | TYPE2，每个结果 4 字节，包含通道信息 |
| A/B 读取缓冲 | 各 512 字节，交替复用 |
| 驱动缓存 | 4096 字节 |
| 数值处理块 | 128 个有效样本，约 32ms |
| 读取超时 | 200ms |
| 数值滤波 | 校准电压块平均，再做 α=0.2 的 IIR 平滑 |
| 波形 | 每 40 个样本（约 10ms）保留最小、最大校准电压 |

当前固定使用 Arduino-ESP32 2.0.17 自带的 `adc_digi_*` 接口。DMA 描述符、中断和实际 DMA 内存由驱动管理。`adc_digi_read_bytes()` 将驱动数据复制到应用 A/B 缓冲，ADCProcessTask 完成处理后再复用缓冲。**这里的 A/B 是应用读取缓冲，不是 DMA 直接写入的硬件双缓冲，也不是读取和计算同时执行。** 采样与 CPU 处理的并行由驱动内部 DMA 缓存提供。

读取返回的长度可能小于 512 字节。程序按实际长度解析，并跨读取累积处理块和波形时间桶，不假定一次读取就是完整的一帧。错误的通道或数据长度会被记录。

校准使用 ESP32-S3 的 eFuse TP fitting 数据；每个 DMA 原始样本通过 `esp_adc_cal_raw_to_voltage()` 转为电压，没有额外调用 `analogRead()` 或 `analogReadMilliVolts()`。缺少校准数据时明确报告错误。

### 异常处理

- DMA 超时、溢出及其他读取错误分别统计。
- 旧驱动的溢出标志会保持到重启。程序停止并重建驱动，丢弃缓存旧数据，清空部分处理块、滤波和波形历史，再恢复采样。
- 初始化失败后每秒重试；清理失败或校准缺失时保持错误状态并继续报告，不输出伪造电压。
- 溢出计数表示检测到的溢出事件，不能据此推算精确丢失样本数。
- 每批处理后至少阻塞一个 tick，避免积压时高优先级任务持续占用 Core 0。
- 显示结果超过 500ms 未更新时显示 `ADC DATA STALE`。时间戳表示处理结果的生成时间，不是硬件逐点采样时间。

## 屏幕内容

- 大字显示平滑后的电压。固定 8 个字符并补空格，只在字符或颜色变化时带背景覆盖，不先清空数字区域，以减少闪烁并避免状态切换时残留字符。
- 显示最近处理块的平均原始 ADC 值。
- 216×90 像素的波形区显示约 **2.16 秒**历史，左旧右新；启动或断流重置后从右侧逐渐填满。
- 波形直接保留各时间桶内的电压极值，不使用数值显示的 IIR 结果，以免把短时变化过度抹平。
- 波形纵轴为 0～3.1V，超出显示范围的点会被限制在边界。
- 底部进度条表示原始 ADC 平均值占 4095 的比例。
- 波形先绘制到内存画布，再整块传输，避免逐线清屏。画布约占 38,880 字节动态内存。
- 屏幕慢时跳过旧显示更新，不让采样任务等待。20 FPS 是目标，实际结果查看串口。

## 状态统计

串口波特率为 **115200**。每秒汇报：

- 成功处理的有效样本数 / 实际经过时间，即实际处理采样率。
- 完成新有效数据绘制的帧数 / 实际经过时间，即数据 FPS。
- DMA 溢出、超时、其他错误、无效样本、采集重启次数。
- `skipped UI`：被较新快照替代而未显示的更新数，**不是 ADC 丢样数**。
- ADC 数据处理和屏幕绘制的累计最大耗时。
- 两个任务的统计更新时间距当前的时间。
- 片内剩余堆、历史最小剩余堆、各任务历史最小剩余栈。
- Flash、PSRAM 检测容量、PSRAM 剩余空间及最近错误码。

出现 DMA 溢出时，成功处理采样率不等于硬件实际转换率。窗口统计可能略有抖动，应观察连续数秒结果。屏幕启动期间 FPS 较低属于初始化过程。

## 接线

| 模块 | 模块引脚 | ESP32-S3 |
| --- | --- | --- |
| 10 kΩ 电位器 | VCC / 外侧端 | 3.3V |
| 10 kΩ 电位器 | OUT / 中间滑动端 | GPIO1（ADC1_CH0） |
| 10 kΩ 电位器 | GND / 另一外侧端 | GND |
| ST7789 | VCC | 3.3V |
| ST7789 | GND | GND |
| ST7789 | SCL / CLK | GPIO12 |
| ST7789 | SDA / MOSI | GPIO11 |
| ST7789 | RES / RST | GPIO10 |
| ST7789 | DC | GPIO9 |
| ST7789 | CS | GPIO8 |
| ST7789 | BLK / BL / LED | GPIO7 |

所有模块共地；屏幕的 SCL / SDA 是 SPI 信号。

背光按高电平开启配置：显示任务初始化屏幕时将 GPIO7 拉低，完成初始画面绘制后拉高，保持背光常亮。此接法要求模块背光引脚支持 3.3V GPIO 控制。

电位器可输出到 3.3V，而当前 ADC 衰减下量程约为 0～3.1V。最高一段可能饱和。处理块中任一样本原始值达到 4000 或电压达到 3050mV 时显示 `NEAR LIMIT: may clip`。平均和滤波不能恢复超出量程的数据。

## 工程文件

```text
ADC_demo/
├── include/
│   ├── app_config.h           引脚、采样率、任务优先级、显示周期
│   ├── app_types.h            显示帧和统计消息结构
│   ├── app_tasks.h            任务入口和共享队列/任务句柄
│   └── adc_processing.h       独立于硬件的信号处理类
├── src/
│   ├── main.cpp              创建队列、任务和启动通知
│   ├── adc_task.cpp          DMA、校准、异常恢复和发布
│   ├── adc_processing.cpp    平均、IIR、波形极值和历史快照
│   ├── display_task.cpp      ST7789 绘制和 FPS 计数
│   └── status_task.cpp       串口与系统统计
├── tests/
│   └── adc_processing_test.cpp
├── platformio.ini
└── README.md
```

## 编译与上传

用 VS Code + PlatformIO 打开工程目录，依赖由 PlatformIO 自动安装。

```powershell
pio run
pio run --target upload
pio device monitor
```

也可以使用 PlatformIO 工具栏中的 Build、Upload、Serial Monitor。

板型以 `esp32-s3-devkitc-1` 为基础，已覆盖为 N16R8：

- 16MB Quad SPI Flash，QIO，80MHz。
- 8MB Octal SPI PSRAM，通过 `qio_opi` 和 `BOARD_HAS_PSRAM` 启用。
- `default_16MB.csv` 分区表，两个各 6.25MiB 的应用分区及数据分区。

构建标题可能仍包含基础板型的 `N8 (8 MB QD, No PSRAM)` 固定名称；实际配置以上述覆盖项为准。静态 RAM 构建统计不包含任务栈、队列、DMA 缓存和画布等运行时堆分配。

使用原生 USB 口查看串口时，在已有 `build_flags` 下追加 `-DARDUINO_USB_CDC_ON_BOOT=1`，保留 `-DBOARD_HAS_PSRAM`。使用 USB 转串口芯片时按板卡连接选择对应端口。

### 常用调整

在 `include/app_config.h` 修改：

- `TftRotation`：0、1、2、3，调整屏幕方向。
- `SpiHz`：默认 20MHz，花屏时可降到 10MHz 并检查连线。
- `DisplayPeriodMs`：默认 50ms，调整目标刷新周期。
- `VoltageFilterAlpha`：越小数字越稳定，响应也越慢。
- `WaveColumnMs`：每列代表的时间；需满足采样点数为整数。
- `SampleRateHz`：默认 4000；受当前驱动支持范围限制，并影响每块处理周期。

颜色反相时，可在 display_task.cpp 的 `tft.init(240, 240)` 后尝试 `tft.invertDisplay(false)`。仅背光亮时检查 CS、DC、RST、CLK、MOSI。

## 验证

已完成：

- Espressif32 6.12.0 / Arduino-ESP32 2.0.17 / N16R8 配置编译成功。
- 静态 RAM 使用 22,276 字节，程序使用 314,533 字节。
- 本机 C++ 测试通过：恒定输入、阶跃滤波、不规则读取分块、波形桶跨块、历史回绕、快照独立性、单点饱和、断流后重置。

有 g++ 的电脑可运行信号处理测试：

```powershell
New-Item -ItemType Directory -Path .pio/host-tests -Force | Out-Null
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/adc_processing.cpp tests/adc_processing_test.cpp -o .pio/host-tests/adc_processing_test.exe
./.pio/host-tests/adc_processing_test.exe
```

尚未上传到实体开发板。本机测试不验证 DMA 外设、任务实际调度和屏幕 SPI 时序。上板后检查：

1. 串口报告约 4,000 samples/s，显示稳定后 FPS 接近 20。
2. PSRAM 容量检测为 8MB，校准状态为 yes。
3. 缓慢转动电位器，电压、进度条和波形随之变化。
4. 连续运行观察 DMA 错误、任务剩余栈及最小剩余堆，按实测调整参数。

## 参考资料

- [Espressif ADC DMA 官方示例（ESP-IDF 4.4.7）](https://github.com/espressif/esp-idf/tree/v4.4.7/examples/peripherals/adc/dma_read)
- [ESP-IDF 4.4.7 ADC 驱动源码](https://github.com/espressif/esp-idf/blob/v4.4.7/components/driver/adc.c)
- [ESP32-S3 ADC 与电压校准接口](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/peripherals/adc.html)
- [Espressif 模组 Flash / PSRAM 模式对照](https://docs.espressif.com/projects/arduino-esp32/en/latest/troubleshooting.html)
