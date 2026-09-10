"""Power measurement helper for the MuteHid firmware.

measure: holds the device in each scenario over USB serial and records the USB
         tester reading you type in, next to the device's own PWR samples.
dump:    reads the battery-voltage record taken while unplugged (serial R/r/O)
         and prints the voltage slope per device state.

The StopWatch PMIC reports voltages only, so current comes from the tester.
Readings are valid only once charging has finished (chg=0); after that the
device runs from USB (checked on 2026-09-11, docs/power-saving-ideas.md §5).
"""
import argparse
import csv
import datetime
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT / "logs"
FIELD = re.compile(r"(\w+)=(\S+)")
# Bit order of the DRAIN state byte, as defined in src/app/PowerProbe.cpp.
STATE_BITS = ("conn", "adv", "disp", "dim", "chg", "usb")

# (id, label, required connection, commands sent after X). X clears every override.
SCENARIOS = [
    (1, "未接続・アドバタイズ・設定輝度", False, "D"),
    (2, "未接続・アドバタイズ・暗転", False, "DD"),
    (3, "未接続・アドバタイズ・パネルsleep", False, "DDDD"),
    (4, "未接続・アドバタイズ停止・パネルsleep", False, "DDDDA"),
    (5, "接続中アイドル・設定輝度", True, "D"),
    (6, "接続中アイドル・パネルsleep", True, "DDDD"),
]


def fields(text):
    out = {}
    for key, value in FIELD.findall(text):
        try:
            out[key] = int(value)
        except ValueError:
            out[key] = value
    return out


def parse_pwr(line):
    index = line.find("PWR ")
    return fields(line[index + 4:]) if index >= 0 else None


def parse_drain(line):
    index = line.find("DRAIN ")
    if index < 0:
        return None
    record = fields(line[index + 6:])
    return record if {"t", "vbat", "st"} <= record.keys() else None


def describe_state(bits):
    return "+".join(name for i, name in enumerate(STATE_BITS) if bits >> i & 1) or "-"


def slope_mv_per_hour(records):
    """Least-squares battery-voltage slope; negative while discharging."""
    if len(records) < 2:
        return None
    ts = [r["t"] / 3600 for r in records]
    vs = [r["vbat"] for r in records]
    mt, mv = sum(ts) / len(ts), sum(vs) / len(vs)
    den = sum((t - mt) ** 2 for t in ts)
    return None if den == 0 else sum((t - mt) * (v - mv) for t, v in zip(ts, vs)) / den


def segments(records):
    """Consecutive runs of the same state, each with its own slope."""
    runs = []
    for record in records:
        if runs and runs[-1][0] == record["st"]:
            runs[-1][1].append(record)
        else:
            runs.append((record["st"], [record]))
    return [{"state": describe_state(st), "start": rs[0]["t"], "end": rs[-1]["t"], "n": len(rs),
             "slope": slope_mv_per_hour(rs)} for st, rs in runs]


def milliwatts(ma, vbus_mv):
    return ma * vbus_mv / 1000


def average(samples, key):
    values = [s[key] for s in samples if isinstance(s.get(key), int)]
    return round(sum(values) / len(values)) if values else None


def open_port(name):
    import serial
    # USB Serial/JTAG treats RTS/DTR as reset/download; set them before opening
    # so attaching never restarts the device (same sequence as serial_probe.py).
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.rts = False
    port.dtr = False
    port.port = name
    port.open()
    port.dtr = True
    port.rts = False
    return port


def resolve_port(name):
    if name:
        return name
    import device
    return device.detect_port()


class Reader(threading.Thread):
    """Collects PWR samples and DRAIN records while the main thread prompts."""

    def __init__(self, port, echo):
        super().__init__(daemon=True)
        self.port, self.echo = port, echo
        self.lock = threading.Lock()
        self.samples, self.drain = [], []
        self.drain_done = threading.Event()
        self.running = True

    def run(self):
        buffer = b""
        while self.running:
            buffer += self.port.read(max(1, self.port.in_waiting))
            *lines, buffer = buffer.split(b"\n")
            for raw in lines:
                self.handle(raw.decode("utf-8", errors="replace").rstrip())

    def handle(self, line):
        pwr = parse_pwr(line)
        record = parse_drain(line)
        with self.lock:
            if pwr is not None:
                self.samples.append((time.monotonic(), pwr))
            elif record is not None:
                self.drain.append(record)
            elif "DRAIN end" in line:
                self.drain_done.set()
        if self.echo and pwr is None and "DRAIN" not in line:
            print(f"  | {line}")

    def recent(self, seconds):
        cutoff = time.monotonic() - seconds
        with self.lock:
            return [s for t, s in self.samples if t >= cutoff]


def send(port, commands):
    for c in commands:
        port.write(c.encode("ascii"))
        time.sleep(0.3)


def firmware_version():
    try:
        return subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def ask(prompt):
    return input(prompt).strip()


def wait_for_connection(reader, connected):
    want = 1 if connected else 0
    while True:
        latest = reader.recent(3)
        if latest and latest[-1].get("conn") == want:
            return True
        need = "PC と接続" if connected else "PC の Bluetooth を切るなどして未接続に"
        if ask(f"  {need}してから Enter（s でスキップ）: ").lower() == "s":
            return False
        time.sleep(2)


def settled_window(reader, window):
    """Recent samples once charging has finished, or None when skipped."""
    while True:
        samples = reader.recent(window)
        if not samples:
            print("  PWR 行が途切れています。")
        elif any(s.get("chg") == 1 for s in samples):
            print("  充電中のため無効です（chg=1）。満充電まで待ってください。")
        else:
            return samples
        if ask("  Enter で再確認、s でスキップ: ").lower() == "s":
            return None
        time.sleep(window)


def ask_milliamps():
    while True:
        text = ask("  テスターの値 [mA]（s でスキップ）: ")
        if text.lower() == "s":
            return None
        try:
            return float(text)
        except ValueError:
            pass


def measure(args):
    version = firmware_version()
    LOGS.mkdir(exist_ok=True)
    path = args.out or LOGS / f"power-{datetime.datetime.now():%Y%m%d-%H%M%S}.csv"
    port = open_port(resolve_port(args.port))
    reader = Reader(port, args.echo)
    reader.start()
    columns = ["time", "id", "scenario", "mA", "vbus_mV", "mW", "vbat_mV", "disp", "bri", "conn", "adv",
               "cpu", "firmware"]
    try:
        send(port, "XL")
        time.sleep(2)
        if not reader.recent(2):
            sys.exit("PWR 行が届きません。ポートとファームウェアを確認してください。")
        print(f"記録先: {path}")
        with path.open("w", newline="", encoding="utf-8") as f:
            out = csv.writer(f)
            out.writerow(columns)
            for sid, label, connected, commands in SCENARIOS:
                if args.only and sid not in args.only:
                    continue
                print(f"\n[{sid}] {label}")
                send(port, "X")
                if not wait_for_connection(reader, connected):
                    continue
                send(port, commands)
                time.sleep(args.settle)
                samples = settled_window(reader, args.window)
                if samples is None:
                    continue
                vbus, vbat, last = average(samples, "vbus"), average(samples, "vbat"), samples[-1]
                print(f"  vbus={vbus} mV vbat={vbat} mV disp={last.get('disp')} bri={last.get('bri')} "
                      f"adv={last.get('adv')} conn={last.get('conn')}")
                ma = ask_milliamps()
                if ma is None:
                    continue
                out.writerow([datetime.datetime.now().isoformat(timespec="seconds"), sid, label, ma, vbus,
                              round(milliwatts(ma, vbus), 1) if vbus else "", vbat, last.get("disp"),
                              last.get("bri"), last.get("conn"), last.get("adv"), last.get("cpu"), version])
                f.flush()
    finally:
        try:
            send(port, "Xl")
        finally:
            reader.running = False
            reader.join(timeout=1)
            port.close()
    print(f"\n保存しました: {path}")


def dump(args):
    port = open_port(resolve_port(args.port))
    reader = Reader(port, False)
    reader.start()
    try:
        send(port, "O")
        if not reader.drain_done.wait(args.timeout):
            print("DRAIN end が届きませんでした。受信できた分だけ保存します。")
    finally:
        reader.running = False
        reader.join(timeout=1)
        port.close()
    records = reader.drain
    if not records:
        sys.exit("記録がありません。R で記録を開始してから USB を抜いてください。")
    LOGS.mkdir(exist_ok=True)
    path = args.out or LOGS / f"drain-{datetime.datetime.now():%Y%m%d-%H%M%S}.csv"
    with path.open("w", newline="", encoding="utf-8") as f:
        out = csv.writer(f)
        out.writerow(["t_s", "vbat_mV", "state"])
        for r in records:
            out.writerow([r["t"], r["vbat"], describe_state(r["st"])])
    print(f"{len(records)} 件を保存しました: {path}")
    overall = slope_mv_per_hour(records)
    print(f"全体の傾き: {overall:+.1f} mV/h" if overall is not None else "全体の傾き: -")
    for s in segments(records):
        slope = f"{s['slope']:+.1f} mV/h" if s["slope"] is not None else "-"
        print(f"  {s['start']:>7}s - {s['end']:>7}s  n={s['n']:>4}  {s['state']:<24} {slope}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="action", required=True)
    m = sub.add_parser("measure", help="USB テスターで状態ごとに記録する")
    m.add_argument("--port")
    m.add_argument("--settle", type=float, default=10, help="状態を切り替えてから待つ秒数")
    m.add_argument("--window", type=float, default=5, help="平均に使う直近の秒数")
    m.add_argument("--only", type=int, nargs="+", help="実行するシナリオ番号")
    m.add_argument("--echo", action="store_true", help="PWR 以外のログも表示する")
    m.add_argument("--out", type=Path)
    d = sub.add_parser("dump", help="電池電圧の記録を吸い出す")
    d.add_argument("--port")
    d.add_argument("--timeout", type=float, default=20)
    d.add_argument("--out", type=Path)
    args = p.parse_args()
    try:
        measure(args) if args.action == "measure" else dump(args)
    except ValueError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
