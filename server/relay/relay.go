package relay

import (
	"crypto/rand"
	"encoding/binary"
	"io"
	"slices"
	"sync"
	"sync/atomic"
)

// Locking. Relay.mu guards the session registry (sessions, order). Session.mu guards one session's
// players, connections and description. Lock order is Relay.mu, then Session.mu, then Relay.idMu (a
// leaf lock for player IDs). Operations inside a session take only that session's lock, so matches don't
// wait on each other. Client.session changes only while both Relay.mu and that session's lock are held.
//
// Sending never blocks: Client.send must queue the frame (see Outbox). Frames are queued while the
// session lock is held, so every client receives a session's frames in the order the relay produced
// them, as it did with a single lock.

// Client is one browser connection.
type Client struct {
	send    func([]byte)
	session atomic.Pointer[Session]
}

// NewClient makes a client whose outgoing frames go to send. send must not block, and must not call
// back into the relay.
func NewClient(send func([]byte)) *Client { return &Client{send: send} }

type player struct {
	dpid        uint32
	client      *Client
	flags       uint32
	short, long []byte
	data        []byte
}

func (p *player) record(w writer) writer {
	return w.u32(p.dpid).u32(p.flags).str(p.short).str(p.long).blob(p.data)
}

// Session is one DirectPlay session (a match and its lobby).
type Session struct {
	mu          sync.Mutex
	instance    guid
	application guid
	flags       uint32
	maxPlayers  uint32
	user        [4]uint32
	name        []byte
	password    []byte
	clients     []*Client // join order; the first one is the host
	players     []*player // creation order
}

func (s *Session) record(w writer) writer {
	w = w.bytes(s.instance[:]).bytes(s.application[:]).u32(s.flags).u32(s.maxPlayers).u32(uint32(len(s.players)))
	for _, v := range s.user {
		w = w.u32(v)
	}
	return w.str(s.name).str(s.password)
}

func (s *Session) acceptsPlayers() bool {
	return s.flags&(SessionNewPlayersDisabled|SessionJoinDisabled) == 0 && uint32(len(s.players)) < s.maxPlayers
}

func (s *Session) player(dpid uint32) *player {
	for _, p := range s.players {
		if p.dpid == dpid {
			return p
		}
	}
	return nil
}

func (s *Session) host() *Client {
	if len(s.clients) == 0 {
		return nil
	}
	return s.clients[0]
}

func (s *Session) others(c *Client, frame []byte) {
	for _, o := range s.clients {
		if o != c {
			o.send(frame)
		}
	}
}

// Relay holds all sessions. The zero value is not usable; call New.
type Relay struct {
	mu       sync.Mutex
	sessions map[guid]*Session
	order    []*Session // creation order, for session lists

	idMu  sync.Mutex
	dpids map[uint32]struct{}
	rand  io.Reader
}

// New makes an empty relay. IDs come from crypto/rand.
func New() *Relay { return NewWithRand(rand.Reader) }

// NewWithRand makes a relay that takes session instances and player IDs from random (tests use a
// deterministic stream). Reads are serialized by the relay.
func NewWithRand(random io.Reader) *Relay {
	return &Relay{sessions: map[guid]*Session{}, dpids: map[uint32]struct{}{}, rand: random}
}

// Stats are counts for monitoring.
type Stats struct {
	Sessions int `json:"sessions"`
	Players  int `json:"players"`
}

// Stats counts the current sessions and players.
func (r *Relay) Stats() Stats {
	r.mu.Lock()
	defer r.mu.Unlock()
	st := Stats{Sessions: len(r.order)}
	for _, s := range r.order {
		s.mu.Lock()
		st.Players += len(s.players)
		s.mu.Unlock()
	}
	return st
}

func (r *Relay) read(b []byte) {
	r.idMu.Lock()
	defer r.idMu.Unlock()
	if _, err := io.ReadFull(r.rand, b); err != nil {
		panic("relay: random source failed: " + err.Error())
	}
}

func (r *Relay) newDPID() uint32 {
	r.idMu.Lock()
	defer r.idMu.Unlock()
	var b [4]byte
	for {
		if _, err := io.ReadFull(r.rand, b[:]); err != nil {
			panic("relay: random source failed: " + err.Error())
		}
		id := binary.LittleEndian.Uint32(b[:])
		if _, used := r.dpids[id]; id != 0 && id != 1 && id != 0xffffffff && !used {
			r.dpids[id] = struct{}{}
			return id
		}
	}
}

func (r *Relay) freeDPID(id uint32) {
	r.idMu.Lock()
	delete(r.dpids, id)
	r.idMu.Unlock()
}

// Handle processes one frame from c. An error means the frame was malformed; the caller then closes the
// connection and calls Disconnect.
func (r *Relay) Handle(c *Client, frame []byte) error {
	if len(frame) == 0 || len(frame) > MaxFrame {
		return ErrProtocol
	}
	op, rd := frame[0], &reader{data: frame[1:]}
	switch op {
	case OpEnum:
		return r.enum(c, rd)
	case OpCreate, OpJoin:
		return r.open(c, op, rd)
	case OpClose:
		r.Disconnect(c)
		return nil
	}
	for {
		s := c.session.Load()
		if s == nil {
			// Not in a session: only CreatePlayer gets an answer; everything else is ignored.
			if op == OpCreatePlayer {
				request := rd.u32()
				if rd.err != nil {
					return rd.err
				}
				c.send(newFrame(OpPlayerReply).u32(request).u32(DPErrInvalidParams).u32(0))
			}
			return nil
		}
		s.mu.Lock()
		if c.session.Load() != s { // left or lost the session meanwhile
			s.mu.Unlock()
			continue
		}
		err := r.inSession(c, s, op, rd)
		s.mu.Unlock()
		return err
	}
}

func (r *Relay) enum(c *Client, rd *reader) error {
	request, application, flags := rd.u32(), rd.guid(), rd.u32()
	if rd.err != nil {
		return rd.err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	var found []byte
	count := uint32(0)
	for _, s := range r.order {
		s.mu.Lock()
		if (application == guid{} || s.application == application) && (flags&EnumSessionsAll != 0 || s.acceptsPlayers()) {
			found = s.record(found)
			count++
		}
		s.mu.Unlock()
	}
	c.send(newFrame(OpEnumReply).u32(request).u32(count).bytes(found))
	return nil
}

func openReply(c *Client, request, result uint32) {
	c.send(newFrame(OpOpenReply).u32(request).u32(result))
}

func (r *Relay) open(c *Client, op byte, rd *reader) error {
	request := rd.u32()
	if rd.err != nil {
		return rd.err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if c.session.Load() != nil {
		openReply(c, request, DPErrAlreadyInitialized)
		return nil
	}
	var s *Session
	if op == OpCreate {
		d := readDesc(rd)
		if rd.err != nil {
			return rd.err
		}
		s = &Session{application: d.application, flags: d.flags, maxPlayers: clampPlayers(d.maxPlayers),
			user: d.user, name: d.name, password: d.password}
		r.read(s.instance[:])
		r.sessions[s.instance] = s
		r.order = append(r.order, s)
		s.mu.Lock()
	} else {
		instance, password := rd.guid(), rd.str()
		if rd.err != nil {
			return rd.err
		}
		s = r.sessions[instance]
		if s == nil {
			openReply(c, request, DPErrNoSessions)
			return nil
		}
		s.mu.Lock()
		if !s.acceptsPlayers() {
			s.mu.Unlock()
			openReply(c, request, DPErrNoNewPlayers)
			return nil
		}
		if len(s.password) > 0 && string(s.password) != string(password) {
			s.mu.Unlock()
			openReply(c, request, DPErrInvalidPassword)
			return nil
		}
	}
	defer s.mu.Unlock()
	s.clients = append(s.clients, c)
	c.session.Store(s)
	w := s.record(newFrame(OpOpenReply).u32(request).u32(DPOK)).u32(uint32(len(s.players)))
	for _, p := range s.players {
		w = p.record(w)
	}
	c.send(w)
	return nil
}

// inSession handles the operations of a client that is in session s; s.mu is held.
func (r *Relay) inSession(c *Client, s *Session, op byte, rd *reader) error {
	switch op {
	case OpCreatePlayer:
		request, flags, short, long, data := rd.u32(), rd.u32(), rd.str(), rd.str(), rd.blob()
		if rd.err != nil {
			return rd.err
		}
		if uint32(len(s.players)) >= s.maxPlayers {
			c.send(newFrame(OpPlayerReply).u32(request).u32(DPErrCantAddPlayer).u32(0))
			return nil
		}
		p := &player{dpid: r.newDPID(), client: c, flags: flags, short: short, long: long, data: data}
		s.players = append(s.players, p)
		c.send(newFrame(OpPlayerReply).u32(request).u32(DPOK).u32(p.dpid))
		s.others(c, p.record(newFrame(OpSysCreate)))
	case OpDestroyPlayer:
		id := rd.u32()
		if rd.err != nil {
			return rd.err
		}
		if p := s.player(id); p != nil && p.client == c {
			r.destroy(s, p)
		}
	case OpSend:
		source, target, _, data := rd.u32(), rd.u32(), rd.u32(), rd.blob()
		if rd.err != nil {
			return rd.err
		}
		if sender := s.player(source); sender == nil || sender.client != c {
			return nil
		}
		for _, p := range s.players {
			if (target == 0 && p.dpid != source) || (target != 0 && p.dpid == target) {
				p.client.send(newFrame(OpReceive).u32(source).u32(p.dpid).blob(data))
			}
		}
	case OpSetData:
		id, flags, data := rd.u32(), rd.u32(), rd.blob()
		if rd.err != nil {
			return rd.err
		}
		if p := s.player(id); p != nil && p.client == c && flags&SetLocal == 0 {
			p.data = data
			s.others(c, newFrame(OpSysSetData).u32(p.dpid).blob(data))
		}
	case OpSetName:
		id, short, long := rd.u32(), rd.str(), rd.str()
		if rd.err != nil {
			return rd.err
		}
		if p := s.player(id); p != nil && p.client == c {
			p.short, p.long = short, long
			s.others(c, newFrame(OpSysSetName).u32(p.dpid).str(short).str(long))
		}
	case OpSetDesc:
		if s.host() == c {
			d := readDesc(rd)
			if rd.err != nil {
				return rd.err
			}
			s.flags, s.maxPlayers, s.user, s.name, s.password = d.flags, clampPlayers(d.maxPlayers), d.user, d.name, d.password
			s.others(c, append(s.record(newFrame(OpDesc)), 1))
		}
	case OpEnableNew:
		if s.host() == c {
			enable := rd.u32()
			if rd.err != nil {
				return rd.err
			}
			if enable != 0 {
				s.flags &^= SessionNewPlayersDisabled
			} else {
				s.flags |= SessionNewPlayersDisabled
			}
			s.others(c, append(s.record(newFrame(OpDesc)), 0))
		}
	default:
		return ErrProtocol
	}
	return nil
}

// destroy removes p from s (s.mu held) and tells the session's other connections.
func (r *Relay) destroy(s *Session, p *player) {
	s.players = slices.DeleteFunc(s.players, func(q *player) bool { return q == p })
	r.freeDPID(p.dpid)
	s.others(p.client, newFrame(OpSysDestroy).u32(p.dpid))
}

// Disconnect removes c from its session: its players are destroyed, and the host's departure migrates
// the host (DPSESSION_MIGRATEHOST) or ends the session for everyone. Safe to call more than once.
func (r *Relay) Disconnect(c *Client) {
	r.mu.Lock()
	defer r.mu.Unlock()
	s := c.session.Load()
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	wasHost := s.host() == c
	for _, p := range slices.Clone(s.players) {
		if p.client == c {
			r.destroy(s, p)
		}
	}
	s.clients = slices.DeleteFunc(s.clients, func(o *Client) bool { return o == c })
	c.session.Store(nil)
	switch {
	case len(s.clients) == 0:
		r.remove(s)
	case wasHost && s.flags&SessionMigrateHost != 0:
		s.clients[0].send(newFrame(OpSysHost))
	case wasHost:
		for _, o := range s.clients {
			o.send(newFrame(OpSysSessionLost))
			o.session.Store(nil)
		}
		for _, p := range s.players {
			r.freeDPID(p.dpid)
		}
		s.clients, s.players = nil, nil
		r.remove(s)
	}
}

func (r *Relay) remove(s *Session) {
	delete(r.sessions, s.instance)
	r.order = slices.DeleteFunc(r.order, func(o *Session) bool { return o == s })
}
