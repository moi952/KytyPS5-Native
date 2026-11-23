#include "common/abi.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace Libs::LibGen5::VrrStatus {

LIB_VERSION("VideoOutVrrStatus", 1, "VideoOut", 1, 1);

// Trace of what a game does with VRR: each call's caller (to find it in the game), its arguments and,
// for the status read, the bytes the game gets. The first calls of each function only.
static bool VrrTrace(std::atomic_int& count) {
	return count.fetch_add(1, std::memory_order_relaxed) < 40;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus_kP2L8t3j_aM() {
	// The observed guest call passes no arguments.
	// Return success for Kyty's fixed-refresh path.
	static std::atomic_int calls {0};
	if (VrrTrace(calls)) {
		std::fprintf(stderr, "vrr: init (kP2L8t3j-aM) called from %p\n", __builtin_return_address(0));
	}
	return OK;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus_LibwuIonIBw(LibKernel::EventQueue::KernelEqueue eq,
                                                       int handle, void* udata) {
	PRINT_NAME();

	LOGF("\t eq     = 0x%016" PRIx64 "\n", static_cast<uint64_t>(eq));
	LOGF("\t handle = %d\n", handle);
	LOGF("\t udata  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(udata));

	static std::atomic_int calls {0};
	if (VrrTrace(calls)) {
		std::fprintf(stderr, "vrr: add status event (LibwuIonIBw) eq 0x%" PRIx64 " handle %d udata %p, from %p\n",
		             static_cast<uint64_t>(eq), handle, udata, __builtin_return_address(0));
	}

	// Fixed-refresh output has no VRR status changes to signal.
	return OK;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus__q_vbO859Tw(LibKernel::EventQueue::KernelEqueue eq,
                                                       int                                 handle) {
	PRINT_NAME();

	LOGF("\t eq     = 0x%016" PRIx64 "\n", static_cast<uint64_t>(eq));
	LOGF("\t handle = %d\n", handle);

	static std::atomic_int calls {0};
	if (VrrTrace(calls)) {
		std::fprintf(stderr, "vrr: delete status event (-q-vbO859Tw) eq 0x%" PRIx64 " handle %d, from %p\n",
		             static_cast<uint64_t>(eq), handle, __builtin_return_address(0));
	}

	return OK;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus_gWT7X8H0bYs(int handle, void* status) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);
	LOGF("\t status = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(status));

	if (status == nullptr) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	const int result = VideoOut::VideoOutGetOutputStatus(
	    handle, static_cast<VideoOut::VideoOutOutputStatus*>(status));
	if (result != OK) {
		return result;
	}

	// The guest's private status layout extends the 0x30-byte output status
	// with an 8-byte VRR range. GetOutputStatus clears flags, including bit 4 (VRR off).
	const uint64_t ext = VideoOut::VrrKnob("ext");
	std::memcpy(static_cast<uint8_t*>(status) + 0x30, &ext, 0x08);

	static std::atomic_int calls {0};
	if (VrrTrace(calls)) {
		const auto* b = static_cast<const uint8_t*>(status);
		char        hex[0x38 * 3 + 1] = {};
		for (int i = 0; i < 0x38; i++) {
			std::snprintf(hex + i * 3, 4, "%02x ", b[i]);
		}
		std::fprintf(stderr, "vrr: status read (gWT7X8H0bYs) handle %d into %p, from %p: %s\n", handle, status,
		             __builtin_return_address(0), hex);
	}
	return OK;
}

LIB_DEFINE(InitVideoOutVrrStatus_1) {
	LIB_FUNC("kP2L8t3j-aM", VideoOutVrrStatus_kP2L8t3j_aM);
	LIB_FUNC("LibwuIonIBw", VideoOutVrrStatus_LibwuIonIBw);
	LIB_FUNC("-q-vbO859Tw", VideoOutVrrStatus__q_vbO859Tw);
	LIB_FUNC("gWT7X8H0bYs", VideoOutVrrStatus_gWT7X8H0bYs);
}

} // namespace Libs::LibGen5::VrrStatus
