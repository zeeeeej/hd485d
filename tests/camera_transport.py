#!/usr/bin/env python3
"""Linux PTY integration with the actual camera UART library; no physical devices.
Set HD485_BUILD and HD485_CAMERA_PEER to the two native build outputs.
"""
import binascii,hashlib,os,select,struct,subprocess,time,unittest
from pathlib import Path
from integration import Integration,frame

class CameraTransport(Integration):
    def setUp(self):
        super().setUp()
        self.peerlog=(self.folder/'camera.log').open('wb')
        self.peer=subprocess.Popen([os.environ['HD485_CAMERA_PEER'],str(self.socket_path)],stdout=self.peerlog,stderr=self.peerlog)
        for _ in range(200):
            if b'PEER_READY' in (self.folder/'camera.log').read_bytes():break
            if self.peer.poll() is not None:self.fail((self.folder/'camera.log').read_text())
            time.sleep(.01)
        else:self.fail('camera peer startup timed out')
    def tearDown(self):
        self.peer.terminate()
        try:self.peer.wait(5)
        except subprocess.TimeoutExpired:self.peer.kill();self.peer.wait();self.fail('camera peer shutdown stuck')
        self.peerlog.close()
        log=(self.folder/'camera.log').read_text()
        self.assertNotIn('ERROR: AddressSanitizer',log);self.assertNotIn('runtime error:',log)
        self.assertEqual(self.peer.returncode,0,log)
        super().tearDown()
    def business(self,cmd,data=b'',addr=0xee):
        p=b'\xaa\x5a'+bytes([addr,cmd])+struct.pack('<I',len(data))+data
        return p+struct.pack('<H',binascii.crc_hqx(p,0))
    def reply(self,timeout=5):
        p=self.serial_read(8,timeout);self.assertEqual(p[:2],b'\xaa\x5a')
        n=struct.unpack_from('<I',p,4)[0];rest=self.serial_read(n+2,timeout)
        self.assertEqual(binascii.crc_hqx(p+rest[:-2],0),struct.unpack('<H',rest[-2:])[0])
        return p[3],rest[:-2]
    def test_qjy_camera_echo_and_long_response(self):
        watcher=self.client();watcher.call(3)
        self.host.hello();self.host.call(10);watcher.call(8);self.assertEqual(watcher.rx,b'')
        payload=frame(b'H485',2,1,1,2,b'not a shell command')
        packet=self.business(1,payload)
        self.serial_write(packet);self.assertEqual(self.reply(),(1,payload))
        self.assertEqual(watcher.received(len(packet)),packet)
        self.serial_write(self.business(0x27));cmd,data=self.reply(15)
        self.assertEqual(cmd,0x27);self.assertEqual(data,bytes(range(256))*2048)
        time.sleep(.05);self.assertEqual(struct.unpack('<6I',watcher.call(8))[1],0)
    def test_qjy_camera_payload_after_idle_gap(self):
        watcher=self.client();watcher.call(3)
        payload=frame(b'H485',2,1,1,2,b'payload after a pause')
        packet=self.business(1,payload)
        self.serial_write(packet[:8]);time.sleep(3)
        self.serial_write(packet[8:]);self.assertEqual(self.reply(),(1,payload))
        self.assertEqual(watcher.received(len(packet)),packet)
    def test_qjy_camera_async_and_broadcast(self):
        self.serial_write(self.business(0xd2));time.sleep(.05)
        self.serial_write(self.business(1,b'alive'))
        self.assertEqual(self.reply(),(1,b'alive'));self.assertEqual(self.reply(),(0xd2,b'\0'))
        self.serial_write(self.business(0xcd,b'event',0xff));time.sleep(.1)
        self.assertFalse(select.select([self.master],[],[],.1)[0])
        c=self.client();self.assertEqual(struct.unpack('<6I',c.call(8))[1],0)
    def test_qjy_camera_raw_ota_payload(self):
        payload=(frame(b'H485',1,1,0,1,b'12345678')+bytes(range(256)))*80
        self.serial_write(self.business(0x1b,struct.pack('<I',len(payload))+hashlib.md5(payload).digest()))
        cmd,data=self.reply();self.assertEqual((cmd,data[0]),(0x1b,0))
        self.serial_write(payload);self.assertEqual(self.reply(),(0x1c,b'\0'))
        time.sleep(.1);self.assertIn('RAW_RESULT 1',(self.folder/'camera.log').read_text())
    def test_interactive_qjy_camera_preemption(self):
        self.pty_open();self.pty_until(b'#' if os.getuid()==0 else b'$')
        self.serial_write(self.business(1,b'priority'));self.assertEqual(self.reply(),(1,b'priority'))
        time.sleep(.7);self.host.hello()
        self.pty_id=struct.unpack_from('<I',self.host.call(48,struct.pack('<HH',24,80)))[0]
        self.host.call(51,struct.pack('<I',self.pty_id))
    def test_qjy_camera_reconnect_does_not_replay(self):
        self.serial_write(self.business(0xd2));time.sleep(.04)
        self.serial_write(self.business(1,b'stale-partial')[:9]);time.sleep(.05)
        args=self.process.args;self.process.terminate();self.process.wait(3)
        self.process=subprocess.Popen(args,stdout=self.log,stderr=self.log)
        time.sleep(1.5)
        self.serial_write(self.business(1,b'new connection'))
        self.assertEqual(self.reply(),(1,b'new connection'))
        self.assertFalse(select.select([self.master],[],[],.3)[0])

if __name__=='__main__':
    names=[n for n in CameraTransport.__dict__ if n.startswith('test_')]
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(CameraTransport(n) for n in names))
    raise SystemExit(not result.wasSuccessful())
