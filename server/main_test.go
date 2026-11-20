package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/chiengyn/aoe-wasm/server/relay"
	"github.com/coder/websocket"
)

func testServer(t *testing.T) *httptest.Server {
	t.Helper()
	web := t.TempDir()
	for name, body := range map[string]string{
		"index.html":        "<!doctype html>index",
		"native.html":       "<!doctype html>game",
		"native-worker.mjs": "export {}",
		"engine/game.wasm":  "\x00asm",
		"game/native.json":  "{}",
		"game/video/a.webm": "webm",
		"shims/x.mjs":       "export {}",
	} {
		path := filepath.Join(web, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(path), 0o755)
		if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	srv := httptest.NewServer(newHandler(web, relay.New(), false))
	t.Cleanup(srv.Close)
	return srv
}

func TestStaticFiles(t *testing.T) {
	srv := testServer(t)
	for path, want := range map[string]string{
		"/":                  "text/html; charset=utf-8",
		"/native.html":       "text/html; charset=utf-8",
		"/native-worker.mjs": "text/javascript; charset=utf-8",
		"/engine/game.wasm":  "application/wasm",
		"/game/native.json":  "application/json",
		"/game/video/a.webm": "video/webm",
	} {
		resp, err := http.Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != 200 || resp.Header.Get("Content-Type") != want {
			t.Errorf("%s: %d %q, want %q", path, resp.StatusCode, resp.Header.Get("Content-Type"), want)
		}
		if resp.Header.Get("Cache-Control") != "no-cache" || resp.Header.Get("X-Content-Type-Options") != "nosniff" {
			t.Errorf("%s: headers %v", path, resp.Header)
		}
	}
	// Revalidation answers 304.
	resp, _ := http.Get(srv.URL + "/engine/game.wasm")
	resp.Body.Close()
	req, _ := http.NewRequest("GET", srv.URL+"/engine/game.wasm", nil)
	req.Header.Set("If-Modified-Since", resp.Header.Get("Last-Modified"))
	if resp, _ := http.DefaultClient.Do(req); resp.StatusCode != http.StatusNotModified {
		t.Errorf("revalidation: %d", resp.StatusCode)
	}
	// No folder listings, no other methods, nothing outside web/.
	for path, want := range map[string]int{"/game/": 404, "/shims/": 404, "/missing.js": 404, "/../main.go": 404} {
		resp, err := http.Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != want || bytes.Contains(body, []byte("native.json")) {
			t.Errorf("%s: %d", path, resp.StatusCode)
		}
	}
	if resp, _ := http.Post(srv.URL+"/native.html", "text/plain", nil); resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("POST: %d", resp.StatusCode)
	}
	resp, _ = http.Get(srv.URL + "/healthz")
	var st status
	if json.NewDecoder(resp.Body).Decode(&st) != nil || resp.StatusCode != 200 {
		t.Errorf("healthz %d", resp.StatusCode)
	}
	resp.Body.Close()
}

func wsURL(srv *httptest.Server) string {
	return "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/dplay"
}

func TestWebSocketOrigin(t *testing.T) {
	srv := testServer(t)
	host := strings.TrimPrefix(srv.URL, "http://")
	for origin, ok := range map[string]bool{"": true, "http://" + host: true, "https://" + host: true,
		"https://evil.example": false, "http://evil.example": false, "null": false} {
		h := http.Header{}
		if origin != "" {
			h.Set("Origin", origin)
		}
		c, resp, err := websocket.Dial(context.Background(), wsURL(srv), &websocket.DialOptions{HTTPHeader: h})
		if (err == nil) != ok {
			t.Errorf("origin %q: err %v", origin, err)
		}
		if err == nil {
			c.CloseNow()
		} else if resp != nil && resp.StatusCode != http.StatusBadRequest {
			t.Errorf("origin %q: status %d", origin, resp.StatusCode)
		}
	}
	// A plain GET of the endpoint is refused.
	if resp, _ := http.Get(srv.URL + "/api/dplay"); resp.StatusCode != http.StatusBadRequest {
		t.Errorf("plain GET: %d", resp.StatusCode)
	}
}

type wsClient struct {
	t *testing.T
	c *websocket.Conn
}

func dial(t *testing.T, srv *httptest.Server) *wsClient {
	t.Helper()
	c, _, err := websocket.Dial(context.Background(), wsURL(srv), nil)
	if err != nil {
		t.Fatal(err)
	}
	c.SetReadLimit(relay.MaxFrame + 64)
	t.Cleanup(func() { c.CloseNow() })
	return &wsClient{t, c}
}

func (w *wsClient) send(frame []byte) {
	w.t.Helper()
	if err := w.c.Write(context.Background(), websocket.MessageBinary, frame); err != nil {
		w.t.Fatal(err)
	}
}

func (w *wsClient) recv() []byte {
	w.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, b, err := w.c.Read(ctx)
	if err != nil {
		w.t.Fatal(err)
	}
	return b
}

type fb []byte

func (b fb) u32(v uint32) fb { return binary.LittleEndian.AppendUint32(b, v) }
func (b fb) str(s string) fb {
	return append(binary.LittleEndian.AppendUint16(b, uint16(len(s))), s...)
}
func (b fb) blob(s []byte) fb { return append(b.u32(uint32(len(s))), s...) }

var testApp = bytes.Repeat([]byte{7}, 16)

func desc() fb {
	return append(fb(make([]byte, 16)), testApp...).u32(0x44).u32(8).u32(0).u32(0).u32(0).u32(0).u32(0).str("Game").str("")
}

func TestWebSocketMatch(t *testing.T) {
	srv := testServer(t)
	host, guest := dial(t, srv), dial(t, srv)
	host.send(append(fb{relay.OpCreate}.u32(1), desc()...))
	reply := host.recv()
	if reply[0] != relay.OpOpenReply || binary.LittleEndian.Uint32(reply[5:]) != 0 {
		t.Fatalf("create % x", reply)
	}
	instance := reply[9:25]
	host.send(fb{relay.OpCreatePlayer}.u32(2).u32(0).str("Host").str("").blob(nil))
	hostID := binary.LittleEndian.Uint32(host.recv()[9:])

	// Text frames are ignored; the next binary frame is still answered.
	guest.c.Write(context.Background(), websocket.MessageText, []byte("hello"))
	guest.send(append(fb{relay.OpJoin}.u32(3), instance...).str(""))
	if r := guest.recv(); r[0] != relay.OpOpenReply || binary.LittleEndian.Uint32(r[5:]) != 0 {
		t.Fatalf("join % x", r)
	}
	guest.send(fb{relay.OpCreatePlayer}.u32(4).u32(0).str("Guest").str("").blob(nil))
	guestID := binary.LittleEndian.Uint32(guest.recv()[9:])
	if r := host.recv(); r[0] != relay.OpSysCreate {
		t.Fatalf("host told % x", r)
	}

	big := bytes.Repeat([]byte{0xab}, 300_000) // fragmented by the WebSocket layer, delivered whole
	for i := range 50 {
		host.send(fb{relay.OpSend}.u32(hostID).u32(0).u32(0).blob(append([]byte{byte(i)}, big[:i*1000]...)))
	}
	for i := range 50 {
		r := guest.recv()
		if r[0] != relay.OpReceive || binary.LittleEndian.Uint32(r[1:]) != hostID || binary.LittleEndian.Uint32(r[5:]) != guestID || r[13] != byte(i) || len(r) != 14+i*1000 {
			t.Fatalf("message %d: % x (len %d)", i, r[:14], len(r))
		}
	}

	// A malformed frame closes the sender's connection; its player leaves the match.
	guest.send([]byte{relay.OpSend, 1, 2})
	if r := host.recv(); r[0] != relay.OpSysDestroy || binary.LittleEndian.Uint32(r[1:]) != guestID {
		t.Fatalf("after malformed frame % x", r)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, _, err := guest.c.Read(ctx); err == nil {
		t.Fatal("malformed sender still connected")
	}

	// So does a frame over the 1 MiB limit.
	third := dial(t, srv)
	third.send(append(fb{relay.OpJoin}.u32(5), instance...).str(""))
	third.recv()
	third.send(fb{relay.OpCreatePlayer}.u32(6).u32(0).str("Third").str("").blob(nil))
	thirdID := binary.LittleEndian.Uint32(third.recv()[9:])
	host.recv() // SYS_CREATE
	third.c.Write(context.Background(), websocket.MessageBinary, make([]byte, relay.MaxFrame+1))
	if r := host.recv(); r[0] != relay.OpSysDestroy || binary.LittleEndian.Uint32(r[1:]) != thirdID {
		t.Fatalf("after oversized frame % x", r)
	}
}

func TestOutbox(t *testing.T) {
	o := newOutbox(10)
	if !o.push([]byte("12345")) || !o.push([]byte("678")) {
		t.Fatal("push within limit failed")
	}
	frames, ok := o.take(nil)
	if !ok || len(frames) != 2 {
		t.Fatalf("take %v %v", frames, ok)
	}
	if o.push(make([]byte, 11)) || !o.overflowed() {
		t.Fatal("overflow not detected")
	}
	if o.push([]byte("x")) {
		t.Fatal("push after overflow accepted")
	}
	if _, ok := o.take(nil); ok {
		t.Fatal("take after overflow returned frames")
	}
	// take waits for a push, and returns when done is closed.
	o = newOutbox(100)
	go func() { time.Sleep(20 * time.Millisecond); o.push([]byte("late")) }()
	if frames, ok := o.take(nil); !ok || string(frames[0]) != "late" {
		t.Fatal("take did not wait")
	}
	done := make(chan struct{})
	close(done)
	if _, ok := o.take(done); ok {
		t.Fatal("take ignored done")
	}
	o.close()
	if o.push([]byte("x")) || o.overflowed() {
		t.Fatal("closed outbox accepted a frame or reported overflow")
	}
}
