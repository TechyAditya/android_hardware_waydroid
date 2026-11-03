#!/usr/bin/env python3
"""Opens an app in its own window and Android display; falls back to stock Waydroid."""

import os
import socket
import subprocess
import sys
import time

WAYDROID = "/usr/bin/waydroid"
DATA_HOME = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
SOCKET = os.path.join(DATA_HOME, "waydroid", "data", "waydroid-app-displays", "control.sock")
# Waydroid needs about 20 s to boot Android; the service starts after that.
START_TIMEOUT = 90


def label(pkg):
    path = os.path.join(DATA_HOME, "applications", f"waydroid.{pkg}.desktop")
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("Name="):
                    return line[5:].strip()
    except OSError:
        pass
    return ""


def request(line):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(5)
        s.connect(SOCKET)
        s.sendall(line.encode() + b"\n")
        return s.recv(4096).decode().strip()


def session_running():
    status = subprocess.run([WAYDROID, "status"], capture_output=True, text=True).stdout
    return "Session:\tRUNNING" in status


def stock(pkg):
    os.execv(WAYDROID, [WAYDROID, "app", "launch", pkg])


def main():
    pkg = sys.argv[1]
    line = f"launch {pkg} {label(pkg)}".rstrip()
    try:
        reply = request(line)
    except OSError:
        if session_running():
            stock(pkg)  # the service is disabled or not running
        subprocess.Popen([WAYDROID, "session", "start"], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.monotonic() + START_TIMEOUT
        while True:
            time.sleep(0.5)
            try:
                reply = request(line)
                break
            except OSError:
                if time.monotonic() > deadline:
                    stock(pkg)
    if not reply.startswith("ok"):
        print(f"waydroid-app-displays: {reply}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
