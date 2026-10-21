#!/usr/bin/env python3
"""FixResendTest.py: a FIX client that reconnects gets back the reports it missed (#160).

Runs the server with this branch's config/fix.cfg, with only its paths and port changed, and credentials for two
counterparties (#147):
  1. CLIENT_A logs on, and rests a buy.
  2. Its connection drops, with no logout.
  3. CLIENT_B sells against the buy, so A's order fills while A is away.
  4. A logs on again, its sequence numbers continuing, and asks with a ResendRequest for what it missed.
The fill must come back, marked PossDupFlag (43=Y). Before #160 it was lost twice over: the gateway forgot a session at
logout and dropped the report, and ResetOnLogon=Y started both sides from 1 at A's logon.

usage: FixResendTest.py <orderProcessorServer> <seedData> <config/fix.cfg> <config/spec>
"""
import os
import sys
import time

sys.dont_write_bytecode = True  # no __pycache__ in the source tree, which the pre-push hook would find untracked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's own directory off the path
from fix_client import Client, Server, expect, now, problems  # noqa: E402

# Both trade the server's default account, as an order naming none did before #147
COUNTERPARTIES = [('CLIENT_A', 'client_a', 'password-a', 'TRADING-1'),
                  ('CLIENT_B', 'client_b', 'password-b', 'TRADING-1')]


def order(clordid, side):
    return [('11', clordid), ('55', 'AAPL'), ('54', side), ('60', now()), ('38', '100'), ('40', '2'),
            ('44', '150.50'), ('59', '0')]


def report(clordid, exec_type):
    return lambda m: m.get('35') == '8' and m.get('11') == clordid and m.get('150') == exec_type


def scenario(port):
    # 1. A logs on, both sides starting from 1, and rests a buy
    a = Client(port, 'CLIENT_A')
    a.logon('client_a', 'password-a')
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
    b.logon('client_b', 'password-b')
    expect(b.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_B logs on')
    b.send('D', order('RESEND-S1', '2'))
    expect(b.read_until(report('RESEND-S1', 'F')) is not None, "CLIENT_B's sell fills (150=F)")
    time.sleep(1)

    # 4. A logs on again, its sequence numbers continuing, and asks for everything after what it saw
    a = Client(port, 'CLIENT_A', next_seq)
    a.logon('client_a', 'password-a', reset=False)
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
    server, seed, cfg, spec = sys.argv[1:]
    with Server(server, seed, cfg, spec, COUNTERPARTIES) as running:
        scenario(running.fix_port)
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
