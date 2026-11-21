#!/usr/bin/env python3
"""HD485 maintenance host, POSIX only; no external Python packages required."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import struct
import sys
import termios
import time
import zlib

HEADER = struct.Struct('<4sBBHIIII')
MAX_PAYLOAD = 8192
HELLO, EXEC, POLL, CANCEL, PUT_BEGIN, PUT_DATA, PUT_COMMIT, GET_BEGIN, GET_DATA, STATUS = range(1, 11)


def frame(magic, op, address, session, request, data=b''):
    if len(data) > MAX_PAYLOAD:
        raise ValueError('payload too large')
    prefix = struct.pack('<4sBBHIII', magic, 1, op, address, session, request, len(data))
    return prefix + struct.pack('<I', zlib.crc32(prefix + data)) + data


class Parser:
    def __init__(self, magic=b'H485'):
        self.magic = magic
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= 4:
            if self.buffer[:4] != self.magic:
                del self.buffer[0]
                continue
            if len(self.buffer) < HEADER.size:
                break
            magic, version, op, address, session, request, length, crc = HEADER.unpack_from(self.buffer)
            if version != 1 or length > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            size = HEADER.size + length
            if len(self.buffer) < size:
                break
            body = bytes(self.buffer[HEADER.size:size])
            if zlib.crc32(self.buffer[:20] + body) != crc:
                del self.buffer[0]
                continue
            result.append((op, address, session, request, body))
            del self.buffer[:size]
        return result


class Host:
    def __init__(self, fd, address=1, timeout=0.8, retries=3, turnaround_ms=0):
        self.fd, self.address, self.timeout, self.retries = fd, address, timeout, retries
        self.session, self.request = 0, 0
        self.parser = Parser()
        if turnaround_ms < 0:
            raise ValueError("turnaround must be nonnegative")
        self.turnaround = turnaround_ms / 1000
        self.next_write = 0

    def _write(self, data):
        time.sleep(max(0, self.next_write - time.monotonic()))
        end = time.monotonic() + self.timeout
        view = memoryview(data)
        while view:
            left = end - time.monotonic()
            if left <= 0 or not select.select([], [self.fd], [], left)[1]:
                raise TimeoutError('serial write timed out')
            try:
                count = os.write(self.fd, view)
                if not count:
                    raise OSError(errno.EIO, 'zero length serial write')
                view = view[count:]
            except BlockingIOError:
                continue

    def call(self, op, data=b''):
        self.request += 1
        if self.request > 0xffffffff:
            raise RuntimeError('request counter exhausted; start a new session')
        request = self.request
        packet = frame(b'H485', op, self.address, 0 if op == HELLO else self.session, request, data)
        for _ in range(self.retries):
            self._write(packet)  # Retries deliberately retain the identical request ID.
            end = time.monotonic() + self.timeout
            while time.monotonic() < end:
                if not select.select([self.fd], [], [], max(0, end-time.monotonic()))[0]:
                    break
                try:
                    raw = os.read(self.fd, 16384)
                except BlockingIOError:
                    continue
                if not raw:
                    raise OSError(errno.EIO, 'serial disconnected')
                for rop, address, session, rid, body in self.parser.feed(raw):
                    if rop != op | 0x80 or rid != request or address != self.address:
                        continue
                    if op != HELLO and session != self.session:
                        continue
                    if len(body) < 4:
                        raise ValueError('short response')
                    self.next_write = time.monotonic() + self.turnaround
                    status, = struct.unpack_from('<I', body)
                    if status:
                        raise OSError(status, f'device error {status}')
                    return body[4:]
            # An incomplete reply must not pin the parser across retry windows.
            self.parser.buffer.clear()
        raise TimeoutError('no response; do not automatically replay a command in a new session')

    def hello(self):
        result = self.call(HELLO, secrets.token_bytes(8))
        self.session, self.block_size = struct.unpack('<II', result)

    def ready_call(self, op, data=b'', deadline_seconds=180):
        end = time.monotonic() + deadline_seconds
        while True:
            try:
                return self.call(op, data)
            except OSError as error:
                # Linux EAGAIN=11; Darwin EAGAIN=35. Wire status is target errno.
                if error.errno not in (11, 35) or time.monotonic() >= end:
                    raise
                time.sleep(0.02)

    def execute(self, command, stdout=None, stderr=None):
        stdout = stdout or sys.stdout.buffer
        stderr = stderr or sys.stderr.buffer
        self.call(EXEC, command.encode())
        offsets = [0, 0]
        truncated = False
        while True:
            finished = []
            exit_code = -1
            for stream, output in enumerate((stdout, stderr)):
                data = self.call(POLL, struct.pack('<BI', stream, offsets[stream]))
                state, cut, exit_code, total, eof = struct.unpack_from('<BBiIB', data)
                output.write(data[11:])
                output.flush()
                offsets[stream] += len(data)-11
                truncated |= bool(cut)
                finished.append(state != 1 and eof and offsets[stream] >= total)
            if all(finished):
                if truncated:
                    print('command output truncated by device limit', file=sys.stderr)
                return exit_code if exit_code >= 0 else 1
            time.sleep(0.02)

    def put(self, local, remote, overwrite=False):
        with open(local, 'rb') as source:
            digest = hashlib.file_digest(source, 'sha256').digest()
            # file_digest can bypass the buffered position; fstat is authoritative.
            size = os.fstat(source.fileno()).st_size
            data = struct.pack('<Q', size) + digest + bytes([overwrite]) + remote.encode()
            offset, = struct.unpack('<Q', self.call(PUT_BEGIN, data))
            if offset > size:
                raise ValueError('invalid resume offset')
            source.seek(offset)
            while offset < size:
                block = source.read(min(240, size-offset))
                if not block:
                    raise ValueError('source changed during upload')
                reply = self.call(PUT_DATA, struct.pack('<Q', offset) + block)
                next_offset, = struct.unpack('<Q', reply)
                if next_offset != offset + len(block):
                    raise ValueError('unexpected upload offset')
                offset = next_offset
            self.ready_call(PUT_COMMIT)
            return digest.hex()

    def get(self, remote, local, overwrite=False):
        target = Path(local)
        if target.exists() and not overwrite:
            raise FileExistsError(target)
        response = self.ready_call(GET_BEGIN, remote.encode())
        size, = struct.unpack_from('<Q', response)
        digest = response[8:40].hex()
        part = target.with_name(target.name + '.hd485-part')
        meta = target.with_name(target.name + '.hd485-meta')
        identity = {'remote': remote, 'size': size, 'sha256': digest}
        if part.is_symlink() or meta.is_symlink():
            raise ValueError('refusing symlink resume files')
        if part.exists():
            if not meta.exists() or json.loads(meta.read_text()) != identity:
                raise ValueError('download resume identity mismatch')
        else:
            # Refuse conflicting sidecars instead of silently overwriting unrelated files.
            if meta.exists() and json.loads(meta.read_text()) != identity:
                raise ValueError('download metadata conflict')
            if not meta.exists():
                with meta.open('x') as f:
                    json.dump(identity, f)
        with part.open('ab') as dest:
            offset = dest.tell()
            if offset > size:
                raise ValueError('partial download larger than source')
            while offset < size:
                chunk = self.call(GET_DATA, struct.pack('<Q', offset))
                received_offset, = struct.unpack_from('<Q', chunk)
                if received_offset != offset or not chunk[8:] or offset+len(chunk)-8 > size:
                    raise ValueError('invalid download chunk')
                dest.write(chunk[8:])
                dest.flush()
                os.fsync(dest.fileno())
                offset += len(chunk)-8
        with part.open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != digest:
                raise ValueError('download checksum mismatch; partial file retained')
        if overwrite:
            os.replace(part, target)
        else:
            os.link(part, target)
            part.unlink()
        meta.unlink()
        return digest


def open_serial(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    exclusive = False
    try:
        import fcntl
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.ioctl(fd, termios.TIOCEXCL)
        exclusive = True
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        speed = getattr(termios, f'B{baud}', None)
        custom = speed is None and sys.platform == 'darwin' and baud in (460800, 921600)
        if speed is None and not custom:
            raise ValueError(f'unsupported baud rate: {baud}')
        attrs[4] = attrs[5] = termios.B115200 if custom else speed
        attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        if custom:
            # Apple IOKit/serial/ioss.h: _IOW('T', 2, speed_t), 64-bit speed_t.
            fcntl.ioctl(fd, 0x80085402, struct.pack('=Q', baud))
        return fd
    except BaseException:
        try:
            if exclusive:
                fcntl.ioctl(fd, termios.TIOCNXCL)
        except OSError:
            pass
        os.close(fd)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--baud', type=int, default=230400)
    parser.add_argument('--address', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=0.8)
    parser.add_argument('--turnaround-ms', type=float, default=60, help='RX to TX guard, matching HDTools (default 60ms)')
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('status')
    sub.add_parser('cancel')
    command = sub.add_parser('exec')
    command.add_argument('command')
    for name in ('put', 'get'):
        cmd = sub.add_parser(name)
        cmd.add_argument('source')
        cmd.add_argument('destination')
        cmd.add_argument('--overwrite', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.address <= 65535 or args.timeout <= 0 or args.turnaround_ms < 0:
        parser.error('invalid address, timeout or turnaround')
    fd = open_serial(args.port, args.baud)
    host = Host(fd, args.address, args.timeout, turnaround_ms=args.turnaround_ms)
    try:
        host.hello()
        if args.action == 'status':
            state, code, offset = struct.unpack('<IiQ', host.call(STATUS))
            print(json.dumps({'command_state': state, 'exit_code': code, 'upload_offset': offset}))
        elif args.action == 'cancel':
            host.call(CANCEL)
        elif args.action == 'exec':
            return host.execute(args.command)
        elif args.action == 'put':
            print(host.put(args.source, args.destination, args.overwrite))
        elif args.action == 'get':
            print(host.get(args.source, args.destination, args.overwrite))
        return 0
    except KeyboardInterrupt:
        try:
            host.call(CANCEL)
        except (OSError, TimeoutError):
            pass
        return 130
    finally:
        import fcntl
        fcntl.ioctl(fd, termios.TIOCNXCL)
        os.close(fd)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, TimeoutError) as error:
        print(f'hd485ctl: {error}', file=sys.stderr)
        sys.exit(1)
