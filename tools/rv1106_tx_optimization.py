#!/usr/bin/env python3
"""RV1106 TX optimization audit 0.1.0; no OTA, preserves OEM and init scripts.
Requires the explicitly deployed optimization bundle and previous AB peer/sampler.
Python >=3.11, macOS serial IOSSDATALAT=0 for all comparison runs.
"""
import argparse,binascii,fcntl,hashlib,io,json,os,re,select,shlex,struct,sys,time,termios
from pathlib import Path
from pty_board_audit import Console,SlowHost,Terminal,snapshot
from rv1106_business_audit import Business,packet,open_serial
VERSION='0.1.0'
Q=shlex.quote
BASE='/userdata/tmp_var/hd-rs485-drive-0.4.0'
AB='/userdata/tmp_var/hd485-ab-0.4.51d-0.4.52d'
class FilesystemHost(SlowHost):
 def call(self,op,data=b''):return super().call({5:37,8:38}.get(op,op),data)
class Runner:
 def __init__(self):
  self.c=Console(a.console);self.fd=None;self.b=None;self.sampler=None;self.tag=None;self.samples=[]
  self.report={'tool':VERSION,'action':a.action,'python':sys.version,'interpreter':sys.executable,'runs':[]};self.changed=False
 def q(self,s,timeout=12):
  result=self.c.command(s,timeout)
  with (a.output/'console-operations.log').open('a') as f:f.write(result+'\n')
  return result
 def save(self):
  tmp=a.output/(a.action+'.json.tmp');tmp.write_text(json.dumps(self.report,indent=2));tmp.replace(a.output/(a.action+'.json'))
 def openbus(self):
  if self.fd is None:self.fd=open_serial(a.port,230400);self.b=Business(self.fd)
 def closebus(self):
  if self.fd is not None:fcntl.ioctl(self.fd,termios.TIOCNXCL);os.close(self.fd);self.fd=None;self.b=None
 def pids(self,name):
  s=self.q('for f in /proc/[0-9]*/exe; do x=$(readlink "$f"); case "$x" in */'+name+') p=${f#/proc/}; echo AB_PROCESS ${p%/exe} "$x";; esac; done')
  return [(int(n),path) for n,path in re.findall(r'^AB_PROCESS (\d+) (.+)$',s,re.M)]
 def stop(self,name):
  found=self.pids(name)
  for pid,exe in found:
   self.q(f'test "$(readlink /proc/{pid}/exe)" = {Q(exe)} && kill -TERM {pid}')
  until=time.monotonic()+20
  while found and time.monotonic()<until:
   time.sleep(.5);found=self.pids(name)
  if found:raise RuntimeError('Normal shutdown failed; no SIGKILL: '+name)
 def launch(self,cmd,label):
  self.q(f'(trap "" HUP; {cmd}) </dev/null >{Q(R+"/"+label+".log")} 2>&1 & echo $! >{Q(R+"/"+label+".pid")}')
  s=self.q(f'printf "AB_PID "; cat {Q(R+"/"+label+".pid")}')
  return int(re.search(r'^AB_PID (\d+)$',s,re.M)[1])
 def stage(self,name):self.q('printf %s '+Q(name)+' >'+Q(R+'/stage'));print('STAGE',self.tag,name,flush=True)
 def sample_start(self,tag,pids):
  self.tag=tag;self.stage('warmup');self.sampler=self.launch('exec '+Q(R+'/ab-sampler')+' '+Q(R+'/stage')+' '+' '.join(map(str,pids)),tag+'-samples')
 def sample_stop(self):
  if self.sampler is None:return
  self.q(f'kill -TERM {self.sampler} 2>/dev/null; true');self.sampler=None;time.sleep(.2)
  self.collect_samples(self.tag)
 def collect_samples(self,tag):
  path=R+'/'+tag+'-samples.log';raw=[];retries=0
  count_text=self.q('wc -l '+Q(path));count=int(re.search(r'^\s*(\d+) '+re.escape(path)+r'$',count_text,re.M)[1])
  for start in range(1,count+1,5):
   expected=min(5,count-start+1)
   for attempt in range(4):
    s=self.q(f"sed -n '{start},{start+4}p' {Q(path)}",10)
    lines=[line for line in s.splitlines() if line.startswith('{"t":')]
    try:
     assert len(lines)==expected
     for line in lines:json.loads(line)
     break
    except (AssertionError,ValueError):
     retries+=1
     if attempt==3:raise
   raw.extend(lines)
  content='\n'.join(raw)+'\n';digest=hashlib.md5(content.encode()).hexdigest()
  remote_hash=self.q('md5sum '+Q(path));assert re.search(r'^'+digest+r'\s+'+re.escape(path)+r'$',remote_hash,re.M),'sampler file hash mismatch'
  (a.output/(tag+'-samples.jsonl')).write_text(content)
  self.report.setdefault('collection_retries',{})[tag]=retries
  print('SAMPLES',tag,len(raw),'retries',retries,flush=True)
 def wait(self,seconds):
  until=time.monotonic()+seconds;observe=time.monotonic()+30
  while time.monotonic()<until:
   time.sleep(min(1,max(0,until-time.monotonic())))
   if self.sampler and time.monotonic()>=observe:
    observe=time.monotonic()+30
    try:
     text=self.q('tail -n 1 '+Q(R+'/'+self.tag+'-samples.log'))
     row=json.loads(next(line for line in text.splitlines() if line.startswith('{"t":')))
     print('OBSERVE',self.tag,row['stage'],'rss_kib',sum(p['rss'] for p in row['processes']),flush=True)
    except Exception as e:print('OBSERVE_RETRY_AT_NEXT_INTERVAL',type(e).__name__,flush=True)
 def heartbeat(self,record,index):
  out,ms=self.b.call(1,bytes([index%256]));assert out==bytes([(index+1)%256]);record['heartbeats_ms'].append(ms)
 def daemon(self,kind,trace=False):
  self.stop('hd485d')
  exe=BASE+'/rv1106/bin/hd485d' if kind=='old' else R+'/rv1106/bin/hd485d'
  if kind=='trace-old':exe=R+'/trace-old/hd485d'
  prefix='export HD485_TX_TRACE=1; ' if trace else 'unset HD485_TX_TRACE; '
  pid=self.launch(prefix+'exec '+Q(exe)+' --device /dev/ttyS3 --socket /run/hd485.sock --root /userdata/hd-rs485/files --baud 230400 --de gpio --gpio 35 --mode idle --rx-profile qjy --allow-shell --allow-pty --fs-root /','daemon-'+kind)
  time.sleep(.5)
  s=self.q(Q(R+'/rv1106/bin/hd485-example')+' /run/hd485.sock status')
  assert 'mode=0' in s and 'error=0' in s
  return pid
 def activate(self,kind,tag,peer=False,trace=False,normal=False):
  self.changed=True;self.closebus()
  for name in ('hd_camera_app2','ab-peer-a','ab-peer-b'):self.stop(name)
  daemon=self.daemon(kind,trace)
  old=kind in ('old','trace-old');lib='/oem/usr/lib:/usr/lib' if old else R+'/lib:/oem/usr/lib:/usr/lib'
  env='export LD_LIBRARY_PATH='+Q(lib)+'; '+('export HD485_TX_TRACE=1; ' if trace else 'unset HD485_TX_TRACE; ')
  if peer:
   cmd=env+'exec '+Q(AB+'/ab-peer-b')+' /run/hd485.sock';ready='AB_PEER_READY'
  else:
   exe='/oem/usr/bin/hd_camera_app2' if old else R+'/hd_camera_app2'
   shim='unset LD_PRELOAD; ' if normal else 'export LD_PRELOAD='+Q(BASE+'/test-only/no_auto_update.so')+'; '
   cmd='cd /userdata/hd_camera_app2 || exit 1; '+env+shim+'exec '+Q(exe);ready='Step6 business components ready'
  pid=self.launch(cmd,tag)
  for _ in range(60):
   s=self.q('grep -q '+Q(ready)+' '+Q(R+'/'+tag+'.log')+' && echo OPT_READY || true')
   if '\nOPT_READY\n' in s:break
   time.sleep(1)
  else:raise RuntimeError('startup failed '+tag)
  self.openbus();fcntl.ioctl(self.fd,0x80085400,struct.pack('L',0))
  record={'tag':tag,'kind':kind,'pid':pid,'daemon_pid':daemon,'heartbeats_ms':[],'transfers':[],'priming':[]}
  # Fixed 5 priming transactions after configuring the USB driver; record, do not pool.
  for i in range(5):
   if peer:
    out,ms=self.b.call(0xe0);assert out==b'HD485-AB-0.1.0'
   else:
    out,ms=self.b.call(0xc2,b'\xd4');assert out==b'\xd4\0'+(b'0.4.52.d' if old else b'0.4.53.d')
   record['priming'].append(ms)
  self.report['runs'].append(record);self.save()
  return record
 def transfer(self,record,direction,size,repeat):
  data=bytes((i*131+17)&255 for i in range(size));cmd=0xe1 if direction=='up' else 0xe2
  start=self.b.send(cmd,data if direction=='up' else struct.pack('<I',size))
  timeout=max(5,2*(size+20)*10/230400+5)
  op,out=self.b.read(timeout);elapsed=time.monotonic()-start;assert op==cmd
  assert out==(struct.pack('<II',size,binascii.crc32(data)&0xffffffff) if direction=='up' else data)
  result={'direction':direction,'bytes':size,'repeat':repeat,'seconds':elapsed,'kib_s':size/1024/elapsed}
  record['transfers'].append(result);self.save();print('TRANSFER',record['kind'],direction,size,repeat,round(elapsed,4),flush=True)
 def peer(self,kinds=('old','new')):
  for kind in kinds:
   r=self.activate(kind,'peer-'+kind,peer=True)
   self.sample_start('wire-'+kind,[r['pid'],r['daemon_pid']]);self.stage('wire')
   for size in (256,4096,8182,8183,65536,524288):
    for direction in ('up','down'):
     for repeat in range(3 if size==524288 else 1):self.transfer(r,direction,size,repeat)
   self.sample_stop()
 def app(self,kinds=('old','new')):
  for kind in kinds:
   r=self.activate(kind,'app-'+kind);self.wait(20)
   self.sample_start('app-'+kind,[r['pid'],r['daemon_pid']]);self.stage('idle');self.wait(60)
   self.stage('heartbeat')
   for i in range(500):self.heartbeat(r,i)
   self.stage('business');r['queries']=[]
   for repeat in range(5):
    for cmd,data in [(2,b'\1'),(2,b'\2'),(2,b'\4'),(2,b'\x15'),(0xc2,b'\6'),(0x27,b'\1\1'),(0x37,b'\1\1')]:
     out,ms=self.b.call(cmd,data);r['queries'].append({'cmd':cmd,'length':len(out),'ms':ms,'sha256':hashlib.sha256(out).hexdigest()})
   for i in range(3):
    self.b.send(0xd2);time.sleep(.08);self.b.send(1,b'\x53');ans={}
    for _ in range(2):op,data=self.b.read(6);ans[op]=data
    assert ans[1]==b'\x54' and 0xd2 in ans
   self.stage('door');action=struct.pack('<I',int(time.time()))+b'\x71'
   self.b.send(0xcd,b'\1'+action,0xff);self.wait(5)
   out,_=self.b.call(1,b'\x65');assert out==b'\x66'
   self.b.send(0xcd,b'\0'+action,0xff);self.wait(8)
   out,_=self.b.call(1,b'\x67');assert out==b'\x68'
   self.sample_stop();self.save()
   r['detection']=self.q("grep -E 'Detection thread stopped \\(frames=|Frame queue final: captured=' "+Q(R+'/app-'+kind+'.log')+' || true')
   r['faults']=self.q("grep -F '[hd485 transport] link reset:' "+Q(R+'/app-'+kind+'.log')+' || true');self.save()
   print('APP_OK',kind,flush=True)
 def trace(self):
  for kind in ('trace-old','new'):
   r=self.activate(kind,'trace-peer-'+kind,peer=True,trace=True)
   self.transfer(r,'down',524288,0)
   self.closebus();self.stop('ab-peer-b');self.openbus()
   h=FilesystemHost(self.fd,timeout=1.5,retries=3);h.hello()
   # Fetch immutable peer log and a snapshot of the daemon log (trace itself grows on RX replies).
   self.q('cp '+Q(R+'/daemon-'+kind+'.log')+' '+Q(R+'/trace-'+kind+'.log'))
   h.get(R+'/trace-'+kind+'.log',a.output/('trace-'+kind+'.log'),True)
   h.get(R+'/trace-peer-'+kind+'.log',a.output/('trace-app-'+kind+'.log'),True)
   self.b.last=time.monotonic();self.save()
 def maintenance(self):
  self.openbus()
  monitor=self.launch('exec '+Q(R+'/rv1106/bin/hd485-example')+' /run/hd485.sock listen >'+Q(R+'/maintenance-rx.bin'),'maintenance-listener')
  before=len(self.b.sent)
  try:
   self.openbus();h=FilesystemHost(self.fd,timeout=1.5,retries=2);h.hello()
   out=io.BytesIO();assert h.execute('@version',out,io.BytesIO())==0 and b'0.4.1' in out.getvalue()
   t=Terminal(h);assert b'PTY_OK' in t.command('printf PTY_OK','READY');t.close()
   h.hello();src=a.output/'file-input.bin';dst=a.output/'file-output.bin';src.write_bytes(bytes(range(256))*4)
   h.put(src,R+'/file-test.bin',True);h.get(R+'/file-test.bin',dst,True);assert src.read_bytes()==dst.read_bytes();h.call(39)
   ident=struct.unpack_from('<I',h.call(48,struct.pack('<HH',24,80)))[0]
   self.b.last=time.monotonic();out,_=self.b.call(1,b'\x78');assert out==b'\x79'
   try:h.call(52,struct.pack('<I',ident));raise AssertionError('maintenance not preempted')
   except TimeoutError:pass
   self.b.last=time.monotonic();self.report['maintenance_ok']=True;self.save()
   time.sleep(.3);expected=bytes(self.b.sent[before:]);digest=hashlib.md5(expected).hexdigest()
   captured=self.q('wc -c '+Q(R+'/maintenance-rx.bin')+'; md5sum '+Q(R+'/maintenance-rx.bin'))
   assert re.search(r'^\s*'+str(len(expected))+r' '+re.escape(R+'/maintenance-rx.bin')+r'$',captured,re.M)
   assert re.search(r'^'+digest+r'\s+'+re.escape(R+'/maintenance-rx.bin')+r'$',captured,re.M)
   self.report['maintenance_subscriber']={'bytes':len(expected),'md5':digest,'exact_business_only':True};self.save()
  finally:self.q(f'kill -TERM {monitor} 2>/dev/null; true')
 def final(self):
  if self.sampler:self.sample_stop()
  r=self.activate('new','running-optimized',normal=True);self.wait(10)
  self.maintenance()
  for i in range(10):self.heartbeat(r,i)
  self.report['final_fds']={}
  for name,pid in [('app',r['pid']),('daemon',r['daemon_pid'])]:
   self.report['final_fds'][name]=self.q(f'for f in /proc/{pid}/fd/*; do x=$(readlink "$f"); case "$x" in /dev/ttyS3|/dev/ttyS4|/dev/watchdog*) echo "${{f##*/}} $x";; esac; done')
  self.report['preload']=self.q(f"if tr '\\000' '\\n' </proc/{r['pid']}/environ | grep -q '^LD_PRELOAD='; then echo PRESENT; else echo ABSENT; fi")
  assert '\nABSENT\n' in self.report['preload']
  self.report['final_boot']=self.q('cat /proc/sys/kernel/random/boot_id')
  self.report['final_ok']=True;self.save();print('RUNNING_0.4.53.d_HD485_0.4.1',flush=True)
 def verify_final(self):
  self.changed=True;self.closebus()
  for name in ('hd_camera_app2','ab-peer-b','hd485d'):self.stop(name)
  self.q('cd '+Q(R)+' && test -x hd485d-final && test -x stream-check && if [ ! -e hd485d-perf ]; then cp rv1106/bin/hd485d hd485d-perf; fi && cp hd485d-final rv1106/bin/hd485d && md5sum hd_camera_app2 lib/libhd_uart_qjy_rv1106.so rv1106/bin/hd485d trace-old/hd485d >MD5SUMS')
  self.report['final_binary_hash']=self.q('md5sum '+Q(R+'/rv1106/bin/hd485d'));self.save()
  self.peer(('new',))
  self.report['abort']=self.q(Q(R+'/stream-check')+' /run/hd485.sock abort');self.wait(.5)
  termios.tcflush(self.fd,termios.TCIFLUSH)
  state=self.q(Q(R+'/rv1106/bin/hd485-example')+' /run/hd485.sock status; cat /sys/class/gpio/gpio35/value')
  assert 'owner=0' in state and 'error=0' in state and '\n0\n' in state
  out,_=self.b.call(0xe0);assert out==b'HD485-AB-0.1.0';self.report['abort_recovered']=True
  self.report['stall']=self.q(Q(R+'/stream-check')+' /run/hd485.sock stall')
  assert 'STREAM_TIMEOUT_OK' in self.report['stall']
  state=self.q('cat /sys/class/gpio/gpio35/value');assert '\n0\n' in state
  out,_=self.b.call(0xe0);assert out==b'HD485-AB-0.1.0';self.report['stall_recovered']=True
  self.stop('hd485d');self.daemon('new');started=time.monotonic();self.wait(2)
  out,_=self.b.call(0xe0);assert out==b'HD485-AB-0.1.0'
  self.report['reconnect_verified_s']=time.monotonic()-started;self.save()
  self.app(('new',))
 def run(self):
  try:
   self.q('test ! -e /userdata/ota/dynamic_sys_transaction && test ! -e /userdata/ota/dynamic_app_transaction && test ! -e /userdata/ota/model_transaction')
   self.q('cp '+Q(AB+'/ab-sampler')+' '+Q(R+'/ab-sampler'))
   self.report['environment']=self.q('cat /proc/version /proc/sys/kernel/random/boot_id; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor; cd '+Q(R)+' && while read expected file; do actual=$(md5sum "$file"); test "${actual%% *}" = "$expected" || exit 1; echo "$file OK"; done <MD5SUMS')
   if a.action=='all':self.peer();self.app();self.trace()
   elif a.action!='final':getattr(self,a.action.replace('-','_'))()
   self.report['ok']=True;self.save()
  except BaseException as e:self.report['error']=repr(e);self.save();raise
  finally:
   try:
    if self.changed or a.action=='final':self.final()
   finally:self.save();self.closebus();self.c.close()
if __name__=='__main__':
 if sys.version_info<(3,11):raise SystemExit('Python >=3.11 required')
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--console',required=True);p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--remote',default='/userdata/tmp_var/hd485-opt-0.4.1')
 p.add_argument('action',choices=['all','peer','app','trace','final','verify-final'])
 a=p.parse_args();R=a.remote;a.output.mkdir(parents=True,exist_ok=True);Runner().run()
