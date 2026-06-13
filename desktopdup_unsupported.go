//go:build !windows || !cgo

package desktopdup

// Session represents one Desktop Duplication capture session.
//
// On unsupported platforms this type exists so packages can compile, but
// constructors return ErrUnsupported.
type Session struct{}

// NewSessionWithOptions returns ErrUnsupported on platforms other than Windows
// with cgo enabled.
func NewSessionWithOptions(opts Options) (*Session, error) {
	return nil, ErrUnsupported
}

// NextFrame returns ErrUnsupported on platforms other than Windows with cgo
// enabled.
func (s *Session) NextFrame() (*Frame, error) {
	return nil, ErrUnsupported
}

// NextFrameWithTimeout returns ErrUnsupported on platforms other than Windows
// with cgo enabled.
func (s *Session) NextFrameWithTimeout(timeoutMS uint32) (*Frame, error) {
	return nil, ErrUnsupported
}

// NextFrameInto returns ErrUnsupported on platforms other than Windows with cgo
// enabled.
func (s *Session) NextFrameInto(dst []byte) (*Frame, error) {
	return nil, ErrUnsupported
}

// Close is a no-op on unsupported platforms.
func (s *Session) Close() error {
	return nil
}
