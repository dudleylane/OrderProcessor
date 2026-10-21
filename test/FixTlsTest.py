#!/usr/bin/env python3
"""FixTlsTest.py: with --fix-tls-cert and --fix-tls-key, the FIX acceptor serves TLS only (#228).

Before #228 FIX ran over plain TCP, so #147's Logon passwords, and every order and report, crossed the network in the
clear. With a certificate made for the run (it needs the openssl command):
  1. --fix-tls-cert without --fix-tls-key exits 2, and so do both without --fix-cfg. A key that isn't there exits 1,
     naming it.
  2. A client that checks the certificate logs on over TLS 1.2 or 1.3, and its order is acknowledged.
  3. A plain TCP client gets no answer, and the server closes the connection.
  4. The settings' own SSLProtocol holds: with TLS 1.3 only, a client limited to TLS 1.2 fails the handshake, and one
     allowed TLS 1.3 logs on. (A TLS 1.1 client says nothing here: this host's crypto policy refuses TLS 1.1 at both
     ends, whatever the server sets. FixTlsTest.cpp checks the protocols the server sets by default.)
  5. Without the flags FIX runs over plain TCP, and the server says so, once.

usage: FixTlsTest.py <orderProcessorServer> <seedData> <config/fix.cfg> <config/spec>
"""
import os
import re
import shutil
import ssl
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # no __pycache__ in the source tree, which the pre-push hook would find untracked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's own directory off the path
from fix_client import Client, Server, expect, now, problems  # noqa: E402

COUNTERPARTIES = [('CLIENT_A', 'client_a', 'password-a', 'CLIENT-A')]
# The plain-TCP warning, as the server logs it: at Warn level, which also writes it to the log at once
PLAIN_WARNING = re.compile(r': \[Warn\]\[\d+\] FIX runs over plain TCP')


def make_certificate(work):
    """A self-signed certificate for localhost and its key, as docker/README.md makes the stack's"""
    cert, key = os.path.join(work, 'tls.crt'), os.path.join(work, 'tls.key')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
                    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1', '-keyout', key, '-out', cert],
                   check=True, capture_output=True)
    return cert, key


def context(cert, maximum=None):
    """A client's TLS: it trusts cert alone, and goes no higher than maximum"""
    ctx = ssl.create_default_context(cafile=cert)
    if maximum:
        ctx.maximum_version = maximum
    return ctx


def order(clordid):
    return [('11', clordid), ('55', 'AAPL'), ('54', '1'), ('60', now()), ('38', '100'), ('40', '2'), ('44', '150.50'),
            ('59', '0')]


def logs_on(port, tls, what):
    """Logs CLIENT_A on, over tls or plain TCP, and checks that it did; returns the client"""
    client = Client(port, 'CLIENT_A', tls=tls)
    client.logon('client_a', 'password-a')
    on = client.read_until(lambda m: m.get('35') == 'A') is not None
    expect(on, f'{what}: CLIENT_A logs on, over {client.sock.version() if tls else "plain TCP"}')
    return client


def startup(args, cert, key):
    server, seed, cfg, spec = args
    unusable = Server(server, seed, cfg, spec, COUNTERPARTIES, tls=(cert, key))
    try:
        unusable.seed_data()
        full = unusable.args()
        at = full.index('--fix-tls-cert')
        only_cert = full[:at + 2] + full[at + 4:]
        run = subprocess.run(only_cert, env=unusable.env, capture_output=True, text=True, timeout=120)
        expect(run.returncode == 2 and '--fix-tls-cert and --fix-tls-key go together' in run.stderr,
               f'--fix-tls-cert without --fix-tls-key: exit 2, saying why ({run.returncode}: {run.stderr.strip()})')
        cfg_at = full.index('--fix-cfg')
        no_cfg = full[:cfg_at] + full[cfg_at + 2:at + 4]
        run = subprocess.run(no_cfg, env=unusable.env, capture_output=True, text=True, timeout=120)
        expect(run.returncode == 2 and 'need --fix-cfg' in run.stderr,
               f'the TLS flags without --fix-cfg: exit 2, saying why ({run.returncode}: {run.stderr.strip()})')
        missing = os.path.join(unusable.work, 'no-such.key')
        no_key = full[:at + 3] + [missing] + full[at + 4:]
        run = subprocess.run(no_key, env=unusable.env, capture_output=True, text=True, timeout=120)
        expect(run.returncode == 1 and f"can't read the FIX TLS key '{missing}'" in run.stderr,
               f'a key that isn\'t there: exit 1, naming it ({run.returncode}: {run.stderr.strip()})')
    finally:
        shutil.rmtree(unusable.work, ignore_errors=True)


def tls_only(args, cert, key):
    with Server(*args, COUNTERPARTIES, tls=(cert, key)) as running:
        port = running.fix_port
        try:
            a = logs_on(port, context(cert), 'over TLS')
        except (ssl.SSLError, OSError) as failed:
            expect(False, f'over TLS: CLIENT_A logs on ({failed!r})')
            return
        expect(a.sock.version() in ('TLSv1.2', 'TLSv1.3'), f'with TLS 1.2 or 1.3 ({a.sock.version()})')
        a.send('D', order('TLS-1'))
        ack = a.read_until(lambda m: m.get('35') == '8' and m.get('11') == 'TLS-1')
        expect(ack is not None and ack.get('150') == '0', f'its order is acknowledged ({ack})')
        a.send('5', [])
        a.read_until(lambda m: m.get('35') == '5', 5)
        a.close()

        plain = Client(port, 'CLIENT_A')
        plain.logon('client_a', 'password-a')
        answer = plain.read_until(lambda m: True, 5)
        expect(answer is None, f'a plain TCP client gets no FIX answer ({answer})')
        expect(plain.closed_by_server(), 'and the server closes its connection')
        plain.close()
        expect(not PLAIN_WARNING.search(running.log()), 'the server serving TLS doesn\'t warn about plain TCP')


def settings_protocols(args, cert, key):
    with Server(*args, COUNTERPARTIES, tls=(cert, key), settings={'SSLProtocol': '-all +TLSv1_3'}) as running:
        try:
            Client(running.fix_port, 'CLIENT_A', tls=context(cert, ssl.TLSVersion.TLSv1_2)).close()
            expect(False, 'with SSLProtocol=-all +TLSv1_3, a client limited to TLS 1.2 is refused')
        except (ssl.SSLError, OSError) as refused:
            expect(True, f'with SSLProtocol=-all +TLSv1_3, a client limited to TLS 1.2 is refused ({refused})')
        client = logs_on(running.fix_port, context(cert), 'with TLS 1.3 allowed')
        expect(client.sock.version() == 'TLSv1.3', f'over TLS 1.3 ({client.sock.version()})')
        client.close()


def plain_warns(args):
    with Server(*args, COUNTERPARTIES) as running:
        client = logs_on(running.fix_port, None, 'without the TLS flags, over plain TCP')
        client.close()
        warnings = len(PLAIN_WARNING.findall(running.log()))
        expect(warnings == 1, f'and the server warns, at Warn level, that FIX runs over plain TCP, once ({warnings})')


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    args = sys.argv[1:]
    work = tempfile.mkdtemp(prefix='fix-tls-')
    try:
        cert, key = make_certificate(work)
        startup(args, cert, key)
        tls_only(args, cert, key)
        settings_protocols(args, cert, key)
        plain_warns(args)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
