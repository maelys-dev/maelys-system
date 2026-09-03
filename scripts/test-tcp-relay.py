#!/usr/bin/env python3
"""End-to-end loopback check for the callback-free TCP relay example."""

import socket
import subprocess
import sys
import threading
import time


def listener():
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 0))
    sock.listen(1)
    return sock


def main():
    if len(sys.argv) != 2:
        return 2
    upstream = listener()
    relay_probe = listener()
    relay_port = relay_probe.getsockname()[1]
    relay_probe.close()
    upstream_port = upstream.getsockname()[1]
    observed = []

    def echo():
        connection, _ = upstream.accept()
        with connection:
            while True:
                data = connection.recv(4096)
                if not data:
                    break
                observed.append(data)
                connection.sendall(data)

    worker = threading.Thread(target=echo)
    worker.start()
    process = subprocess.Popen(
        [sys.argv[1], str(relay_port), "127.0.0.1", str(upstream_port)]
    )
    try:
        for _ in range(100):
            try:
                client = socket.create_connection(("127.0.0.1", relay_port), 0.05)
                break
            except OSError:
                if process.poll() is not None:
                    raise RuntimeError("relay exited before accepting")
                time.sleep(0.01)
        else:
            raise RuntimeError("relay did not listen")
        with client:
            payload = b"maelys-system relay proof" * 1024
            client.sendall(payload)
            client.shutdown(socket.SHUT_WR)
            reply = bytearray()
            while True:
                block = client.recv(4096)
                if not block:
                    break
                reply.extend(block)
            if bytes(reply) != payload:
                raise RuntimeError(
                    f"relay response mismatch: {len(reply)} != {len(payload)}; "
                    f"exit={process.poll()}"
                )
        if process.wait(timeout=5) != 0:
            raise RuntimeError("relay failed")
        worker.join(timeout=5)
        if worker.is_alive() or b"".join(observed) != payload:
            raise RuntimeError("upstream did not observe payload")
    finally:
        upstream.close()
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
