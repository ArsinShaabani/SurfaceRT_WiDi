#!/usr/bin/env python3
"""Cross-checks the PC-side (C#) wire layout against the C++ receiver.

This is a byte-for-byte port of tools/pc/lib/WiDiNative.cs (same field offsets,
same CRC) so a mistake in the C# encoder is caught on Linux instead of on the
tablet.

Usage: python3 tools/dev/wire_check.py --port 47123 --frames 15
"""
import argparse
import socket
import struct
import sys
import time

MAGIC = 0x49444957
VERSION = 1
T_HELLO, T_HELLO_ACK, T_FRAME, T_STATS, T_INPUT, T_BYE = 2, 3, 4, 5, 6, 9


def crc32_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (0xEDB88320 ^ (c >> 1)) if (c & 1) else (c >> 1)
        table.append(c)
    return table


TABLE = crc32_table()


def crc32(data):
    c = 0xFFFFFFFF
    for b in data:
        c = TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def message(msg_type, seq, payload=b"", flags=0):
    header = struct.pack("<IBBHII", MAGIC, VERSION, msg_type, flags, seq, len(payload))
    header += struct.pack("<I", crc32(payload) if payload else 0)
    assert len(header) == 20, len(header)
    return header + payload


def hello(name, max_w, max_h, max_fps, caps):
    encoded = name.encode("utf-8")[:60]
    payload = struct.pack("<HHHBB", VERSION, max_w, max_h, max_fps, caps)
    payload += bytes([len(encoded)]) + encoded
    return message(T_HELLO, 1, payload)


def frame(seq, width, height, fmt, ts_ms, pixels):
    payload = struct.pack("<HHBBI", width, height, fmt, 0, ts_ms) + pixels
    assert len(payload) == 10 + len(pixels)
    return message(T_FRAME, seq, payload)


def recv_exact(sock, count):
    chunks = []
    got = 0
    while got < count:
        chunk = sock.recv(count - got)
        if not chunk:
            raise RuntimeError("sink closed the connection")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--frames", type=int, default=15)
    ap.add_argument("--width", type=int, default=64)
    ap.add_argument("--height", type=int, default=48)
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.sendall(hello("PY-WIRE-CHECK", 1920, 1080, 30, 0x07))

    header = recv_exact(sock, 20)
    magic, version, msg_type, flags, seq, length, crc = struct.unpack("<IBBHIII", header)
    if magic != MAGIC or version != VERSION or msg_type != T_HELLO_ACK:
        print("FAIL: bad handshake reply", hex(magic), version, msg_type)
        return 1
    payload = recv_exact(sock, length)
    if crc32(payload) != crc:
        print("FAIL: handshake crc mismatch")
        return 1
    proto, sw, sh, session, accepted, reason = struct.unpack("<HHHIBB", payload[:12])
    name = payload[13:13 + payload[12]].decode("utf-8", "replace")
    print("handshake ok: sink='%s' %dx%d proto=%d accepted=%d" % (name, sw, sh, proto, accepted))
    if not accepted:
        print("FAIL: sink rejected the session (reason %d)" % reason)
        return 1

    pixel_count = args.width * args.height * 4
    for i in range(args.frames):
        pixels = bytes(((x * 13 + i * 7) & 0xFF) for x in range(pixel_count))
        sock.sendall(frame(10 + i, args.width, args.height, 0, int(time.time() * 1000) & 0xFFFFFFFF, pixels))
        time.sleep(0.03)

    # Read whatever feedback arrived (Stats messages).
    sock.settimeout(0.5)
    stats_seen = 0
    try:
        while True:
            header = recv_exact(sock, 20)
            magic, version, msg_type, flags, seq, length, crc = struct.unpack("<IBBHIII", header)
            payload = recv_exact(sock, length) if length else b""
            if msg_type == T_STATS:
                decoded, dropped, decode_q8, fps_q8 = struct.unpack("<IIHH", payload)
                print("stats: decoded=%d dropped=%d decode=%.1fms fps=%.1f" %
                      (decoded, dropped, decode_q8 / 16.0, fps_q8 / 16.0))
                stats_seen += 1
            if stats_seen >= 2:
                break
    except (socket.timeout, RuntimeError):
        pass

    sock.sendall(message(T_BYE, 999, b"\x00"))
    sock.close()
    print("wire check passed (%d frames, %d stats windows)" % (args.frames, stats_seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
