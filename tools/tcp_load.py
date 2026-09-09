#!/usr/bin/env python3
"""Drive the lwIP echo server on CPU1 so the PL interrupt on CPU0 is measured
under real network traffic rather than a synthetic memory load.

Each worker sends a block and reads the same number of bytes back, so both the
receive and the transmit DMA paths of the EMAC stay busy.

    python tcp_load.py --host 192.168.1.10 --conns 4 --block 4096
"""

import argparse
import socket
import threading
import time

stop = threading.Event()
sent = [0]
lock = threading.Lock()


def worker(host, port, block, wid):
    payload = bytes((wid + i) & 0xFF for i in range(block))
    try:
        s = socket.create_connection((host, port), timeout=5)
    except OSError as e:
        print(f"worker {wid}: connect failed: {e}")
        return
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(5)
    view = memoryview(bytearray(block))
    try:
        while not stop.is_set():
            s.sendall(payload)
            got = 0
            while got < block:
                n = s.recv_into(view[got:], block - got)
                if n == 0:
                    raise ConnectionError("closed by peer")
                got += n
            with lock:
                sent[0] += block
    except OSError as e:
        if not stop.is_set():
            print(f"worker {wid}: {e}")
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.10")
    ap.add_argument("--port", type=int, default=7)
    ap.add_argument("--conns", type=int, default=4)
    ap.add_argument("--block", type=int, default=4096)
    ap.add_argument("--seconds", type=int, default=0, help="0 = until Ctrl-C")
    a = ap.parse_args()

    threads = [threading.Thread(target=worker,
                                args=(a.host, a.port, a.block, i), daemon=True)
               for i in range(a.conns)]
    for t in threads:
        t.start()

    print(f"echoing against {a.host}:{a.port}, {a.conns} connections, "
          f"{a.block} byte blocks")
    t0 = time.time()
    last = 0
    try:
        while a.seconds == 0 or time.time() - t0 < a.seconds:
            time.sleep(1.0)
            with lock:
                now = sent[0]
            mbps = (now - last) * 8 / 1e6
            print(f"{time.time() - t0:6.0f}s  {mbps:7.2f} Mbit/s "
                  f"(echoed both ways)  total {now / 1e6:.1f} MB")
            last = now
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=2)


if __name__ == "__main__":
    main()
