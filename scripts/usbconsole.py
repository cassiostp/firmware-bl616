#!/usr/bin/env python3
"""Talk to the TangCore USB debug console and log everything to a file.

The console works with any terminal (tio /dev/ttyACM0, screen); this
wrapper only adds a timestamped transcript, which is what you want when
hunting a hang: leave it running, reproduce, and the log file holds the
firmware's view with wall-clock times.

Usage: scripts/usbconsole.py [/dev/ttyACM0] [logfile]
Defaults: first /dev/ttyACM*, usbconsole-YYYYmmdd-HHMMSS.log in cwd.
Ctrl-C quits. Stdlib only (Linux).
"""
import glob
import os
import select
import sys
import termios
import time
import tty


def main() -> int:
    dev = sys.argv[1] if len(sys.argv) > 1 else None
    if not dev:
        cands = sorted(glob.glob("/dev/ttyACM*"))
        if not cands:
            print("no /dev/ttyACM* found - is the console on? "
                  "(Options -> USB debug console)", file=sys.stderr)
            return 1
        dev = cands[0]
    logname = sys.argv[2] if len(sys.argv) > 2 else \
        time.strftime("usbconsole-%Y%m%d-%H%M%S.log")

    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # CDC ACM ignores baud, but the port must be raw and DTR asserted:
    # opening with termios defaults + HUPCL off keeps DTR up.
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.HUPCL
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    stdin_fd = sys.stdin.fileno()
    old_stdin = termios.tcgetattr(stdin_fd)
    tty.setraw(stdin_fd)

    print(f"{dev} -> {logname} (Ctrl-C quits)\r", file=sys.stderr)
    line_start = True
    try:
        with open(logname, "ab", buffering=0) as log:
            while True:
                r, _, _ = select.select([fd, stdin_fd], [], [], 0.2)
                if fd in r:
                    try:
                        data = os.read(fd, 4096)
                    except OSError:
                        break
                    if not data:
                        break
                    for b in data:
                        if line_start:
                            stamp = time.strftime("[%H:%M:%S] ").encode()
                            os.write(sys.stdout.fileno(), stamp)
                            log.write(stamp)
                            line_start = False
                        os.write(sys.stdout.fileno(), bytes([b]))
                        log.write(bytes([b]))
                        if b == 0x0A:
                            line_start = True
                if stdin_fd in r:
                    data = os.read(stdin_fd, 256)
                    if b"\x03" in data:      # Ctrl-C
                        break
                    os.write(fd, data)
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSANOW, old_stdin)
        os.close(fd)
        print("\nbye", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
