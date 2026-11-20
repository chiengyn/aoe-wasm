// Command loadtest plays many simulated matches against a running server's DirectPlay relay and checks
// every message: delivered once, in order per sender, only within its own match. It reports delivery
// rate and latency.
//
//	go run ./loadtest -url ws://127.0.0.1:8788/api/dplay -matches 50 -players 8 -rate 20 -duration 30s
package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	"github.com/chiengyn/aoe-wasm/server/relay"
	"github.com/coder/websocket"
)

type fb []byte

func (b fb) u32(v uint32) fb { return binary.LittleEndian.AppendUint32(b, v) }
func (b fb) str(s string) fb {
	return append(binary.LittleEndian.AppendUint16(b, uint16(len(s))), s...)
}
func (b fb) blob(s []byte) fb { return append(b.u32(uint32(len(s))), s...) }

var app = bytes.Repeat([]byte{0x5a}, 16)

type result struct {
	Matches        int     `json:"matches"`
	Players        int     `json:"players_per_match"`
	Connections    int     `json:"connections"`
	RatePerPlayer  float64 `json:"messages_per_second_per_player"`
	Size           int     `json:"payload_bytes"`
	Sent           int64   `json:"sent"`
	Expected       int64   `json:"expected_deliveries"`
	Delivered      int64   `json:"delivered"`
	OutOfOrder     int64   `json:"out_of_order"`
	WrongMatch     int64   `json:"wrong_match"`
	Errors         int64   `json:"connection_errors"`
	SendSeconds    float64 `json:"send_seconds"`
	DeliveriesPerS float64 `json:"deliveries_per_second"`
	LatencyP50ms   float64 `json:"latency_p50_ms"`
	LatencyP95ms   float64 `json:"latency_p95_ms"`
	LatencyP99ms   float64 `json:"latency_p99_ms"`
	LatencyMaxms   float64 `json:"latency_max_ms"`
	SetupSeconds   float64 `json:"setup_seconds"`
	Passed         bool    `json:"passed"`
}

type stats struct {
	sent, delivered, outOfOrder, wrongMatch, errors atomic.Int64
	mu                                              sync.Mutex
	latencies                                       []time.Duration
}

type seat struct {
	conn    *websocket.Conn
	id      uint32
	control chan []byte // replies other than OP_RECEIVE
	next    map[uint32]uint32
	got     atomic.Int64
	lat     []time.Duration
	done    chan struct{} // closed when read returns
}

func main() {
	url := flag.String("url", "ws://127.0.0.1:8788/api/dplay", "relay WebSocket URL")
	matches := flag.Int("matches", 20, "concurrent matches")
	players := flag.Int("players", 8, "players per match")
	rate := flag.Float64("rate", 20, "messages per second per player (0: as fast as possible)")
	duration := flag.Duration("duration", 20*time.Second, "how long players send")
	size := flag.Int("size", 64, "payload bytes per message (at least 16)")
	flag.Parse()
	*size = max(*size, 16)

	st := &stats{}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	setupStart := time.Now()
	games := make([][]*seat, *matches)
	var setup sync.WaitGroup
	setupErr := make(chan error, *matches)
	for m := range games {
		setup.Add(1)
		go func() {
			defer setup.Done()
			g, err := openMatch(ctx, *url, m, *players, st)
			if err != nil {
				setupErr <- fmt.Errorf("match %d: %w", m, err)
				return
			}
			games[m] = g
		}()
	}
	setup.Wait()
	close(setupErr)
	for err := range setupErr {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	setupTime := time.Since(setupStart)

	// Every player broadcasts for the duration.
	start := time.Now()
	var senders sync.WaitGroup
	for m, g := range games {
		for _, s := range g {
			senders.Add(1)
			go func() {
				defer senders.Done()
				var tick *time.Ticker
				if *rate > 0 {
					tick = time.NewTicker(time.Duration(float64(time.Second) / *rate))
					defer tick.Stop()
				}
				payload := make([]byte, *size)
				for seq := uint32(0); time.Since(start) < *duration; seq++ {
					if tick != nil {
						<-tick.C
					}
					binary.LittleEndian.PutUint32(payload[0:], uint32(m))
					binary.LittleEndian.PutUint32(payload[4:], seq)
					binary.LittleEndian.PutUint64(payload[8:], uint64(time.Now().UnixNano()))
					frame := fb{relay.OpSend}.u32(s.id).u32(0).u32(0).blob(payload)
					if err := s.conn.Write(ctx, websocket.MessageBinary, frame); err != nil {
						st.errors.Add(1)
						return
					}
					st.sent.Add(1)
				}
			}()
		}
	}
	senders.Wait()
	sendTime := time.Since(start)
	expected := st.sent.Load() * int64(*players-1)
	deadline := time.Now().Add(30 * time.Second)
	for st.delivered.Load() < expected && time.Now().Before(deadline) && st.errors.Load() == 0 {
		time.Sleep(20 * time.Millisecond)
	}
	drain := time.Since(start)
	cancel()

	for _, g := range games {
		for _, s := range g {
			s.conn.CloseNow()
			<-s.done
			st.latencies = append(st.latencies, s.lat...)
		}
	}
	slices.Sort(st.latencies)
	pct := func(p float64) float64 {
		if len(st.latencies) == 0 {
			return 0
		}
		return float64(st.latencies[min(len(st.latencies)-1, int(p*float64(len(st.latencies))))]) / 1e6
	}
	res := result{Matches: *matches, Players: *players, Connections: *matches * *players, RatePerPlayer: *rate, Size: *size,
		Sent: st.sent.Load(), Expected: expected, Delivered: st.delivered.Load(), OutOfOrder: st.outOfOrder.Load(),
		WrongMatch: st.wrongMatch.Load(), Errors: st.errors.Load(), SendSeconds: sendTime.Seconds(),
		DeliveriesPerS: float64(st.delivered.Load()) / drain.Seconds(),
		LatencyP50ms:   pct(0.50), LatencyP95ms: pct(0.95), LatencyP99ms: pct(0.99), LatencyMaxms: pct(1),
		SetupSeconds: setupTime.Seconds()}
	res.Passed = res.Delivered == res.Expected && res.OutOfOrder == 0 && res.WrongMatch == 0 && res.Errors == 0
	out, _ := json.MarshalIndent(res, "", "  ")
	fmt.Println(string(out))
	if !res.Passed {
		os.Exit(1)
	}
}

func openMatch(ctx context.Context, url string, m, players int, st *stats) ([]*seat, error) {
	var seats []*seat
	var instance []byte
	for p := range players {
		conn, _, err := websocket.Dial(ctx, url, &websocket.DialOptions{CompressionMode: websocket.CompressionDisabled})
		if err != nil {
			return nil, err
		}
		conn.SetReadLimit(relay.MaxFrame + 64)
		s := &seat{conn: conn, control: make(chan []byte, 16), next: map[uint32]uint32{}, done: make(chan struct{})}
		go s.read(ctx, m, st)
		if p == 0 {
			desc := append(fb(make([]byte, 16)), app...).u32(0x44).u32(uint32(players)).u32(0).u32(0).u32(0).u32(0).u32(0).
				str(fmt.Sprint("Load ", m)).str("")
			if err := conn.Write(ctx, websocket.MessageBinary, append(fb{relay.OpCreate}.u32(1), desc...)); err != nil {
				return nil, err
			}
		} else if err := conn.Write(ctx, websocket.MessageBinary, append(fb{relay.OpJoin}.u32(1), instance...).str("")); err != nil {
			return nil, err
		}
		reply, err := s.wait(relay.OpOpenReply)
		if err != nil {
			return nil, err
		}
		if res := binary.LittleEndian.Uint32(reply[5:]); res != 0 {
			return nil, fmt.Errorf("open: %#x", res)
		}
		if p == 0 {
			instance = append([]byte{}, reply[9:25]...)
		}
		if err := conn.Write(ctx, websocket.MessageBinary, fb{relay.OpCreatePlayer}.u32(2).u32(0).str(fmt.Sprint("P", p)).str("").blob(nil)); err != nil {
			return nil, err
		}
		reply, err = s.wait(relay.OpPlayerReply)
		if err != nil {
			return nil, err
		}
		s.id = binary.LittleEndian.Uint32(reply[9:])
		seats = append(seats, s)
	}
	return seats, nil
}

func (s *seat) wait(op byte) ([]byte, error) {
	timeout := time.After(15 * time.Second)
	for {
		select {
		case f := <-s.control:
			if f[0] == op {
				return f, nil
			}
		case <-timeout:
			return nil, fmt.Errorf("no reply %d", op)
		}
	}
}

func (s *seat) read(ctx context.Context, m int, st *stats) {
	defer close(s.done)
	for {
		_, f, err := s.conn.Read(ctx)
		if err != nil {
			if ctx.Err() == nil {
				st.errors.Add(1)
			}
			return
		}
		if f[0] != relay.OpReceive {
			if f[0] == relay.OpOpenReply || f[0] == relay.OpPlayerReply {
				s.control <- f
			}
			continue
		}
		from, payload := binary.LittleEndian.Uint32(f[1:]), f[13:]
		if int(binary.LittleEndian.Uint32(payload[0:])) != m {
			st.wrongMatch.Add(1)
		}
		seq := binary.LittleEndian.Uint32(payload[4:])
		if seq != s.next[from] {
			st.outOfOrder.Add(1)
		}
		s.next[from] = seq + 1
		s.lat = append(s.lat, time.Since(time.Unix(0, int64(binary.LittleEndian.Uint64(payload[8:])))))
		st.delivered.Add(1)
	}
}
