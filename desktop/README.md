# ESP32 ADC 上位机

入口：`wave_viewer.py`；Windows 可双击 `start.bat`。需要 Python 3.10+ 和 tkinter，串口依赖为 `requirements.txt` 中的 pyserial。演示模式无需 pyserial。

在工程根目录运行：

```powershell
py -3 -m pip install -r desktop/requirements.txt
py -3 desktop/wave_viewer.py
# 或先查看演示
py -3 desktop/wave_viewer.py --demo
```

请先上传配套固件，再关闭串口监视器、选择 COM 端口并连接。枚举不到串口时可手动输入端口名。上传固件前先断开上位机。非 Windows 平台使用 `python3` 代替 `py -3`，Linux 可能需要通过系统包管理器安装 tkinter 并配置串口访问权限。

当前固件已配置原生 USB CDC：数据线连接开发板标注 **USB** 的接口即可，无需再接 COM/UART 接口。烧录后刷新串口列表，选择原生 USB 对应的 COM 编号。

上位机仍以 115200 打开端口以统一配置；当前原生 USB Serial/JTAG 实现不会用这个数值限制实际 USB 传输速率。

界面使用 Tk Canvas 绘制包络和包络中点连线，不需要 matplotlib。串口在后台线程中以有限超时读取，只保留最新快照；Tk 主线程每 50ms 处理显示。拔线后可重新连接，不会无限累积旧数据。串口行为参考 [pySerial 官方 API](https://pyserial.readthedocs.io/en/latest/pyserial_api.html)。

## ADCW v1 协议

所有多字节整数均为小端。每帧为：

```text
"ADCW" (4 bytes) | payload_length (uint16) | payload | CRC16 (uint16)
```

CRC 为 CRC-16/CCITT-FALSE（poly=0x1021、init=0xFFFF、无反射、xorout=0），覆盖长度字段和 payload，不含 magic。CRC 结果同样按小端发送。

payload 的固定头为 24 字节，对应 Python `struct.Struct("<BBBBIIHHHHHH")`：

| 偏移 | 类型 | 字段 |
| --- | --- | --- |
| 0 | uint8 | version，固定 1 |
| 1 | uint8 | flags：bit0 valid，bit1 nearLimit，bit2 stale |
| 2 | uint8 | ADC 状态：0 启动、1 运行、2 超时、3 恢复、4 错误 |
| 3 | uint8 | 保留，固定 0 |
| 4 | uint32 | sequence，处理快照序号，允许回绕或重启归零 |
| 8 | uint32 | timestamp_ms，处理结果生成时的 millis() |
| 12 | uint16 | raw_mean，最近处理块的平均 RAW |
| 14 | uint16 | millivolts，平滑后的数值电压 |
| 16 | uint16 | count，有效时间桶数量 |
| 18 | uint16 | column_ms，每桶持续时间 |
| 20 | uint16 | columns，最大历史桶数 |
| 22 | uint16 | max_mv，绘图纵轴上限 |
| 24 起 | count × (uint16, uint16) | 每桶 min_mV / max_mV，按从旧到新排列 |

默认最多 216 桶，完整帧 896 字节；10 帧/秒约 8960 字节/秒，另有每秒文本状态日志，在 115200、8N1 的链路预算内。串口阻塞时可能降低刷新速度；ADC 和屏幕不会等待发送任务。帧缓冲使用静态内存，不占用状态任务的栈。

接收端按 magic、长度、CRC 和字段合法性解析，能处理任意分包、粘包、启动日志和损坏帧；长度受限，错误后重新搜索 magic。每帧包含完整历史，连接中途或丢弃旧帧后不需恢复增量上下文。固件 ADC 超过 500ms 未更新会设置 stale；上位机超过 600ms 未收帧时也隐藏实时数据。暂停时保留冻结快照，并明确显示暂停状态。

## 验证

在工程根目录运行：

```powershell
pio run
New-Item -ItemType Directory -Path .pio/host-tests -Force | Out-Null
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude src/wave_protocol.cpp tests/wave_protocol_test.cpp -o .pio/host-tests/wave_protocol_test.exe
./.pio/host-tests/wave_protocol_test.exe .pio/host-tests/wave_packet.bin
$env:ADCW_FIXTURE = '.pio/host-tests/wave_packet.bin'
$env:ADCW_GUI_TEST = '1'
py -3 -m unittest discover -s tests -p test_desktop.py -v
```

C++ 测试验证编码容量、空/满历史、非法数量、时间回绕和过期标记，并输出一帧供 Python 交叉解码。Python 测试验证分包、日志混入、损坏恢复、有限缓冲、CSV、线程断线以及隐藏 Tk 窗口中的绘图/暂停/过期行为。取消 `ADCW_GUI_TEST` 可跳过 GUI 测试。

实体板的串口持续吞吐、实际刷新率、DMA 错误计数和小屏幕同时运行效果仍需上板检查。
