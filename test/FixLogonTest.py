#!/usr/bin/env python3
"""FixLogonTest.py: only a counterparty with its Username and Password logs on, and it trades only its accounts (#147).

Before #147 the acceptor checked nothing at logon: whoever reached port 5001 could log on as the CompID config/fix.cfg
named, CLIENT_A, and trade any account. Now:
  1. The server won't start FIX without a credentials file (exit 2), nor with one giving a counterparty an account the
     data directory doesn't have, nor with settings for anything but FIX 5.0 SP2 over FIXT.1.1 (exit 1, with the
     reason).
  2. A Logon with a wrong password, or with none, is answered with a Logout and a closed connection, and so is one
     naming another FIX version. QuickFIX drops without an answer one from a CompID no line names, and one of FIX 4.4,
     whose BeginString no session has.
  3. With its Username and Password, CLIENT_A logs on.
  4. Its order naming one of its accounts is acknowledged; one naming another account is refused (150=8, 103=15).
  5. --hash-fix-password makes a hash that Python's own PBKDF2 agrees with.

usage: FixLogonTest.py <orderProcessorServer> <seedData> <config/fix.cfg> <config/spec>
"""
import hashlib
import os
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the source tree, which the pre-push hook would find untracked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's own directory off the path
from fix_client import FIX50SP2, Client, Server, expect, now, problems  # noqa: E402

COUNTERPARTIES = [('CLIENT_A', 'client_a', 'password-a', 'CLIENT-A,TRADING-2')]


def order(clordid, account=None):
    fields = [('11', clordid), ('55', 'AAPL'), ('54', '1'), ('60', now()), ('38', '100'), ('40', '2'),
              ('44', '150.50'), ('59', '0')]
    return fields + ([('1', account)] if account else [])


def refused_logon(port, comp_id, username, password, what, appl_ver_id=FIX50SP2, begin_string='FIXT.1.1'):
    client = Client(port, comp_id, begin_string=begin_string)
    client.logon(username, password, appl_ver_id=appl_ver_id)
    answer = client.read_until(lambda m: m.get('35') in ('A', '5'), 10)
    if comp_id == 'CLIENT_A' and begin_string == 'FIXT.1.1':
        expect(answer is not None and answer.get('35') == '5' and 'Logon refused' in answer.get('58', ''),
               f'{what}: answered with a Logout saying the logon was refused ({answer})')
    else:
        expect(answer is None, f'{what}: not answered ({answer})')
    expect(client.closed_by_server(), f'{what}: the server closes the connection')
    client.close()


def startup(args):
    server, seed, cfg, spec = args
    unusable = Server(server, seed, cfg, spec, [('CLIENT_A', 'client_a', 'password-a', 'CLIENT-A,NO-SUCH')])
    try:
        unusable.seed_data()
        full = unusable.args()
        without = subprocess.run(full[:-2], env=unusable.env, capture_output=True, text=True, timeout=120)
        expect(without.returncode == 2 and '--fix-cfg needs --fix-credentials' in without.stderr,
               f'--fix-cfg without --fix-credentials: exit 2, saying why ({without.returncode}: {without.stderr.strip()})')
        unknown = subprocess.run(full, env=unusable.env, capture_output=True, text=True, timeout=120)
        expect(unknown.returncode == 1 and 'NO-SUCH' in unknown.stderr,
               f'an account the data directory lacks: exit 1, naming it ({unknown.returncode}: '
               f'{unknown.stderr.strip()})')
    finally:
        shutil.rmtree(unusable.work, ignore_errors=True)
    # The gateway speaks FIX 5.0 SP2 over FIXT.1.1 alone (#377)
    for setting, value in (('BeginString', 'FIX.4.4'), ('DefaultApplVerID', 'FIX.5.0')):
        other = Server(server, seed, cfg, spec, COUNTERPARTIES, settings={setting: value})
        try:
            other.seed_data()
            run = subprocess.run(other.args(), env=other.env, capture_output=True, text=True, timeout=120)
            expect(run.returncode == 1 and 'speaks FIX 5.0 SP2 over FIXT.1.1 alone' in run.stderr,
                   f'{setting}={value}: exit 1, saying why ({run.returncode}: {run.stderr.strip()})')
        finally:
            shutil.rmtree(other.work, ignore_errors=True)


def logons_and_accounts(args):
    with Server(*args, COUNTERPARTIES) as running:
        port = running.fix_port
        refused_logon(port, 'CLIENT_A', 'client_a', 'wrong-password', 'a wrong password')
        refused_logon(port, 'CLIENT_A', 'client_a', '', 'no password')
        refused_logon(port, 'CLIENT_A', 'client_b', 'password-a', 'another username')
        refused_logon(port, 'CLIENT_A', 'client_a', 'password-a', 'FIX 5.0, not SP2', appl_ver_id='7')
        refused_logon(port, 'CLIENT_A', 'client_a', 'password-a', 'FIX 4.4', begin_string='FIX.4.4')
        refused_logon(port, 'CLIENT_Z', 'client_a', 'password-a', 'a CompID no line names')
        expect('FIX logon refused for FIXT.1.1:ORDER_PROCESSOR->CLIENT_A: the Username or Password is wrong'
               in running.log(), 'the server logs why it refused a logon')
        expect('wrong-password' not in running.log(), 'the server log has no password in it')

        a = Client(port, 'CLIENT_A')
        a.logon('client_a', 'password-a')
        expect(a.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_A logs on with its password')
        a.send('D', order('LOGON-1', 'TRADING-2'))
        ack = a.read_until(lambda m: m.get('35') == '8' and m.get('11') == 'LOGON-1')
        expect(ack is not None and ack.get('150') == '0', f'its order on its own account is acknowledged ({ack})')
        a.send('D', order('LOGON-2', 'TRADING-1'))
        refusal = a.read_until(lambda m: m.get('35') == '8' and m.get('11') == 'LOGON-2')
        expect(refusal is not None and refusal.get('150') == '8' and refusal.get('103') == '15',
               f'its order on another account is refused, 150=8 103=15 ({refusal})')
        a.send('D', order('LOGON-3'))
        own = a.read_until(lambda m: m.get('35') == '8' and m.get('11') == 'LOGON-3')
        expect(own is not None and own.get('150') == '0', f'its order naming no account is acknowledged ({own})')
        a.send('5', [])
        a.read_until(lambda m: m.get('35') == '5', 5)
        a.close()


def hash_tool(server):
    made = subprocess.run([server, '--hash-fix-password'], input='a password\n', capture_output=True, text=True,
                          timeout=120)
    scheme, iterations, salt, key = (made.stdout.strip().split('$') + ['', '', '', ''])[:4]
    agrees = (made.returncode == 0 and scheme == 'pbkdf2-sha256' and iterations.isdigit() and
              hashlib.pbkdf2_hmac('sha256', b'a password', bytes.fromhex(salt), int(iterations), 32).hex() == key)
    expect(agrees, f'--hash-fix-password makes a hash Python agrees with ({made.returncode}: {made.stdout.strip()})')
    expect(iterations == '600000', f'with 600000 iterations ({iterations})')
    empty = subprocess.run([server, '--hash-fix-password'], input='', capture_output=True, text=True, timeout=60)
    expect(empty.returncode == 2, f'--hash-fix-password with no password: exit 2 ({empty.returncode})')


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    args = sys.argv[1:]
    startup(args)
    logons_and_accounts(args)
    hash_tool(args[0])
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
