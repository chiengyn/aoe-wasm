package main

import (
	"encoding/json"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/chiengyn/aoe-wasm/server/relay"
)

// The web lobby (web/index.html) lists rooms with GET /api/rooms and checks a room's password with POST
// /api/rooms/check before the game loads. The relay checks the password again when the game joins.

const (
	checkBurst  = 20               // password checks an address may make at once
	checkRefill = 3 * time.Second  // then one more every refill interval (20 a minute)
	checkIdle   = 10 * time.Minute // limiter entries unused this long are forgotten
)

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func (h *handler) rooms(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"rooms": h.relay.Rooms()})
}

func (h *handler) roomCheck(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	// A JSON body can't be posted cross-site without CORS preflight, but the Origin check keeps other pages
	// from probing passwords through a visitor's browser either way.
	if !sameOrigin(r) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	ip, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		ip = r.RemoteAddr
	}
	if !h.checks.allow(ip, time.Now()) {
		w.Header().Set("Retry-After", "3")
		writeJSON(w, http.StatusTooManyRequests, map[string]string{"result": "slow-down"})
		return
	}
	var req struct {
		ID       string `json:"id"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&req); err != nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	writeJSON(w, http.StatusOK, map[string]relay.RoomCheck{"result": h.relay.CheckRoom(req.ID, req.Password)})
}

// limiter is a token bucket per address.
type limiter struct {
	mu      sync.Mutex
	buckets map[string]*bucket
	swept   time.Time
}

type bucket struct {
	tokens float64
	seen   time.Time
}

func (l *limiter) allow(key string, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.buckets == nil {
		l.buckets = map[string]*bucket{}
	}
	if now.Sub(l.swept) > checkIdle {
		for k, b := range l.buckets {
			if now.Sub(b.seen) > checkIdle {
				delete(l.buckets, k)
			}
		}
		l.swept = now
	}
	b := l.buckets[key]
	if b == nil {
		b = &bucket{tokens: checkBurst, seen: now}
		l.buckets[key] = b
	}
	b.tokens = min(checkBurst, b.tokens+float64(now.Sub(b.seen))/float64(checkRefill))
	b.seen = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}
