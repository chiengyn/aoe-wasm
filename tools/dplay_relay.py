"""DirectPlay session relay for the browser service provider (engine/platform/win32_dplay.cpp).

Each browser game is one connection. The relay keeps what a DirectPlay session host and service provider
kept: sessions, their players and player data, and routes the game's messages between players. It does
not look inside game messages. Wire format: one opcode byte, then little-endian fields; strings are u16
length + bytes (Windows-1252), blobs are u32 length + bytes.
"""
import os
import struct
import threading

OP_ENUM, OP_CREATE, OP_JOIN, OP_CREATE_PLAYER, OP_DESTROY_PLAYER, OP_SEND = 1, 2, 3, 4, 5, 6
OP_SET_DATA, OP_SET_NAME, OP_SET_DESC, OP_ENABLE_NEW, OP_CLOSE = 7, 8, 9, 10, 11
OP_ENUM_REPLY, OP_OPEN_REPLY, OP_PLAYER_REPLY, OP_DESC = 101, 102, 104, 110
OP_SYS_CREATE, OP_SYS_DESTROY, OP_SYS_SETDATA, OP_SYS_SETNAME, OP_SYS_HOST, OP_SYS_SESSIONLOST = 120, 121, 122, 123, 124, 125
OP_RECEIVE = 130

DP_OK = 0
DPERR_INVALIDPARAMS = 0x80070057
DPERR_ACCESSDENIED = 0x88770014
DPERR_CANTADDPLAYER = 0x88770028
DPERR_INVALIDPLAYER = 0x88770096
DPERR_NOSESSIONS = 0x887700DC
DPERR_NONEWPLAYERS = 0x8877014A
DPERR_INVALIDPASSWORD = 0x88770154

DPSESSION_NEWPLAYERSDISABLED = 0x1
DPSESSION_MIGRATEHOST = 0x4
DPSESSION_JOINDISABLED = 0x20
DPENUMSESSIONS_ALL = 0x2
DPSET_LOCAL = 0x1
GUID_NULL = bytes(16)
MAX_FRAME = 1 << 20


class ProtocolError(ValueError):
    pass


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        if self.at + n > len(self.data):
            raise ProtocolError('truncated frame')
        chunk = self.data[self.at:self.at + n]
        self.at += n
        return chunk

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def str(self):
        return self.take(struct.unpack('<H', self.take(2))[0])

    def blob(self):
        return self.take(self.u32())


def put_str(value):
    value = value[:0xffff]
    return struct.pack('<H', len(value)) + value


def put_blob(value):
    return struct.pack('<I', len(value)) + value


class Player:
    def __init__(self, dpid, conn, flags, short, long, data):
        self.dpid, self.conn, self.flags, self.short, self.long, self.data = dpid, conn, flags, short, long, data

    def record(self):
        return struct.pack('<II', self.dpid, self.flags) + put_str(self.short) + put_str(self.long) + put_blob(self.data)


class Session:
    def __init__(self, application, flags, max_players, user, name, password):
        self.instance = os.urandom(16)
        self.application, self.flags, self.max_players = application, flags, max_players
        self.user, self.name, self.password = list(user), name, password
        self.conns = []        # join order; the first one is the host
        self.players = {}      # dpid -> Player

    def record(self):
        return (self.instance + self.application + struct.pack('<7I', self.flags, self.max_players, len(self.players), *self.user)
                + put_str(self.name) + put_str(self.password))

    def accepts_players(self):
        return not self.flags & (DPSESSION_NEWPLAYERSDISABLED | DPSESSION_JOINDISABLED) and len(self.players) < self.max_players


def read_desc(r):
    instance, application = r.take(16), r.take(16)
    flags, max_players, _current, *user = struct.unpack('<7I', r.take(28))
    return instance, application, flags, max_players, user, r.str(), r.str()


class Relay:
    """Thread-safe session state. Connections are objects with send(bytes) and a `session` slot."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = {}     # instance -> Session
        self.dpids = set()

    def _new_dpid(self):
        while True:
            dpid = struct.unpack('<I', os.urandom(4))[0]
            if dpid not in (0, 1, 0xffffffff) and dpid not in self.dpids:
                self.dpids.add(dpid)
                return dpid

    @staticmethod
    def _others(session, conn):
        return [c for c in session.conns if c is not conn]

    def handle(self, conn, frame):
        if not frame or len(frame) > MAX_FRAME:
            raise ProtocolError('bad frame size')
        with self.lock:
            op, r = frame[0], Reader(frame[1:])
            session = getattr(conn, 'session', None)
            if op == OP_ENUM:
                request, application, flags = r.u32(), r.take(16), r.u32()
                found = [s for s in self.sessions.values()
                         if (application == GUID_NULL or s.application == application) and (flags & DPENUMSESSIONS_ALL or s.accepts_players())]
                conn.send(bytes([OP_ENUM_REPLY]) + struct.pack('<II', request, len(found)) + b''.join(s.record() for s in found))
            elif op in (OP_CREATE, OP_JOIN):
                request = r.u32()
                if session:
                    self._open_reply(conn, request, 0x88770005)  # DPERR_ALREADYINITIALIZED
                    return
                if op == OP_CREATE:
                    _instance, application, flags, max_players, user, name, password = read_desc(r)
                    session = Session(application, flags, max(1, min(max_players or 8, 16)), user, name, password)
                    self.sessions[session.instance] = session
                else:
                    session = self.sessions.get(r.take(16))
                    password = r.str()
                    if not session:
                        self._open_reply(conn, request, DPERR_NOSESSIONS)
                        return
                    if not session.accepts_players():
                        self._open_reply(conn, request, DPERR_NONEWPLAYERS)
                        return
                    if session.password and session.password != password:
                        self._open_reply(conn, request, DPERR_INVALIDPASSWORD)
                        return
                session.conns.append(conn)
                conn.session = session
                players = list(session.players.values())
                conn.send(bytes([OP_OPEN_REPLY]) + struct.pack('<II', request, DP_OK) + session.record()
                          + struct.pack('<I', len(players)) + b''.join(p.record() for p in players))
            elif not session:
                if op == OP_CREATE_PLAYER:
                    conn.send(bytes([OP_PLAYER_REPLY]) + struct.pack('<III', r.u32(), DPERR_INVALIDPARAMS, 0))
            elif op == OP_CREATE_PLAYER:
                request, flags, short, long, data = r.u32(), r.u32(), r.str(), r.str(), r.blob()
                if len(session.players) >= session.max_players:
                    conn.send(bytes([OP_PLAYER_REPLY]) + struct.pack('<III', request, DPERR_CANTADDPLAYER, 0))
                    return
                player = Player(self._new_dpid(), conn, flags, short, long, data)
                session.players[player.dpid] = player
                conn.send(bytes([OP_PLAYER_REPLY]) + struct.pack('<III', request, DP_OK, player.dpid))
                for other in self._others(session, conn):
                    other.send(bytes([OP_SYS_CREATE]) + player.record())
            elif op == OP_DESTROY_PLAYER:
                player = session.players.get(r.u32())
                if player and player.conn is conn:
                    self._destroy(session, player)
            elif op == OP_SEND:
                source, target, _flags, data = r.u32(), r.u32(), r.u32(), r.blob()
                sender = session.players.get(source)
                if not sender or sender.conn is not conn:
                    return
                targets = [p for p in session.players.values() if p.dpid != source] if target == 0 else \
                    [session.players[target]] if target in session.players else []
                for player in targets:
                    player.conn.send(bytes([OP_RECEIVE]) + struct.pack('<II', source, player.dpid) + put_blob(data))
            elif op == OP_SET_DATA:
                player, flags, data = session.players.get(r.u32()), r.u32(), r.blob()
                if player and player.conn is conn and not flags & DPSET_LOCAL:
                    player.data = data
                    for other in self._others(session, conn):
                        other.send(bytes([OP_SYS_SETDATA]) + struct.pack('<I', player.dpid) + put_blob(data))
            elif op == OP_SET_NAME:
                player, short, long = session.players.get(r.u32()), r.str(), r.str()
                if player and player.conn is conn:
                    player.short, player.long = short, long
                    for other in self._others(session, conn):
                        other.send(bytes([OP_SYS_SETNAME]) + struct.pack('<I', player.dpid) + put_str(short) + put_str(long))
            elif op == OP_SET_DESC:
                if session.conns and session.conns[0] is conn:
                    _instance, _app, session.flags, max_players, session.user, session.name, session.password = read_desc(r)
                    session.max_players = max(1, min(max_players or 8, 16))
                    for other in self._others(session, conn):
                        other.send(bytes([OP_DESC]) + session.record() + b'\x01')
            elif op == OP_ENABLE_NEW:
                if session.conns and session.conns[0] is conn:
                    if r.u32():
                        session.flags &= ~DPSESSION_NEWPLAYERSDISABLED
                    else:
                        session.flags |= DPSESSION_NEWPLAYERSDISABLED
                    for other in self._others(session, conn):
                        other.send(bytes([OP_DESC]) + session.record() + b'\x00')
            elif op == OP_CLOSE:
                self._leave(conn)
            else:
                raise ProtocolError('unknown opcode %d' % op)

    def _open_reply(self, conn, request, result):
        conn.send(bytes([OP_OPEN_REPLY]) + struct.pack('<II', request, result))

    def _destroy(self, session, player):
        del session.players[player.dpid]
        self.dpids.discard(player.dpid)
        for other in session.conns:
            if other is not player.conn:
                other.send(bytes([OP_SYS_DESTROY]) + struct.pack('<I', player.dpid))

    def _leave(self, conn):
        session = getattr(conn, 'session', None)
        if not session:
            return
        was_host = session.conns and session.conns[0] is conn
        for player in [p for p in session.players.values() if p.conn is conn]:
            self._destroy(session, player)
        session.conns.remove(conn)
        conn.session = None
        if not session.conns:
            self.sessions.pop(session.instance, None)
        elif was_host:
            if session.flags & DPSESSION_MIGRATEHOST:
                session.conns[0].send(bytes([OP_SYS_HOST]))
            else:
                for other in session.conns:
                    other.send(bytes([OP_SYS_SESSIONLOST]))
                    other.session = None
                self.sessions.pop(session.instance, None)

    def disconnect(self, conn):
        with self.lock:
            self._leave(conn)
