#!/usr/bin/env python3
"""Move files to and from an esp-console-kit `fs` volume over the serial console, with XMODEM-1K.

  fs_xfer.py [-p PORT] put [-f] [--to DIR] [--no-verify] FILE...
  fs_xfer.py [-p PORT] get REMOTE [LOCAL]
  fs_xfer.py [-p PORT] sha256 REMOTE...
  fs_xfer.py [-p PORT] run CMD...

`put` sends each file's size with it, so the board stores exactly that many bytes, then
compares SHA-256: the hash of what the board received, and (unless --no-verify) the hash of
the file read back from flash with `fs sha256`. Uploads run at --xfer-baud (460800) and
downloads at --get-baud (230400); the console drops back to --baud afterwards.

The defaults were measured with a CH343P bridge on macOS (WCH's CH34x DriverKit driver),
uploading 100 KB of random data:

  230400, whole blocks            16.4 KB/s
  460800, 512-byte pieces, drained 21.2 KB/s   <- default
  921600, 512-byte pieces, drained  7.5 KB/s   (each tcdrain costs ~35 ms)
  460800+ whole blocks, or pieces paced by time or by the output-queue count: data lost

The 1.6-1.9 MB Flash_PNG clips went up at 7-10 KB/s at the default (drain times and flash
writes add up over thousands of stop-and-wait blocks); downloading one ran at 21.6 KB/s.

Incoming data above 230400 loses bytes whatever the block size, so downloads run there.
These are properties of the host driver, not the protocol: try higher rates elsewhere.

Needs only pyserial. Close `idf.py monitor` first: one program at a time can hold the port.
The port defaults to $ESPPORT.
"""
import argparse
import binascii
import glob
import hashlib
import os
import re
import sys
import time

import serial

SOH, STX, EOT, ACK, NAK, CAN, SUB = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x1A
ANSI = re.compile(rb'\x1b\[[0-9;?]*[A-Za-z]')
PROMPT = re.compile(rb'(?:^|\n)[^\n]*\S+> $')
READY = re.compile(r'xmodem: ready to (receive|send) (\d+|\?) bytes at (\d+) baud: (\S+)')
SHA_LINE = re.compile(r'\b([0-9a-f]{64})\b')


class XmodemError(Exception):
    pass


class EotUnconfirmed(XmodemError):
    """Every block was acknowledged but the end of transfer was not: the ACK may have been
    lost on the way back, so the board's own report has the last word."""


# ---------------------------------------------------------------- console

class Console:
    """The board's REPL, opened without resetting it.

    On boards with the usual DTR/RTS auto-download transistors, EN is pulled low while RTS
    is asserted and DTR is not. macOS asserts both on open and pyserial then drops DTR
    before RTS -- a reset. Opening with both asserted drives neither pin; dropping RTS
    first only ever touches GPIO0, which is harmless while the app runs.
    """

    def __init__(self, port, baud):
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        s.timeout = 0.05
        s.dtr = True
        s.rts = True
        s.open()
        s.rts = False
        s.dtr = False
        self.s = s
        self.baud = baud
        self.buf = b''

    def close(self):
        self.s.rts = False
        self.s.dtr = False
        self.s.close()

    def pump(self):
        data = self.s.read(4096)
        if data:
            # linenoise asks where the cursor is at the start of every line, and swallows
            # typed input until it gets an answer.
            if b'\x1b[6n' in data:
                self.s.write(b'\x1b[24;80R')
            if b'\x1b[5n' in data:
                self.s.write(b'\x1b[0n')
            self.buf += data
        return data

    def take(self):
        out, self.buf = self.buf, b''
        return ANSI.sub(b'', out).decode('utf-8', 'replace')

    def at_prompt(self):
        return PROMPT.search(ANSI.sub(b'', self.buf)) is not None

    def wait_prompt(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.pump()
            if self.at_prompt():
                return True
        return False

    def sync(self):
        self.take()
        self.s.write(b'\r')
        if not self.wait_prompt(3):
            raise SystemExit('no console prompt -- is the board running, and the port free?')
        self.take()

    def command(self, cmd, timeout=10):
        self.sync()
        self.s.write(cmd.encode() + b'\r')
        ok = self.wait_prompt(timeout)
        text = self.take()
        if not ok:
            raise SystemExit(f'no prompt after {cmd!r}:\n{text}')
        return strip_echo(text, cmd)

    def start_transfer(self, cmd, timeout=10):
        """Send cmd; return (READY match, text so far), or (None, output) if it refused."""
        self.sync()
        self.s.write(cmd.encode() + b'\r')
        end = time.time() + timeout
        while time.time() < end:
            self.pump()
            text = ANSI.sub(b'', self.buf).decode('utf-8', 'replace')
            m = READY.search(text)
            if m and text.find('\n', m.start()) >= 0:
                self.buf = b''
                return m, text
            if self.at_prompt():
                return None, strip_echo(self.take(), cmd)
        return None, self.take()

    def set_baud(self, baud):
        if self.s.baudrate != baud:
            self.s.baudrate = baud


def strip_echo(text, cmd):
    """Drop the echoed command line and the trailing prompt."""
    lines = text.replace('\r', '').split('\n')
    if lines and cmd in lines[0]:
        lines = lines[1:]
    if lines and PROMPT.search(lines[-1].encode()):
        lines = lines[:-1]
    return '\n'.join(lines).strip('\n')


# ---------------------------------------------------------------- XMODEM

def crc16(data):
    return binascii.crc_hqx(data, 0)   # CRC-16/XMODEM


def read_exact(s, n, timeout, partial=None):
    s.timeout = timeout
    data = s.read(n)
    if partial is not None:
        partial[:] = data
    return data if len(data) == n else None


def wait_reply(s, timeout):
    end = time.time() + timeout
    s.timeout = 0.2
    while time.time() < end:
        c = s.read(1)
        if c and c[0] in (ACK, NAK, CAN):
            return c[0]
    return None


WRITE_CHUNK = 0   # 0: each frame in one write; otherwise pieces of this size, each drained


def write_frame(s, frame):
    """Write a frame without letting the driver's queue run ahead of the bridge.

    Each piece is drained (tcdrain) before the next. Measured on macOS's CH34x driver, that
    is the only backpressure that holds: sleeping for each piece's line time let pieces
    overtake each other, and polling the output-queue count (TIOCOUTQ) reads empty too soon.
    """
    if not WRITE_CHUNK:
        s.write(frame)
        return
    for i in range(0, len(frame), WRITE_CHUNK):
        s.write(frame[i:i + WRITE_CHUNK])
        s.flush()


def xmodem_send(s, data, progress=None):
    """Send data as XMODEM-1K (CRC). Returns the number of resent blocks."""
    s.reset_input_buffer()
    end = time.time() + 60
    s.timeout = 1
    while True:
        c = s.read(1)
        if c == b'C':
            break
        if c and c[0] == CAN and s.read(1) == bytes([CAN]):
            raise XmodemError('the board cancelled before the first block')
        if time.time() > end:
            raise XmodemError('the board never asked for data')

    num, off, retries = 1, 0, 0
    while off < len(data):
        chunk = data[off:off + 1024]
        size = 128 if len(chunk) <= 128 else 1024
        payload = chunk.ljust(size, bytes([SUB]))
        frame = (bytes([STX if size == 1024 else SOH, num & 0xFF, 0xFF - (num & 0xFF)])
                 + payload + crc16(payload).to_bytes(2, 'big'))
        for attempt in range(11):
            write_frame(s, frame)
            reply = wait_reply(s, 10)
            if reply == ACK:
                break
            if reply == CAN:
                raise XmodemError(f'the board cancelled at block {num}')
            retries += 1
        else:
            raise XmodemError(f'block {num} was never acknowledged')
        off += len(chunk)
        num += 1
        if progress:
            progress(off, len(data))

    # Briefly: the board answers repeats for 2 s and then goes back to the console rate, so
    # the check after this must be back at that rate before it reports.
    for attempt in range(3):
        s.write(bytes([EOT]))
        if wait_reply(s, 0.6) == ACK:
            return retries
    raise EotUnconfirmed('end of transfer was never acknowledged')


def xmodem_receive(s, size, progress=None):
    """Receive XMODEM-1K (CRC); returns exactly `size` bytes."""
    s.reset_input_buffer()
    out = bytearray()
    expected, errors = 1, 0
    first_bad = None
    end = time.time() + 60
    header = None
    while header is None:
        s.write(b'C')
        s.timeout = 1
        c = s.read(1)
        if c and c[0] in (SOH, STX, EOT):
            header = c[0]
        elif time.time() > end:
            raise XmodemError('the board never started sending')

    while True:
        if header == EOT:
            s.write(bytes([ACK]))
            break
        if header == CAN:
            raise XmodemError('the board cancelled')
        if header in (SOH, STX):
            n = 1024 if header == STX else 128
            partial = bytearray()
            body = read_exact(s, 2 + n + 2, 2, partial)
            good = (body is not None and body[0] == 0xFF - body[1]
                    and crc16(body[2:2 + n]) == int.from_bytes(body[2 + n:], 'big'))
            if good:
                if body[0] == expected & 0xFF:
                    out += body[2:2 + n]
                    expected += 1
                    errors = 0
                    s.write(bytes([ACK]))
                    if progress:
                        progress(min(len(out), size), size)
                elif body[0] == (expected - 1) & 0xFF:
                    s.write(bytes([ACK]))
                else:
                    s.write(bytes([CAN] * 3))
                    raise XmodemError('blocks out of sequence')
            else:
                if first_bad is None:
                    got = bytes(partial)
                    crc_rx = int.from_bytes(body[2 + n:], 'big') if body is not None else None
                    crc_calc = crc16(body[2:2 + n]) if body is not None else None
                    first_bad = (f'first bad block: header {header:#04x}, {len(got)} of '
                                 f"{2 + n + 2} bytes after it, starts {got[:8].hex(' ')}, "
                                 f'crc {crc_rx} computed {crc_calc}')
                errors += 1
                if errors > 10:
                    s.write(bytes([CAN] * 3))
                    raise XmodemError(f'too many bad blocks; {first_bad}')
                s.timeout = 0.1
                while s.read(256):
                    pass
                s.write(bytes([NAK]))
        s.timeout = 10
        c = s.read(1)
        if not c:
            errors += 1
            s.write(bytes([NAK]))
            c = s.read(1)
            if not c:
                raise XmodemError('the board went quiet')
        header = c[0]
    if len(out) < size:
        raise XmodemError(f'only {len(out)} of {size} bytes arrived'
                          + (f'; {first_bad}' if first_bad else ''))
    return bytes(out[:size])


def show_progress(done, total, t0=[0.0]):
    now = time.time()
    if done == 0 or t0[0] == 0:
        t0[0] = now
    rate = done / 1024 / max(now - t0[0], 1e-3)
    sys.stdout.write(f'\r  {done * 100 // max(total, 1):3d}%  {done}/{total} bytes  {rate:6.1f} KB/s')
    sys.stdout.flush()
    if done >= total:
        sys.stdout.write('\n')
        t0[0] = 0.0


# ---------------------------------------------------------------- operations

def remote_sha256(con, remote):
    out = con.command(f'fs sha256 {remote}', timeout=120)
    m = SHA_LINE.search(out)
    return (m.group(1) if m else None), out


def put(con, args, local):
    with open(local, 'rb') as f:
        data = f.read()
    local_sha = hashlib.sha256(data).hexdigest()
    name = os.path.basename(local)
    remote = f"{args.to.rstrip('/')}/{name}" if args.to else name
    flags = ' -f' if args.force else ''
    if args.xfer_baud != args.baud:
        flags += f' -b {args.xfer_baud}'
    print(f'{local} -> {remote} ({len(data)} bytes, sha256 {local_sha[:16]}...)')

    m, text = con.start_transfer(f'fs put{flags} {remote} {len(data)}')
    if not m:
        print(f'  refused: {text.strip()}')
        return False
    if args.verbose:
        print(f'  board: {m.group(0)}')
    # The rate asked for, not the one the board reports: that is what its divider achieved
    # (115200 comes back as 115211 or so), and retuning the bridge to it only adds error.
    con.set_baud(args.xfer_baud)
    t0 = time.time()
    retries = None
    try:
        retries = xmodem_send(con.s, data, show_progress)
    except EotUnconfirmed:
        pass   # every block was acknowledged; the board's report below decides
    except XmodemError as e:
        print(f'\n  transfer failed: {e}')
        con.set_baud(args.baud)
        con.wait_prompt(15)
        print('  ' + con.take().strip().replace('\n', '\n  '))
        return False
    secs = time.time() - t0
    con.set_baud(args.baud)
    con.wait_prompt(15)
    result = con.take()
    m = SHA_LINE.search(result)
    received_sha = m.group(1) if m else None
    print(f'  sent in {secs:.1f} s ({len(data) / 1024 / secs:.1f} KB/s, '
          + (f'{retries} resent blocks)' if retries is not None
             else 'end-of-transfer ACK lost on the way back)'))
    ok = received_sha == local_sha
    print(f"  board received: sha256 {received_sha or '?'}  {'OK' if ok else 'MISMATCH'}")
    if ok and not args.no_verify:
        flash_sha, out = remote_sha256(con, remote)
        ok = flash_sha == local_sha
        print(f"  read back:      sha256 {flash_sha or '?'}  {'OK' if ok else 'MISMATCH'}")
        if not ok:
            print('  ' + out.replace('\n', '\n  '))
    return ok


def get(con, args):
    flags = f' -b {args.get_baud}' if args.get_baud != args.baud else ''
    flags += f' -s {args.get_block}'
    m, text = con.start_transfer(f'fs get{flags} {args.remote}')
    if not m:
        print(f'refused: {text.strip()}')
        return False
    size = int(m.group(2))
    remote = m.group(4)
    local = args.local or os.path.basename(remote)
    if args.verbose:
        print(f'board: {m.group(0)}')
    con.set_baud(args.get_baud)
    time.sleep(0.1)   # the board switches rate 50 ms after the ready line
    t0 = time.time()
    try:
        data = xmodem_receive(con.s, size, show_progress)
    except XmodemError as e:
        print(f'\ntransfer failed: {e}')
        con.set_baud(args.baud)
        con.wait_prompt(15)
        print(con.take().strip())
        return False
    secs = time.time() - t0
    con.set_baud(args.baud)
    con.wait_prompt(15)
    con.take()
    with open(local, 'wb') as f:
        f.write(data)
    print(f'{remote} -> {local} ({size} bytes in {secs:.1f} s, {size / 1024 / secs:.1f} KB/s)')
    print(f'sha256 {hashlib.sha256(data).hexdigest()}  {local}')
    return True


def default_port():
    if os.environ.get('ESPPORT'):
        return os.environ['ESPPORT']
    found = sorted(glob.glob('/dev/cu.wchusbserial*') + glob.glob('/dev/ttyACM*') +
                   glob.glob('/dev/ttyUSB*'))
    return found[0] if found else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-p', '--port', default=default_port())
    ap.add_argument('--baud', type=int, default=115200, help='the console rate (default 115200)')
    ap.add_argument('--xfer-baud', type=int, default=460800,
                    help='the rate during an upload (default 460800; = --baud to not switch)')
    ap.add_argument('--get-baud', type=int, default=230400,
                    help='the rate during a download (default 230400). On macOS, WCH\'s CH34x '
                         'driver loses incoming data above that; a Linux host may manage more.')
    ap.add_argument('-v', '--verbose', action='store_true', help="show the board's transfer announcements")
    ap.add_argument('--get-block', type=int, default=1024, choices=(128, 1024),
                    help='block size the board sends with for `get` (default 1024)')
    ap.add_argument('--chunk', type=int, default=512,
                    help='write each block in pieces of this many bytes, draining each (default 512; '
                         '0 = whole). A whole 1029-byte block written at once at 460800+ baud loses '
                         "data somewhere between macOS's CH34x driver and the bridge.")
    sub = ap.add_subparsers(dest='op', required=True)
    p = sub.add_parser('put', help='upload files')
    p.add_argument('files', nargs='+')
    p.add_argument('-f', '--force', action='store_true', help='replace existing files')
    p.add_argument('--to', help='remote directory (default: the volume root)')
    p.add_argument('--no-verify', action='store_true', help='skip reading the file back to hash it')
    g = sub.add_parser('get', help='download a file')
    g.add_argument('remote')
    g.add_argument('local', nargs='?')
    h = sub.add_parser('sha256', help='hash files on the board')
    h.add_argument('remotes', nargs='+')
    r = sub.add_parser('run', help='run console commands and print their output')
    r.add_argument('cmds', nargs='+')
    args = ap.parse_args()
    global WRITE_CHUNK
    WRITE_CHUNK = args.chunk
    if not args.port:
        raise SystemExit('no port: pass -p or set ESPPORT')

    con = Console(args.port, args.baud)
    try:
        if args.op == 'put':
            results = [put(con, args, f) for f in args.files]
            ok = all(results)
            print(f"{sum(results)}/{len(results)} file(s) uploaded and verified" if len(results) > 1 else '')
        elif args.op == 'get':
            ok = get(con, args)
        elif args.op == 'sha256':
            ok = True
            for remote in args.remotes:
                sha, out = remote_sha256(con, remote)
                print(out)
                ok &= sha is not None
        else:
            ok = True
            for cmd in args.cmds:
                print(con.command(cmd, timeout=60))
    finally:
        con.close()
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
