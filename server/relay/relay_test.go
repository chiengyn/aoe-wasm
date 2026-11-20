package relay

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"sync"
	"testing"
)

var app = func() (g guid) {
	for i := range g {
		g[i] = byte(i)
	}
	return g
}()

type conn struct {
	*Client
	mu     sync.Mutex
	frames [][]byte
}

func newConn() *conn {
	c := &conn{}
	c.Client = NewClient(func(f []byte) {
		c.mu.Lock()
		c.frames = append(c.frames, append([]byte{}, f...))
		c.mu.Unlock()
	})
	return c
}

func (c *conn) take() [][]byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	f := c.frames
	c.frames = nil
	return f
}

func (c *conn) last(t *testing.T) []byte {
	t.Helper()
	f := c.take()
	if len(f) == 0 {
		t.Fatal("no frame")
	}
	return f[len(f)-1]
}

func u32(b []byte, at int) uint32 { return binary.LittleEndian.Uint32(b[at:]) }

func ops(frames [][]byte) []byte {
	var o []byte
	for _, f := range frames {
		o = append(o, f[0])
	}
	return o
}

func descFrame(name string, flags, maxPlayers uint32, password string) writer {
	w := writer{}.bytes(make([]byte, 16)).bytes(app[:]).u32(flags).u32(maxPlayers).u32(1).u32(0).u32(0).u32(0).u32(0)
	return w.str([]byte(name)).str([]byte(password))
}

func mustHandle(t *testing.T, r *Relay, c *conn, frame []byte) {
	t.Helper()
	if err := r.Handle(c.Client, frame); err != nil {
		t.Fatalf("Handle(op %d): %v", frame[0], err)
	}
}

func create(t *testing.T, r *Relay, c *conn, name string, flags, maxPlayers uint32, password string) []byte {
	t.Helper()
	mustHandle(t, r, c, newFrame(OpCreate).u32(1).bytes(descFrame(name, flags, maxPlayers, password)))
	reply := c.last(t)
	if reply[0] != OpOpenReply || u32(reply, 1) != 1 || u32(reply, 5) != DPOK {
		t.Fatalf("create reply % x", reply[:9])
	}
	return reply[9:25]
}

func join(t *testing.T, r *Relay, c *conn, instance []byte, password string) (uint32, []byte) {
	t.Helper()
	mustHandle(t, r, c, newFrame(OpJoin).u32(2).bytes(instance).str([]byte(password)))
	reply := c.last(t)
	if reply[0] != OpOpenReply || u32(reply, 1) != 2 {
		t.Fatalf("join reply % x", reply)
	}
	return u32(reply, 5), reply
}

func addPlayer(t *testing.T, r *Relay, c *conn, name string) uint32 {
	t.Helper()
	mustHandle(t, r, c, newFrame(OpCreatePlayer).u32(3).u32(0).str([]byte(name)).str(nil).blob(nil))
	reply := c.last(t)
	if reply[0] != OpPlayerReply || u32(reply, 1) != 3 || u32(reply, 5) != DPOK {
		t.Fatalf("player reply % x", reply)
	}
	return u32(reply, 9)
}

func enum(t *testing.T, r *Relay, c *conn, application guid, flags uint32) (count uint32, reply []byte) {
	t.Helper()
	mustHandle(t, r, c, newFrame(OpEnum).u32(9).bytes(application[:]).u32(flags))
	reply = c.last(t)
	if reply[0] != OpEnumReply || u32(reply, 1) != 9 {
		t.Fatalf("enum reply % x", reply)
	}
	return u32(reply, 5), reply
}

func send(t *testing.T, r *Relay, c *conn, from, to uint32, data string) {
	t.Helper()
	mustHandle(t, r, c, newFrame(OpSend).u32(from).u32(to).u32(0).blob([]byte(data)))
}

// Ported from the former Python relay tests (tests/test_dplay_relay.py): test_create_enumerate_join_and_messages.
func TestCreateEnumerateJoinAndMessages(t *testing.T) {
	r, host, guest := New(), newConn(), newConn()
	instance := create(t, r, host, "Host Game", 0x44, 8, "")
	hostID := addPlayer(t, r, host, "Host")
	n, reply := enum(t, r, guest, app, 1)
	if n != 1 || !bytes.Equal(reply[9:25], instance) || !bytes.Contains(reply, []byte("Host Game")) {
		t.Fatalf("enum: %d % x", n, reply)
	}
	result, reply := join(t, r, guest, instance, "")
	if result != DPOK || !bytes.Contains(reply, []byte("Host")) {
		t.Fatalf("join: %#x", result) // existing players come with the open reply
	}
	guestID := addPlayer(t, r, guest, "Guest")
	created := host.take()
	if created[0][0] != OpSysCreate || u32(created[0], 1) != guestID {
		t.Fatalf("host not told about guest: % x", created[0])
	}

	// Broadcast reaches everyone but the sender; directed sends reach one player.
	send(t, r, host, hostID, 0, "hello")
	if f := host.take(); len(f) != 0 {
		t.Fatal("broadcast echoed to sender")
	}
	got := guest.take()[0]
	if got[0] != OpReceive || u32(got, 1) != hostID || u32(got, 5) != guestID || !bytes.HasSuffix(got, []byte("hello")) {
		t.Fatalf("receive % x", got)
	}
	send(t, r, guest, guestID, hostID, "ack")
	if f := host.take(); len(f) != 1 || !bytes.HasSuffix(f[0], []byte("ack")) {
		t.Fatal("directed send lost")
	}

	// Senders can only speak for their own players.
	send(t, r, guest, hostID, 0, "forged")
	if n := len(host.take()) + len(guest.take()); n != 0 {
		t.Fatal("forged send delivered")
	}

	// Remote player data (the host's lobby options) becomes DPSYS_SETPLAYERORGROUPDATA elsewhere.
	mustHandle(t, r, host, newFrame(OpSetData).u32(hostID).u32(0).blob([]byte("options")))
	got = guest.take()[0]
	if got[0] != OpSysSetData || u32(got, 1) != hostID || !bytes.HasSuffix(got, []byte("options")) {
		t.Fatalf("set data % x", got)
	}
	// DPSET_LOCAL data stays local.
	mustHandle(t, r, host, newFrame(OpSetData).u32(hostID).u32(SetLocal).blob([]byte("local")))
	if len(guest.take()) != 0 {
		t.Fatal("local data forwarded")
	}
}

// Ported from test_new_players_disabled_and_errors.
func TestNewPlayersDisabledAndErrors(t *testing.T) {
	r, host, guest := New(), newConn(), newConn()
	instance := create(t, r, host, "Game", 0x44, 8, "")
	addPlayer(t, r, host, "Host")
	mustHandle(t, r, host, newFrame(OpEnableNew).u32(0))
	if n, _ := enum(t, r, guest, app, 1); n != 0 {
		t.Fatal("started games are listed")
	}
	if n, _ := enum(t, r, guest, app, EnumSessionsAll); n != 1 {
		t.Fatal("DPENUMSESSIONS_ALL hides started games")
	}
	if res, _ := join(t, r, guest, instance, ""); res != DPErrNoNewPlayers {
		t.Fatalf("join started game: %#x", res)
	}
	if res, _ := join(t, r, guest, make([]byte, 16), ""); res != DPErrNoSessions {
		t.Fatalf("join missing game: %#x", res)
	}
	mustHandle(t, r, host, newFrame(OpEnableNew).u32(1))
	if res, _ := join(t, r, guest, instance, ""); res != DPOK {
		t.Fatalf("join reopened game: %#x", res)
	}
}

// Ported from test_host_migration_and_session_lost.
func TestHostMigrationAndSessionLost(t *testing.T) {
	r, host, guest := New(), newConn(), newConn()
	instance := create(t, r, host, "Game", 0x44, 8, "")
	hostID := addPlayer(t, r, host, "Host")
	join(t, r, guest, instance, "")
	addPlayer(t, r, guest, "Guest")
	host.take()
	r.Disconnect(host.Client)
	frames := guest.take()
	if !bytes.Equal(ops(frames), []byte{OpSysDestroy, OpSysHost}) || u32(frames[0], 1) != hostID {
		t.Fatalf("MIGRATEHOST: %v", ops(frames))
	}

	r, a, b := New(), newConn(), newConn()
	instance = create(t, r, a, "Game", 0, 8, "")
	addPlayer(t, r, a, "A")
	join(t, r, b, instance, "")
	r.Disconnect(a.Client)
	if got := ops(b.take()); !bytes.Equal(got, []byte{OpSysDestroy, OpSysSessionLost}) {
		t.Fatalf("session lost: %v", got)
	}
	if st := r.Stats(); st.Sessions != 0 || len(r.sessions) != 0 {
		t.Fatalf("session not removed: %+v", st)
	}
	if b.session.Load() != nil {
		t.Fatal("guest still in the lost session")
	}
}

func TestEnumOrderAndApplicationFilter(t *testing.T) {
	r, viewer := New(), newConn()
	var instances [][]byte
	for i := range 3 {
		instances = append(instances, create(t, r, newConn(), fmt.Sprint("Game ", i), 0, 8, ""))
	}
	n, reply := enum(t, r, viewer, guid{}, 0) // GUID_NULL lists every application
	if n != 3 {
		t.Fatalf("listed %d", n)
	}
	at := 9
	for i, inst := range instances {
		if !bytes.Equal(reply[at:at+16], inst) {
			t.Fatalf("session %d out of creation order", i)
		}
		at += 16 + 16 + 28
		at += 2 + int(binary.LittleEndian.Uint16(reply[at:]))
		at += 2 + int(binary.LittleEndian.Uint16(reply[at:]))
	}
	other := app
	other[0] = 0xff
	if n, _ := enum(t, r, viewer, other, 0); n != 0 {
		t.Fatal("other application's sessions listed")
	}
}

func TestPasswordAndAlreadyInitialized(t *testing.T) {
	r, host, guest := New(), newConn(), newConn()
	instance := create(t, r, host, "Locked", 0, 8, "secret")
	if res, _ := join(t, r, guest, instance, "wrong"); res != DPErrInvalidPassword {
		t.Fatalf("wrong password: %#x", res)
	}
	if res, _ := join(t, r, guest, instance, "secret"); res != DPOK {
		t.Fatalf("right password: %#x", res)
	}
	if res, _ := join(t, r, guest, instance, "secret"); res != DPErrAlreadyInitialized {
		t.Fatalf("second open: %#x", res)
	}
	mustHandle(t, r, host, newFrame(OpCreate).u32(7).bytes(descFrame("x", 0, 8, "")))
	if reply := host.last(t); u32(reply, 1) != 7 || u32(reply, 5) != DPErrAlreadyInitialized {
		t.Fatalf("create while open: % x", reply)
	}
}

func TestPlayerLimitsAndOutsideSession(t *testing.T) {
	r, c := New(), newConn()
	mustHandle(t, r, c, newFrame(OpCreatePlayer).u32(5).u32(0).str(nil).str(nil).blob(nil))
	if reply := c.last(t); reply[0] != OpPlayerReply || u32(reply, 1) != 5 || u32(reply, 5) != DPErrInvalidParams {
		t.Fatalf("player outside session: % x", reply)
	}
	// Everything else outside a session is ignored, even unknown opcodes.
	for _, op := range []byte{OpDestroyPlayer, OpSend, OpSetData, OpSetName, OpSetDesc, OpEnableNew, OpClose, 99} {
		mustHandle(t, r, c, []byte{op})
	}
	if len(c.take()) != 0 {
		t.Fatal("reply outside session")
	}

	create(t, r, c, "Small", 0, 2, "")
	addPlayer(t, r, c, "1")
	addPlayer(t, r, c, "2")
	mustHandle(t, r, c, newFrame(OpCreatePlayer).u32(6).u32(0).str(nil).str(nil).blob(nil))
	if reply := c.last(t); u32(reply, 5) != DPErrCantAddPlayer {
		t.Fatalf("third player in 2-player game: %#x", u32(reply, 5))
	}
	for maxp, want := range map[uint32]uint32{0: 8, 1: 1, 16: 16, 17: 16, 1000: 16} {
		if got := clampPlayers(maxp); got != want {
			t.Fatalf("clampPlayers(%d) = %d, want %d", maxp, got, want)
		}
	}
}

func TestOwnershipNamesAndDescription(t *testing.T) {
	r, host, guest := New(), newConn(), newConn()
	instance := create(t, r, host, "Game", 0x44, 8, "")
	hostID := addPlayer(t, r, host, "Host")
	join(t, r, guest, instance, "")
	guestID := addPlayer(t, r, guest, "Guest")
	host.take()

	// Only the owner may rename or destroy a player.
	mustHandle(t, r, guest, newFrame(OpSetName).u32(hostID).str([]byte("X")).str(nil))
	mustHandle(t, r, guest, newFrame(OpDestroyPlayer).u32(hostID))
	if len(host.take()) != 0 {
		t.Fatal("guest changed the host's player")
	}
	mustHandle(t, r, guest, newFrame(OpSetName).u32(guestID).str([]byte("New")).str([]byte("Long")))
	if f := host.last(t); f[0] != OpSysSetName || u32(f, 1) != guestID || !bytes.HasSuffix(f, []byte("Long")) {
		t.Fatalf("set name % x", f)
	}

	// Only the host may change the description; others get OP_DESC with trailer 1.
	mustHandle(t, r, guest, newFrame(OpSetDesc).bytes(descFrame("Hijack", 0, 8, "")))
	if len(host.take()) != 0 {
		t.Fatal("guest changed the description")
	}
	mustHandle(t, r, host, newFrame(OpSetDesc).bytes(descFrame("Renamed", 0x44, 4, "")))
	f := guest.last(t)
	if f[0] != OpDesc || f[len(f)-1] != 1 || !bytes.Contains(f, []byte("Renamed")) || u32(f, 1+32+4) != 4 || u32(f, 1+32+8) != 2 {
		t.Fatalf("description % x", f)
	}
	mustHandle(t, r, host, newFrame(OpEnableNew).u32(0))
	if f := guest.last(t); f[0] != OpDesc || f[len(f)-1] != 0 || u32(f, 1+32)&SessionNewPlayersDisabled == 0 {
		t.Fatalf("enable new % x", f)
	}

	// Destroying one's own player tells the others; the ID is free again.
	mustHandle(t, r, guest, newFrame(OpDestroyPlayer).u32(guestID))
	if f := host.last(t); f[0] != OpSysDestroy || u32(f, 1) != guestID {
		t.Fatalf("destroy % x", f)
	}
	if _, used := r.dpids[guestID]; used {
		t.Fatal("destroyed player's ID still reserved")
	}
	// OP_CLOSE leaves like a disconnect.
	mustHandle(t, r, guest, []byte{OpClose})
	if guest.session.Load() != nil {
		t.Fatal("still in session after OP_CLOSE")
	}
}

func TestProtocolErrors(t *testing.T) {
	r, c := New(), newConn()
	for name, frame := range map[string][]byte{
		"empty":            {},
		"too large":        make([]byte, MaxFrame+1),
		"truncated enum":   {OpEnum, 1, 2},
		"truncated create": newFrame(OpCreate).u32(1).bytes(make([]byte, 20)),
		"bad blob length":  nil, // filled below, needs a session
	} {
		if frame == nil {
			continue
		}
		if err := r.Handle(c.Client, frame); !errors.Is(err, ErrProtocol) {
			t.Errorf("%s: err = %v", name, err)
		}
	}
	create(t, r, c, "Game", 0, 8, "")
	id := addPlayer(t, r, c, "P")
	for name, frame := range map[string][]byte{
		"unknown opcode in session": {99},
		"blob longer than frame":    newFrame(OpSend).u32(id).u32(0).u32(0).u32(1 << 30),
		"truncated set name":        newFrame(OpSetName).u32(id).bytes([]byte{5, 0, 'a'}),
	} {
		if err := r.Handle(c.Client, frame); !errors.Is(err, ErrProtocol) {
			t.Errorf("%s: err = %v", name, err)
		}
	}
	if len(c.take()) != 0 {
		t.Fatal("malformed frames produced output")
	}
}

func TestDPIDsAvoidReservedAndUsedValues(t *testing.T) {
	// A random stream that offers 0, 1, 0xffffffff and a used ID before good ones.
	stream := writer{}.bytes(make([]byte, 16)) // session instance
	stream = stream.u32(7)                     // first player
	stream = stream.u32(0).u32(1).u32(0xffffffff).u32(7).u32(8)
	r, c := NewWithRand(bytes.NewReader(stream)), newConn()
	create(t, r, c, "Game", 0, 8, "")
	if a, b := addPlayer(t, r, c, "A"), addPlayer(t, r, c, "B"); a != 7 || b != 8 {
		t.Fatalf("IDs %d, %d", a, b)
	}
}

// Many matches at once: run with -race. Every message must arrive once, in order per sender, and only
// within its own match.
func TestConcurrentMatches(t *testing.T) {
	const matches, players, messages = 24, 4, 200
	r := New()
	type seat struct {
		c  *conn
		id uint32
	}
	games := make([][]seat, matches)
	for m := range games {
		host := newConn()
		instance := create(t, r, host, fmt.Sprint("Match ", m), 0x44, 8, "")
		games[m] = append(games[m], seat{host, addPlayer(t, r, host, "P0")})
		for p := 1; p < players; p++ {
			c := newConn()
			join(t, r, c, instance, "")
			games[m] = append(games[m], seat{c, addPlayer(t, r, c, fmt.Sprint("P", p))})
		}
		for _, s := range games[m] {
			s.c.take()
		}
	}
	var wg sync.WaitGroup
	for m, game := range games {
		for _, s := range game {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for i := range messages {
					payload := binary.LittleEndian.AppendUint32(binary.LittleEndian.AppendUint32(nil, uint32(m)), uint32(i))
					if err := r.Handle(s.c.Client, newFrame(OpSend).u32(s.id).u32(0).u32(0).blob(payload)); err != nil {
						t.Error(err)
						return
					}
				}
			}()
		}
		// Unrelated churn: a lobby that opens and closes while the matches run.
		wg.Add(1)
		go func() {
			defer wg.Done()
			for range 20 {
				c := newConn()
				if err := r.Handle(c.Client, newFrame(OpCreate).u32(1).bytes(descFrame("churn", 0, 8, ""))); err != nil {
					t.Error(err)
				}
				r.Handle(c.Client, newFrame(OpEnum).u32(1).bytes(make([]byte, 16)).u32(EnumSessionsAll))
				r.Disconnect(c.Client)
			}
		}()
	}
	wg.Wait()
	for m, game := range games {
		for _, s := range game {
			next := map[uint32]uint32{}
			for _, f := range s.c.take() {
				if f[0] != OpReceive || u32(f, 5) != s.id {
					t.Fatalf("match %d: unexpected frame % x", m, f[:9])
				}
				from, gm, seq := u32(f, 1), u32(f, 13), u32(f, 17)
				if gm != uint32(m) {
					t.Fatalf("match %d received match %d's message", m, gm)
				}
				if seq != next[from] {
					t.Fatalf("match %d: from %d got #%d, want #%d", m, from, seq, next[from])
				}
				next[from]++
			}
			if len(next) != players-1 {
				t.Fatalf("match %d: heard from %d players", m, len(next))
			}
			for from, n := range next {
				if n != messages {
					t.Fatalf("match %d: %d of %d messages from %d", m, n, messages, from)
				}
			}
		}
	}
	if st := r.Stats(); st.Sessions != matches || st.Players != matches*players {
		t.Fatalf("stats %+v", st)
	}
}
