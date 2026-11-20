// Package relay is the DirectPlay session relay for the browser service provider
// (engine/platform/win32_dplay.cpp, web/native-dplay.mjs).
//
// Each browser game is one connection (a Client). The relay keeps what a DirectPlay session host and service
// provider kept: sessions, their players and player data. It routes the game's messages between players and
// never looks inside them.
//
// Wire format: one opcode byte, then little-endian fields. Strings are a u16 length + bytes (Windows-1252),
// blobs a u32 length + bytes. A session description is instance GUID (16) + application GUID (16) + flags,
// max players, current players and 4 user u32s + name string + password string.
package relay

import (
	"encoding/binary"
	"errors"
)

// Client -> relay.
const (
	OpEnum          = 1
	OpCreate        = 2
	OpJoin          = 3
	OpCreatePlayer  = 4
	OpDestroyPlayer = 5
	OpSend          = 6
	OpSetData       = 7
	OpSetName       = 8
	OpSetDesc       = 9
	OpEnableNew     = 10
	OpClose         = 11
)

// Relay -> client.
const (
	OpEnumReply      = 101
	OpOpenReply      = 102
	OpPlayerReply    = 104
	OpDesc           = 110
	OpSysCreate      = 120
	OpSysDestroy     = 121
	OpSysSetData     = 122
	OpSysSetName     = 123
	OpSysHost        = 124
	OpSysSessionLost = 125
	OpReceive        = 130
)

// DirectPlay results.
const (
	DPOK                    = 0
	DPErrInvalidParams      = 0x80070057
	DPErrAlreadyInitialized = 0x88770005
	DPErrAccessDenied       = 0x88770014
	DPErrCantAddPlayer      = 0x88770028
	DPErrInvalidPlayer      = 0x88770096
	DPErrNoSessions         = 0x887700DC
	DPErrNoNewPlayers       = 0x8877014A
	DPErrInvalidPassword    = 0x88770154
)

// DirectPlay flags.
const (
	SessionNewPlayersDisabled = 0x1
	SessionMigrateHost        = 0x4
	SessionJoinDisabled       = 0x20
	EnumSessionsAll           = 0x2
	SetLocal                  = 0x1
)

// MaxFrame is the largest frame a client may send (and the WebSocket read limit).
const MaxFrame = 1 << 20

// ErrProtocol is returned for malformed frames; the connection is then closed.
var ErrProtocol = errors.New("relay: protocol error")

type guid [16]byte

type reader struct {
	data []byte
	at   int
	err  error
}

func (r *reader) take(n int) []byte {
	if r.err != nil {
		return nil
	}
	if n < 0 || r.at+n > len(r.data) {
		r.err = ErrProtocol
		return nil
	}
	chunk := r.data[r.at : r.at+n]
	r.at += n
	return chunk
}

func (r *reader) u32() uint32 {
	b := r.take(4)
	if b == nil {
		return 0
	}
	return binary.LittleEndian.Uint32(b)
}

func (r *reader) guid() (g guid) {
	copy(g[:], r.take(16))
	return g
}

// str and blob copy, so stored values never alias the caller's frame buffer.
func (r *reader) str() []byte {
	b := r.take(2)
	if b == nil {
		return nil
	}
	return clone(r.take(int(binary.LittleEndian.Uint16(b))))
}

func (r *reader) blob() []byte {
	n := r.u32()
	if r.err != nil {
		return nil
	}
	if uint64(n) > uint64(len(r.data)-r.at) {
		r.err = ErrProtocol
		return nil
	}
	return clone(r.take(int(n)))
}

func clone(b []byte) []byte {
	if b == nil {
		return nil
	}
	return append([]byte{}, b...)
}

type writer []byte

func newFrame(op byte) writer { return writer{op} }

func (w writer) u32(v uint32) writer { return binary.LittleEndian.AppendUint32(w, v) }

func (w writer) bytes(b []byte) writer { return append(w, b...) }

func (w writer) str(b []byte) writer {
	if len(b) > 0xffff {
		b = b[:0xffff]
	}
	return append(binary.LittleEndian.AppendUint16(w, uint16(len(b))), b...)
}

func (w writer) blob(b []byte) writer { return append(w.u32(uint32(len(b))), b...) }

// desc is a session description as sent by the game (OP_CREATE, OP_SET_DESC).
type desc struct {
	application guid
	flags       uint32
	maxPlayers  uint32
	user        [4]uint32
	name        []byte
	password    []byte
}

func readDesc(r *reader) (d desc) {
	r.take(16) // instance: chosen by the relay
	d.application = r.guid()
	d.flags = r.u32()
	d.maxPlayers = r.u32()
	r.u32() // current players: counted by the relay
	for i := range d.user {
		d.user[i] = r.u32()
	}
	d.name = r.str()
	d.password = r.str()
	return d
}

// clampPlayers maps the game's max-player count to 1..16 (0 means 8).
func clampPlayers(n uint32) uint32 {
	if n == 0 {
		n = 8
	}
	return max(1, min(n, 16))
}
