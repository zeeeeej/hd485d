#!/usr/bin/env python3
"""Explicit real RV1106 protocol audit. Read-only business queries by default.
--maintenance writes only its named fixture under the configured H485 root.
--door-cycle explicitly sends one open/close cycle, invoking normal camera business.
"""
import argparse,binascii,fcntl,hashlib,io,json,os,select,struct,tempfile,termios,time
from pathlib import Path
from hd485ctl import open_serial
from pty_board_audit import SlowHost

def packet(cmd,data=b'',addr=0xee):
    p=b'\xaa\x5a'+bytes([addr,cmd])+struct.pack('<I',len(data))+data
    return p+struct.pack('<H',binascii.crc_hqx(p,0))

class Business:
    def __init__(self,fd):self.fd=fd;self.last=0;self.sent=bytearray()
    def send(self,cmd,data=b'',addr=0xee):
        time.sleep(max(0,self.last+.06-time.monotonic()))
        p=packet(cmd,data,addr);self.sent.extend(p);view=memoryview(p)
        start=time.monotonic()
        while view:
            if select.select([],[self.fd],[],2)[1]:view=view[os.write(self.fd,view):]
            else:raise TimeoutError('business TX')
        self.last=time.monotonic();return start
    def read(self,timeout=5):
        deadline=time.monotonic()+timeout
        def exact(n):
            out=bytearray()
            while len(out)<n:
                left=deadline-time.monotonic()
                if left<=0 or not select.select([self.fd],[],[],left)[0]:raise TimeoutError('business RX')
                out.extend(os.read(self.fd,n-len(out)))
            return bytes(out)
        h=exact(8);assert h[:2]==b'\xaa\x5a',repr(h)
        n=struct.unpack_from('<I',h,4)[0];assert n<=512*1024
        rest=exact(n+2);assert binascii.crc_hqx(h+rest[:-2],0)==struct.unpack('<H',rest[-2:])[0]
        self.last=time.monotonic();return h[3],rest[:-2]
    def call(self,cmd,data=b''):
        start=self.send(cmd,data);op,out=self.read();assert op==cmd,(op,cmd)
        return out,(time.monotonic()-start)*1000

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--baud',type=int,default=230400)
    ap.add_argument('--label',required=True);ap.add_argument('--count',type=int,default=500)
    ap.add_argument('--output',type=Path,required=True);ap.add_argument('--maintenance',action='store_true');ap.add_argument('--door-cycle',action='store_true')
    a=ap.parse_args();fd=open_serial(a.port,a.baud);b=Business(fd);report={'label':a.label,'queries':{},'heartbeat_ms':[],'checks':[]}
    def save():a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2))
    try:
        for i in range(a.count):
            out,ms=b.call(1,bytes([i%256]));assert out==bytes([(i+1)%256]);report['heartbeat_ms'].append(ms)
        times=sorted(report['heartbeat_ms']);report['latency_ms']={k:times[min(len(times)-1,int(len(times)*q))] for k,q in [('p50',.5),('p95',.95),('p99',.99),('max',1)]}
        print('HEARTBEAT',a.label,a.count,report['latency_ms'],flush=True);save()
        for cmd,data,name in [(2,b'\x01','address'),(2,b'\x02','sdk'),(2,b'\x04','baud'),(2,b'\x15','system'),(0xc2,b'\xd4','app'),(0xc2,b'\x06','summary'),(0x27,b'\x01\x01','static_records'),(0x37,b'\x01\x01','dynamic_records')]:
            out,ms=b.call(cmd,data)
            report['queries'][name]={'length':len(out),'sha256':hashlib.sha256(out).hexdigest(),'ms':ms}
            if name in ('address','sdk','baud','system','app'):report['queries'][name]['hex']=out.hex()
        report['checks'].append('CRC-valid attributes and paged static/dynamic records')
        b.send(0xd2);time.sleep(.08);b.send(1,b'\x53')
        answers={}
        for _ in range(2):cmd,data=b.read(6);answers[cmd]=data
        assert answers[1]==b'\x54' and 0xd2 in answers
        report['checks'].append('async OSS probe with concurrent heartbeat')
        if a.door_cycle:
            action=struct.pack('<I',int(time.time()))+b'\x71'
            b.send(0xcd,b'\x01'+action,0xff);time.sleep(3)
            out,_=b.call(1,b'\x65');assert out==b'\x66'
            b.send(0xcd,b'\x00'+action,0xff);time.sleep(3)
            out,_=b.call(1,b'\x67');assert out==b'\x68'
            report['checks'].append('one door open/close broadcast; heartbeat remains responsive')
        if a.maintenance:
            out,ms=b.call(0xaa,b'\xee'+packet(2,b'\x02'))
            inner=out[1:]
            assert out[0]==0xee and inner[:2]==b'\xaa\x5a' and inner[3]==2
            assert binascii.crc_hqx(inner[:-2],0)==struct.unpack('<H',inner[-2:])[0]
            report['forward_ms']=ms
            report['checks'].append('ttyS4 forwarded SDK query with nested CRC')
            h=SlowHost(fd,timeout=1.5,retries=2);h.hello()
            out=io.BytesIO();err=io.BytesIO();assert h.execute('@version',out,err)==0 and b'0.4.0' in out.getvalue()
            ident=struct.unpack_from('<I',h.call(48,struct.pack('<HH',24,80)))[0]
            raw=h.call(49,struct.pack('<IQQ',ident,0,0)+b'printf HD485PTY_OK\n')
            time.sleep(.1);out,_=b.call(1,b'\x78');assert out==b'\x79'
            try:h.call(52,struct.pack('<I',ident));raise AssertionError('old maintenance session survived business')
            except TimeoutError:pass
            h.hello();time.sleep(.7)
            out=io.BytesIO();assert h.execute('@version',out,io.BytesIO())==0
            assert len(h.call(32))>=16
            h.call(33,b'/userdata/tmp_var/hd-rs485-drive-0.4.0')
            assert len(h.call(34,struct.pack('<Q',0)))>=9
            h.call(35)
            tmp=Path.home()/'Documents/tmp/hd-rs485-drive';tmp.mkdir(parents=True,exist_ok=True)
            with tempfile.TemporaryDirectory(dir=tmp,prefix='live-business-') as d:
                src=Path(d)/'input';dst=Path(d)/'output';src.write_bytes(bytes(range(256))*4)
                name='rv1106-integration-0.4.0.bin';h.put(src,name,True);h.get(name,dst);assert src.read_bytes()==dst.read_bytes()
            report['checks'].append('maintenance EXEC, PTY preemption, explicit reconnect, filesystem browse, 1KiB file roundtrip')
            # Give the final H485 response its normal RS485 DE turnaround.
            b.last=time.monotonic()
            out,_=b.call(1,b'\x7b');assert out==b'\x7c'
        report['business_tx_bytes']=len(b.sent);report['business_tx_md5']=hashlib.md5(b.sent).hexdigest()
        report['ok']=True;save();print('BUSINESS_AUDIT_OK',a.output,flush=True)
    except BaseException as e:report['error']=repr(e);save();raise
    finally:fcntl.ioctl(fd,termios.TIOCNXCL);os.close(fd)
if __name__=='__main__':main()
