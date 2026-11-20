#!/usr/bin/env python3
"""Unit tests for the DirectPlay relay protocol (tools/dplay_relay.py)."""
import json
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import dplay_relay as d  # noqa: E402

APP = bytes(range(16))


class Conn:
    def __init__(self):
        self.frames, self.session = [], None

    def send(self, data):
        self.frames.append(bytes(data))

    def take(self):
        frames, self.frames = self.frames, []
        return frames


def desc(name=b'Game', flags=0x44, max_players=8):
    return bytes(16) + APP + struct.pack('<7I', flags, max_players, 1, 0, 0, 0, 0) + d.put_str(name) + d.put_str(b'')


def create(relay, conn, request=1, **kw):
    relay.handle(conn, bytes([d.OP_CREATE]) + struct.pack('<I', request) + desc(**kw))
    reply = conn.take()[-1]
    assert reply[0] == d.OP_OPEN_REPLY and struct.unpack_from('<II', reply, 1) == (request, 0)
    return reply[9:25]  # session instance


def join(relay, conn, instance, request=2):
    relay.handle(conn, bytes([d.OP_JOIN]) + struct.pack('<I', request) + instance + d.put_str(b''))
    reply = conn.take()[-1]
    return struct.unpack_from('<I', reply, 5)[0], reply


def player(relay, conn, name, request=3):
    relay.handle(conn, bytes([d.OP_CREATE_PLAYER]) + struct.pack('<II', request, 0) + d.put_str(name) + d.put_str(b'') + d.put_blob(b''))
    reply = conn.take()[-1]
    assert reply[0] == d.OP_PLAYER_REPLY
    request_id, result, dpid = struct.unpack_from('<III', reply, 1)
    assert (request_id, result) == (request, 0)
    return dpid


class RelayTests(unittest.TestCase):
    def setUp(self):
        self.relay, self.host, self.guest = d.Relay(), Conn(), Conn()

    def test_create_enumerate_join_and_messages(self):
        instance = create(self.relay, self.host, name=b'Host Game')
        host_id = player(self.relay, self.host, b'Host')
        self.relay.handle(self.guest, bytes([d.OP_ENUM]) + struct.pack('<I', 9) + APP + struct.pack('<I', 1))
        reply = self.guest.take()[0]
        self.assertEqual(reply[0], d.OP_ENUM_REPLY)
        self.assertEqual(struct.unpack_from('<II', reply, 1), (9, 1))
        self.assertEqual(reply[9:25], instance)
        self.assertIn(b'Host Game', reply)

        result, reply = join(self.relay, self.guest, instance)
        self.assertEqual(result, 0)
        self.assertIn(b'Host', reply)  # existing players come with the open reply
        guest_id = player(self.relay, self.guest, b'Guest')
        created = self.host.take()
        self.assertEqual(created[0][0], d.OP_SYS_CREATE)
        self.assertEqual(struct.unpack_from('<I', created[0], 1)[0], guest_id)

        # Broadcast reaches everyone but the sender; directed sends reach one player.
        self.relay.handle(self.host, bytes([d.OP_SEND]) + struct.pack('<III', host_id, 0, 0) + d.put_blob(b'hello'))
        self.assertEqual(self.host.take(), [])
        got = self.guest.take()[0]
        self.assertEqual(got[0], d.OP_RECEIVE)
        self.assertEqual(struct.unpack_from('<II', got, 1), (host_id, guest_id))
        self.assertTrue(got.endswith(b'hello'))
        self.relay.handle(self.guest, bytes([d.OP_SEND]) + struct.pack('<III', guest_id, host_id, 0) + d.put_blob(b'ack'))
        self.assertTrue(self.host.take()[0].endswith(b'ack'))

        # Senders can only speak for their own players.
        self.relay.handle(self.guest, bytes([d.OP_SEND]) + struct.pack('<III', host_id, 0, 0) + d.put_blob(b'forged'))
        self.assertEqual(self.host.take() + self.guest.take(), [])

        # Remote player data (the host's lobby options) becomes DPSYS_SETPLAYERORGROUPDATA elsewhere.
        self.relay.handle(self.host, bytes([d.OP_SET_DATA]) + struct.pack('<II', host_id, 0) + d.put_blob(b'options'))
        got = self.guest.take()[0]
        self.assertEqual((got[0], struct.unpack_from('<I', got, 1)[0]), (d.OP_SYS_SETDATA, host_id))
        self.assertTrue(got.endswith(b'options'))

    def test_new_players_disabled_and_errors(self):
        instance = create(self.relay, self.host)
        player(self.relay, self.host, b'Host')
        self.relay.handle(self.host, bytes([d.OP_ENABLE_NEW]) + struct.pack('<I', 0))
        self.relay.handle(self.guest, bytes([d.OP_ENUM]) + struct.pack('<I', 1) + APP + struct.pack('<I', 1))
        self.assertEqual(struct.unpack_from('<I', self.guest.take()[0], 5)[0], 0, 'Started games are not listed')
        self.assertEqual(join(self.relay, self.guest, instance)[0], d.DPERR_NONEWPLAYERS)
        self.assertEqual(join(self.relay, self.guest, bytes(16))[0], d.DPERR_NOSESSIONS)

    def test_host_migration_and_session_lost(self):
        instance = create(self.relay, self.host)
        host_id = player(self.relay, self.host, b'Host')
        join(self.relay, self.guest, instance)
        player(self.relay, self.guest, b'Guest')
        self.host.take()
        self.relay.disconnect(self.host)
        frames = self.guest.take()
        self.assertEqual([f[0] for f in frames], [d.OP_SYS_DESTROY, d.OP_SYS_HOST], 'MIGRATEHOST session moves its host')
        self.assertEqual(struct.unpack_from('<I', frames[0], 1)[0], host_id)

        relay, a, b = d.Relay(), Conn(), Conn()
        instance = create(relay, a, flags=0)
        player(relay, a, b'A')
        join(relay, b, instance)
        relay.disconnect(a)
        self.assertEqual([f[0] for f in b.take()], [d.OP_SYS_DESTROY, d.OP_SYS_SESSIONLOST])
        self.assertEqual(relay.sessions, {})


if __name__ == '__main__':
    result = unittest.main(exit=False).result
    report = {'passed': result.wasSuccessful(), 'tests': result.testsRun,
              'scope': 'DirectPlay relay protocol: sessions, players, routing, player data, new-player gating, host migration.'}
    (ROOT / 'reports/dplay-relay-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    sys.exit(0 if result.wasSuccessful() else 1)
