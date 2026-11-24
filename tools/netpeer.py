#!/usr/bin/env python3
# MatrixGame - SR2 Planetary battles engine
# Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
# Licensed under GPLv2 or any later version
"""UDP-партнёр для проверки сетевого слоя MatrixGame (фаза 3).

Играет одну сторону: берёт приказы из лога, который записал командный бот
(`MATRIXGAME_CMDLOG`), и отправляет их в игру по протоколу NetPlay. Держит
барьер READY, подтверждает пачки и сверяет хеши мира с эталонным прогоном.

Пример:
  python tools/netpeer.py --seed 12345 --side 1 ^
      --cmds tools/logs/p3ref-cmd.txt --ref tools/logs/p3ref.txt

Протокол (little-endian, без выравнивания):

  заголовок: u32 magic 'MGNP', u16 version, u16 type, u32 len
  HELLO      <II   side, seed
  HELLO_ACK  <IIII ok, seed, tickPeriod, lag
  CMD        <II   batchSeq, cnt; далее cnt записей:
                    <HHHHIiiiI  type, flags, side, selCnt, tick, objId, x, y, seq
                    + selCnt * u32
  ACK        <Ii   ackedBatchSeq, appliedTick
  READY      <i    tick
  HASH       <IQ   tick, hash
"""

import argparse
import os
import socket
import struct
import sys
import time

MAGIC = 0x504E474D
VERSION = 1

HELLO = 1
HELLO_ACK = 2
CMD = 3
ACK = 4
READY = 5
HASH = 6

HDR = struct.Struct('<IHHI')
CMD_HEAD = struct.Struct('<II')
CMD_FIXED = struct.Struct('<HHHHIiiiI' + 'i' * 9)  # + 9 int params (build robot spec)

TYPES = {
    'STOP': 1,
    'MOVE': 2,
    'ATTACK': 3,
    'CAPTURE': 4,
    'PATROL': 5,
    'REPAIR': 6,
    'BOMB': 7,
    'AUTOCAP': 8,
    'AUTOATK': 9,
    'AUTODEF': 10,
    'BUILD': 11,
}

TICK_MS = 10


def parse_cmdlog(path):
    """Читает лог команд: по строке на команду, ключи key=value."""
    cmds = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or not line.startswith('t='):
                continue
            kv = {}
            for token in line.split(' '):
                if '=' in token:
                    k, v = token.split('=', 1)
                    kv[k] = v
            sel = [int(x) for x in kv.get('sel', '').split(',') if x]
            params = [int(kv.get('p%d' % i, 0)) for i in range(9)]
            cmds.append({
                'tick': int(kv.get('t', 0)),
                'side': int(kv.get('side', 0)),
                'type': TYPES[kv['type']],
                'obj': int(kv.get('obj', -1)),
                'x': int(kv.get('x', 0)),
                'y': int(kv.get('y', 0)),
                'flags': int(kv.get('flags', 0)),
                'seq': int(kv.get('seq', 0)),
                'sel': sel,
                'p': params,
            })
    cmds.sort(key=lambda c: (c['tick'], c['seq']))
    return cmds


def parse_ref(path):
    """Читает эталонный хеш-лог харнесса: t=<мс> ... hash=<hex>."""
    ref = {}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if 'hash=' not in line or 't=' not in line:
                continue
            parts = dict(p.split('=', 1) for p in line.split(' ') if '=' in p)
            try:
                ref[int(parts['t'])] = parts['hash']
            except (KeyError, ValueError):
                continue
    return ref


def pack_cmd(c):
    sel = c['sel'][:64]
    params = list((c.get('p') or [])[:9])
    while len(params) < 9:
        params.append(0)
    body = CMD_FIXED.pack(c['type'], c['flags'], c['side'], len(sel), c['tick'], c['obj'], c['x'], c['y'],
                          c['seq'], *params)
    body += b''.join(struct.pack('<I', s) for s in sel)
    return body


def parse_cmd_batch(body):
    batch, cnt = CMD_HEAD.unpack_from(body, 0)
    pos = CMD_HEAD.size
    out = []
    for _ in range(cnt):
        fields = CMD_FIXED.unpack_from(body, pos)
        ctype, flags, side, selcnt, tick, obj, x, y, seq = fields[:9]
        params = list(fields[9:])
        pos += CMD_FIXED.size
        sel = list(struct.unpack_from('<%dI' % selcnt, body, pos)) if selcnt else []
        pos += selcnt * 4
        out.append({
            'tick': tick,
            'side': side,
            'type': ctype,
            'obj': obj,
            'x': x,
            'y': y,
            'flags': flags,
            'seq': seq,
            'sel': list(sel),
            'p': params,
        })
    return batch, out


CMP_FIELDS = ('tick', 'side', 'type', 'obj', 'x', 'y', 'flags', 'sel', 'p')


class Peer(object):
    def __init__(self, args):
        self.args = args
        cmds = parse_cmdlog(args.cmds) if args.cmds else []
        # Играем только своей стороной: лог может содержать приказы обеих.
        self.cmds = [c for c in cmds if c['side'] == args.side] if args.filter_side else cmds
        self.ref = parse_ref(args.ref) if args.ref else {}

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', args.local_port))
        self.sock.setblocking(False)
        self.host = ('127.0.0.1', args.port)

        self.handshaked = False
        self.hello_sent = 0.0

        self.window = args.start_window
        self.sent = 0  # сколько команд уже отправлено
        self.batch_seq = 0
        self.batches = []  # [(seq, [cmd,...], last_send)]
        self.acked_cmds = 0  # сколько первых команд подтверждено
        self.ready = 0
        self.applied = 0

        self.hashes = 0
        self.hashes_ok = 0
        self.hashes_bad = 0
        self.fake_sent = 0
        self.finished = False
        self.last_progress = 0.0
        self.received = []
        self.ready_sent = False
        self.drops = 0
        self.expect_path = args.expect
        self.end_tick = args.end_tick
        if self.ref:
            self.end_tick = max(self.end_tick, max(self.ref.keys()) // TICK_MS)

    def send(self, mtype, payload):
        packet = HDR.pack(MAGIC, VERSION, mtype, len(payload)) + payload
        try:
            self.sock.sendto(packet, self.host)
        except OSError:
            pass

    def recv(self):
        while True:
            try:
                data, _addr = self.sock.recvfrom(4096)
            except BlockingIOError:
                return
            except ConnectionResetError:
                # Windows: ICMP port unreachable from an earlier send surfaces
                # here as WSAECONNRESET. Nothing to do — just keep listening.
                continue
            except OSError:
                return
            if len(data) < HDR.size:
                continue
            magic, ver, mtype, length = HDR.unpack_from(data, 0)
            if magic != MAGIC or ver != VERSION:
                continue
            body = data[HDR.size:HDR.size + length]
            self.handle(mtype, body)

    def handle(self, mtype, body):
        if mtype == HELLO_ACK:
            ok, seed, period, lag = struct.unpack('<IIII', body[:16])
            if ok:
                if not self.handshaked:
                    print('handshake ok: seed=%d tick=%d ms lag=%d' % (seed, period, lag))
                self.handshaked = True
                if self.args.echo:
                    # Мы не играем: сразу разрешаем хосту ехать до конца.
                    self.send(READY, struct.pack('<i', self.end_tick))
            else:
                print('handshake rejected: host seed=%d' % seed)
                self.finished = True
        elif mtype == ACK:
            acked, applied = struct.unpack('<Ii', body[:8])
            self.on_ack(acked, applied)
        elif mtype == HASH:
            tick, value = struct.unpack('<IQ', body[:12])
            self.on_hash(tick, value)
        elif mtype == CMD:
            self.on_cmd(body)

    def on_cmd(self, body):
        batch, cmds = parse_cmd_batch(body)
        if self.args.echo:
            self.received.extend(cmds)
            if self.args.verbose:
                for c in cmds:
                    print('<- t=%d side=%d type=%d obj=%d x=%d y=%d sel=%d' %
                          (c['tick'], c['side'], c['type'], c['obj'], c['x'], c['y'], len(c['sel'])))

        # Подтверждаем всё, что пришло, и разрешаем ехать дальше: в режиме
        # эха мы не играем, а только принимаем приказы.
        self.send(ACK, struct.pack('<Ii', batch, self.end_tick))
        if self.args.echo:
            self.send(READY, struct.pack('<i', self.end_tick))

    def on_ack(self, acked_batch, applied):
        self.applied = applied

        # Подтверждённые пачки уходят из очереди: партнёр принял их целиком.
        while self.batches and self.batches[0][0] <= acked_batch:
            _seq, cmds, _t = self.batches.pop(0)
            self.acked_cmds += len(cmds)

        if self.args.pace == 'ack':
            self.window = min(self.end_tick, max(self.window, applied + self.args.lookahead))

    def on_hash(self, tick, value):
        self.hashes += 1
        t_ms = tick * TICK_MS

        if self.ref:
            want = self.ref.get(t_ms)
            if want is None:
                print('hash at t=%d: not in reference' % t_ms)
                return
            got = '%016x' % value
            if got == want:
                self.hashes_ok += 1
            else:
                self.hashes_bad += 1
                print('DESYNC at t=%d: host=%s reference=%s' % (t_ms, got, want))

        if self.args.fake_hash and self.fake_sent < self.args.fake_hash:
            self.fake_sent += 1
            self.send(HASH, struct.pack('<IQ', tick, (value + 1) & 0xFFFFFFFFFFFFFFFF))
            print('sent a deliberately wrong hash for t=%d' % t_ms)

    def send_pending(self):
        """Отправляет пачку со всеми командами до window."""
        take = []
        while self.sent < len(self.cmds) and self.cmds[self.sent]['tick'] <= self.window:
            take.append(self.cmds[self.sent])
            self.sent += 1

        if not take:
            return

        self.batch_seq += 1
        payload = CMD_HEAD.pack(self.batch_seq, len(take)) + b''.join(pack_cmd(c) for c in take)
        self.batches.append([self.batch_seq, take, time.time()])

        # --drop-every N: первую отправку каждой N-й пачки «теряем». Партнёр
        # промолчит, и пачку дошлёт resend() — это проверка перепосылки.
        lost = self.args.drop_every > 0 and (self.batch_seq % self.args.drop_every) == 0
        if lost:
            self.drops += 1
            print('x batch %d dropped on purpose (%d commands)' % (self.batch_seq, len(take)))
        else:
            self.send(CMD, payload)

        if self.batch_seq == 1:
            print('-> first batch: %d commands, ticks %d..%d' % (len(take), take[0]['tick'], take[-1]['tick']))

    def resend(self):
        now = time.time()
        for batch in self.batches:
            if now - batch[2] >= 0.1:
                seq, cmds, _t = batch
                payload = CMD_HEAD.pack(seq, len(cmds)) + b''.join(pack_cmd(c) for c in cmds)
                self.send(CMD, payload)
                batch[2] = now

    def send_ready(self):
        # Безопасный такт: до первой неподтверждённой команды, иначе — до конца
        # известного лога. Раньше этого такта партнёр точно не поедет.
        if self.acked_cmds < len(self.cmds):
            safe = self.cmds[self.acked_cmds]['tick'] - 1
        else:
            tail = self.cmds[-1]['tick'] if self.cmds else 0
            safe = max(self.end_tick, tail)

        ready = min(self.window, safe)
        if ready > self.ready:
            self.ready = ready
            self.send(READY, struct.pack('<i', ready))

    def run(self):
        started = time.time()
        last_hello = 0.0

        while not self.finished:
            now = time.time()

            if now - started > self.args.timeout:
                print('timeout')
                return 1

            if not self.handshaked:
                if now - last_hello >= 0.2:
                    self.send(HELLO, struct.pack('<II', self.args.side, self.args.seed))
                    last_hello = now
                time.sleep(0.01)
                self.recv()
                continue

            self.recv()

            if self.args.pace == 'all':
                self.window = self.end_tick

            self.send_pending()
            self.resend()
            if not self.args.echo:
                self.send_ready()

            if now - self.last_progress >= 2.0:
                self.last_progress = now
                print('progress: host t=%d ms, sent %d/%d commands, ready %d, hashes %d (ok %d, bad %d)' %
                      (self.applied * TICK_MS, self.sent, len(self.cmds), self.ready, self.hashes, self.hashes_ok,
                       self.hashes_bad))

            if self.args.echo:
                if self.applied >= self.end_tick:
                    time.sleep(0.3)
                    self.recv()
                    break
            elif self.applied >= self.end_tick and self.hashes > 0:
                # Хеши идут на каждом 50-м такте; даём последним дойти.
                time.sleep(0.3)
                self.recv()
                break

            time.sleep(0.005)

        print('done: host reached t=%d ms, hashes %d (ok %d, bad %d), commands sent %d, dropped %d' %
              (self.applied * TICK_MS, self.hashes, self.hashes_ok, self.hashes_bad, self.sent, self.drops))

        if self.args.echo:
            return self.report_echo()

        if self.args.fake_hash:
            return 0 if self.hashes_bad > 0 else 1
        return 0 if (self.hashes_bad == 0 and self.hashes_ok > 0) else 1

    def report_echo(self):
        print('received %d commands from the host' % len(self.received))
        if not self.expect_path:
            return 0

        # Лог хоста читаем только сейчас: он пишется, пока идёт матч.
        expect = parse_cmdlog(self.expect_path)
        print('host log %s: %d commands' % (self.expect_path, len(expect)))

        ok = 0
        bad = 0
        for i, got in enumerate(self.received):
            if i >= len(expect):
                bad += 1
                print('extra command at %d' % i)
                continue
            want = expect[i]
            diff = [f for f in CMP_FIELDS if got[f] != want[f]]
            if diff:
                bad += 1
                print('command %d differs in %s: got %s want %s' % (i, diff, [got[f] for f in diff],
                                                                    [want[f] for f in diff]))
            else:
                ok += 1

        print('received vs host log: %d ok, %d bad, expected %d' % (ok, bad, len(expect)))
        return 0 if bad == 0 and ok == len(expect) else 1


def main():
    ap = argparse.ArgumentParser(description='UDP peer for MatrixGame netplay testing')
    ap.add_argument('--port', type=int, default=27777, help='host UDP port')
    ap.add_argument('--local-port', type=int, default=27778, help='local UDP port')
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--side', type=int, default=1)
    ap.add_argument('--filter-side', action='store_true',
                    help='play only the commands of --side from the log (a two-sided log)')
    ap.add_argument('--cmds', default='', help='command log to play back')
    ap.add_argument('--expect', default='', help='command log the host is expected to send (echo mode)')
    ap.add_argument('--echo', action='store_true', help='do not play: accept the host commands and compare')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--ref', default='', help='reference hash log')
    ap.add_argument('--pace', choices=['ack', 'all'], default='ack')
    ap.add_argument('--lookahead', type=int, default=200, help='ticks the host may run ahead of ACK')
    ap.add_argument('--start-window', type=int, default=200)
    ap.add_argument('--end-tick', type=int, default=6000)
    ap.add_argument('--fake-hash', type=int, default=0, help='send N deliberately wrong hashes')
    ap.add_argument('--drop-every', type=int, default=0, help='drop the first send of every Nth command batch')
    ap.add_argument('--timeout', type=float, default=300.0)
    args = ap.parse_args()

    peer = Peer(args)
    return peer.run()


if __name__ == '__main__':
    sys.exit(main())
