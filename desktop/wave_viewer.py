"""Run with Python 3.10+: py desktop/wave_viewer.py [--demo]."""
import argparse
import csv
import math
from pathlib import Path
import queue
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from protocol import Frame, StreamParser


class SerialReader(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.stop_event = threading.Event()
        self.latest = queue.Queue(maxsize=1)
        self.error = None
        self.parser = StreamParser()
        self.count = 0

    def run(self):
        try:
            while not self.stop_event.is_set():
                data = self.port.read(min(4096, max(1, self.port.in_waiting)))
                for frame in self.parser.feed(data):
                    self.count += 1
                    try:
                        self.latest.get_nowait()
                    except queue.Empty:
                        pass
                    self.latest.put_nowait((frame, time.monotonic()))
        except Exception as exc:
            if not self.stop_event.is_set():
                self.error = str(exc)
        finally:
            self.port.close()

    def stop(self):
        self.stop_event.set()
        self.join(timeout=0.3)  # Port read timeout is 50 ms.


def demo_frame(elapsed):
    wave = []
    for i in range(216):
        t = elapsed - (215 - i) * 0.01
        center = 1550 + 1000 * math.sin(t * 3.2) + 130 * math.sin(t * 21)
        wave.append((int(center - 25), int(center + 25)))
    mv = sum(wave[-1]) // 2
    return Frame(1, 1, int(elapsed * 10), int(elapsed * 1000),
                 int(mv * 4095 / 3100), mv, 10, 216, 3100, tuple(wave))


def write_csv(path, frame):
    with open(path, "w", newline="", encoding="utf-8-sig") as output:
        writer = csv.writer(output)
        writer.writerow(["sequence", "device_timestamp_ms", "flags", "adc_state",
                         "relative_bucket_start_s", "relative_bucket_end_s",
                         "min_mV", "max_mV", "filtered_mV", "raw_mean"])
        count = len(frame.wave)
        for i, (low, high) in enumerate(frame.wave):
            writer.writerow([frame.sequence, frame.timestamp_ms, frame.flags, frame.state,
                             (i - count) * frame.column_ms / 1000,
                             (i - count + 1) * frame.column_ms / 1000,
                             low, high, frame.millivolts, frame.raw_mean])


class WaveViewer:
    def __init__(self, root, demo=False):
        self.root = root
        self.reader = None
        self.demo = False
        self.demo_start = 0.0
        self.latest = None
        self.displayed = None
        self.last_received = 0.0
        self.connected_at = 0.0
        self.paused = False
        self.root.title("ESP32 ADC · 实时波形")
        self.root.geometry("1040x650")
        self.root.minsize(760, 480)
        self.root.configure(bg="#101827")
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TFrame", background="#101827")
        style.configure("TLabel", background="#101827", foreground="#d7e3f4", font=("Microsoft YaHei UI", 10))
        style.configure("TButton", font=("Microsoft YaHei UI", 10), padding=6)
        toolbar = ttk.Frame(root, padding=12)
        toolbar.pack(fill="x")
        ttk.Label(toolbar, text="串口").pack(side="left")
        self.port_var = tk.StringVar()
        self.ports = ttk.Combobox(toolbar, textvariable=self.port_var, width=14)
        self.ports.pack(side="left", padx=8)
        self.refresh_button = ttk.Button(toolbar, text="刷新", command=self.refresh_ports)
        self.refresh_button.pack(side="left")
        ttk.Label(toolbar, text="115200 baud").pack(side="left", padx=12)
        self.connect_button = ttk.Button(toolbar, text="连接", command=self.toggle_connection)
        self.connect_button.pack(side="left")
        self.demo_button = ttk.Button(toolbar, text="演示模式", command=self.start_demo)
        self.demo_button.pack(side="left", padx=8)
        self.pause_button = ttk.Button(toolbar, text="暂停", command=self.toggle_pause)
        self.pause_button.pack(side="right")
        ttk.Button(toolbar, text="导出当前波形", command=self.export).pack(side="right", padx=8)

        info = ttk.Frame(root, padding=(20, 8))
        info.pack(fill="x")
        self.voltage = tk.StringVar(value="--.--- V")
        tk.Label(info, textvariable=self.voltage, font=("Consolas", 36, "bold"),
                 fg="#40dfcc", bg="#101827").pack(side="left")
        self.details = tk.StringVar(value="GPIO1 / ADC1_CH0\n等待连接开发板")
        ttk.Label(info, textvariable=self.details, justify="right").pack(side="right")
        self.canvas = tk.Canvas(root, bg="#0b1220", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=16, pady=8)
        self.canvas.bind("<Configure>", lambda _event: self.draw())
        self.status = tk.StringVar(value="请选择串口并连接，或进入演示模式。")
        ttk.Label(root, textvariable=self.status, padding=(18, 8), wraplength=980).pack(fill="x")
        ttk.Label(root, text="波形保留每个时间桶的最小/最大电压；导出当前显示快照。", padding=(18, 0, 18, 12)).pack(anchor="w")
        self.refresh_ports()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.timer = self.root.after(50, self.tick)
        if demo:
            self.start_demo()

    def refresh_ports(self):
        try:
            from serial.tools import list_ports
            names = [port.device for port in list_ports.comports()]
            self.ports["values"] = names
            if names and self.port_var.get() not in names:
                self.port_var.set(names[0])
        except ImportError:
            self.status.set("未安装串口依赖：py -m pip install -r desktop/requirements.txt；演示模式可直接使用。")

    def disconnect(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
        self.demo = False
        self.connect_button.configure(text="连接")
        self.ports.configure(state="normal")
        self.refresh_button.configure(state="normal")

    def reset_view(self):
        self.latest = self.displayed = None
        self.last_received = 0.0
        self.connected_at = time.monotonic()
        self.paused = False
        self.pause_button.configure(text="暂停")
        self.voltage.set("--.--- V")
        self.details.set("GPIO1 / ADC1_CH0\n等待波形数据")
        self.draw()

    def toggle_connection(self):
        if self.reader or self.demo:
            self.disconnect()
            self.reset_view()
            self.status.set("已断开连接。")
            return
        port_name = self.port_var.get().strip()
        if not port_name:
            messagebox.showinfo("选择串口", "请连接开发板并刷新串口，也可手动输入 COM 端口。")
            return
        port = None
        try:
            import serial
            port = serial.Serial(port=None, baudrate=115200, timeout=0.05)
            port.dtr = False
            port.rts = False
            port.port = port_name
            port.open()
        except (ImportError, OSError, ValueError) as exc:
            if port:
                port.close()
            messagebox.showerror("连接失败", f"{exc}\n\n请先关闭 PlatformIO 串口监视器，确认已安装 requirements.txt 中的依赖。")
            return
        self.reset_view()
        self.reader = SerialReader(port)
        self.reader.start()
        self.connect_button.configure(text="断开")
        self.ports.configure(state="disabled")
        self.refresh_button.configure(state="disabled")
        self.status.set("已连接，等待 ADCW 波形数据……")

    def start_demo(self):
        self.disconnect()
        self.reset_view()
        self.demo = True
        self.demo_start = time.monotonic()
        self.connect_button.configure(text="停止演示")

    def toggle_pause(self):
        self.paused = not self.paused
        self.pause_button.configure(text="继续" if self.paused else "暂停")
        if not self.paused:
            self.displayed = self.latest
        self.draw()

    def tick(self):
        now = time.monotonic()
        if self.demo:
            self.latest = demo_frame(now - self.demo_start)
            self.last_received = now
        elif self.reader:
            if self.reader.error:
                error = self.reader.error
                self.disconnect()
                self.reset_view()
                self.status.set(f"串口已断开：{error}。请检查连接后重新连接。")
            else:
                try:
                    self.latest, self.last_received = self.reader.latest.get_nowait()
                except queue.Empty:
                    pass
        if self.reader or self.demo:
            if not self.paused:
                self.displayed = self.latest
            stale = self.latest is not None and now - self.last_received > 0.6
            mode = "演示数据" if self.demo else "串口实时数据"
            if self.latest is None:
                hint = "等待波形数据……"
                if now - self.connected_at > 3:
                    hint = "未收到波形，请确认已烧录新版固件、端口正确且波特率为 115200。"
            elif stale:
                hint = "数据已停止更新，请检查开发板。"
            elif not self.latest.usable:
                hint = "ADC 数据过期" if self.latest.flags & 4 else ["ADC 启动中", "等待有效采样", "ADC 超时", "ADC 恢复中", "ADC 错误"][self.latest.state]
            else:
                hint = "接近量程上限，可能削顶" if self.latest.flags & 2 else "正在接收"
            if self.reader:
                hint += f" | 已收 {self.reader.count} 帧 · 校验/格式错误 {self.reader.parser.errors}"
            self.status.set(f"{mode} | {'显示已暂停，后台继续接收 | ' if self.paused else ''}{hint}")
            self.draw()
        self.timer = self.root.after(50, self.tick)

    def draw(self):
        canvas = self.canvas
        canvas.delete("all")
        width, height = canvas.winfo_width(), canvas.winfo_height()
        if width < 100 or height < 100:
            return
        left, right, top, bottom = 64, width - 24, 28, height - 45
        frame = self.displayed
        maximum = frame.max_mv if frame else 3100
        columns = frame.columns if frame else 216
        step = frame.column_ms if frame else 10
        span = columns * step / 1000
        for i in range(5):
            y = top + (bottom - top) * i / 4
            canvas.create_line(left, y, right, y, fill="#253248")
            canvas.create_text(left - 10, y, text=f"{maximum / 1000 * (1 - i / 4):.2f}", anchor="e", fill="#96aac3")
        for i in range(5):
            x = left + (right - left) * i / 4
            canvas.create_line(x, top, x, bottom, fill="#253248")
            canvas.create_text(x, bottom + 18, text=f"{-span * (1 - i / 4):.2f}", fill="#96aac3")
        canvas.create_text(left, 12, text="电压 / V", anchor="w", fill="#b4c7df")
        canvas.create_text(right, height - 8, text="相对最新完整时间桶 / s", anchor="e", fill="#b4c7df")
        stale = not self.paused and time.monotonic() - self.last_received > 0.6
        usable = frame is not None and frame.usable and not stale
        if not usable:
            self.voltage.set("--.--- V")
            self.details.set("GPIO1 / ADC1_CH0\n暂无有效数据")
            canvas.create_text((left + right) / 2, (top + bottom) / 2,
                               text="暂无有效波形", fill="#7e91aa", font=("Microsoft YaHei UI", 16))
            return
        self.voltage.set(f"{frame.millivolts / 1000:.3f} V")
        self.details.set(f"RAW {frame.raw_mean} / 4095   ·   帧 {frame.sequence}\n"
                         f"{span:.2f} s 历史   ·   {step} ms / 桶" + ("   ·   暂停快照" if self.paused else ""))
        color = "#f3bc61" if self.paused else "#40dfcc"
        def voltage_y(mv):
            return bottom - min(maximum, max(0, mv)) / maximum * (bottom - top)
        centers = []
        count = len(frame.wave)
        for i, (low, high) in enumerate(frame.wave):
            x = left + (columns - count + i + 0.5) / columns * (right - left)
            y1, y2 = voltage_y(high), voltage_y(low)
            canvas.create_line(x, y1, x, max(y1 + 1, y2), fill=color, width=2)
            centers.extend((x, voltage_y((low + high) / 2)))
        if len(centers) >= 4:
            canvas.create_line(*centers, fill=color, width=1)
        if self.demo:
            canvas.create_text(right - 8, top + 16, text="演示数据", anchor="e", fill="#f3bc61")

    def export(self):
        frame = self.displayed
        stale = not self.paused and time.monotonic() - self.last_received > 0.6
        if frame is None or not frame.wave or not frame.usable or stale:
            messagebox.showinfo("没有波形", "请先连接开发板或进入演示模式。")
            return
        path = filedialog.asksaveasfilename(defaultextension=".csv",
            initialfile=f"{'demo' if self.demo else 'adc'}_wave_{frame.sequence}.csv",
            filetypes=[("CSV", "*.csv")])
        if path:
            try:
                write_csv(path, frame)
            except OSError as exc:
                messagebox.showerror("导出失败", str(exc))
            else:
                messagebox.showinfo("导出完成", f"已保存当前快照：{Path(path).name}")

    def close(self):
        self.root.after_cancel(self.timer)
        self.disconnect()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="ESP32 ADC 实时波形上位机")
    parser.add_argument("--demo", action="store_true", help="使用演示波形，不需要开发板或 pyserial")
    args = parser.parse_args()
    root = tk.Tk()
    WaveViewer(root, demo=args.demo)
    root.mainloop()


if __name__ == "__main__":
    main()
