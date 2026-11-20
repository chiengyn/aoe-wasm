package main

import "sync"

// outbox queues frames for one connection without blocking the relay. A writer goroutine drains it. If a
// client stops reading and its backlog passes maxBytes, the outbox overflows and the connection is
// dropped, so one stalled browser can't hold up its match or grow the server's memory without bound.
type outbox struct {
	mu       sync.Mutex
	frames   [][]byte
	bytes    int
	maxBytes int
	closed   bool
	overflow bool
	ready    chan struct{} // capacity 1: "frames were added or the outbox closed"
}

func newOutbox(maxBytes int) *outbox {
	return &outbox{maxBytes: maxBytes, ready: make(chan struct{}, 1)}
}

func (o *outbox) signal() {
	select {
	case o.ready <- struct{}{}:
	default:
	}
}

// push queues a frame; it never blocks. It reports false once the outbox is closed or has overflowed.
func (o *outbox) push(frame []byte) bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return false
	}
	if o.bytes+len(frame) > o.maxBytes {
		o.closed, o.overflow, o.frames, o.bytes = true, true, nil, 0
		o.signal()
		return false
	}
	o.frames = append(o.frames, frame)
	o.bytes += len(frame)
	o.signal()
	return true
}

// take returns every queued frame, waiting for one if none is queued. ok is false once the outbox is
// closed and empty.
func (o *outbox) take(done <-chan struct{}) (frames [][]byte, ok bool) {
	for {
		o.mu.Lock()
		if len(o.frames) > 0 {
			frames, o.frames, o.bytes = o.frames, nil, 0
			o.mu.Unlock()
			return frames, true
		}
		if o.closed {
			o.mu.Unlock()
			return nil, false
		}
		o.mu.Unlock()
		select {
		case <-o.ready:
		case <-done:
			return nil, false
		}
	}
}

func (o *outbox) close() {
	o.mu.Lock()
	o.closed = true
	o.mu.Unlock()
	o.signal()
}

func (o *outbox) overflowed() bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.overflow
}
