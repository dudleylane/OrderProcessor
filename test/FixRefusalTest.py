#!/usr/bin/env python3
"""FixRefusalTest.py: the FIX gateway answers an order it refuses to its sender alone (#189).

Two counterparties log on. CLIENT_A sends orders the gateway refuses itself, and each must come back to it as an
ExecutionReport (150=8, 39=8) with its OrdRejReason (103), while CLIENT_B gets nothing:
  1. A NewOrderSingle for a symbol the server doesn't have (103=1). It used to get no answer at all.
  2. A NewOrderMultileg that isn't an FX swap (103=11), an FX swap with one leg (103=11), and one for a symbol the
     server doesn't have (103=1). These used to go to every session logged on, as a BusinessMessageReject that named no
     order.

usage: FixRefusalTest.py <orderProcessorServer> <seedData> <config/fix.cfg> <config/spec/FIX44.xml>
"""
import os
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the source tree, which the pre-push hook would find untracked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's own directory off the path
from fix_client import Client, Server, expect, now, problems  # noqa: E402

COUNTERPARTIES = [('CLIENT_A', 'client_a', 'password-a', 'TRADING-1'),
                  ('CLIENT_B', 'client_b', 'password-b', 'TRADING-1')]
TWO_LEGS = [('1', '1.0850', '20261015'), ('2', '1.0872', '20261115')]


def single(clordid, symbol):
    return [('11', clordid), ('55', symbol), ('54', '1'), ('60', now()), ('38', '100'), ('40', '2'), ('44', '1.08'),
            ('59', '0')]


def multileg(clordid, symbol, ord_type, legs):
    fields = [('11', clordid), ('55', symbol), ('54', '1'), ('60', now()), ('38', '1000000'), ('40', ord_type),
              ('555', str(len(legs)))]
    for side, price, date in legs:
        fields += [('600', symbol), ('624', side), ('566', price), ('588', date)]
    return fields


def logged_on(port, comp_id, username, password):
    client = Client(port, comp_id)
    client.logon(username, password)
    expect(client.read_until(lambda m: m.get('35') == 'A') is not None, f'{comp_id} logs on')
    return client


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    with Server(*sys.argv[1:], COUNTERPARTIES) as running:
        a = logged_on(running.fix_port, 'CLIENT_A', 'client_a', 'password-a')
        b = logged_on(running.fix_port, 'CLIENT_B', 'client_b', 'password-b')
        cases = [('D', single('REFUSE-1', 'NO-SUCH'), '1', 'a NewOrderSingle for an unknown symbol'),
                 ('AB', multileg('REFUSE-2', 'EURUSD', '2', TWO_LEGS), '11', "a NewOrderMultileg that isn't an FX swap"),
                 ('AB', multileg('REFUSE-3', 'EURUSD', 'G', TWO_LEGS[:1]), '11', 'an FX swap with one leg'),
                 ('AB', multileg('REFUSE-4', 'NO-SUCH', 'G', TWO_LEGS), '1', 'an FX swap for an unknown symbol')]
        for msg_type, fields, reason, what in cases:
            clordid = fields[0][1]
            a.send(msg_type, fields)
            answer = a.read_until(lambda m, c=clordid: m.get('35') in ('8', 'j', '3') and m.get('11', c) == c, 10)
            expect(answer is not None and answer.get('35') == '8' and answer.get('150') == '8' and
                   answer.get('39') == '8' and answer.get('103') == reason,
                   f'{what}: refused to CLIENT_A, 150=8 39=8 103={reason} ({answer})')
        stray = b.read_until(lambda m: m.get('35') in ('8', 'j'), 3)
        expect(stray is None, f"CLIENT_B gets none of CLIENT_A's refusals ({stray})")
        for client in (a, b):
            client.send('5', [])
            client.read_until(lambda m: m.get('35') == '5', 5)
            client.close()
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
