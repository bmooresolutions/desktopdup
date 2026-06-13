//go:build windows && cgo

package desktopdup

/*
#cgo CXXFLAGS: -std=c++17
#cgo LDFLAGS: -ld3d11 -ldxgi -lole32
#include <stdlib.h>
#include "shim/desktopdup_shim.h"
*/
import "C"

import (
	"fmt"
	"runtime"
	"sync"
	"unsafe"
)

// Session represents one Desktop Duplication capture session.
//
// A Session is safe for concurrent use by multiple goroutines. Capture calls
// are serialized because IDXGIOutputDuplication requires a strict
// acquire/copy/release sequence. Close may be called concurrently with capture
// methods.
type Session struct {
	mu        sync.Mutex
	handle    *C.dd_session
	timeoutMS uint32
	closed    bool
}

// NewSessionWithOptions creates a Desktop Duplication session using opts.
//
// The underlying C++ shim creates a Direct3D 11 device on the adapter that owns
// the requested output, asks DXGI for an IDXGIOutputDuplication, and prepares a
// reusable CPU staging texture for frame readback.
func NewSessionWithOptions(opts Options) (*Session, error) {
	if opts.OutputIndex < 0 {
		return nil, fmt.Errorf("desktopdup: output index must be non-negative")
	}
	timeout := opts.TimeoutMS
	if timeout == 0 {
		timeout = DefaultTimeoutMS
	}

	var res C.dd_result
	handle := C.dd_new_session(C.int(opts.OutputIndex), &res)
	if handle == nil {
		return nil, resultError("create session", &res)
	}

	s := &Session{handle: handle, timeoutMS: timeout}
	runtime.SetFinalizer(s, (*Session).finalize)
	return s, nil
}

// NextFrame captures the next available frame using the session default
// timeout.
//
// The returned Frame owns its Data buffer. Callers that capture in a tight loop
// should prefer NextFrameInto to reuse an existing buffer and reduce garbage.
func (s *Session) NextFrame() (*Frame, error) {
	return s.NextFrameWithTimeout(s.timeoutMS)
}

// NextFrameWithTimeout captures the next available frame, waiting up to
// timeoutMS milliseconds for desktop content to change.
func (s *Session) NextFrameWithTimeout(timeoutMS uint32) (*Frame, error) {
	return s.nextFrame(timeoutMS, nil)
}

// NextFrameInto captures the next available frame into dst.
//
// If dst has enough capacity, its backing array is reused; otherwise a new
// slice is allocated. The returned Frame.Data is the slice containing the frame
// bytes.
func (s *Session) NextFrameInto(dst []byte) (*Frame, error) {
	return s.nextFrame(s.timeoutMS, dst)
}

// Close releases the DXGI duplication object, Direct3D device/context, staging
// texture, and internal frame buffer.
//
// Close is idempotent. After Close, capture methods return ErrClosed.
func (s *Session) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.closeLocked()
}

func (s *Session) finalize() {
	_ = s.Close()
}

func (s *Session) closeLocked() error {
	if s.closed {
		return nil
	}
	s.closed = true
	runtime.SetFinalizer(s, nil)
	if s.handle != nil {
		C.dd_close_session(s.handle)
		s.handle = nil
	}
	return nil
}

func (s *Session) nextFrame(timeoutMS uint32, dst []byte) (*Frame, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.closed || s.handle == nil {
		return nil, ErrClosed
	}

	var cframe C.dd_frame
	res := C.dd_next_frame(s.handle, C.uint32_t(timeoutMS), &cframe)
	if res.code != C.DD_OK {
		return nil, resultError("capture frame", &res)
	}

	n := int(cframe.len)
	if cap(dst) < n {
		dst = make([]byte, n)
	} else {
		dst = dst[:n]
	}

	// The shim buffer belongs to the C++ Session and is reused on the next
	// capture. Copy once into Go memory so the returned Frame is safe under cgo
	// pointer rules and remains valid independently of the session.
	src := unsafe.Slice((*byte)(unsafe.Pointer(cframe.data)), n)
	copy(dst, src)

	return &Frame{
		Width:       int(cframe.width),
		Height:      int(cframe.height),
		Stride:      int(cframe.stride),
		PixelFormat: PixelFormatBGRA,
		Data:        dst,
	}, nil
}

func resultError(op string, res *C.dd_result) error {
	switch res.code {
	case C.DD_TIMEOUT:
		return ErrTimeout
	case C.DD_ACCESS_LOST:
		return ErrAccessLost
	case C.DD_DEVICE_LOST:
		return fmt.Errorf("desktopdup: %s: device lost: %s", op, cMessage(res))
	case C.DD_INVALID_ARGUMENT:
		return fmt.Errorf("desktopdup: %s: invalid argument: %s", op, cMessage(res))
	default:
		msg := cMessage(res)
		if msg == "" {
			return fmt.Errorf("desktopdup: %s failed with HRESULT 0x%08x", op, uint32(res.hr))
		}
		return fmt.Errorf("desktopdup: %s failed with HRESULT 0x%08x: %s", op, uint32(res.hr), msg)
	}
}

func cMessage(res *C.dd_result) string {
	return C.GoString(&res.message[0])
}
