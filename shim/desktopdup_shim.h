#ifndef DESKTOPDUP_SHIM_H
#define DESKTOPDUP_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// dd_session is intentionally opaque to Go. The C++ implementation owns the
// COM interfaces, Direct3D resources, and reusable CPU buffer.
typedef struct dd_session dd_session;

typedef enum dd_code {
	DD_OK = 0,
	DD_ERROR = 1,
	DD_TIMEOUT = 2,
	DD_ACCESS_LOST = 3,
	DD_DEVICE_LOST = 4,
	DD_INVALID_ARGUMENT = 5
} dd_code;

typedef struct dd_result {
	dd_code code;
	long hr;
	char message[512];
} dd_result;

typedef struct dd_frame {
	int width;
	int height;
	int stride;
	uint8_t* data;
	uint64_t len;
} dd_frame;

dd_session* dd_new_session(int output_index, dd_result* out);
dd_result dd_next_frame(dd_session* session, uint32_t timeout_ms, dd_frame* out);
void dd_close_session(dd_session* session);

#ifdef __cplusplus
}
#endif

#endif
