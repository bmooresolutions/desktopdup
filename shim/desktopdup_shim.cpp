#include "desktopdup_shim.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

template <typename T>
void safe_release(T*& ptr) {
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

void clear_result(dd_result* out) {
	if (!out) {
		return;
	}
	out->code = DD_OK;
	out->hr = S_OK;
	out->message[0] = '\0';
}

void set_message(dd_result* out, const char* fmt, ...) {
	if (!out) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	vsnprintf(out->message, sizeof(out->message), fmt, args);
	va_end(args);
	out->message[sizeof(out->message) - 1] = '\0';
}

void set_hresult(dd_result* out, dd_code code, HRESULT hr, const char* context) {
	if (!out) {
		return;
	}
	out->code = code;
	out->hr = static_cast<long>(hr);

	char system_message[384] = {};
	DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	DWORD len = FormatMessageA(flags, nullptr, static_cast<DWORD>(hr), 0, system_message,
	                           static_cast<DWORD>(sizeof(system_message)), nullptr);
	if (len == 0) {
		snprintf(out->message, sizeof(out->message), "%s", context);
		return;
	}

	while (len > 0 && (system_message[len - 1] == '\r' || system_message[len - 1] == '\n' ||
	                   system_message[len - 1] == ' ')) {
		system_message[--len] = '\0';
	}
	snprintf(out->message, sizeof(out->message), "%s: %s", context, system_message);
}

dd_code classify_hresult(HRESULT hr) {
	if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		return DD_TIMEOUT;
	}
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		return DD_ACCESS_LOST;
	}
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		return DD_DEVICE_LOST;
	}
	return DD_ERROR;
}

// Session contains every COM and Direct3D object needed for one duplicated
// output. The struct is exposed to C as an opaque dd_session.
struct Session {
	IDXGIAdapter1* adapter = nullptr;
	IDXGIOutput* output = nullptr;
	IDXGIOutput1* output1 = nullptr;
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	IDXGIOutputDuplication* duplication = nullptr;
	ID3D11Texture2D* staging = nullptr;
	DXGI_OUTPUT_DESC output_desc = {};
	DXGI_OUTDUPL_DESC duplication_desc = {};
	std::vector<uint8_t> buffer;
	int width = 0;
	int height = 0;
	int stride = 0;

	~Session() {
		safe_release(staging);
		safe_release(duplication);
		safe_release(context);
		safe_release(device);
		safe_release(output1);
		safe_release(output);
		safe_release(adapter);
	}

	HRESULT init(int output_index, dd_result* out);
	HRESULT ensure_staging(ID3D11Texture2D* texture);
	HRESULT copy_to_buffer(ID3D11Texture2D* texture, dd_frame* out);
};

HRESULT Session::init(int output_index, dd_result* out) {
	// DXGI factory enumeration is used to locate the requested monitor. We walk
	// adapters and outputs in DXGI order and treat output_index as a flat,
	// zero-based index across all active outputs.
	IDXGIFactory1* factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
	                                reinterpret_cast<void**>(&factory));
	if (FAILED(hr)) {
		set_hresult(out, classify_hresult(hr), hr, "CreateDXGIFactory1 failed");
		return hr;
	}

	int seen_outputs = 0;
	for (UINT adapter_index = 0;; ++adapter_index) {
		IDXGIAdapter1* candidate_adapter = nullptr;
		hr = factory->EnumAdapters1(adapter_index, &candidate_adapter);
		if (hr == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if (FAILED(hr)) {
			safe_release(factory);
			set_hresult(out, classify_hresult(hr), hr, "EnumAdapters1 failed");
			return hr;
		}

		for (UINT output_index_on_adapter = 0;; ++output_index_on_adapter) {
			IDXGIOutput* candidate_output = nullptr;
			hr = candidate_adapter->EnumOutputs(output_index_on_adapter, &candidate_output);
			if (hr == DXGI_ERROR_NOT_FOUND) {
				break;
			}
			if (FAILED(hr)) {
				safe_release(candidate_adapter);
				safe_release(factory);
				set_hresult(out, classify_hresult(hr), hr, "EnumOutputs failed");
				return hr;
			}

			if (seen_outputs == output_index) {
				adapter = candidate_adapter;
				output = candidate_output;
				goto found_output;
			}
			safe_release(candidate_output);
			++seen_outputs;
		}
		safe_release(candidate_adapter);
	}

	safe_release(factory);
	set_message(out, "output index %d was not found; %d output(s) enumerated", output_index,
	            seen_outputs);
	out->code = DD_INVALID_ARGUMENT;
	out->hr = static_cast<long>(DXGI_ERROR_NOT_FOUND);
	return DXGI_ERROR_NOT_FOUND;

found_output:
	safe_release(factory);

	hr = output->GetDesc(&output_desc);
	if (FAILED(hr)) {
		set_hresult(out, classify_hresult(hr), hr, "IDXGIOutput::GetDesc failed");
		return hr;
	}

	hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
	if (FAILED(hr)) {
		set_hresult(out, classify_hresult(hr), hr, "IDXGIOutput1 query failed");
		return hr;
	}

	// Create a Direct3D 11 device on the adapter that owns the target output.
	// BGRA support is requested because the duplicated desktop is normally
	// DXGI_FORMAT_B8G8R8A8_UNORM and many consumers can use that layout
	// directly without channel swizzling.
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL levels[] = {
	    D3D_FEATURE_LEVEL_11_1,
	    D3D_FEATURE_LEVEL_11_0,
	    D3D_FEATURE_LEVEL_10_1,
	    D3D_FEATURE_LEVEL_10_0,
	};
	D3D_FEATURE_LEVEL selected_level = D3D_FEATURE_LEVEL_11_0;
	hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
	                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, &selected_level,
	                       &context);
	if (FAILED(hr)) {
		set_hresult(out, classify_hresult(hr), hr, "D3D11CreateDevice failed");
		return hr;
	}

	// DuplicateOutput creates the Desktop Duplication interface. Windows allows
	// a small number of concurrent duplicators per session; callers should close
	// sessions promptly when done.
	hr = output1->DuplicateOutput(device, reinterpret_cast<IDXGIOutputDuplication**>(&duplication));
	if (FAILED(hr)) {
		set_hresult(out, classify_hresult(hr), hr, "IDXGIOutput1::DuplicateOutput failed");
		return hr;
	}

	duplication->GetDesc(&duplication_desc);
	width = static_cast<int>(duplication_desc.ModeDesc.Width);
	height = static_cast<int>(duplication_desc.ModeDesc.Height);
	stride = width * 4;
	return S_OK;
}

HRESULT Session::ensure_staging(ID3D11Texture2D* texture) {
	D3D11_TEXTURE2D_DESC src_desc = {};
	texture->GetDesc(&src_desc);

	D3D11_TEXTURE2D_DESC current_desc = {};
	if (staging) {
		staging->GetDesc(&current_desc);
	}

	if (staging && current_desc.Width == src_desc.Width && current_desc.Height == src_desc.Height &&
	    current_desc.Format == src_desc.Format) {
		return S_OK;
	}

	// The GPU-owned duplicated image cannot be mapped by the CPU. CopyResource
	// moves it into this staging texture, which is created with CPU read access.
	D3D11_TEXTURE2D_DESC staging_desc = src_desc;
	staging_desc.BindFlags = 0;
	staging_desc.MiscFlags = 0;
	staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	staging_desc.Usage = D3D11_USAGE_STAGING;
	staging_desc.MipLevels = 1;
	staging_desc.ArraySize = 1;
	staging_desc.SampleDesc.Count = 1;
	staging_desc.SampleDesc.Quality = 0;

	ID3D11Texture2D* new_staging = nullptr;
	HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &new_staging);
	if (FAILED(hr)) {
		return hr;
	}

	safe_release(staging);
	staging = new_staging;
	width = static_cast<int>(staging_desc.Width);
	height = static_cast<int>(staging_desc.Height);
	stride = width * 4;
	buffer.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));
	return S_OK;
}

HRESULT Session::copy_to_buffer(ID3D11Texture2D* texture, dd_frame* out) {
	HRESULT hr = ensure_staging(texture);
	if (FAILED(hr)) {
		return hr;
	}

	// GPU copy. This is the unavoidable transfer from the duplicated desktop
	// texture into CPU-readable memory.
	context->CopyResource(staging, texture);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		return hr;
	}

	// DXGI rows may be padded. The public Go API exposes tightly packed rows, so
	// copy row-by-row into a reusable contiguous buffer.
	const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
	uint8_t* dst = buffer.data();
	for (int y = 0; y < height; ++y) {
		memcpy(dst + static_cast<size_t>(y) * stride,
		       src + static_cast<size_t>(y) * mapped.RowPitch, static_cast<size_t>(stride));
	}

	context->Unmap(staging, 0);

	out->width = width;
	out->height = height;
	out->stride = stride;
	out->data = buffer.data();
	out->len = static_cast<uint64_t>(buffer.size());
	return S_OK;
}

}  // namespace

extern "C" dd_session* dd_new_session(int output_index, dd_result* out) {
	clear_result(out);
	if (output_index < 0) {
		if (out) {
			out->code = DD_INVALID_ARGUMENT;
			out->hr = E_INVALIDARG;
			set_message(out, "output index must be non-negative");
		}
		return nullptr;
	}

	std::unique_ptr<Session> session(new Session());
	HRESULT hr = session->init(output_index, out);
	if (FAILED(hr)) {
		return nullptr;
	}
	return reinterpret_cast<dd_session*>(session.release());
}

extern "C" dd_result dd_next_frame(dd_session* raw, uint32_t timeout_ms, dd_frame* out) {
	dd_result result;
	clear_result(&result);
	if (!raw || !out) {
		result.code = DD_INVALID_ARGUMENT;
		result.hr = E_INVALIDARG;
		set_message(&result, "session and output frame pointers must be non-null");
		return result;
	}

	memset(out, 0, sizeof(*out));
	Session* session = reinterpret_cast<Session*>(raw);

	IDXGIResource* desktop_resource = nullptr;
	DXGI_OUTDUPL_FRAME_INFO frame_info = {};

	// AcquireNextFrame blocks up to timeout_ms for a changed desktop frame. The
	// acquired frame must always be paired with ReleaseFrame after successful
	// acquisition, even if later processing fails.
	HRESULT hr = session->duplication->AcquireNextFrame(timeout_ms, &frame_info,
	                                                    &desktop_resource);
	if (FAILED(hr)) {
		set_hresult(&result, classify_hresult(hr), hr,
		            "IDXGIOutputDuplication::AcquireNextFrame failed");
		return result;
	}

	ID3D11Texture2D* texture = nullptr;
	hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D),
	                                      reinterpret_cast<void**>(&texture));
	if (SUCCEEDED(hr)) {
		hr = session->copy_to_buffer(texture, out);
	}
	safe_release(texture);
	safe_release(desktop_resource);

	// ReleaseFrame tells DXGI that this duplicator is done with the acquired
	// frame. Failing to release prevents future frames from being acquired.
	HRESULT release_hr = session->duplication->ReleaseFrame();
	if (FAILED(hr)) {
		set_hresult(&result, classify_hresult(hr), hr, "copy duplicated frame failed");
		return result;
	}
	if (FAILED(release_hr)) {
		set_hresult(&result, classify_hresult(release_hr), release_hr,
		            "IDXGIOutputDuplication::ReleaseFrame failed");
		return result;
	}

	return result;
}

extern "C" void dd_close_session(dd_session* raw) {
	if (!raw) {
		return;
	}

	// Deleting Session releases COM objects in a deterministic order and frees
	// the reusable CPU buffer.
	Session* session = reinterpret_cast<Session*>(raw);
	delete session;
}
