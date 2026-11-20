#!/usr/bin/env python3
"""Whole-frame TX, bounded producer credit, timeout and owner recovery regressions."""
import errno
import struct
import threading
import time
import unittest
from integration import Integration,frame

class StreamTX(Integration):
    def owner(self):
        c=self.client();c.call(1,struct.pack('<I',0));return c
    def done(self,c,ticket,timeout=4):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            c.call(8)
            hits=[e for e in c.events if e[0]==2 and e[1]==ticket]
            if hits:
                self.assertEqual(len(hits),1)
                return hits[0][2]
            time.sleep(.01)
        self.fail('missing final TX_DONE')
    def test_stream_boundaries_and_backpressure(self):
        c=self.owner();watcher=self.client()
        self.assertTrue(struct.unpack('<I',c.call(9))[0]&8)
        for size in (8182,8183,8192,8193,65536,524298):
            data=bytes((i*131+17)&255 for i in range(size))
            c.call(12,struct.pack('<I',size));ticket=c.id
            errors=[]
            def send():
                try:
                    for i in range(0,size,8192):c.call(13,data[i:i+8192])
                except Exception as e:errors.append(e)
            thread=threading.Thread(target=send);thread.start()
            if size>65536:
                time.sleep(.2)
                self.assertTrue(thread.is_alive(),'producer must wait for bounded credit')
                self.assertFalse(any(e[0]==2 and e[1]==ticket for e in c.events))
                start=time.monotonic();watcher.call(8)
                self.assertLess(time.monotonic()-start,.5,'blocked producer must not block daemon')
            received=self.serial_read(size,8)
            thread.join(5);self.assertFalse(thread.is_alive());self.assertFalse(errors,errors)
            self.assertEqual(received,data);self.assertEqual(self.done(c,ticket),0)
        c.call(2)
    def test_stream_abort_timeout_and_legacy(self):
        c=self.owner()
        for explicit in (True,False):
            c.call(12,struct.pack('<I',20000));ticket=c.id
            c.call(13,b'partial');self.assertEqual(self.serial_read(7),b'partial')
            if explicit:c.call(14)
            self.assertEqual(self.done(c,ticket),errno.ECANCELED if explicit else errno.ETIMEDOUT)
            self.assertEqual(struct.unpack('<6I',c.call(8))[4],0)
            c.call(5,b'legacy');old=c.id
            self.assertEqual(self.serial_read(6),b'legacy');self.assertEqual(self.done(c,old),0)
        c.call(2)
    def test_stream_validation(self):
        c=self.owner()
        for total in (0,16*1024*1024+1):
            with self.assertRaises(OSError):c.call(12,struct.pack('<I',total))
        outsider=self.client()
        for op,data in ((12,struct.pack('<I',10)),(13,b'x'),(14,b'')):
            with self.assertRaises(OSError) as e:outsider.call(op,data)
            self.assertEqual(e.exception.errno,errno.EPERM)
        c.call(12,struct.pack('<I',10));ticket=c.id
        with self.assertRaises(OSError) as e:c.call(2)
        self.assertEqual(e.exception.errno,errno.EBUSY)
        with self.assertRaises(OSError) as e:c.call(13,b'x'*11)
        self.assertEqual(e.exception.errno,errno.EMSGSIZE)
        with self.assertRaises(OSError) as e:c.call(5,b'legacy')
        self.assertEqual(e.exception.errno,errno.EBUSY)
        c.call(13,b'0123456789');self.assertEqual(self.serial_read(10),b'0123456789')
        self.assertEqual(self.done(c,ticket),0);c.call(2)
    def test_stream_disconnect_releases_owner(self):
        c=self.owner();c.call(12,struct.pack('<I',524288))
        # Pipeline enough data to saturate the PTY and producer ring, then disappear.
        for _ in range(10):
            c.id+=1;c.sock.sendall(frame(b'I485',13,0,0,c.id,b'x'*8192))
        c.close();self.clients.remove(c)
        other=self.client();other.call(1,struct.pack('<I',3000),timeout=4)
        self.assertEqual(struct.unpack('<6I',other.call(8))[4],0)
        other.call(2)

if __name__=='__main__':
    names=[n for n in StreamTX.__dict__ if n.startswith('test_')]
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(StreamTX(n) for n in names))
    raise SystemExit(not result.wasSuccessful())
