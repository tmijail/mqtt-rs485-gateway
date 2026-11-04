"""Bounded host CPU load for reproducing load-dependent E2E failures (issue #88).

    python3 tools/e2e/cpu_load.py <processes, at most 2> <seconds> &

Each process is a busy loop that ends by itself after <seconds>; SIGTERM to the parent stops
them earlier. Capped at two so that it cannot starve other work on a shared host."""
import multiprocessing
import signal
import sys
import time


def spin(end):
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    x = 0
    while time.time() < end:
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF


if __name__ == "__main__":
    n, t = min(int(sys.argv[1]), 2), float(sys.argv[2])
    end = time.time() + t
    procs = [multiprocessing.Process(target=spin, args=(end,), daemon=True) for _ in range(n)]
    for p in procs:
        p.start()

    def stop(*_):
        for p in procs:
            p.terminate()
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    for p in procs:
        p.join()
