#!/usr/bin/env python3
"""Force a Schwung chain slot to reload its AUDIO FX module.

WHY THIS EXISTS. Deploying a new 4k-eq.so does not change what the device is
running. The chain host dlopen()s the plugin into the shim (inside
MoveOriginal); the atomic mv in deploy.sh swaps the directory entry while the
running process keeps the OLD INODE mapped. `kill shadow_ui` does not help —
shadow_ui is a different process. Worse, an on-device loadtest dlopens the
file itself, so it passes against code nobody is hearing: green tests, stale
code.

Re-writing the FX position's module key makes the chain host unload that
position and dlopen a fresh instance — what re-picking the effect in the UI
does.

The key is "fx<N>:module", ONE-BASED and with no leading zero. That shape is
not a guess: it is what src/host/shadow_fx_key.h in the Schwung tree matches,
and the header is explicit that "synth:module" (which the sound-generator
version of this script writes) deliberately does NOT match — a synth has its
own activation branch.

    ./scripts/reload_fx_slot.py <host> [track-slot] [fx-position] [module-id]

Standard library only (the Mac has no toolchain and no venv here).
"""
import base64
import json
import os
import socket
import struct
import sys
import time


def send(sock, obj):
    payload = json.dumps(obj).encode()
    header = bytearray([0x81])
    n = len(payload)
    if n < 126:
        header.append(0x80 | n)
    elif n < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", n)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", n)
    mask = os.urandom(4)
    header += mask
    sock.sendall(bytes(header) + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "move.local"
    slot = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    fxpos = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    module = sys.argv[4] if len(sys.argv) > 4 else "4k-eq"

    if fxpos < 1:
        print("reload: fx position is one-based; got %d" % fxpos)
        return 2

    key = base64.b64encode(os.urandom(16)).decode()
    try:
        sock = socket.create_connection((host, 7700), timeout=6)
    except OSError as e:
        print("reload: cannot reach schwung-manager on %s:7700 (%s)" % (host, e))
        return 1
    sock.sendall((
        "GET /ws/remote-ui HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())

    head = b""
    sock.settimeout(6)
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(1)
        if not chunk:
            print("reload: websocket handshake closed early")
            return 1
        head += chunk
    if b" 101" not in head.split(b"\r\n")[0]:
        print("reload: websocket upgrade refused:",
              head.split(b"\r\n")[0].decode(errors="replace"))
        return 1

    send(sock, {"type": "subscribe", "slot": slot})
    time.sleep(0.3)
    send(sock, {"type": "set_param", "slot": slot,
                "key": "fx%d:module" % fxpos, "value": module})
    time.sleep(1.5)

    # VERIFY, do not announce.
    #
    # This used to print "the new .so is now the running one" straight after
    # the write, having checked nothing. That is the precise shape of the
    # failure this script exists to prevent: a confident line in the log while
    # the device runs the old code. The write is silently ignored when the
    # position is out of range or the slot has no chain, which is exactly what
    # happens on a first install — so re-subscribe and ask the manager what is
    # actually in the slot.
    send(sock, {"type": "subscribe", "slot": slot})
    loaded = None
    deadline = time.time() + 5.0
    sock.settimeout(5)
    buf = b""
    while time.time() < deadline and loaded is None:
        try:
            data = sock.recv(65536)
        except OSError:
            break
        if not data:
            break
        buf += data
        while len(buf) >= 2:
            ln, off = buf[1] & 0x7F, 2
            if ln == 126:
                if len(buf) < 4:
                    break
                ln, off = struct.unpack(">H", buf[2:4])[0], 4
            elif ln == 127:
                if len(buf) < 10:
                    break
                ln, off = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < off + ln:
                break
            frame, buf = buf[off:off + ln], buf[off + ln:]
            try:
                msg = json.loads(frame)
            except ValueError:
                continue
            if msg.get("type") == "slot_info":
                loaded = msg.get("fx%d" % fxpos)
    sock.close()

    if loaded == module:
        print("reload: slot %d fx%d is running '%s' — verified via slot_info"
              % (slot, fxpos, module))
        return 0
    print("reload: slot %d fx%d reports %r, not '%s'."
          % (slot, fxpos, loaded, module))
    print("        The module is INSTALLED but nothing is running it. That is")
    print("        normal on a first install: pick the effect in an FX slot on")
    print("        the device, then re-run this to pick up new code.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
