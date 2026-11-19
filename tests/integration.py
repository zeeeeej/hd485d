#!/usr/bin/env python3
"""End-to-end PTY tests. Retains logs/files as build/test-artifacts evidence."""
import errno
import hashlib
import io
import os
from pathlib import Path
import pty
import select
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
            if (self.inherited and 'hd485d 0.1.1' in (self.folder/'daemon.log').read_text()) or (not self.inherited and self.socket_path.exists()):
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

    def test_shell_output_and_status(self):
        self.host.hello();out,err=io.BytesIO(),io.BytesIO()
        rc=self.host.execute("printf 'hello'; printf 'error' >&2; exit 7",out,err)
        self.assertEqual((rc,out.getvalue(),err.getvalue()),(7,b'hello',b'error'))
        out,err=io.BytesIO(),io.BytesIO()
        self.assertEqual(self.host.execute('@version',out,err),0)
        self.assertEqual(out.getvalue(),b'hd485d 0.1.1\n')

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
        self.assertIn(b'H485',a.rx)
        out,err=io.BytesIO(),io.BytesIO()
        self.assertEqual(self.host.execute('printf coexist',out,err),0)
        self.assertEqual(out.getvalue(),b'coexist')
        a.call(5,b'business')
        self.assertEqual(self.serial_read(8),b'business')

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


if __name__=='__main__':
    print(f'Test artifacts: {ARTIFACTS}',flush=True)
    unittest.main(verbosity=2)
