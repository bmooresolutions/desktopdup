// Command capture demonstrates the desktopdup package by capturing a handful of
// frames from the primary monitor.
package main

import (
	"errors"
	"fmt"
	"log"
	"time"

	"example.com/desktopdup"
)

func main() {
	// Output index 0 is normally the primary monitor. Additional monitors are
	// exposed in the order returned by DXGI adapter/output enumeration.
	session, err := desktopdup.NewSessionWithOptions(desktopdup.Options{
		OutputIndex: 0,
		TimeoutMS:   100,
	})
	if err != nil {
		log.Fatalf("create capture session: %v", err)
	}
	defer func() {
		if err := session.Close(); err != nil {
			log.Printf("close capture session: %v", err)
		}
	}()

	// Reuse the Go-side frame buffer across captures. The native shim also
	// reuses its staging texture and CPU buffer, so the steady-state hot path is
	// one GPU readback plus one native-to-Go copy.
	var reuse []byte
	for i := 0; i < 10; i++ {
		frame, err := session.NextFrameInto(reuse)
		if errors.Is(err, desktopdup.ErrTimeout) {
			fmt.Println("timeout waiting for desktop changes")
			continue
		}
		if errors.Is(err, desktopdup.ErrAccessLost) {
			log.Fatal("display configuration changed; recreate the session to continue")
		}
		if err != nil {
			log.Fatalf("capture frame: %v", err)
		}
		reuse = frame.Data

		preview := 16
		if len(frame.Data) < preview {
			preview = len(frame.Data)
		}
		fmt.Printf("frame %02d: %dx%d stride=%d format=%s first=% x\n",
			i+1, frame.Width, frame.Height, frame.Stride, frame.PixelFormat,
			frame.Data[:preview])

		time.Sleep(16 * time.Millisecond)
	}
}
