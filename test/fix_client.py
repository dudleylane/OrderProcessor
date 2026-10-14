"""fix_client.py: what the scripts that run a real FIX server share (FixResendTest.py, FixLogonTest.py).

  - Client: a FIX 4.4 client on a raw socket, which sends what it is told to and keeps every message it reads.
  - Server: the server on a new, seeded data directory, with this branch's config/fix.cfg (only its paths and port
    changed) and a credentials file for the counterparties given, as the server needs since #147.
  - hash_password(): a password as the credentials file stores it, made with Python's own PBKDF2, so the server's is
    checked against an independent implementation.
  - expect() and problems: each check's line, and the ones that failed.
"""
import datetime
import hashlib
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


def hash_password(password, iterations=1000):
    """The credentials file's form of a password, as orderProcessorServer --hash-fix-password makes it, with few
    iterations, so the tests stay fast under the sanitizers"""
    salt = os.urandom(16)
    key = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, iterations, 32)
    return f'pbkdf2-sha256${iterations}${salt.hex()}${key.hex()}'


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

    def logon(self, username, password, reset=True):
        """Sends a Logon with Username (553) and Password (554), each left out when empty (#147); reset asks both sides
        to start again from sequence number 1 (141=Y)"""
        fields = [('98', '0'), ('108', '30')] + ([('141', 'Y')] if reset else [])
        fields += [(tag, value) for tag, value in (('553', username), ('554', password)) if value]
        self.send('A', fields)

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

    def closed_by_server(self, seconds=10):
        """Whether the server closes the connection within seconds, reading what it sends until then"""
        self.read_until(lambda m: False, seconds)
        try:
            self.sock.settimeout(1)
            return self.sock.recv(1) == b''
        except OSError:
            return True

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
    """This branch's settings, with their paths and port moved into work. They name no session: the credentials do."""
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
    path = os.path.join(work, 'fix.cfg')
    with open(path, 'w') as cfg:
        cfg.writelines(lines)
    return path


def write_credentials(work, counterparties):
    """A credentials file with a line per (CompID, Username, password, accounts)"""
    path = os.path.join(work, 'fix-credentials')
    with open(path, 'w') as f:
        f.write('# CompID  Username  PasswordHash  Accounts\n')
        for comp_id, username, password, accounts in counterparties:
            f.write(f'{comp_id} {username} {hash_password(password)} {accounts}\n')
    return path


class Server:
    """The server with FIX on a free port: on a new, seeded data directory, with this branch's settings and a
    credentials file for the counterparties given. On leaving, it stops the server, shows the end of its log if a check
    failed, and removes the directory."""

    def __init__(self, server, seed, cfg, dictionary, counterparties):
        self.server, self.seed, self.cfg, self.dictionary = server, seed, cfg, dictionary
        self.counterparties = counterparties
        self.work = tempfile.mkdtemp(prefix='fix-server-')
        self.env = dict(os.environ, EXCHANGE_LOG=os.path.join(self.work, 'exchange.log'))
        self.data = os.path.join(self.work, 'data')
        self.proc = None
        self.fix_port = None

    def seed_data(self):
        subprocess.run([self.seed, '--data-dir', self.data], check=True, env=self.env, stdout=subprocess.DEVNULL)

    def args(self):
        """The server's arguments, with FIX on fix_port"""
        self.fix_port = free_port()
        return [self.server, '--port', str(free_port()), '--data-dir', self.data,
                '--fix-cfg', write_cfg(self.cfg, self.work, self.fix_port, self.dictionary),
                '--fix-credentials', write_credentials(self.work, self.counterparties)]

    def __enter__(self):
        self.seed_data()
        self.proc = subprocess.Popen(self.args(), env=self.env)
        deadline = time.time() + 60
        while time.time() < deadline and self.proc.poll() is None:
            try:
                socket.create_connection(('127.0.0.1', self.fix_port), timeout=1).close()
                return self
            except OSError:
                time.sleep(0.2)
        self.__exit__(None, None, None)
        sys.exit(f'the server did not listen on {self.fix_port} (exit {self.proc.poll()})')

    def log(self):
        path = os.path.join(self.work, 'exchange.log')
        if not os.path.exists(path):
            return ''
        with open(path) as f:
            return f.read()

    def __exit__(self, *exc):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if problems:
            print('--- the server log, last lines:')
            print(''.join(self.log().splitlines(keepends=True)[-30:]))
        shutil.rmtree(self.work, ignore_errors=True)
        return False
