#!/usr/bin/env python3
"""Simulate and write this controller's EM4100 ids with the Proxmark client.

provision-admin holds the admin id on the LF antenna so a reader can see it.
test-reader does the same with a door card and checks the ESP32 serial log.
The write commands program a T5577 (TAG=lf) or a magic 1K (TAG=hf) on the antenna.
"""

import argparse
import os
import re
import select
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r"\x1b\[[0-9;]*m")
BOOT_MARKERS = ("STA IP", "AP IP", "LittleFS")
BOOT_WAIT_SECONDS = 15
SERIAL_BAUD = termios.B115200

MODE_CARDS = (
    ("Read", 0x1E6DC032),
    ("Door", 0x1E6D9861),
    ("Add", 0x1E6DFCA0),
    ("Remove", 0x1E6DE636),
)


def em4100(uid):
    """10 hex digits, the leading 00 plus the 32-bit id the controller stores."""
    return f"{uid & 0xFFFFFFFF:010X}"


def uid8(uid):
    return f"{uid & 0xFFFFFFFF:08X}"


def load_uids(path):
    uids = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        uids.append(int(text, 0))
    if not uids:
        raise SystemExit(f"{path} has no card id.")
    return uids


def uid_in_output(output, uid):
    """True when a Proxmark read printed this id back."""
    cleaned = ANSI.sub("", output).upper()
    compact = re.sub(r"\s+", "", cleaned)
    ten = em4100(uid)
    eight = uid8(uid)
    spaced = " ".join(eight[i:i + 2] for i in range(0, 8, 2))
    if ten in compact:
        return True
    if spaced in cleaned:
        return True
    return re.search(rf"\b{eight}\b", cleaned) is not None


def run_pm3(pm3, commands, seconds=None):
    cmd = []
    if seconds is not None:
        cmd.extend(["timeout", "--foreground", str(seconds)])
    cmd.append(pm3)
    for command in commands:
        cmd.extend(["-c", command])
    print("+ " + " ".join(cmd), flush=True)
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError as exc:
        missing = cmd[0]
        raise SystemExit(f"Could not run {missing}: {exc}") from exc
    chunks = []
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        chunks.append(line)
    return proc.wait(), "".join(chunks)


def provision_admin(pm3, admin_file, seconds):
    uid = load_uids(admin_file)[0]
    ident = em4100(uid)
    print(
        f"Simulating admin {uid} ({ident}) for {seconds}s. "
        "Hold the Proxmark LF antenna against the HID reader.",
        flush=True,
    )
    code, _output = run_pm3(pm3, [f"lf em 410x sim --id {ident}"], seconds=seconds)
    # timeout(1) exits 124 when the sim is stopped after SIM_SECONDS.
    if code not in (0, 124):
        raise SystemExit(f"Proxmark sim failed with status {code}.")
    print("Admin simulation finished.", flush=True)


def write_commands(tag, uid):
    if tag == "lf":
        return [f"lf em 410x clone --id {em4100(uid)}"], ["lf em 410x reader"]
    if tag == "hf":
        return [f"hf mf csetuid -u {uid8(uid)}"], ["hf 14a reader"]
    raise SystemExit("TAG must be lf or hf.")


def write_one(pm3, tag, uid, label):
    program, verify = write_commands(tag, uid)
    shown = em4100(uid) if tag == "lf" else uid8(uid)
    print(f"Writing {label} ({uid}) as {shown}.", flush=True)
    code, _output = run_pm3(pm3, program)
    if code != 0:
        raise SystemExit(f"Proxmark write of {label} failed with status {code}.")
    code, output = run_pm3(pm3, verify)
    if code != 0 or not uid_in_output(output, uid):
        raise SystemExit(f"{label} did not read back as {shown}.")
    print(f"{label} read back ok.", flush=True)


def tag_kind(tag):
    if tag == "lf":
        return "T5577"
    if tag == "hf":
        return "magic 1K"
    raise SystemExit("TAG must be lf or hf.")


def confirm_tag(label):
    print(f"Place the {label} on the Proxmark antenna, then press Enter.", flush=True)
    try:
        input()
    except EOFError:
        raise SystemExit(f"No terminal input while waiting for the {label} tag.") from None


def write_admin(pm3, admin_file, tag):
    uid = load_uids(admin_file)[0]
    print(f"The admin {tag_kind(tag)} tag needs to be on the Proxmark antenna.", flush=True)
    write_one(pm3, tag, uid, "admin")


def write_cards(pm3, cards_file, tag):
    for index, uid in enumerate(load_uids(cards_file), start=1):
        confirm_tag(f"card {index} {tag_kind(tag)} ({uid})")
        write_one(pm3, tag, uid, f"card {index}")


def write_modes(pm3, tag):
    for name, uid in MODE_CARDS:
        confirm_tag(f"{name} mode {tag_kind(tag)} (0x{uid:08X})")
        write_one(pm3, tag, uid, f"{name} mode")


def esp_port(cli, config):
    if os.environ.get("ESP_PORT"):
        return os.environ["ESP_PORT"]
    script = Path(__file__).resolve().parent / "resolve_ports.py"
    proc = subprocess.run(
        [sys.executable, str(script), "--config", config, "--cli", cli, "--need", "esp", "--print"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr or "")
        raise SystemExit(proc.returncode)
    port = proc.stdout.strip()
    if not port:
        raise SystemExit("Could not resolve the ESP32 serial port. Set ESP_PORT.")
    return port


def open_serial(port):
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as exc:
        raise SystemExit(f"Could not open {port}: {exc}") from exc
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, _ispeed, _ospeed, cc = attrs
    iflag &= ~(
        termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
        | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON
    )
    oflag &= ~termios.OPOST
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.HUPCL)
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(
        fd,
        termios.TCSANOW,
        [iflag, oflag, cflag, lflag, SERIAL_BAUD, SERIAL_BAUD, cc],
    )
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_serial(fd, buf, stop):
    while not stop.is_set():
        ready, _, _ = select.select([fd], [], [], 0.2)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if chunk:
            buf.extend(chunk)


def test_reader(pm3, admin_file, cards_file, seconds, cli, config):
    admin = load_uids(admin_file)[0]
    card = next((uid for uid in load_uids(cards_file) if uid != admin), None)
    if card is None:
        raise SystemExit(f"{cards_file} has no id different from the admin id.")
    ident = em4100(card)
    needle = f"Card: 0x{uid8(card)}"
    port = esp_port(cli, config)
    print(
        f"Simulating door card {card} ({ident}) for {seconds}s. "
        f"Expect {needle} on {port}. The ESP32 must be in READ mode.",
        flush=True,
    )
    fd = open_serial(port)
    buf = bytearray()
    stop = threading.Event()
    thread = threading.Thread(target=read_serial, args=(fd, buf, stop), daemon=True)
    thread.start()
    time.sleep(1.5)
    opened = buf.decode("utf-8", "replace")
    if any(marker in opened for marker in BOOT_MARKERS):
        print("ESP32 reset when the serial port opened. Waiting for setup to finish.", flush=True)
        remaining = BOOT_WAIT_SECONDS - 1.5
        if remaining > 0:
            time.sleep(remaining)
    code, _output = run_pm3(pm3, [f"lf em 410x sim --id {ident}"], seconds=seconds)
    time.sleep(1)
    stop.set()
    thread.join(timeout=2)
    os.close(fd)
    serial_text = buf.decode("utf-8", "replace")
    if code not in (0, 124):
        sys.stdout.write(serial_text)
        raise SystemExit(f"Proxmark sim failed with status {code}.")
    if needle.lower() in serial_text.lower():
        print(f"Reader reported {needle}.", flush=True)
        return
    sys.stdout.write(serial_text)
    if serial_text and not serial_text.endswith("\n"):
        sys.stdout.write("\n")
    raise SystemExit(
        f"ESP32 serial did not report {needle}. "
        "The board has to be in READ mode, with the Proxmark LF antenna against the reader."
    )


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pm3", required=True, help="path to the pm3 client script")
    sub = parser.add_subparsers(dest="command", required=True)

    provision = sub.add_parser("provision-admin", help="simulate the admin id at the reader")
    provision.add_argument("--admin", type=Path, required=True)
    provision.add_argument("--seconds", type=int, default=8)

    test = sub.add_parser("test-reader", help="simulate a door card and check ESP32 serial")
    test.add_argument("--admin", type=Path, required=True)
    test.add_argument("--cards", type=Path, required=True)
    test.add_argument("--seconds", type=int, default=8)
    test.add_argument("--config", default="arduino-cli.yaml")
    test.add_argument("--cli", default="arduino-cli")

    write_admin_cmd = sub.add_parser("write-admin", help="write admin.txt onto one tag")
    write_admin_cmd.add_argument("--admin", type=Path, required=True)
    write_admin_cmd.add_argument("--tag", choices=("lf", "hf"), default="lf")

    write_cards_cmd = sub.add_parser("write-cards", help="write each cards.txt id onto its own tag")
    write_cards_cmd.add_argument("--cards", type=Path, required=True)
    write_cards_cmd.add_argument("--tag", choices=("lf", "hf"), default="lf")

    write_modes_cmd = sub.add_parser("write-modes", help="write the four mode cards onto tags")
    write_modes_cmd.add_argument("--tag", choices=("lf", "hf"), default="lf")
    return parser


def main():
    args = build_parser().parse_args()
    if args.command == "provision-admin":
        provision_admin(args.pm3, args.admin, args.seconds)
    elif args.command == "test-reader":
        test_reader(args.pm3, args.admin, args.cards, args.seconds, args.cli, args.config)
    elif args.command == "write-admin":
        write_admin(args.pm3, args.admin, args.tag)
    elif args.command == "write-cards":
        write_cards(args.pm3, args.cards, args.tag)
    elif args.command == "write-modes":
        write_modes(args.pm3, args.tag)
    else:
        raise SystemExit(f"Unknown command {args.command}")


if __name__ == "__main__":
    main()
