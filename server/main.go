// Command aoe-server serves the browser game (web/) and relays DirectPlay traffic between the players of
// each match over a WebSocket at /api/dplay (see the relay package). The web lobby lists the matches as
// rooms (GET /api/rooms) and checks room passwords (POST /api/rooms/check).
//
//	aoe-server [-host 127.0.0.1] [-port 8788] [-web web]
//	aoe-server healthcheck [-port 8788]
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"mime"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/chiengyn/aoe-wasm/server/relay"
	"github.com/coder/websocket"
)

const (
	maxBacklog   = 32 << 20         // queued bytes per connection before a stalled client is dropped
	writeTimeout = 15 * time.Second // one frame to one client
	pingInterval = 30 * time.Second // keeps idle lobbies alive through proxies
	pingTimeout  = 20 * time.Second
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("aoe-server: ")
	if len(os.Args) > 1 && os.Args[1] == "healthcheck" {
		os.Exit(healthcheck(os.Args[2:]))
	}
	flags := flag.NewFlagSet("aoe-server", flag.ExitOnError)
	host := flags.String("host", "127.0.0.1", "address to listen on; 0.0.0.0 makes the game reachable on the network")
	port := flags.Int("port", defaultPort(), "port to listen on (default from $PORT, else 8788)")
	web := flags.String("web", "web", "folder with the page, the engine build and the prepared game data")
	logRequests := flags.Bool("log-requests", false, "log every file request")
	flags.Parse(os.Args[1:])

	if info, err := os.Stat(*web); err != nil || !info.IsDir() {
		log.Fatalf("web folder %q not found (run from the repository root or pass -web)", *web)
	}
	for _, need := range []string{"engine/game.wasm", "game/native.json"} {
		if _, err := os.Stat(filepath.Join(*web, need)); err != nil {
			log.Printf("warning: %s is missing; the game can't start until it is built/prepared", filepath.Join(*web, need))
		}
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	srv := &http.Server{
		Handler:           newHandler(*web, relay.New(), *logRequests),
		ReadHeaderTimeout: 10 * time.Second,
		BaseContext:       func(net.Listener) context.Context { return ctx },
	}
	listener, err := net.Listen("tcp", net.JoinHostPort(*host, strconv.Itoa(*port)))
	if err != nil {
		log.Fatal(err)
	}
	shown := *host
	if shown == "0.0.0.0" || shown == "" || shown == "::" {
		shown = "127.0.0.1"
	}
	fmt.Printf("Open http://%s\n", net.JoinHostPort(shown, strconv.Itoa(*port)))

	done := make(chan error, 1)
	go func() { done <- srv.Serve(listener) }()
	select {
	case err := <-done:
		log.Fatal(err)
	case <-ctx.Done():
	}
	// WebSocket connections end with ctx; plain requests get a few seconds to finish.
	shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	srv.Shutdown(shutdown)
}

func defaultPort() int {
	if p, err := strconv.Atoi(os.Getenv("PORT")); err == nil && p > 0 {
		return p
	}
	return 8788
}

func healthcheck(args []string) int {
	flags := flag.NewFlagSet("healthcheck", flag.ExitOnError)
	port := flags.Int("port", defaultPort(), "port the server listens on")
	flags.Parse(args)
	client := http.Client{Timeout: 4 * time.Second}
	resp, err := client.Get("http://127.0.0.1:" + strconv.Itoa(*port) + "/healthz")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		fmt.Fprintln(os.Stderr, "status", resp.Status)
		return 1
	}
	return 0
}

// Content types used by the page and the game data, independent of the host's mime.types.
func init() {
	for ext, typ := range map[string]string{
		".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
		".mjs": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
		".json": "application/json", ".wasm": "application/wasm", ".webm": "video/webm",
		".wav": "audio/wav", ".mid": "audio/midi", ".bmp": "image/bmp", ".png": "image/png",
		".jpg": "image/jpeg", ".txt": "text/plain; charset=utf-8",
	} {
		if err := mime.AddExtensionType(ext, typ); err != nil {
			panic(err)
		}
	}
}

type handler struct {
	files       http.Handler
	relay       *relay.Relay
	logRequests bool
	checks      limiter // POST /api/rooms/check, per address

	connections                   atomic.Int64
	framesIn, framesOut, overflow atomic.Uint64
}

// status is what /healthz reports.
type status struct {
	relay.Stats
	Connections int64  `json:"connections"`
	FramesIn    uint64 `json:"frames_in"`  // since start
	FramesOut   uint64 `json:"frames_out"` // since start
	Dropped     uint64 `json:"dropped_slow_clients"`
}

func newHandler(web string, r *relay.Relay, logRequests bool) *handler {
	return &handler{files: http.FileServer(noListing{http.Dir(web)}), relay: r, logRequests: logRequests}
}

func (h *handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch r.URL.Path {
	case "/api/dplay":
		h.dplay(w, r)
		return
	case "/api/rooms":
		h.rooms(w, r)
		return
	case "/api/rooms/check":
		h.roomCheck(w, r)
		return
	case "/healthz":
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(status{Stats: h.relay.Stats(), Connections: h.connections.Load(),
			FramesIn: h.framesIn.Load(), FramesOut: h.framesOut.Load(), Dropped: h.overflow.Load()})
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if h.logRequests {
		log.Printf("%s %s %s", r.RemoteAddr, r.Method, r.URL.RequestURI())
	}
	// Revalidate on every load (304 when unchanged), so a rebuilt engine or re-prepared data is picked up.
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	h.files.ServeHTTP(w, r)
}

// sameOrigin accepts no Origin (non-browser clients) or the page's own origin, over http or https (behind
// a TLS proxy that passes the Host header on).
func sameOrigin(r *http.Request) bool {
	origins := r.Header.Values("Origin")
	if len(origins) == 0 {
		return true
	}
	return len(origins) == 1 && (origins[0] == "http://"+r.Host || origins[0] == "https://"+r.Host)
}

func (h *handler) dplay(w http.ResponseWriter, r *http.Request) {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") || r.Header.Get("Sec-WebSocket-Key") == "" || !sameOrigin(r) {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{CompressionMode: websocket.CompressionDisabled})
	if err != nil {
		return // Accept has answered the request
	}
	defer conn.CloseNow()
	conn.SetReadLimit(relay.MaxFrame)
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	h.connections.Add(1)
	defer h.connections.Add(-1)

	box := newOutbox(maxBacklog)
	client := relay.NewClient(func(frame []byte) {
		h.framesOut.Add(1)
		if !box.push(frame) && box.overflowed() {
			cancel() // too far behind: drop the connection (its players leave the match)
		}
	})
	go func() { // writer
		defer cancel()
		for {
			frames, ok := box.take(ctx.Done())
			if !ok {
				return
			}
			for _, frame := range frames {
				wctx, done := context.WithTimeout(ctx, writeTimeout)
				err := conn.Write(wctx, websocket.MessageBinary, frame)
				done()
				if err != nil {
					return
				}
			}
		}
	}()
	go func() { // keepalive
		ticker := time.NewTicker(pingInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				pctx, done := context.WithTimeout(ctx, pingTimeout)
				err := conn.Ping(pctx)
				done()
				if err != nil {
					cancel()
					return
				}
			}
		}
	}()

	for {
		typ, frame, err := conn.Read(ctx)
		if err != nil {
			break
		}
		if typ != websocket.MessageBinary {
			continue // the game only sends binary frames
		}
		h.framesIn.Add(1)
		if err := h.relay.Handle(client, frame); err != nil {
			break
		}
	}
	h.relay.Disconnect(client)
	box.close()
	if box.overflowed() {
		h.overflow.Add(1)
		log.Printf("dropped %s: outgoing backlog over %d bytes", r.RemoteAddr, maxBacklog)
	}
}

// noListing serves files and folders with an index.html, but no folder listings.
type noListing struct{ fs http.FileSystem }

func (n noListing) Open(name string) (http.File, error) {
	f, err := n.fs.Open(name)
	if err != nil {
		return nil, err
	}
	if info, err := f.Stat(); err == nil && info.IsDir() {
		index, err := n.fs.Open(strings.TrimSuffix(name, "/") + "/index.html")
		if err != nil {
			f.Close()
			return nil, fs.ErrNotExist
		}
		index.Close()
	}
	return f, nil
}
