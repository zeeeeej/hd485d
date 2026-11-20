#!/usr/bin/env python3
"""Explicit real-board PTY audit. Writes only the supplied board test directory.
Run after checking UART ownership and obtaining deployment approval.
"""
import argparse, json, os, queue, re, select, shlex, struct, threading, time, uuid
from pathlib import Path
from hd485ctl import Host, open_serial

class Console:
    def __init__(self, device):
        self.fd=open_serial(device,115200)
    def command(self, command, timeout=8):
        token='HDTEST_'+uuid.uuid4().hex
        packet=(command+"; printf '\\n"+token+":%s\\n' \"$?\"\r").encode()
        for pos in range(0,len(packet),64):
            data=memoryview(packet[pos:pos+64])
            while data:
                if select.select([], [self.fd], [], 1)[1]:
                    n=os.write(self.fd,data);data=data[n:]
            time.sleep(.01)
        raw=bytearray();deadline=time.monotonic()+timeout
        pattern=re.compile(rb'\n'+token.encode()+rb':(\d+)\r?\n')
        while time.monotonic()<deadline:
            if select.select([self.fd],[],[],.1)[0]:raw.extend(os.read(self.fd,65536))
            match=pattern.search(raw)
            if match:
                if int(match[1]):raise RuntimeError(raw.decode(errors='replace'))
                return raw[:match.start()].decode(errors='replace').replace('\r','')
        raise TimeoutError(raw.decode(errors='replace'))
    def close(self):
        import fcntl,termios
        fcntl.ioctl(self.fd,termios.TIOCNXCL);os.close(self.fd)

class SlowHost(Host):
    def call(self,op,data=b''):
        time.sleep(.06)
        return super().call(op,data)

class Terminal:
    def __init__(self,host):
        self.host=host;self.output=bytearray();self.queue=queue.Queue();self.stop=threading.Event();self.error=None
        self.id=struct.unpack_from('<I',host.call(48,struct.pack('<HH',24,80)))[0]
        self.thread=threading.Thread(target=self.pump);self.thread.start()
    def pump(self):
        inp=out=0;pending=b''
        try:
            while not self.stop.is_set():
                if not pending:
                    try:pending=self.queue.get_nowait()
                    except queue.Empty:pass
                chunk=pending[:200]
                raw=self.host.call(49,struct.pack('<IQQ',self.id,inp,out)+chunk)
                ident,state,code,eof,next_in,offset,more,size=struct.unpack_from('<IBiBQQBH',raw)
                assert ident==self.id and next_in==inp+len(chunk) and offset==out
                pending=pending[len(chunk):];inp=next_in;out+=size
                self.output.extend(raw[29:])
                if eof:return
                if not more and not size and not pending:time.sleep(.05)
            self.host.call(51,struct.pack('<I',self.id))
        except BaseException as e:self.error=e
    def send(self,text):self.queue.put(text.encode() if isinstance(text,str) else text)
    def until(self,needle,timeout=15,start=0):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if self.error:raise self.error
            if needle in self.output[start:]:return bytes(self.output[start:])
            time.sleep(.05)
        raise TimeoutError(bytes(self.output[-1000:]))
    def command(self,text,tag):
        start=len(self.output);self.send(text+"; printf '__%s__\\n' "+shlex.quote(tag)+"\n")
        return self.until(('__'+tag+'__').encode(),start=start)
    def close(self):
        self.stop.set();self.thread.join(8)
        if self.thread.is_alive():raise TimeoutError('PTY pump stuck')
        if self.error:raise self.error
        time.sleep(.65)

def snapshot(console,pid):
    # Only daemon and terminal jobs in this daemon's descendant tree. State is
    # parsed on the host, avoiding reliance on smaps_rollup in Linux 4.4.
    cmd=f'''echo METRIC_BEGIN; awk '/^(MemTotal|MemFree|MemAvailable):/' /proc/meminfo; cat /proc/sys/kernel/pty/nr; ls /proc/{pid}/fd | wc -l; for p in {pid} $(awk '$4=={pid} {{print $1}}' /proc/[0-9]*/stat); do echo PROC=$p; awk '/^(Name|VmRSS|VmHWM|RssAnon):/' /proc/$p/status 2>/dev/null; awk '/^Pss:/ {{s+=$2}} END {{print "Pss:",s,"kB"}}' /proc/$p/smaps 2>/dev/null; for c in $(awk -v parent="$p" '$4==parent {{print $1}}' /proc/[0-9]*/stat); do echo CHILD=$c; awk '/^(Name|VmRSS|VmHWM|RssAnon):/' /proc/$c/status 2>/dev/null; awk '/^Pss:/ {{s+=$2}} END {{print "Pss:",s,"kB"}}' /proc/$c/smaps 2>/dev/null; done; done; echo METRIC_END'''
    try:text=console.command(cmd,8)
    except TimeoutError:
        # Read-only sampling can be repeated if the diagnostic UART drops output.
        text=console.command(cmd,8)
    text=text.split('\nMETRIC_BEGIN\n')[-1].split('\nMETRIC_END')[0]
    metrics={'time':time.time(),'raw':text,'processes':{}}
    current=None;counts=[]
    for line in text.splitlines():
        if line.startswith(('PROC=','CHILD=')):
            current=line.split('=')[1];metrics['processes'].setdefault(current,{})
        elif current:
            if line.startswith('Name:'):metrics['processes'][current]['name']=line.split(':',1)[1].strip()
            elif line.startswith(('VmRSS:','VmHWM:','RssAnon:','Pss:')):
                parts=line.split()
                if len(parts)>1 and parts[1].isdigit():metrics['processes'][current][parts[0][:-1]]=int(parts[1])
        elif line.isdigit():counts.append(int(line))
        elif line.startswith(('MemAvailable:','MemFree:','MemTotal:')):metrics[line.split(':')[0]]=int(line.split()[1])
    if len(counts)>=2:metrics['pty_count'],metrics['fd_count']=counts[:2]
    return metrics

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--console',required=True);ap.add_argument('--port',required=True)
    ap.add_argument('--baud',type=int,required=True);ap.add_argument('--pid',type=int,required=True)
    ap.add_argument('--remote-dir',required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--seconds',type=int,default=60);ap.add_argument('--baseline-only',action='store_true');ap.add_argument('--memory-only',action='store_true');ap.add_argument('--skip-initial',action='store_true')
    args=ap.parse_args();c=Console(args.console);fd=None;t=None
    report={'board_directory':args.remote_dir,'pid':args.pid,'seconds':args.seconds,'stages':{},'checks':[]}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    def save():args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2))
    def stage(name):
        if args.memory_only and name in ('no_session','closed'):return
        if args.skip_initial and name=='no_session':return
        print('STAGE',name,flush=True);end=time.monotonic()+args.seconds;values=[]
        while time.monotonic()<end:
            values.append(snapshot(c,args.pid))
            if t and t.error:raise t.error
            time.sleep(1)
        report['stages'][name]=values;save()
    try:
        stage('baseline' if args.baseline_only else 'no_session')
        if args.baseline_only:return
        fd=open_serial(args.port,args.baud);h=SlowHost(fd,timeout=1.5,retries=3);h.hello()
        t=Terminal(h)
        t.command('stty -echo; cd '+shlex.quote(args.remote_dir)+'; export HDPTY=kept','READY')
        assert b'kept' in t.command('printf "%s\\n" "$HDPTY"; pwd','STATE')
        stage('shell')
        t.send('top -d 2\n');time.sleep(2);stage('top');t.send('q');time.sleep(1)
        t.command('printf ok','TOP_EXIT')
        filename=shlex.quote(args.remote_dir+'/pty-test.txt')
        make="awk 'BEGIN {for(i=0;i<1024;i++) print \""+('x'*63)+"\"}' > "+filename
        t.command(make,'FILE_READY')
        t.send('vi '+filename+'\n');time.sleep(2);stage('vi_64k')
        t.send(b'gg0iHDPTY_SAVED');time.sleep(.4);t.send(b'\x1b');time.sleep(.7);t.send(b':wq\n');time.sleep(1)
        result=t.command('head -c 32 '+filename,'VI_EXIT');assert b'HDPTY_SAVED' in result
        report['checks'].append('vi save/readback, top q, shell directory/environment')
        t.send('sleep 30\n');time.sleep(.3);t.send(b'\x03');time.sleep(.3)
        t.command('printf alive','CTRL_C');report['checks'].append('Ctrl+C foreground sleep')
        t.close();report['terminal_output']=bytes(t.output).decode(errors='replace');t=None
        stage('closed')
        if args.memory_only:
            report['after_memory']=snapshot(c,args.pid);save();print('MEMORY_OK',flush=True);return
        for i in range(20):
            t=Terminal(h);t.command('stty -echo','CYCLE_READY');t.close();t=None
            print('CYCLE',i+1,flush=True)
        report['after_20_cycles']=snapshot(c,args.pid)
        assert report['after_20_cycles'].get('pty_count')==0
        assert report['after_20_cycles'].get('fd_count')==report['stages']['closed'][-1].get('fd_count')
        report['checks'].append('20 open/close cycles')
        # Leave a shell without polls, then verify the lease and cleanup.
        ident=struct.unpack_from('<I',h.call(48,struct.pack('<HH',24,80)))[0]
        time.sleep(21)
        raw=h.call(52,struct.pack('<I',ident));assert raw[4]==4 and raw[9]==1
        report['after_timeout']=snapshot(c,args.pid)
        report['checks'].append('21s link inactivity: state timeout and EOF')
        save();print('AUDIT_OK',args.output,flush=True)
    except BaseException as e:
        report['error']=repr(e);save();raise
    finally:
        if t:
            try:t.close()
            except Exception:pass
        if fd is not None:
            import fcntl,termios
            fcntl.ioctl(fd,termios.TIOCNXCL);os.close(fd)
        c.close()
if __name__=='__main__':main()
