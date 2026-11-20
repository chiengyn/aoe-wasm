package relay

import (
	"encoding/hex"
	"strings"
	"unicode/utf8"
)

// Room is a session as the web lobby lists it (GET /api/rooms). Names are decoded from Windows-1252.
type Room struct {
	ID         string `json:"id"` // session instance GUID, hex
	Name       string `json:"name"`
	Host       string `json:"host"` // the first player's name; empty until the host's player exists
	Players    int    `json:"players"`
	MaxPlayers int    `json:"maxPlayers"`
	Locked     bool   `json:"locked"` // a password is needed to join
	Open       bool   `json:"open"`   // accepts players (false when full or once the match started)
}

// Rooms lists the sessions in creation order.
func (r *Relay) Rooms() []Room {
	r.mu.Lock()
	defer r.mu.Unlock()
	rooms := make([]Room, 0, len(r.order))
	for _, s := range r.order {
		s.mu.Lock()
		name, host := splitTitle(DecodeCP1252(s.name))
		room := Room{ID: hex.EncodeToString(s.instance[:]), Name: name, Host: host, Players: len(s.players),
			MaxPlayers: int(s.maxPlayers), Locked: len(s.password) > 0, Open: s.acceptsPlayers()}
		if len(s.players) > 0 {
			room.Host = DecodeCP1252(s.players[0].short)
		}
		s.mu.Unlock()
		rooms = append(rooms, room)
	}
	return rooms
}

// splitTitle splits the session name the game makes, "<game name> [<host's name>]"
// (TCommunications_Handler::SetGameTitle), so the lobby shows the room's own name.
func splitTitle(title string) (name, host string) {
	if at := strings.LastIndex(title, " ["); at > 0 && strings.HasSuffix(title, "]") {
		return title[:at], title[at+2 : len(title)-1]
	}
	return title, ""
}

// RoomCheck is the outcome of CheckRoom.
type RoomCheck string

const (
	RoomOK            RoomCheck = "ok"
	RoomWrongPassword RoomCheck = "wrong-password"
	RoomClosed        RoomCheck = "closed" // full, or the match has started
	RoomGone          RoomCheck = "gone"
)

// CheckRoom tells whether a player with this password could join room id now. The relay checks again when
// the game joins (OpJoin); this lets the lobby answer before the game loads.
func (r *Relay) CheckRoom(id, password string) RoomCheck {
	var instance guid
	if b, err := hex.DecodeString(id); err != nil || len(b) != len(instance) {
		return RoomGone
	} else {
		copy(instance[:], b)
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	s := r.sessions[instance]
	if s == nil {
		return RoomGone
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.password) > 0 {
		given, ok := EncodeCP1252(password)
		if !ok || string(given) != string(s.password) {
			return RoomWrongPassword
		}
	}
	if !s.acceptsPlayers() {
		return RoomClosed
	}
	return RoomOK
}

// Windows-1252 0x80-0x9F; the other bytes are the Unicode code points of the same value. 0 marks the
// five undefined bytes, which decode as U+FFFD.
var cp1252High = [32]rune{
	0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
	0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
}

// DecodeCP1252 converts the game's Windows-1252 text to UTF-8.
func DecodeCP1252(b []byte) string {
	var out strings.Builder
	for _, c := range b {
		switch {
		case c < 0x80 || c >= 0xA0:
			out.WriteRune(rune(c))
		case cp1252High[c-0x80] != 0:
			out.WriteRune(cp1252High[c-0x80])
		default:
			out.WriteRune(utf8.RuneError)
		}
	}
	return out.String()
}

// EncodeCP1252 converts UTF-8 to Windows-1252, as the lobby page does for the game. ok is false when a
// character has no Windows-1252 byte.
func EncodeCP1252(s string) (b []byte, ok bool) {
	for _, c := range s {
		switch {
		case c < 0x80 || c >= 0xA0 && c < 0x100:
			b = append(b, byte(c))
		default:
			i := -1
			for j, h := range cp1252High {
				if h != 0 && h == c {
					i = j
					break
				}
			}
			if i < 0 {
				return nil, false
			}
			b = append(b, byte(0x80+i))
		}
	}
	return b, true
}
