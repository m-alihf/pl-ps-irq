#!/usr/bin/env python3
"""Send an array to the board, get it back after the other core has worked on
it, check every element and time the round trip.

    python amp_xchg.py 192.168.1.199
"""
import socket
import struct
import sys
import time

WORDS = 256                             # ARR_WORDS in amp_shared.h
FMT = struct.Struct(f"<{WORDS}I")

s = socket.create_connection((sys.argv[1], 7))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

base = 0
wrong = 0
trips = []
t_report = time.time()

while True:
    req = [(base + i) & 0xFFFFFFFF for i in range(WORDS)]
    base += WORDS                       # a new array every time, so a stale answer cannot pass

    t0 = time.perf_counter()
    s.sendall(FMT.pack(*req))
    buf = b""
    while len(buf) < FMT.size:
        chunk = s.recv(FMT.size - len(buf))
        if not chunk:
            sys.exit("connection closed")
        buf += chunk
    trips.append(time.perf_counter() - t0)

    if list(FMT.unpack(buf)) != [(x * 3 + 1) & 0xFFFFFFFF for x in req]:
        wrong += 1

    if time.time() - t_report >= 1.0:
        trips.sort()
        print(f"{len(trips)} exchanges/s   wrong so far {wrong}   round trip "
              f"min {trips[0] * 1e6:.0f}  median {trips[len(trips) // 2] * 1e6:.0f}  "
              f"max {trips[-1] * 1e6:.0f} us")
        trips = []
        t_report = time.time()
