// Package desktopdup provides a Go-native wrapper around the Windows Desktop
// Duplication API (DXGI Output Duplication).
//
// The package captures monitor frames through Direct3D 11 and returns pixels as
// Go-owned byte slices in BGRA order. The DirectX and COM work is isolated in a
// small C++ shim, while this package exposes ordinary Go types, methods, and
// errors.
package desktopdup

import "errors"

// PixelFormat identifies the byte layout used by Frame.Data.
type PixelFormat string

const (
	// PixelFormatBGRA is 32-bit BGRA with 8 bits per channel.
	//
	// Desktop Duplication commonly exposes the desktop as
	// DXGI_FORMAT_B8G8R8A8_UNORM, which maps naturally to this layout.
	PixelFormatBGRA PixelFormat = "BGRA"
)

var (
	// ErrClosed is returned when a method is called after Session.Close.
	ErrClosed = errors.New("desktopdup: session is closed")

	// ErrTimeout is returned by NextFrame and NextFrameInto when no new frame is
	// available before the configured timeout expires.
	ErrTimeout = errors.New("desktopdup: frame acquisition timed out")

	// ErrAccessLost is returned when Windows invalidates the duplication
	// interface, usually because of a display mode change, monitor hot-plug, GPU
	// reset, or desktop switch. Create a new Session to continue capturing.
	ErrAccessLost = errors.New("desktopdup: output duplication access lost")

	// ErrUnsupported is returned on platforms where Desktop Duplication is not
	// available.
	ErrUnsupported = errors.New("desktopdup: desktop duplication is only supported on Windows with cgo")
)

// Frame contains one captured desktop image.
//
// Data is tightly packed: len(Data) == Stride * Height, and each pixel uses
// four bytes in PixelFormat order. The byte slice is owned by Go and remains
// valid after the next capture call.
type Frame struct {
	Width       int
	Height      int
	Stride      int
	PixelFormat PixelFormat
	Data        []byte
}

// Options configures a capture Session.
type Options struct {
	// OutputIndex selects the monitor to capture. Index 0 is normally the
	// primary monitor, followed by the remaining active outputs reported by
	// DXGI adapter enumeration.
	OutputIndex int

	// TimeoutMS is the default timeout used by NextFrame. If zero, a practical
	// default of 16 ms is used, which suits low-latency capture loops.
	TimeoutMS uint32
}

// DefaultTimeoutMS is the timeout used by NewSession and NextFrame when the
// caller does not provide one.
const DefaultTimeoutMS uint32 = 16

// NewSession starts capturing the monitor identified by outputIndex.
//
// It is a convenience wrapper around NewSessionWithOptions. Output index 0 is
// usually the primary monitor.
func NewSession(outputIndex int) (*Session, error) {
	return NewSessionWithOptions(Options{OutputIndex: outputIndex})
}
