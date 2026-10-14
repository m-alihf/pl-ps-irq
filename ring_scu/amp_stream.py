#!/usr/bin/env python3
"""Receive the sample stream CPU0 produces and CPU1 sends, check that no
sample is missing, and time a command round trip through both cores.

    python amp_stream.py 192.168.1.199
"""
import socket
import struct
import sys
import time

REC = struct.Struct("<4I")      # seq, data, ts, cmd - the same 16 bytes as sample_t

s = socket.create_connection((sys.argv[1], 7))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

buf = b""
last_seq = None
count = missing = 0
cmd = 0
sent_at = None
t_report = time.time()

while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
    whole = len(buf) - len(buf) % REC.size

    for seq, data, ts, got_cmd in REC.iter_unpack(buf[:whole]):
        if last_seq is not None:
            missing += (seq - last_seq - 1) & 0xFFFFFFFF
        last_seq = seq
        count += 1
        if sent_at is not None and got_cmd == cmd:
            print(f"  command {cmd} came back in {(time.perf_counter() - sent_at) * 1e3:.2f} ms")
            sent_at = None
    buf = buf[whole:]

    if time.time() - t_report >= 1.0:
        print(f"{count} samples/s   missing so far {missing}")
        count = 0
        t_report = time.time()
        cmd += 1
        s.sendall(struct.pack("<I", cmd))
        sent_at = time.perf_counter()
