#!/usr/bin/env python3
"""Test-only TCP-to-PTY peer: runs the actual daemon against an isolated filesystem."""
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
binary = Path(sys.argv[2]).resolve()
master, slave = pty.openpty()
listener = socket.socket()
listener.bind(('127.0.0.1', 0))
listener.listen(1)
log = (root / 'daemon.log').open('wb')
process = subprocess.Popen([str(binary), '--device', os.ttyname(slave),
    '--socket', str(root / 'ipc.sock'), '--root', str(root / 'legacy'),
    '--fs-root', str(root), '--lock-dir', str(root), '--allow-shell'], stdout=log, stderr=log)
connection = None
try:
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError('daemon failed: ' + (root / 'daemon.log').read_text())
        if (root / 'ipc.sock').exists(): break
        time.sleep(.02)
    else: raise RuntimeError('daemon start timed out')
    print(listener.getsockname()[1], flush=True)
    listener.settimeout(20)
    connection, _ = listener.accept()
    while True:
        ready, _, _ = select.select([connection, master], [], [], 2)
        for fd in ready:
            if fd is connection:
                data = connection.recv(65536)
                if not data: sys.exit(0)
                view = memoryview(data)
                while view:
                    n = os.write(master, view)
                    view = view[n:]
            else:
                data = os.read(master, 65536)
                connection.sendall(data)
finally:
    if connection: connection.close()
    listener.close()
    process.terminate()
    try: process.wait(5)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
    os.close(master); os.close(slave); log.close()
