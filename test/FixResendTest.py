#!/usr/bin/env python3
"""FixResendTest.py: a FIX client that reconnects gets back the reports it missed (#160), those the server made as it
started again too (#526).

Runs the server with this branch's config/fix.cfg, with only its paths and port changed, and credentials for two
counterparties (#147). First, a report made while its client is away (#160):
  1. CLIENT_A logs on, and rests a buy.
  2. Its connection drops, with no logout.
  3. CLIENT_B sells against the buy, so A's order fills while A is away.
  4. A logs on again, its sequence numbers continuing, and asks with a ResendRequest for what it missed.
The fill must come back, marked PossDupFlag (43=Y). Before #160 it was lost twice over: the gateway forgot a session at
logout and dropped the report, and ResetOnLogon=Y started both sides from 1 at A's logon.

Then, on a server of its own, a report made as the server starts (#526):
  1. CLIENT_A logs on, and rests a GTD buy that expires in a few seconds.
  2. The server is killed before the buy expires, as a crash would stop it: a clean stop takes seconds, and the buy
     could expire during it. It is started again on its data directory once the buy has expired.
  3. As the server starts its timers, it expires the buy. The report is made on the engine's threads, which nothing in
     the startup waits for, so the test waits for it in A's session's store.
  4. A logs on again, its sequence numbers continuing, and asks with a ResendRequest for what it missed.
The expiry's report (150=C) must come back, marked PossDupFlag (43=Y), and the server's log must show A's session made
before the restored orders' timers were set, and FIX started after them. Before #526 the server made its FIX sessions
only as FIX started, after the restore's work, so the gateway found no session for the report and dropped it.

usage: FixResendTest.py <orderProcessorServer> <seedData> <config/fix.cfg> <config/spec>
"""
import datetime
import glob
import os
import re
import sys
import time

sys.dont_write_bytecode = True  # no __pycache__ in the source tree, which the pre-push hook would find untracked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's own directory off the path
from fix_client import SOH, Client, Server, expect, now, problems  # noqa: E402

# Both trade the server's default account, as an order naming none did before #147
COUNTERPARTIES = [('CLIENT_A', 'client_a', 'password-a', 'TRADING-1'),
                  ('CLIENT_B', 'client_b', 'password-b', 'TRADING-1')]
# How long the GTD buy of the second scenario lives: long enough to be acknowledged, and the server killed, first
EXPIRES_IN = datetime.timedelta(seconds=4)


def order(clordid, side, tif='0'):
    return [('11', clordid), ('55', 'AAPL'), ('54', side), ('60', now()), ('38', '100'), ('40', '2'),
            ('44', '150.50'), ('59', tif)]


def report(clordid, exec_type):
    return lambda m: m.get('35') == '8' and m.get('11') == clordid and m.get('150') == exec_type


def while_away(port):
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


def stored(running, target):
    """The messages the server's session with target has stored to send: its FileStore's body file, which holds them
    one after another, each flushed as it is stored"""
    files = glob.glob(os.path.join(running.work, 'fix-store', f'*-{target}.body'))
    if len(files) != 1:
        return []
    with open(files[0], 'rb') as body:
        text = body.read().decode(errors='replace')
    return [dict(field.split('=', 1) for field in msg.split(SOH) if '=' in field)
            for msg in re.findall(f'8=FIXT\\.1\\.1{SOH}.*?{SOH}10=\\d{{3}}{SOH}', text, flags=re.S)]


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


def at_startup(running):
    # 1. A logs on, both sides starting from 1, and rests a GTD buy that expires in a few seconds
    a = Client(running.fix_port, 'CLIENT_A')
    a.logon('client_a', 'password-a')
    expect(a.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_A logs on')
    expires = utc_now() + EXPIRES_IN
    a.send('D', order('STARTUP-B1', '1', '6') + [('126', expires.strftime('%Y%m%d-%H:%M:%S.%f')[:-3])])
    expect(a.read_until(report('STARTUP-B1', '0')) is not None, 'its GTD buy is acknowledged (150=0)')
    seen = max(int(m['34']) for m in a.received)
    next_seq = a.seq

    # 2. The server is killed before the buy expires, and started again on its directory once it has
    running.kill()
    a.close()
    expect(utc_now() < expires, 'the server was killed before the buy expired')
    time.sleep(max(0.0, (expires - utc_now()).total_seconds()) + 1)
    logged = len(running.log())
    running.start()

    # 3. As the server starts its timers, it expires the buy: the report waits in A's session's store
    expired = report('STARTUP-B1', 'C')
    deadline = time.time() + 20
    while time.time() < deadline and not any(expired(m) for m in stored(running, 'CLIENT_A')):
        time.sleep(0.1)
    expect(any(expired(m) for m in stored(running, 'CLIENT_A')),
           "the expiry's report (150=C) is in CLIENT_A's store, made as the server started")

    # 4. A logs on again, its sequence numbers continuing, and asks for everything after what it saw
    a = Client(running.fix_port, 'CLIENT_A', next_seq)
    a.logon('client_a', 'password-a', reset=False)
    expect(a.read_until(lambda m: m.get('35') == 'A') is not None, 'CLIENT_A logs on again')
    a.send('2', [('7', str(seen + 1)), ('16', '0')])
    expiry = a.read_until(expired)
    expect(expiry is not None, "CLIENT_A's ResendRequest gets back the expiry's report (150=C)")
    expect(expiry is not None and expiry.get('43') == 'Y', 'resent with PossDupFlag (43=Y)')
    a.send('5', [])
    a.read_until(lambda m: m.get('35') == '5', 5)
    a.close()

    # The order the restarted server started in, from its log: the warning that FIX runs over plain TCP follows FIX's
    # start, and writes out the notes before it
    deadline = time.time() + 20
    while time.time() < deadline and 'FIX runs over plain TCP' not in running.log()[logged:]:
        time.sleep(0.1)
    started = running.log()[logged:]
    made = started.find('FIX session created: FIXT.1.1:ORDER_PROCESSOR->CLIENT_A')
    timers = started.find('Expiry and restatement timers set for restored orders')
    listening = started.find('FIX gateway started')
    expect(0 <= made < timers, "starting again, the server made CLIENT_A's session before it set the restored orders' "
                               'timers')
    expect(0 <= timers < listening, 'and started FIX after them')


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    server, seed, cfg, spec = sys.argv[1:]
    with Server(server, seed, cfg, spec, COUNTERPARTIES) as running:
        while_away(running.fix_port)
    with Server(server, seed, cfg, spec, COUNTERPARTIES) as running:
        at_startup(running)
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
