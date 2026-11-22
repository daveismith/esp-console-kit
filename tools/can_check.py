#!/usr/bin/env python3
"""Raw CAN frames from a computer, through a gs_usb adapter (candleLight, CANable), for testing a
board's `can` command (esp-console-kit's canbus component) and the wiring under it.

    tools/can_check.py listen [-t 10]                 print every frame, then a count by ID
    tools/can_check.py send 123#DEADBEEF              one frame
    tools/can_check.py send 1ABCDEF0#0102 -n 1000     1000 frames, numbered: bytes 0-3 count up
    tools/can_check.py echo -n 10000                  numbered frames to a board running
                                                      `can echo on`; count what comes back
    tools/can_check.py flood -t 10                    as fast as the adapter will send
    tools/can_check.py flush                          empty the adapter for another program
                                                      (run before cyphal_service after this or pytest)

An ID of up to three hex digits is an 11-bit one, eight digits a 29-bit one, as on the board.
`-b 500000` (or 500k) changes the bitrate; the board's default is 1 Mbit/s.

"Taken" counts the adapter's echoes of what it was given. A candleLight adapter echoes a frame
when it hands it to its CAN controller, not when another node acknowledges it, so the echo proves
nothing about the bus. Acknowledgement shows in the error reports instead: the adapter sends an
error frame when its controller's state changes, and "bus errors" counts the ones that report
an error on the bus (no ACK, a protocol error, bus-off). Its status is the last controller state
it reported (0x30: error-passive, which a missing ACK soon causes; 0x40: back to error-active).

A frame nobody acknowledges is retried by the adapter's controller until a node does or the
program closes it. An earlier adapter firmware kept such a frame across a close and reopen; the
CANable's current one (2026-11-21) has not been seen to. If it happens, a board running
`can start` (normal mode) acknowledges it and clears it.

Only one program can hold the adapter: stop cyphal_service (and any other CAN tool) first.

`flush` is for the program that opens the adapter next. python-can asks a candleLight for
timestamped frames (24 bytes each); a frame left waiting from that session then overflows a program
that reads 20-byte untimestamped ones, as cyphal_service does, and macOS halts the endpoint, which
that program never clears: it then receives nothing. `flush` starts the adapter without timestamps,
reads whatever it holds for the computer with a 64-byte buffer for 0.3 s, and stops it. It sends
nothing. Run it while the board is quiet, as any open and close.
Needs python-can and gs_usb (`pip install python-can gs_usb`).
"""

from __future__ import annotations

import argparse
import platform
import struct
import sys
import threading
import time
import traceback
from collections import Counter, defaultdict

try:
    import can
    import usb.core
except ImportError:
    sys.exit("can_check.py needs python-can and gs_usb: pip install python-can gs_usb")


def patch_gs_usb_for_macos() -> None:
    """gs_usb asks libusb to detach a kernel driver, which macOS refuses without root, though none
    is attached: tell it there is none (the library itself is not patched). A test harness that
    opens the adapter itself needs the same."""
    if platform.system() == "Darwin":
        usb.core.Device.is_kernel_driver_active = lambda self, interface: False


def open_bus(bitrate: int) -> can.BusABC:
    patch_gs_usb_for_macos()
    return can.Bus(interface="gs_usb", channel=0, bitrate=bitrate)


def flush(bitrate: int) -> int:
    """`flush` (the module's docstring): the adapter through the gs_usb library, not python-can"""
    from gs_usb.constants import GS_CAN_MODE_NORMAL
    from gs_usb.gs_usb import GsUsb
    import usb.util

    patch_gs_usb_for_macos()
    devs = GsUsb.scan()
    if not devs:
        print("no gs_usb adapter found", file=sys.stderr)
        return 1
    dev = devs[0]
    dev.set_bitrate(bitrate)
    dev.start(GS_CAN_MODE_NORMAL)       # a USB reset, then START, with no timestamps
    sizes: Counter = Counter()
    t0 = time.monotonic()
    failed = None
    while time.monotonic() - t0 < 0.3:
        try:
            sizes[len(bytes(dev.gs_usb.read(0x81, 64, 50)))] += 1
        except usb.core.USBError as e:
            if "timed out" not in str(e).lower() and e.errno not in (60, 110):
                failed = e
                break
    dev.stop()
    usb.util.dispose_resources(dev.gs_usb)
    print(f"flushed: {sum(sizes.values())} reads, by size {dict(sizes)}" + (f"; read error: {failed}" if failed else ""))
    return 1 if failed else 0


def parse_bitrate(s: str) -> int:
    s = s.lower()
    mult = 1_000_000 if s.endswith("m") else 1000 if s.endswith("k") else 1
    return int(float(s.rstrip("mk")) * mult)


def parse_frame(s: str) -> tuple[int, bool, bytes]:
    ident, sep, data = s.partition("#")
    if not sep or not ident or (len(ident) > 3 and len(ident) != 8):
        raise argparse.ArgumentTypeError("write it <id>#<hex>: up to 3 hex digits (11-bit) or 8 (29-bit)")
    try:
        value, payload = int(ident, 16), bytes.fromhex(data)
    except ValueError:
        raise argparse.ArgumentTypeError("the ID and the data are hex") from None
    ext = len(ident) == 8
    if value > (0x1FFFFFFF if ext else 0x7FF):
        raise argparse.ArgumentTypeError("a 29-bit ID is 1FFFFFFF at most" if ext else "an 11-bit ID is 7FF at most")
    if len(payload) > 8:
        raise argparse.ArgumentTypeError("8 data bytes at most")
    return value, ext, payload


def show(msg: can.Message) -> str:
    ident = f"{msg.arbitration_id:08X}" if msg.is_extended_id else f"{msg.arbitration_id:03X}"
    body = "remote" if msg.is_remote_frame else " ".join(f"{b:02X}" for b in msg.data)
    return f"{ident} [{msg.dlc}] {body}"


# SocketCAN's error classes, in an error frame's ID (linux/can/error.h), which gs_usb uses
ERR_CRTL, ERR_PROT, ERR_ACK, ERR_BUSOFF, ERR_BUSERROR, ERR_CNT = 0x4, 0x8, 0x20, 0x40, 0x80, 0x200


class Reader(threading.Thread):
    """Everything the adapter reports, sorted: frames from the bus, its own sends taken, and
    error frames."""

    def __init__(self, bus: can.BusABC, on_frame=None):
        super().__init__(daemon=True)
        self.bus, self.on_frame = bus, on_frame
        self.received = 0       # frames from the bus
        self.taken = 0
        self.errors = 0         # bus errors reported
        self.tec = self.rec = None    # the adapter's counters, when it has reported them
        self.status = 0               # the last controller status it reported (CAN_ERR_CRTL_*)
        self.running = True
        self.failure = None           # what stopped the thread early, if anything did

    def run(self):
        try:
            self._run()
        except Exception as e:      # shown, and failed by ok(), rather than counts that just stop
            self.failure = e
            print("reader stopped:", file=sys.stderr)
            traceback.print_exc()

    def _run(self):
        while self.running:
            msg = self.bus.recv(0.05)
            if msg is None:
                continue
            if msg.is_error_frame:
                if msg.arbitration_id & (ERR_PROT | ERR_ACK | ERR_BUSOFF | ERR_BUSERROR):
                    self.errors += 1
                if msg.arbitration_id & ERR_CRTL and len(msg.data) >= 2:
                    self.status = msg.data[1]
                if msg.arbitration_id & ERR_CNT and len(msg.data) >= 8:
                    self.tec, self.rec = msg.data[6], msg.data[7]
            elif not msg.is_rx:
                self.taken += 1
            else:
                self.received += 1
                if self.on_frame:
                    self.on_frame(msg)

    def stop(self):
        self.running = False
        self.join(1)

    def adapter(self) -> str:
        counters = "" if self.tec is None else f", tx errors {self.tec}, rx errors {self.rec}"
        failed = f"; READER STOPPED: {self.failure!r}" if self.failure else ""
        return f"bus errors {self.errors}; adapter status 0x{self.status:02x}{counters}{failed}"

    def ok(self) -> bool:
        return self.failure is None and self.errors == 0


def seq_of(m: can.Message) -> int | None:
    """Bytes 0-3 of a numbered frame; None for anything shorter, or a remote frame (no data)."""
    if m.is_remote_frame or len(m.data) < 4:
        return None
    return struct.unpack(">I", bytes(m.data[:4]))[0]


def numbered(seq: int, rest: bytes) -> bytes:
    return (struct.pack(">I", seq & 0xFFFFFFFF) + rest)[:8]


def send_one(bus, ident, ext, data) -> bool:
    try:
        bus.send(can.Message(arbitration_id=ident, is_extended_id=ext, data=data))
        return True
    except can.CanError as e:
        print(f"send failed: {e}", file=sys.stderr)
        return False


def wait_taken(reader: Reader, want: int, timeout: float):
    end = time.time() + timeout
    while reader.taken < want and time.time() < end:
        time.sleep(0.01)


def cmd_listen(bus, args) -> int:
    by_id: Counter = Counter()
    seqs: defaultdict[tuple[int, bool], list[int]] = defaultdict(list)    # only with --numbered
    flush = sys.stdout.isatty()

    def on_frame(m):
        key = (m.arbitration_id, m.is_extended_id)
        by_id[key] += 1
        seq = seq_of(m) if args.numbered else None
        if seq is not None:
            seqs[key].append(seq)
        if not args.quiet:
            print(show(m), flush=flush)

    reader = Reader(bus, on_frame)
    reader.start()
    try:
        if args.time:
            time.sleep(args.time)
        else:
            threading.Event().wait()
    except KeyboardInterrupt:
        pass
    reader.stop()
    print(f"{reader.received} frames; {reader.adapter()}")
    for (ident, ext), n in sorted(by_id.items()):
        print(f"  {ident:08X}" if ext else f"  {ident:03X}", n)
    if args.numbered:
        for (ident, ext) in sorted(by_id):
            report_sequence(f"{ident:X}", seqs.get((ident, ext), []))
    return 0 if reader.failure is None else 1


def report_sequence(label: str, seqs: list[int], expect: int | None = None) -> bool:
    in_order = all(b == a + 1 for a, b in zip(seqs, seqs[1:]))
    missing = (expect - len(set(seqs))) if expect is not None else None
    dupes = len(seqs) - len(set(seqs))
    print(f"  {label}: {len(seqs)} numbered, {'in order' if in_order else 'OUT OF ORDER'}, {dupes} repeated"
          + (f", {missing} missing" if missing is not None else "")
          + (f", first {seqs[0]}, last {seqs[-1]}" if seqs else ""))
    return in_order and dupes == 0 and (missing in (None, 0))


def cmd_send(bus, args) -> int:
    ident, ext, data = args.frame
    reader = Reader(bus)
    reader.start()
    failed = 0
    t0 = time.time()
    for i in range(args.count):
        if not send_one(bus, ident, ext, numbered(i, data) if args.count > 1 else data):
            failed += 1
        if args.gap:
            time.sleep(args.gap / 1000)
    wait_taken(reader, args.count - failed, 2)
    reader.stop()
    took = time.time() - t0
    print(f"sent {args.count - failed} of {args.count} in {took:.2f} s; taken {reader.taken}; "
          f"send errors {failed}; {reader.adapter()}")
    return 0 if failed == 0 and reader.ok() else 1


def cmd_echo(bus, args) -> int:
    """Numbered frames on ID, which a board with `can echo on` sends back on ID + 1. A window of
    frames is kept in flight so that both directions run at once."""
    ident, ext = args.id, args.id > 0x7FF or args.ext
    back = (ident + 1) & (0x1FFFFFFF if ext else 0x7FF)
    got: list[int] = []
    others = Counter()
    arrived = threading.Condition()     # notified at each frame back

    def on_frame(m):
        seq = seq_of(m)
        if m.arbitration_id == back and m.is_extended_id == ext and seq is not None:
            with arrived:
                got.append(seq)
                arrived.notify()
        else:
            others[f"{m.arbitration_id:X}"] += 1

    reader = Reader(bus, on_frame)
    reader.start()
    failed = 0
    t0 = time.time()
    give_up = t0 + args.count / 100 + 10     # past this, the window no longer holds sending back
    for i in range(args.count):
        with arrived:
            arrived.wait_for(lambda: i - len(got) < args.window, max(0.0, give_up - time.time()))
        if not send_one(bus, ident, ext, numbered(i, bytes(range(4, 8)))):
            failed += 1
    with arrived:
        arrived.wait_for(lambda: len(got) >= args.count, 3)
    wait_taken(reader, args.count - failed, 1)
    reader.stop()
    took = time.time() - t0
    print(f"sent {args.count - failed} of {args.count} on {ident:X}, taken {reader.taken}, send errors "
          f"{failed}; back on {back:X}: {len(got)} in {took:.2f} s; {reader.adapter()}")
    ok = report_sequence(f"{back:X}", got, args.count)
    if others:
        print(f"  other frames: {dict(others)}")
    return 0 if ok and failed == 0 and reader.ok() else 1


def cmd_flood(bus, args) -> int:
    ident, ext, data = args.frame
    reader = Reader(bus)
    reader.start()
    sent = failed = 0
    t0 = time.time()
    try:
        while (time.time() - t0 < args.time) if args.count == 0 else sent + failed < args.count:
            if send_one(bus, ident, ext, numbered(sent, data)):
                sent += 1
            else:
                failed += 1
                time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    took = time.time() - t0
    wait_taken(reader, sent, 2)
    reader.stop()
    print(f"sent {sent} in {took:.2f} s ({sent / took:.0f}/s); taken {reader.taken}; send errors "
          f"{failed}; {reader.adapter()}; received {reader.received}")
    return 0 if reader.failure is None else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-b", "--bitrate", type=parse_bitrate, default=1_000_000, help="bits/s (default 1m)")
    sub = ap.add_subparsers(dest="op", required=True)
    p = sub.add_parser("listen", help="print every frame, then a count by ID")
    p.add_argument("-t", "--time", type=float, default=0, help="seconds (default: until Ctrl-C)")
    p.add_argument("-q", "--quiet", action="store_true", help="only the counts")
    p.add_argument("--numbered", action="store_true", help="check that bytes 0-3 count up, per ID")
    p = sub.add_parser("send", help="send a frame, or N numbered ones")
    p.add_argument("frame", type=parse_frame)
    p.add_argument("-n", "--count", type=int, default=1)
    p.add_argument("--gap", type=float, default=0, help="milliseconds between frames")
    p = sub.add_parser("echo", help="numbered frames to a board running `can echo on`")
    p.add_argument("-n", "--count", type=int, default=10000)
    p.add_argument("--id", type=lambda s: int(s, 16), default=0x100, help="hex (default 100)")
    p.add_argument("--ext", action="store_true", help="a 29-bit ID")
    p.add_argument("--window", type=int, default=8, help="frames in flight (default 8)")
    p = sub.add_parser("flood", help="send as fast as the adapter takes them")
    p.add_argument("frame", type=parse_frame, nargs="?", default=parse_frame("7FF#0000000000000000"))
    p.add_argument("-t", "--time", type=float, default=10)
    p.add_argument("-n", "--count", type=int, default=0, help="frames, instead of seconds")
    sub.add_parser("flush", help="empty the adapter for the next program (cyphal_service); sends nothing")
    args = ap.parse_args()

    if args.op == "flush":
        return flush(args.bitrate)
    bus = open_bus(args.bitrate)
    try:
        return {"listen": cmd_listen, "send": cmd_send, "echo": cmd_echo, "flood": cmd_flood}[args.op](bus, args)
    finally:
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
