# desktopdup

`desktopdup` is a Go-native package for high-performance Windows screen capture
using the Desktop Duplication API (`IDXGIOutputDuplication`). It exposes a small,
idiomatic Go API while keeping Direct3D 11, DXGI, and COM details inside a
minimal C++ shim compiled through cgo.

## Architecture Overview

The package has two layers:

1. **Go API (`package desktopdup`)**
   - Exposes `Session`, `Frame`, `Options`, and Go `error` values.
   - Serializes access to the native capture session with a mutex.
   - Converts native result codes and `HRESULT`s into meaningful Go errors.
   - Copies each captured frame into Go-owned memory so callers never hold a
     dangling pointer to native memory.

2. **C++ shim (`shim/desktopdup_shim.cpp`)**
   - Enumerates DXGI adapters and outputs to find the requested monitor.
   - Creates an ID3D11Device and ID3D11DeviceContext on the adapter that owns the
     selected output.
   - Calls `IDXGIOutput1::DuplicateOutput` to create the duplication session.
   - Uses `AcquireNextFrame` to obtain the next changed desktop frame.
   - Copies the GPU texture into a reusable staging texture with CPU read access.
   - Maps the staging texture and copies padded GPU rows into a contiguous BGRA
     buffer.
   - Releases each acquired frame and cleans up COM resources through WRL
     `ComPtr`.

The C ABI is deliberately tiny: create a session, get the next frame, close the
session. All DirectX objects remain hidden behind an opaque `dd_session`.

## Frame Representation

Frames are returned as:

```go
type Frame struct {
    Width       int
    Height      int
    Stride      int
    PixelFormat PixelFormat
    Data        []byte
}
```

`Data` is tightly packed BGRA, four bytes per pixel, with
`len(Data) == Stride * Height`. `Stride` is currently `Width * 4`.

The C++ shim maintains a reusable native buffer for each session. Go copies that
buffer into Go-owned memory before returning a frame. This single copy is the
main safety trade-off: it keeps the public API idiomatic and cgo-safe while still
avoiding repeated native allocations. Use `NextFrameInto` to reuse the Go-side
buffer and minimize garbage in hot capture loops.

## Resource Lifetime

Create a session with `NewSession` or `NewSessionWithOptions`, then call
`Close` when finished:

```go
s, err := desktopdup.NewSession(0)
if err != nil {
    return err
}
defer s.Close()
```

`Close` releases the duplication object, Direct3D device/context, staging
texture, and native frame buffer. `Close` is idempotent. A finalizer is also set
as a last resort, but production code should always close sessions explicitly.

## Error Handling

Common errors:

- `ErrTimeout`: no changed frame arrived within the requested timeout.
- `ErrAccessLost`: DXGI invalidated the duplication interface. This commonly
  happens after display mode changes, monitor hot-plug events, GPU reset, or
  desktop switches. Create a new `Session` to recover.
- `ErrClosed`: capture was attempted after `Close`.

Other failures include the underlying `HRESULT` and a Windows system message
where available.

## Concurrency

`Session` is safe for concurrent use by multiple goroutines. Capture calls are
serialized internally because Desktop Duplication requires an acquire/copy/release
sequence. For highest throughput, use one capture goroutine per session and pass
frames to consumers through channels.

## Prerequisites

- Windows 8 or newer.
- A GPU and driver supporting Direct3D 11 and DXGI output duplication.
- Go with cgo enabled.
- A C++ compiler compatible with cgo on Windows, such as MSVC Build Tools or a
  MinGW-w64 toolchain that can link `d3d11`, `dxgi`, and `ole32`.

## Minimal Example

```go
package main

import (
    "errors"
    "fmt"
    "log"

    "example.com/desktopdup"
)

func main() {
    session, err := desktopdup.NewSession(0)
    if err != nil {
        log.Fatal(err)
    }
    defer session.Close()

    frame, err := session.NextFrame()
    if errors.Is(err, desktopdup.ErrTimeout) {
        fmt.Println("no desktop changes yet")
        return
    }
    if err != nil {
        log.Fatal(err)
    }

    fmt.Printf("%dx%d %s %d bytes\n",
        frame.Width, frame.Height, frame.PixelFormat, len(frame.Data))
}
```

Run the included example from this module on Windows:

```powershell
go run ./cmd/capture
```

## Build Notes

This package uses build tags:

- `windows && cgo`: full Desktop Duplication implementation.
- all other platforms or cgo disabled: compile-time stubs that return
  `ErrUnsupported`.

If you publish the module, replace `example.com/desktopdup` in `go.mod` and the
example import path with your real module path.
