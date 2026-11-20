#!/usr/bin/env python3
"""End-to-end PTY tests. Retains logs/files as build/test-artifacts evidence."""
import errno
import hashlib
import io
import os
from pathlib import Path
import pty
import select
import shlex
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hd485ctl import Host, Parser, frame, EXEC, POLL, PUT_BEGIN, PUT_DATA, PUT_COMMIT, GET_BEGIN, GET_DATA, STATUS

BUILD = Path(os.environ.get('HD485_BUILD', ROOT / 'build')).resolve()
ARTIFACTS = BUILD / 'test-artifacts' / str(time.time_ns())
ARTIFACTS.mkdir(parents=True)


class IPC:
    def __init__(self, path, sock=None):
        kind = socket.SOCK_SEQPACKET if sys.platform.startswith('linux') else socket.SOCK_STREAM
        self.sock = sock or socket.socket(socket.AF_UNIX, kind)
        if sock is None:
            self.sock.connect(str(path))
        self.parser = Parser(b'I485')
        self.frames = []
        self.events = []
        self.rx = bytearray()
        self.id = 0

    def close(self):
        self.sock.close()

    def recv(self, timeout=3):
        end = time.monotonic()+timeout
        while not self.frames:
            left = end-time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                raise TimeoutError('IPC response')
            data = self.sock.recv(16384)
            if not data:
                raise EOFError('IPC closed')
            self.frames.extend(self.parser.feed(data))
        return self.frames.pop(0)

    def collect(self, response=None, timeout=3):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            op, _, _, rid, data = self.recv(max(0.001, end-time.monotonic()))
            if op == 0x70:
                self.rx.extend(data[8:])
            elif op == 0x71:
                self.events.append(struct.unpack('<III', data))
            elif response is None or rid == response:
                return op, data
        raise TimeoutError('IPC collect')

    def call(self, op, data=b'', timeout=3):
        self.id += 1
        packet = frame(b'I485', op, 0, 0, self.id, data)
        self.sock.sendall(packet)
        got_op, result = self.collect(self.id, timeout)
        if got_op != op | 0x80:
            raise AssertionError('wrong operation')
        error, = struct.unpack_from('<I', result)
        if error:
            raise OSError(error, 'IPC error')
        return result[4:]

    def received(self, length, timeout=3):
        end = time.monotonic()+timeout
        while len(self.rx) < length:
            op, _, _, _, data = self.recv(max(0.001, end-time.monotonic()))
            if op == 0x70:
                self.rx.extend(data[8:])
            elif op == 0x71:
                self.events.append(struct.unpack('<III', data))
        data = bytes(self.rx[:length])
        del self.rx[:length]
        return data


class Integration(unittest.TestCase):
    def setUp(self):
        self.folder = ARTIFACTS / self._testMethodName
        self.folder.mkdir()
        self.socket_path = BUILD / 'integration.sock'
        self.master, self.slave = pty.openpty()
        os.set_blocking(self.master, False)
        self.log = (self.folder / 'daemon.log').open('wb')
        self.inherited = os.environ.get('HD485_TEST_FDS') == '1'
        self.pairs = []
        extra = []
        if self.inherited:
            kind = socket.SOCK_SEQPACKET if sys.platform.startswith('linux') else socket.SOCK_STREAM
            self.pairs = [socket.socketpair(socket.AF_UNIX,kind) for _ in range(16)]
            extra = ['--test-client-fds', ','.join(str(b.fileno()) for a,b in self.pairs)]
        if 'qjy_' in self._testMethodName:
            extra += ['--rx-profile','qjy']
        if 'fs_' in self._testMethodName:
            extra += ['--fs-root', str(self.folder)]
        if 'interactive_' in self._testMethodName:
            extra += ['--allow-pty', '--pty-timeout-ms', '5000']
        no_flock = 'no_flock' in self._testMethodName
        binary = 'hd485d-test' if self.inherited or no_flock else 'hd485d'
        environment = os.environ.copy()
        if no_flock:
            environment['HD485_TEST_NO_FLOCK'] = '1'
        mode = 'coexist' if 'coexist' in self._testMethodName else 'idle'
        self.process = subprocess.Popen([
            str(BUILD/binary), '--device', os.ttyname(self.slave), '--socket', str(self.socket_path),
            '--root', str(self.folder/'files'), '--lock-dir', str(self.folder), '--mode', mode, '--allow-shell',
            '--command-timeout-ms', '800', '--response-ms', '200']+extra, stdout=self.log, stderr=self.log,
            pass_fds=[b.fileno() for a,b in self.pairs], env=environment)
        for a,b in self.pairs:
            b.close()
        self.available = [a for a,b in self.pairs]
        self.clients = []
        for _ in range(200):
            if self.process.poll() is not None:
                self.log.flush()
                self.fail((self.folder/'daemon.log').read_text())
            if 'hd485d ' in (self.folder/'daemon.log').read_text() and (self.inherited or self.socket_path.exists()):
                break
            time.sleep(0.01)
        else:
            self.fail('daemon start timed out')
        self.host = Host(self.master, timeout=0.35, retries=2)

    def tearDown(self):
        for client in self.clients:
            client.close()
        for sock in self.available:
            sock.close()
        self.process.terminate()
        try:
            self.process.wait(3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            self.fail('daemon failed to stop')
        os.close(self.master)
        os.close(self.slave)
        self.log.close()
        log = (self.folder/'daemon.log').read_text()
        self.assertNotIn('ERROR: AddressSanitizer', log)
        self.assertNotIn('runtime error:', log)
        self.assertEqual(self.process.returncode, 0, log)
        self.assertEqual(list(self.folder.glob('hd485-uart-*.lock')), [])

    def client(self):
        c = IPC(self.socket_path,self.available.pop(0) if self.inherited else None)
        self.clients.append(c)
        return c

    def serial_read(self, length, timeout=3):
        data = bytearray()
        end = time.monotonic()+timeout
        while len(data) < length:
            left = end-time.monotonic()
            if left <= 0 or not select.select([self.master], [], [], left)[0]:
                raise TimeoutError('serial read')
            data.extend(os.read(self.master, length-len(data)))
        return bytes(data)

    def serial_write(self, data):
        pending=memoryview(data)
        while pending:
            self.assertTrue(select.select([], [self.master], [], 3)[1])
            pending=pending[os.write(self.master,pending):]

    def assert_errno(self, code, fn, *args):
        with self.assertRaises(OSError) as context:
            fn(*args)
        self.assertEqual(context.exception.errno, code)

    def fs_records(self, path, stat=False):
        self.host.call(36 if stat else 33, path.encode())
        result = bytearray()
        while True:
            block = self.host.ready_call(34, struct.pack('<Q', len(result)))
            self.assertEqual(struct.unpack_from('<Q', block, 1)[0], len(result))
            result.extend(block[9:])
            if block[0]: break
        self.host.call(35)
        records = []
        while result:
            size, mode, length, modified, names, links = struct.unpack_from('<IIQqHH', result)
            self.assertEqual(size, 24+names+links)
            records.append((bytes(result[28:28+names]).decode(), mode, length,
                            bytes(result[28+names:4+size]).decode()))
            del result[:4+size]
        return records

    def test_fs_listing_and_boundaries(self):
        self.host.hello()
        info = self.host.call(32)
        self.assertEqual(struct.unpack_from('<HH', info), (1, 7))
        name = '中文 空格\n' + 'a'*220
        (self.folder/name).write_bytes(b'abc')
        (self.folder/'.hidden').write_bytes(b'')
        (self.folder/'link').symlink_to('/etc')
        entries = {r[0]: r for r in self.fs_records('/')}
        self.assertEqual(entries[name][2], 3)
        self.assertIn('.hidden', entries)
        self.assertEqual(entries['link'][3], '/etc')
        for path in ['/../etc', '/link/passwd', '//etc']:
            with self.assertRaises(OSError): self.host.call(38, path.encode())
        self.assertEqual(self.fs_records('/'+name, True)[0][0], name)
        (self.folder/'empty').mkdir()
        self.assertEqual(self.fs_records('/empty'), [])
        before = self.folder.stat()
        self.host.call(33, b'/')
        (self.folder/'added').touch()
        # Explicit timestamp change avoids coarse filesystem timestamp resolution.
        os.utime(self.folder, ns=(before.st_atime_ns, before.st_mtime_ns+2_000_000_000))
        with self.assertRaises(OSError): self.host.call(34, struct.pack('<Q',0))

    def test_fs_transfer_resume_conflict(self):
        self.host.hello()
        data = bytes(range(256))*3
        begin = struct.pack('<Q',len(data))+hashlib.sha256(data).digest()+b'\0/new.bin'
        self.assertEqual(self.host.call(37,begin),struct.pack('<Q',0))
        self.host.call(PUT_DATA,struct.pack('<Q',0)+data[:240])
        self.host.call(39)
        self.assertEqual(self.host.call(37,begin),struct.pack('<Q',240))
        for off in range(240,len(data),240):
            self.host.call(PUT_DATA,struct.pack('<Q',off)+data[off:off+240])
        self.host.ready_call(PUT_COMMIT)
        target = self.folder/'new.bin'
        self.assertEqual(target.read_bytes(),data)
        self.assertEqual(target.stat().st_mode & 0o777,0o644)
        info = self.host.ready_call(38,b'/new.bin')
        self.assertEqual(info,struct.pack('<Q',len(data))+hashlib.sha256(data).digest())
        self.assertEqual(self.host.call(GET_DATA,struct.pack('<Q',240))[8:],data[240:480])
        self.host.call(39)
        payload = b'replacement'
        self.host.call(37,struct.pack('<Q',len(payload))+hashlib.sha256(payload).digest()+b'\1/new.bin')
        self.host.call(PUT_DATA,struct.pack('<Q',0)+payload)
        target.write_bytes(b'changed by business')
        with self.assertRaises(OSError): self.host.ready_call(PUT_COMMIT)
        self.assertEqual(target.read_bytes(),b'changed by business')

    def test_fs_special_permissions_and_bad_hash(self):
        self.host.hello()
        os.mkfifo(self.folder/'fifo')
        with self.assertRaises(OSError): self.host.call(38,b'/fifo')
        # A rejected FIFO must not block the daemon event loop.
        self.assertEqual(len(self.host.call(10)),16)
        locked=self.folder/'locked';locked.mkdir();locked.chmod(0)
        try:
            if os.geteuid()!=0:
                with self.assertRaises(OSError): self.host.call(33,b'/locked')
        finally: locked.chmod(0o700)
        (self.folder/'target').write_bytes(b'original')
        (self.folder/'target').chmod(0o750)
        data=b'valid replacement'
        begin=struct.pack('<Q',len(data))+hashlib.sha256(data).digest()+b'\1/target'
        self.host.call(37,begin)
        self.host.call(PUT_DATA,struct.pack('<Q',0)+b'x'*len(data))
        with self.assertRaises(OSError): self.host.ready_call(PUT_COMMIT)
        self.assertEqual((self.folder/'target').read_bytes(),b'original')
        self.host.call(39)
        for temporary in self.folder.glob('.hd485-*'): temporary.unlink()
        self.host.call(37,begin)
        self.host.call(PUT_DATA,struct.pack('<Q',0)+data)
        self.host.ready_call(PUT_COMMIT)
        self.assertEqual((self.folder/'target').stat().st_mode & 0o777,0o750)
        self.host.ready_call(38,b'/target')
        (self.folder/'target').write_bytes(b'changed')
        with self.assertRaises(OSError): self.host.call(GET_DATA,struct.pack('<Q',0))

    def test_extension_disabled(self):
        self.host.hello()
        revision,flags=struct.unpack_from('<HH',self.host.call(32))
        self.assertEqual((revision,flags),(1,6))
        with self.assertRaises(OSError): self.host.call(33,b'/')
        with self.assertRaises(OSError): self.host.call(38,b'/etc/passwd')

    def test_fs_preemption_and_legacy_root(self):
        self.host.hello()
        self.host.call(33,b'/')
        client = self.client()
        client.call(1,struct.pack('<I',0))
        client.call(2)
        self.host.hello()
        with self.assertRaises(OSError): self.host.call(34,struct.pack('<Q',0))
        empty = struct.pack('<Q',0)+hashlib.sha256(b'').digest()+b'\0old'
        self.host.call(PUT_BEGIN,empty)
        self.host.ready_call(PUT_COMMIT)
        self.assertTrue((self.folder/'files'/'old').is_file())
        self.assertFalse((self.folder/'old').exists())

    def test_exclusive_and_permissions(self):
        a, b = self.client(), self.client()
        a.call(1, struct.pack('<I', 0))
        self.assert_errno(errno.EBUSY, b.call, 1, struct.pack('<I', 0))
        self.assert_errno(errno.EPERM, b.call, 5, b'forbidden')
        self.assert_errno(errno.EPERM, b.call, 7, struct.pack('<IIII', 9600,8,0,1))
        a.call(2)
        b.call(1, struct.pack('<I', 0))

    def test_no_flock_atomic_lock(self):
        locks=list(self.folder.glob('hd485-uart-*.lock'))
        self.assertEqual(len(locks),1)
        self.assertEqual(int(locks[0].read_text()),self.process.pid)
        self.assertIn('flock unavailable', (self.folder/'daemon.log').read_text())
        a=self.client();a.call(1,struct.pack('<I',0));a.call(5,b'fallback')
        self.assertEqual(self.serial_read(8),b'fallback')
        a.call(2)

    def test_broadcast_and_raw_bytes(self):
        a,b,c = self.client(),self.client(),self.client()
        a.call(1, struct.pack('<I',0))
        b.call(3);c.call(3)
        raw = bytes(range(256))*8
        self.serial_write(raw)
        for client in (a,b,c):
            self.assertEqual(client.received(len(raw)),raw)
        payload=b'raw-business\x00\xff'
        a.call(5,payload)
        self.assertEqual(self.serial_read(len(payload)),payload)
        a.call(2)
        self.assertTrue(any(e[0]==2 and e[2]==0 for e in a.events))

    def test_wait_fifo_and_disconnect(self):
        a,b,c=self.client(),self.client(),self.client()
        a.call(1,struct.pack('<I',0))
        result=[]
        def acquire(client,name):
            client.call(1,struct.pack('<I',2000),timeout=3)
            result.append(name)
        tb=threading.Thread(target=acquire,args=(b,'b'));tb.start();time.sleep(.05)
        tc=threading.Thread(target=acquire,args=(c,'c'));tc.start();time.sleep(.05)
        a.close();tb.join(2)
        self.assertEqual(result,['b'])
        b.call(2);tc.join(2);self.assertEqual(result,['b','c'])

    def test_wait_timeout(self):
        a,b=self.client(),self.client();a.call(1,struct.pack('<I',0))
        self.assert_errno(errno.ETIMEDOUT,b.call,1,struct.pack('<I',80))
        a.call(2);b.call(1,struct.pack('<I',0))

    def test_config_and_restore(self):
        a=self.client();a.call(1,struct.pack('<I',0))
        self.assert_errno(errno.EINVAL,a.call,7,struct.pack('<IIII',12345,8,0,1))
        a.call(7,struct.pack('<IIII',115200,8,0,1))
        self.assertEqual(struct.unpack('<IIII',a.call(6)),(115200,8,0,1))
        a.call(2)
        self.assertEqual(struct.unpack('<IIII',a.call(6)),(230400,8,0,1))

    def shell(self, command):
        out, err = io.BytesIO(), io.BytesIO()
        code = self.host.execute(command, out, err)
        return code, out.getvalue(), err.getvalue()

    def test_shell_cwd_persists_relative_quotes_and_cd_minus(self):
        self.host.hello()
        target=self.folder/"cwd with ' quote\n"
        target.mkdir();(target/'child').mkdir()
        self.assertEqual(self.shell('cd '+shlex.quote(str(target)))[0],0)
        self.assertEqual(self.shell('pwd')[1],(str(target)+'\n').encode())
        self.assertEqual(self.shell('printf cwd-ok > marker')[0],0)
        self.assertEqual((target/'marker').read_bytes(),b'cwd-ok')
        self.assertEqual(self.shell('cd child')[0],0)
        self.assertEqual(self.shell('pwd')[1],(str(target/'child')+'\n').encode())
        self.assertEqual(self.shell('cd -')[1],(str(target)+'\n').encode())
        self.assertNotEqual(self.shell('cd /hd485-nonexistent-directory')[0],0)
        self.assertEqual(self.shell('pwd')[1],(str(target)+'\n').encode())
        link=self.folder/'logical-link';link.symlink_to(target/'child')
        self.assertEqual(self.shell('cd '+shlex.quote(str(link)))[0],0)
        self.assertEqual(self.shell('pwd')[1],(str(link)+'\n').encode())
        self.assertEqual(self.shell('cd ..')[0],0)
        self.assertEqual(self.shell('pwd')[1],(str(self.folder)+'\n').encode())

    def test_shell_cwd_exit_status_stdout_and_new_session(self):
        self.host.hello();initial=self.shell('pwd')[1]
        target=self.folder/'cwd';target.mkdir()
        code,out,err=self.shell('cd '+shlex.quote(str(target))+"; printf 'hello'; printf 'error' >&2; exit 7")
        self.assertEqual((code,out,err),(7,b'hello',b'error'))
        self.assertEqual(self.shell('pwd')[1],(str(target)+'\n').encode())
        self.assertEqual(self.shell('cd ..; false')[0],1)
        self.assertEqual(self.shell('pwd')[1],(str(self.folder)+'\n').encode())
        self.host.hello()
        self.assertEqual(self.shell('pwd')[1],initial)

    def test_shell_cwd_timeout_cancel_and_preemption(self):
        self.host.hello();initial=self.shell('pwd')[1]
        anchor=self.folder/'anchor';anchor.mkdir()
        other=self.folder/'other';other.mkdir()
        self.assertEqual(self.shell('cd '+shlex.quote(str(anchor)))[0],0)
        self.shell('cd '+shlex.quote(str(other))+'; sleep 5')
        self.assertEqual(self.shell('pwd')[1],(str(anchor)+'\n').encode())
        self.host.call(EXEC,('cd '+shlex.quote(str(other))+'; sleep 5').encode())
        self.host.call(4)
        time.sleep(.7)
        self.assertEqual(self.shell('pwd')[1],(str(anchor)+'\n').encode())
        self.host.call(EXEC,('cd '+shlex.quote(str(other))+'; sleep 5').encode())
        client=self.client();client.call(1,struct.pack('<I',0));client.call(2)
        self.host.hello();time.sleep(.7)
        self.assertEqual(self.shell('pwd')[1],initial)

    def test_shell_output_and_status(self):
        self.host.hello();out,err=io.BytesIO(),io.BytesIO()
        rc=self.host.execute("printf 'hello'; printf 'error' >&2; exit 7",out,err)
        self.assertEqual((rc,out.getvalue(),err.getvalue()),(7,b'hello',b'error'))
        out,err=io.BytesIO(),io.BytesIO()
        self.assertEqual(self.host.execute('@version',out,err),0)
        self.assertEqual(out.getvalue(),b'hd485d 0.4.1\n')

    def test_shell_timeout_and_output_cap(self):
        self.host.hello();self.host.call(EXEC,b'sleep 5')
        time.sleep(1)
        data=self.host.call(POLL,struct.pack('<BI',0,0))
        self.assertEqual(data[0],4)
        out,err=io.BytesIO(),io.BytesIO()
        self.host.execute('yes X | head -c 300000',out,err)
        self.assertEqual(len(out.getvalue()),128*1024)

    def test_duplicate_exec_and_noise(self):
        self.host.hello()
        path=self.folder/'count'
        command=f"printf X >> '{path}'".encode()
        self.host.call(EXEC,command)
        packet=frame(b'H485',EXEC,1,self.host.session,self.host.request,command)
        os.write(self.master,b'noise\x00'+packet+packet)
        time.sleep(.1)
        self.assertEqual(path.read_bytes(),b'X')
        bad=bytearray(frame(b'H485',EXEC,1,self.host.session,self.host.request+1,b'echo bad'));bad[-1]^=1
        os.write(self.master,bad)
        self.host.call(STATUS)

    def test_idle_does_not_parse_business(self):
        a=self.client();a.call(1,struct.pack('<I',0))
        with self.assertRaises(TimeoutError):self.host.hello()
        a.call(2);self.host.hello()
        self.host.call(STATUS)

    def test_preempt_command_and_return_to_idle(self):
        self.host.hello();self.host.call(EXEC,b'sleep 10')
        a=self.client();a.call(1,struct.pack('<I',0));a.call(2)
        self.host.hello();time.sleep(.1)
        state,_,_=struct.unpack('<IiQ',self.host.call(STATUS))
        self.assertEqual(state,3)

    def test_coexist_preserves_rx(self):
        a=self.client();a.call(1,struct.pack('<I',0))
        self.host.hello();self.host.call(STATUS)
        a.call(8)
        self.assertEqual(a.rx,b'')
        out,err=io.BytesIO(),io.BytesIO()
        self.assertEqual(self.host.execute('printf coexist',out,err),0)
        self.assertEqual(out.getvalue(),b'coexist')
        a.call(5,b'business')
        self.assertEqual(self.serial_read(8),b'business')

    @unittest.skipUnless(hasattr(hashlib, "file_digest"), "host file helper requires Python >=3.11")
    def test_file_upload_download_and_overwrite(self):
        self.host.hello();source=self.folder/'source.bin';source.write_bytes(bytes(range(256))*13)
        digest=self.host.put(source,'remote.bin')
        target=self.folder/'download.bin'
        self.assertEqual(self.host.get('remote.bin',target),digest)
        self.assertEqual(target.read_bytes(),source.read_bytes())
        self.assert_errno(errno.EEXIST,self.host.put,source,'remote.bin')
        self.assertEqual(self.host.put(source,'remote.bin',True),digest)

    def test_upload_resume_and_bad_hash(self):
        self.host.hello();content=b'resume'*200;digest=hashlib.sha256(content).digest()
        begin=struct.pack('<Q',len(content))+digest+b'\0resume.bin'
        self.host.call(PUT_BEGIN,begin)
        self.host.call(PUT_DATA,struct.pack('<Q',0)+content[:240])
        a=self.client();a.call(1,struct.pack('<I',0));a.call(2)
        self.host.hello()
        self.assertEqual(struct.unpack('<Q',self.host.call(PUT_BEGIN,begin))[0],240)
        for offset in range(240,len(content),240):self.host.call(PUT_DATA,struct.pack('<Q',offset)+content[offset:offset+240])
        self.host.ready_call(PUT_COMMIT)
        self.assertEqual((self.folder/'files/resume.bin').read_bytes(),content)
        self.host.call(PUT_BEGIN,struct.pack('<Q',1)+b'\0'*32+b'\0bad.bin')
        self.host.call(PUT_DATA,struct.pack('<Q',0)+b'x')
        self.assert_errno(errno.EBADMSG,self.host.ready_call,PUT_COMMIT)
        self.assertFalse((self.folder/'files/bad.bin').exists())

    def test_paths_and_download_change(self):
        self.host.hello()
        for path in ('../escape','/etc/passwd','.hd485-secret'):
            self.assert_errno(errno.EINVAL,self.host.call,GET_BEGIN,path.encode())
        (self.folder/'files/a').mkdir()
        self.assert_errno(errno.EINVAL,self.host.call,GET_BEGIN,b'a//b')
        os.symlink('/etc',self.folder/'files/link')
        with self.assertRaises(OSError):self.host.call(GET_BEGIN,b'link/passwd')
        path=self.folder/'files/data';path.write_bytes(b'abc')
        self.host.ready_call(GET_BEGIN,b'data')
        path.write_bytes(b'changed')
        self.assert_errno(errno.ESTALE,self.host.call,GET_DATA,struct.pack('<Q',0))

    def test_slow_reader_isolated(self):
        fast,slow=self.client(),self.client()
        fast.call(3);slow.call(3)
        payload=bytes(range(256))*8
        for _ in range(256):
            self.serial_write(payload)
            self.assertEqual(fast.received(len(payload)),payload)
        slow.call(8)
        # Drain until the deferred loss event has been delivered.
        for _ in range(4):
            slow.call(8)
            if any(e[0]==3 for e in slow.events):break
        self.assertTrue(any(e[0]==3 and e[1]>0 for e in slow.events))
        self.assertLess(len(slow.rx),len(payload)*256)

    @unittest.skipUnless(hasattr(hashlib, "file_digest"), "host file helper requires Python >=3.11")
    def test_download_resume(self):
        self.host.hello();content=b'partial-download'*130
        remote=self.folder/'files/data';remote.write_bytes(content)
        response=self.host.ready_call(GET_BEGIN,b'data')
        destination=self.folder/'resumed.bin'
        destination.with_name(destination.name+'.hd485-part').write_bytes(content[:240])
        import json
        destination.with_name(destination.name+'.hd485-meta').write_text(json.dumps({
            'remote':'data','size':len(content),'sha256':hashlib.sha256(content).hexdigest()}))
        self.host.get('data',destination)
        self.assertEqual(destination.read_bytes(),content)

    def test_fragmented_request_and_long_tx(self):
        self.host.hello()
        self.host.request+=1
        packet=frame(b'H485',STATUS,1,self.host.session,self.host.request)
        for part in (packet[:3],packet[3:13],packet[13:]):
            self.serial_write(part);time.sleep(.02)
        reply=self.serial_read(44)
        decoded=Parser().feed(reply)
        self.assertEqual(decoded[0][0],STATUS|0x80)
        a=self.client();a.call(1,struct.pack('<I',0))
        payload=bytes(range(256))*32
        a.call(5,payload)
        self.assertEqual(self.serial_read(len(payload)),payload)
        a.call(2)

    def test_rejected_upload_cannot_write(self):
        self.host.hello()
        source=self.folder/'files/existing';source.write_bytes(b'original')
        # A hardlinked staging file must never become an active writable upload.
        name='target';stem=hashlib.sha256(name.encode()).hexdigest()
        os.link(source,self.folder/f'files/.hd485-{stem}.part')
        request=struct.pack('<Q',8)+hashlib.sha256(b'original').digest()+b'\0'+name.encode()
        self.assert_errno(errno.EINVAL,self.host.call,PUT_BEGIN,request)
        self.assert_errno(errno.EINVAL,self.host.call,PUT_DATA,struct.pack('<Q',0)+b'BAD')
        self.assertEqual(source.read_bytes(),b'original')

    def test_serial_disconnect_reports_fault(self):
        a=self.client();a.call(1,struct.pack('<I',0))
        os.close(self.master)
        # Keep teardown's descriptor valid without reconnecting the PTY master.
        self.master=os.open('/dev/null',os.O_RDONLY)
        for _ in range(50):
            status=struct.unpack('<IIIIII',a.call(8))
            if status[4]:break
            time.sleep(.01)
        self.assertNotEqual(status[4],0)
        self.assert_errno(status[4],a.call,5,b'cannot-send')

    def test_sdk_example(self):
        def sdk(args):
            if self.inherited:
                sock=self.available.pop(0)
                try:return subprocess.run([str(BUILD/'hd485-example'),f'fd:{sock.fileno()}']+args,pass_fds=[sock.fileno()],capture_output=True,timeout=3)
                finally:sock.close()
            return subprocess.run([str(BUILD/'hd485-example'),str(self.socket_path)]+args,capture_output=True,timeout=3)
        result=sdk(['status'])
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(b'owner=0',result.stdout)
        received=[]
        reader=threading.Thread(target=lambda:received.append(self.serial_read(3)))
        reader.start()
        result=sdk(['send','SDK'])
        self.assertEqual(result.returncode,0,result.stderr)
        reader.join(3)
        self.assertEqual(received,[b'SDK'])

    def test_filter_maintenance_and_fragmentation(self):
        a,b=self.client(),self.client();a.call(3);b.call(3)
        self.host.hello();self.host.call(STATUS)
        packet=frame(b'H485',STATUS|0x80,1,self.host.session,self.host.request+1)
        for byte in packet:self.serial_write(bytes([byte]));time.sleep(.001)
        time.sleep(.03)
        for c in (a,b):c.call(8);self.assertEqual(c.rx,b'')
        bad=bytearray(packet);bad[-1]^=1
        payload=b'before'+bad+b'after'
        self.serial_write(payload)
        for c in (a,b):self.assertEqual(c.received(len(payload)),payload)

    def test_filter_owner_also_hides_maintenance(self):
        a=self.client();a.call(1,struct.pack('<I',0))
        packet=frame(b'H485',1,2,0,1,b'12345678')
        self.serial_write(b'foo'+packet+b'bar')
        self.assertEqual(a.received(6),b'foobar')
        a.call(8);self.assertEqual(a.rx,b'')

    def test_filter_partial_timeout(self):
        a=self.client();a.call(3);self.serial_write(b'H48')
        self.assertEqual(a.received(3),b'H48')
        self.serial_write(b'plain');self.assertEqual(a.received(5),b'plain')

    def test_qjy_payload_and_raw_lease(self):
        a,b=self.client(),self.client();a.call(3);b.call(3)
        self.assertEqual(struct.unpack('<I',a.call(9))[0],15)
        h485=frame(b'H485',1,1,0,1,b'12345678')
        payload=(h485+b'x')*400
        qjy=b'\xaa\x5a\xee\x01'+struct.pack('<I',len(payload))+payload+b'XX'
        self.serial_write(qjy)
        for c in (a,b):self.assertEqual(c.received(len(qjy)),qjy)
        self.assert_errno(errno.EPERM,a.call,10,struct.pack('<Q',len(payload)))
        a.call(1,struct.pack('<I',0));a.call(10,struct.pack('<Q',len(payload)))
        self.serial_write(payload+h485+b'end')
        for c in (a,b):self.assertEqual(c.received(len(payload)+3),payload+b'end')
        a.call(11);a.call(2)

    def test_raw_lease_release_and_disconnect(self):
        a,b=self.client(),self.client();b.call(3)
        a.call(1,struct.pack('<I',0));a.call(10,struct.pack('<Q',100));a.call(2)
        packet=frame(b'H485',1,2,0,1,b'12345678')
        self.serial_write(packet+b'one');self.assertEqual(b.received(3),b'one')
        a.call(1,struct.pack('<I',0));a.call(10,struct.pack('<Q',100));a.close()
        time.sleep(.05);self.serial_write(packet+b'two');self.assertEqual(b.received(3),b'two')

    def pty_open(self):
        self.host.hello()
        b=self.host.call(48,struct.pack('<HH',24,80))
        self.pty_id=struct.unpack_from('<I',b)[0]
        self.pty_in=self.pty_out=0
        return self.pty_id

    def pty_io(self,data=b''):
        b=self.host.call(49,struct.pack('<IQQ',self.pty_id,self.pty_in,self.pty_out)+data)
        ident,state,code,eof,inp,off,more,length=struct.unpack_from('<IBiBQQBH',b)
        self.assertEqual(ident,self.pty_id)
        self.assertEqual(inp,self.pty_in+len(data))
        self.assertEqual(off,self.pty_out)
        self.assertEqual(len(b),29+length)
        self.pty_in=inp;self.pty_out+=length
        return b[29:],state,eof,code

    def pty_until(self,needle,timeout=4):
        result=b'';end=time.monotonic()+timeout
        while time.monotonic()<end:
            data,_,_,_=self.pty_io();result+=data
            if needle in result:return result
            time.sleep(.02)
        self.fail(repr(result))

    def test_interactive_shell_resize_and_exit(self):
        self.pty_open()
        self.pty_until(b'#' if os.getuid()==0 else b'$')
        self.pty_io(b"stty -echo; export HDPTY=works; cd /; printf '__%s__' $HDPTY\n")
        self.pty_until(b'__works__')
        self.host.call(50,struct.pack('<IHH',self.pty_id,31,97))
        self.pty_io(b"pwd; stty size; printf '__%s__' $HDPTY\n")
        out=self.pty_until(b'__works__')
        self.assertIn(b'31 97',out);self.assertIn(b'/\r\n',out)
        self.pty_io(b'exit 7\n')
        end=time.monotonic()+4
        while time.monotonic()<end:
            _,state,eof,code=self.pty_io()
            if eof:break
            time.sleep(.02)
        self.assertEqual((state,eof,code),(2,1,7))
        self.host.call(EXEC,b'printf ordinary')

    def test_interactive_ctrl_c_close_and_timeout(self):
        self.pty_open();self.pty_until(b'#' if os.getuid()==0 else b'$')
        self.pty_io(b'stty -echo; sleep 30\n');time.sleep(.1)
        self.pty_io(b'\x03');time.sleep(.1)
        self.pty_io(b"printf '__%s__' alive\n");self.pty_until(b'__alive__')
        with self.assertRaises(OSError) as e:self.host.call(EXEC,b'true')
        self.assertEqual(e.exception.errno,errno.EBUSY)
        self.host.call(51,struct.pack('<I',self.pty_id))
        self.host.call(51,struct.pack('<I',self.pty_id))
        # The final output was sent but not yet acknowledged when CLOSE freed
        # the buffer. A delayed IO ACK must not underflow the buffer length.
        self.assert_errno(errno.EINVAL,self.host.call,49,
                          struct.pack('<IQQ',self.pty_id,self.pty_in,self.pty_out))
        self.assertEqual(self.host.call(52,struct.pack('<I',self.pty_id))[4],3)
        time.sleep(.7)
        self.host.call(EXEC,b'true');time.sleep(.1)
        self.pty_open()
        self.pty_until(b'#' if os.getuid()==0 else b'$')
        time.sleep(5.3)
        self.assert_errno(errno.EINVAL,self.host.call,49,
                          struct.pack('<IQQ',self.pty_id,self.pty_in,self.pty_out))
        b=self.host.call(52,struct.pack('<I',self.pty_id))
        self.assertEqual(b[4],4)
        time.sleep(.6)
        self.host.call(EXEC,b'true')

    def test_interactive_replay_and_backpressure(self):
        self.pty_open();self.pty_until(b'#' if os.getuid()==0 else b'$')
        self.pty_io(b'stty -echo\n');time.sleep(.1)
        # Repeat the identical wire request ID: no duplicated command execution.
        payload=struct.pack('<IQQ',self.pty_id,self.pty_in,self.pty_out)+b"printf '__%s__' ONCE\n"
        first=self.host.call(49,payload)
        self.host.request-=1
        second=self.host.call(49,payload)
        self.assertEqual(first,second)
        b=first
        self.pty_in=struct.unpack_from('<Q',b,10)[0]
        self.pty_out+=struct.unpack_from('<H',b,27)[0]
        collected=b[29:]+self.pty_until(b'__ONCE__') if b'__ONCE__' not in b[29:] else b[29:]
        for _ in range(3):collected+=self.pty_io()[0]
        self.assertEqual(collected.count(b'__ONCE__'),1)
        self.pty_io(b"head -c 90000 /dev/zero | tr '\\0' X; printf '__%s__' END\n")
        time.sleep(.3)
        data=self.pty_until(b'__END__',15)
        self.assertEqual(data.count(b'X'),90000)

    def test_interactive_business_preemption(self):
        self.pty_open()
        client=self.client();client.call(1,struct.pack('<I',0))
        time.sleep(.7)
        client.call(2)
        self.host.hello()
        self.host.call(EXEC,b'printf business-released')


if __name__=='__main__':
    print(f'Test artifacts: {ARTIFACTS}',flush=True)
    unittest.main(verbosity=2)
