#!/usr/bin/env python3
"""Explicit real-board SDK preemption check; sends a diagnostic raw business marker."""
import argparse, os, struct, time, shlex
from pty_board_audit import Console, SlowHost, Terminal, open_serial
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--console',required=True);p.add_argument('--port',required=True)
p.add_argument('--baud',type=int,required=True);p.add_argument('--example',required=True);p.add_argument('--socket',required=True)
a=p.parse_args();c=Console(a.console);fd=open_serial(a.port,a.baud);t=None
try:
 h=SlowHost(fd,timeout=.8,retries=3);h.hello();t=Terminal(h)
 t.command('stty -echo','PREEMPT_READY');t.send('sleep 30\n');time.sleep(.2)
 print(c.command(shlex.quote(a.example)+' '+shlex.quote(a.socket)+' send PTY_PREEMPT_CHECK'))
 t.thread.join(6)
 assert not t.thread.is_alive() and isinstance(t.error,TimeoutError),repr(t.error)
 print('Old PTY session stopped responding after SDK acquire/release (expected).')
 print(c.command(shlex.quote(a.example)+' '+shlex.quote(a.socket)+' status; cat /proc/sys/kernel/pty/nr'))
 h.hello();h.call(2,b'@version');time.sleep(.1)
 reply=h.call(3,struct.pack('<BI',0,0));assert b'hd485d 0.3.0' in reply[11:]
 print('PREEMPT_AND_EXPLICIT_RECONNECT_OK')
finally:
 if t and t.thread.is_alive():
  try:t.close()
  except Exception:pass
 import fcntl,termios
 fcntl.ioctl(fd,termios.TIOCNXCL);os.close(fd);c.close()
