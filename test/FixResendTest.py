#!/usr/bin/env python3
"""FixResendTest.py: a FIX client that reconnects gets back the reports it missed (#160).

Runs the server with this branch's config/fix.cfg, with only its paths and port changed and a second session added:
  1. CLIENT_A logs on, and rests a buy.
  2. Its connection drops, with no logout.
  3. CLIENT_B sells against the buy, so A's order fills while A is away.
  4. A logs on again, its sequence numbers continuing, and asks with a ResendRequest for what it missed.
The fill must come back, marked PossDupFlag (43=Y). Before #160 it was lost twice over: the gateway forgot a session at
logout and dropped the report, and ResetOnLogon=Y started both sides from 1 at A's logon.

usage: FixResendTest.py <orderProcessorServer> <seedData> <config/fix.cfg> <config/spec/FIX44.xml>
"""
import datetime
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SOH = '\x01'
problems = []


def expect(condition, what):
    print(('ok    ' if condition else 'FAIL  ') + what)
    if not condition:
        problems.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H:%M:%S.%f')[:-3]


class Client:
    """A FIX 4.4 client on a raw socket: it sends what it is told to, and keeps every message it reads"""

    def __init__(self, port, sender, seq=1):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.sender = sender
        self.seq = seq
        self.buf = b''
        self.received = []

    def send(self, msg_type, body):
        fields = [('35', msg_type), ('49', self.sender), ('56', 'ORDER_PROCESSOR'), ('34', str(self.seq)),
                  ('52', now())] + body
        self.seq += 1
        payload = ''.join(f'{tag}={value}{SOH}' for tag, value in fields)
        msg = f'8=FIX.4.4{SOH}9={len(payload)}{SOH}{payload}'
        self.sock.sendall(f'{msg}10={sum(msg.encode()) % 256:03d}{SOH}'.encode())

    def read_until(self, wanted, seconds=20):
        """The first message wanted(msg) holds for; None at the deadline, or once the server closes the connection"""
        deadline = time.time() + seconds
        while True:
            for msg in self._parse():
                self.received.append(msg)
                if wanted(msg):
                    return msg
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except OSError:
                return None
            if not chunk:
                return None
            self.buf += chunk

    def _parse(self):
        msgs = []
        while True:
            checksum = self.buf.find(b'\x0110=')
            end = self.buf.find(b'\x01', checksum + 1) if checksum >= 0 else -1
            if end < 0:
                return msgs
            one, self.buf = self.buf[:end + 1], self.buf[end + 1:]
            msgs.append(dict(field.split('=', 1) for field in one.decode().split(SOH) if field))

    def close(self):
        self.sock.close()


def write_cfg(source, work, port, dictionary):
    """This branch's settings, with its paths and port moved into work, and a session for CLIENT_B added"""
    overrides = {'FileStorePath': os.path.join(work, 'fix-store'), 'FileLogPath': os.path.join(work, 'fix-log'),
                 'SocketAcceptPort': str(port), 'DataDictionary': dictionary}
    lines = []
    with open(source) as cfg:
        for line in cfg:
            key = line.split('=', 1)[0].strip()
            if '=' in line and key in overrides:
                line = f'{key}={overrides.pop(key)}\n'
            lines.append(line if line.endswith('\n') else line + '\n')
    if overrides:
        sys.exit(f'{source} has no {", ".join(sorted(overrides))}')
    lines.append('\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=ORDER_PROCESSOR\nTargetCompID=CLIENT_B\n')
    path = os.path.join(work, 'fix.cfg')
    with open(path, 'w') as cfg:
        cfg.writelines(lines)
    return path


def order(clordid, side):
    return [('11', clordid), ('55', 'AAPL'), ('54', side), ('60', now()), ('38', '100'), ('40', '2'),
            ('44', '150.50'), ('59', '0')]


def report(clordid, exec_type):
    return lambda m: m.get('35') == '8' and m.get('11') == clordid and m.get('150') == exec_type


def scenario(port):
    # 1. A logs on, both sides starting from 1, and rests a buy
    a = Client(port, 'CLIENT_A')
    a.send('A', [('98', '0'), ('108', '30'), ('141', 'Y')])
    expect(a.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_A logs on')
    a.send('D', order('RESEND-B1', '1'))
    expect(a.read_until(report('RESEND-B1', '0')) is not None, 'its buy is acknowledged (150=0)')
    seen = max(int(m['34']) for m in a.received)
    next_seq = a.seq

    # 2. Its connection drops, with no logout
    a.close()
    time.sleep(1)

    # 3. B sells against the buy: A's order fills while A is away
    b = Client(port, 'CLIENT_B')
    b.send('A', [('98', '0'), ('108', '30'), ('141', 'Y')])
    expect(b.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_B logs on')
    b.send('D', order('RESEND-S1', '2'))
    expect(b.read_until(report('RESEND-S1', 'F')) is not None, "CLIENT_B's sell fills (150=F)")
    time.sleep(1)

    # 4. A logs on again, its sequence numbers continuing, and asks for everything after what it saw
    a = Client(port, 'CLIENT_A', next_seq)
    a.send('A', [('98', '0'), ('108', '30')])
    expect(a.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_A logs on again')
    a.send('2', [('7', str(seen + 1)), ('16', '0')])
    fill = a.read_until(report('RESEND-B1', 'F'))
    expect(fill is not None, "CLIENT_A's ResendRequest gets back the fill it missed")
    expect(fill is not None and fill.get('43') == 'Y', 'resent with PossDupFlag (43=Y)')
    for client in (a, b):
        client.send('5', [])
        client.read_until(lambda m: m.get('35') == '5', 5)
        client.close()


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    server, seed, cfg, dictionary = sys.argv[1:]
    work = tempfile.mkdtemp(prefix='fix-resend-')
    env = dict(os.environ, EXCHANGE_LOG=os.path.join(work, 'exchange.log'))
    proc = None
    try:
        data = os.path.join(work, 'data')
        subprocess.run([seed, '--data-dir', data], check=True, env=env, stdout=subprocess.DEVNULL)
        fix_port = free_port()
        proc = subprocess.Popen([server, '--port', str(free_port()), '--data-dir', data, '--fix-cfg',
                                 write_cfg(cfg, work, fix_port, dictionary)], env=env)
        deadline = time.time() + 60
        while time.time() < deadline and proc.poll() is None:
            try:
                socket.create_connection(('127.0.0.1', fix_port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.2)
        if proc.poll() is not None or time.time() >= deadline:
            sys.exit(f'the server did not listen on {fix_port} (exit {proc.poll()})')
        scenario(fix_port)
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
        if problems:
            log = os.path.join(work, 'exchange.log')
            if os.path.exists(log):
                print('--- the server log, last lines:')
                with open(log) as f:
                    print(''.join(f.readlines()[-30:]))
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
