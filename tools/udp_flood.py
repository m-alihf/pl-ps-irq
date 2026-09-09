#!/usr/bin/env python3
"""Flood the board with UDP broadcast so CPU1's lwIP has real network work to do.

The board's transmit path does not reach this host, so it cannot answer ARP and
no unicast or TCP session is possible. Broadcast needs neither: the frames carry
FF:FF:FF:FF:FF:FF, the MAC accepts them, and every one still costs CPU1 an EMAC
receive interrupt, a DMA write into the shared DDR, a pbuf allocation and a walk
up the stack - which is the interference the 12.5 us interrupt on CPU0 is being
measured against.

    python udp_flood.py --src 192.168.1.100 --bcast 192.168.1.255 --size 1472
"""

import argparse
import socket
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="192.168.1.100",
                    help="address of the adapter facing the board, so the "
                         "broadcast leaves the right interface")
    ap.add_argument("--bcast", default="192.168.1.255")
    ap.add_argument("--port", type=int, default=7)
    ap.add_argument("--size", type=int, default=1472, help="UDP payload bytes")
    ap.add_argument("--seconds", type=int, default=0, help="0 = until Ctrl-C")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    s.bind((a.src, 0))

    payload = bytes(range(256)) * (a.size // 256 + 1)
    payload = payload[:a.size]
    dst = (a.bcast, a.port)

    print(f"flooding {a.bcast}:{a.port} from {a.src}, {a.size} byte payloads")
    t0 = time.time()
    tick = t0
    sent = 0
    last = 0
    try:
        while a.seconds == 0 or time.time() - t0 < a.seconds:
            for _ in range(200):
                try:
                    s.sendto(payload, dst)
                    sent += 1
                except OSError:
                    time.sleep(0.001)
            now = time.time()
            if now - tick >= 1.0:
                n = sent - last
                mbps = n * (a.size + 42) * 8 / (now - tick) / 1e6
                print(f"{now - t0:6.0f}s  {n / (now - tick):8.0f} pkt/s  "
                      f"{mbps:6.2f} Mbit/s  total {sent}")
                tick = now
                last = sent
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        print(f"stopped after {sent} packets")


if __name__ == "__main__":
    main()
