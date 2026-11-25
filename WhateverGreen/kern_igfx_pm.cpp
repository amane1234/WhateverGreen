//
//  kern_igfx_pm.cpp
//  WhateverGreen
//
//  Created by Pb on 22/06/2020.
//  Copyright © 2020 vit9696. All rights reserved.
//

/*
* Portions Copyright © 2013 Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice (including the next
* paragraph) shall be included in all copies or substantial portions of the
* Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include <IOKit/IOService.h>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_disasm.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <mach/clock.h>
#include <kern/thread_call.h>

#include "kern_igfx.hpp"
#include "kern_igfx_kexts.hpp"

namespace {
constexpr const char* log = "igfx_pm";

// For debugging
struct [[gnu::packed]] IGHwCsDesc {
  char type;
  char gap[4];
  char *title;
  char unk0[48];
  char unk1[12];
};

constexpr uint32_t MCHBAR_MIRROR_BASE_SNB = 0x140000;
constexpr uint32_t GEN6_RP_STATE_CAP = MCHBAR_MIRROR_BASE_SNB + 0x5998;

constexpr uint32_t GEN6_RC_CONTROL = 0xA090;
constexpr uint32_t GEN6_RC_CTL_RC6_ENABLE = 1U << 18;
constexpr uint32_t GEN9_PG_ENABLE = 0xA210;
constexpr uint32_t GEN6_GT_GFX_RC6 = 0x138108;

/**
 *  RC6 configuration registers in the order i915 (gen9_rc6_enable) programs them.
 *  RC_CONTROL and PG_ENABLE come last, after the parameters they depend on.
 */
constexpr uint32_t RC6_REGISTERS[] {
	0xA09C, // GEN6_RC6_WAKE_RATE_LIMIT
	0xA0A8, // GEN6_RC_EVALUATION_INTERVAL
	0xA0AC, // GEN6_RC_IDLE_HYSTERSIS
	0xA0B0, // GEN6_RC_SLEEP
	0xA0B8, // GEN6_RC6_THRESHOLD
	0xA0C4, // GEN9_MEDIA_PG_IDLE_HYSTERESIS
	0xA0C8, // GEN9_RENDER_PG_IDLE_HYSTERESIS
	GEN6_RC_CONTROL,
	GEN9_PG_ENABLE,
};

enum RC6Event : uint32_t {
	RC6EventSnapshot  = 1U << 0,
	RC6EventRestored  = 1U << 1,
	RC6EventGiveUp    = 1U << 2,
	RC6EventNever     = 1U << 3,
	RC6EventStall     = 1U << 4,
	RC6EventTelemetry = 1U << 5,
};

/**
 *  AppleIntelFramebufferController::doPMWork commands that set the minimum GPU frequency (GuC mode),
 *  sent from AGPM attributes ' g2f' and ' g3f'. The argument points to the floor in 50 MHz units.
 */
constexpr uint32_t PMWORK_GT2_FLOOR = 6;
constexpr uint32_t PMWORK_GT3_FLOOR = 7;

constexpr uint32_t GEN9_FREQUENCY_SHIFT = 23;
constexpr uint32_t GEN9_FREQ_SCALER  = 3;

constexpr uint32_t FORCEWAKE_KERNEL_FALLBACK = 1 << 15;

constexpr uint32_t FORCEWAKE_ACK_TIMEOUT_MS = 50;

constexpr uint32_t FORCEWAKE_MEDIA_GEN9 = 0xa270;
constexpr uint32_t FORCEWAKE_RENDER_GEN9 = 0xa278;
constexpr uint32_t FORCEWAKE_BLITTER_GEN9 = 0xa188;

constexpr uint32_t FORCEWAKE_ACK_MEDIA_GEN9 = 0x0D88;
constexpr uint32_t FORCEWAKE_ACK_RENDER_GEN9 = 0x0D84;
constexpr uint32_t FORCEWAKE_ACK_BLITTER_GEN9 = 0x130044;

enum FORCEWAKE_DOM_BITS : unsigned {
	DOM_RENDER = 0b001,
	DOM_MEDIA = 0b010,
	DOM_BLITTER = 0b100,
	DOM_LAST = DOM_BLITTER,
	DOM_FIRST = DOM_RENDER
};

constexpr uint32_t regForDom(unsigned d) {
	if (d == DOM_RENDER)
		return FORCEWAKE_RENDER_GEN9;
	if (d == DOM_MEDIA)
		return FORCEWAKE_MEDIA_GEN9;
	if (d == DOM_BLITTER)
		return FORCEWAKE_BLITTER_GEN9;
	assertf(false, "Unknown force wake domain %d", d);
	return 0;
}

constexpr uint32_t ackForDom(unsigned d) {
	if (d == DOM_RENDER)
		return FORCEWAKE_ACK_RENDER_GEN9;
	if (d == DOM_MEDIA)
		return FORCEWAKE_ACK_MEDIA_GEN9;
	if (d == DOM_BLITTER)
		return FORCEWAKE_ACK_BLITTER_GEN9;
	assertf(false, "Unknown force wake domain %d", d);
	return 0;
}

constexpr uint32_t masked_field(uint32_t mask, uint32_t value) {
	return (mask << 16) | value;
}

constexpr uint32_t fw_set(uint32_t v) {
	return masked_field(v, v);
}

constexpr uint32_t fw_clear(uint32_t v) {
	return masked_field(v, 0);
}
}

// MARK: - RPS Control Patch

void IGFX::RPSControlPatch::init() {
	// We need to patch both drivers
	requiresPatchingGraphics = true;
	requiresPatchingFramebuffer = true;
	
	// Requires access to global framebuffer controllers
	requiresGlobalFramebufferControllersAccess = true;
	
	// Requires read access to MMIO registers
	requiresMMIORegistersReadAccess = true;
}

void IGFX::RPSControlPatch::publish(const char *key, uint32_t value) {
	if (igpu)
		igpu->setProperty(key, value, 32);
}

void IGFX::RPSControlPatch::publish(const char *key, const char *value) {
	if (igpu)
		igpu->setProperty(key, value);
}

void IGFX::RPSControlPatch::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	uint32_t rpsc = 0;
	if (PE_parse_boot_argn("igfxrpsc", &rpsc, sizeof(rpsc)) ||
		WIOKit::getOSDataValue(info->videoBuiltin, "rps-control", rpsc)) {
		enabled = rpsc > 0 && available;
		mode = rpsc == 0 ? 0 : (rpsc >= 2 ? 2 : 1);
		DBGLOG("weg", "RPS control patch overriden (%u) availabile %d", rpsc, available);
	}

	// The RC6 monitor runs in the same pmNotifyWrapper hook, with or without RPS control
	rc6Fix = available && (checkKernelArgument("-igfxrc6fix") ||
		(info->videoBuiltin && info->videoBuiltin->getProperty("enable-rc6-fix") != nullptr));
	diagnostics = (callbackIGFX->powerOptions & (Diagnose | RespectEPP)) != 0;
	if (diagnostics) {
		nanoseconds_to_absolutetime(5000000000ULL, &telemetryInterval);
		if (callbackIGFX->powerOptions & RespectEPP) {
			uint32_t features = 0, ecx = 0;
			if (CPUInfo::getCpuid(1, 0, nullptr, nullptr, &ecx) && !(ecx & (1U << 31)) &&
				CPUInfo::getCpuid(6, 0, &features) && (features & (1U << 7)) && (features & (1U << 10))) {
				readMsr = reinterpret_cast<decltype(readMsr)>(patcher.solveSymbol(KernelPatcher::KernelID, "_rdmsr_carefully"));
				if (!readMsr)
					patcher.clearError();
			}
		}
	}
	if (rc6Fix || diagnostics) {
		enabled = true;
		requiresMMIORegistersWriteAccess = rc6Fix;
		if (rc6Fix)
			nanoseconds_to_absolutetime(1000ULL * 1000 * 1000, &rc6CheckInterval);
		else
			rc6CheckInterval = telemetryInterval;
	}

	// Submissions closer together than this mean the GPU is under sustained load
	nanoseconds_to_absolutetime(8 * 1000 * 1000, &busyInterval);

	// With Apple GuC (igfxfw=2) AGPM sets a minimum frequency (e.g. 500 MHz) that the host driver ignores
	// otherwise. igfxgtfloor=MHz caps it; any value at or below the hardware minimum (RPn) gives RPn.
	uint32_t floorMHz = 0;
	if (available && (PE_parse_boot_argn("igfxgtfloor", &floorMHz, sizeof(floorMHz)) ||
		WIOKit::getOSDataValue(info->videoBuiltin, "gt-floor-freq", floorMHz)) && floorMHz > 0) {
		gtFloorCap = min(floorMHz / 50 + static_cast<uint32_t>(floorMHz % 50 != 0), 255U);
		enabled = true;
		SYSLOG(log, "GT floor capped at %u MHz", gtFloorCap * 50);
	}

	if (enabled) {
		if ((gtFloorCap || rc6Fix || diagnostics) && info->videoBuiltin && !igpu) {
			igpu = info->videoBuiltin;
			igpu->retain();
		}
		if (gtFloorCap) {
			publish("weg-gt-floor-cap", gtFloorCap * 50);
			publish("weg-gt-floor-status", "waiting for AGPM floor request");
		}
		if (info->videoBuiltin)
			info->videoBuiltin->setProperty("weg-rps-control-mode", mode, 32);
		if (diagnostics)
			publish("weg-power-status", "waiting for GPU notifications");
		if (rc6Fix)
			publish("weg-rc6-status", "monitoring");
	}
}

void IGFX::RPSControlPatch::deinit() {
	__atomic_fetch_or(&rc6Sampler, 2U, __ATOMIC_ACQ_REL);
	while (__atomic_load_n(&rc6Sampler, __ATOMIC_ACQUIRE) & 1)
		IOSleep(1);
	if (rc6LogCall) {
		auto call = static_cast<thread_call_t>(rc6LogCall);
		// No more producers. Wait for the sole consumer to finish using
		// the registry entry before releasing it; this also works on XNU
		// versions without thread_call_cancel_wait.
		for (;;) {
			auto irq = IOSimpleLockLockDisableInterrupt(logLock);
			bool scheduled = logScheduled;
			IOSimpleLockUnlockEnableInterrupt(logLock, irq);
			if (!scheduled)
				break;
			IOSleep(1);
		}
		while (!thread_call_free(call))
			IOSleep(1);
		rc6LogCall = nullptr;
	}
	if (logLock) {
		IOSimpleLockFree(logLock);
		logLock = nullptr;
	}
	OSSafeReleaseNULL(igpu);
}

uint32_t IGFX::RPSControlPatch::readCaps(void *controller) {
	uint32_t cached = __atomic_load_n(&rpCaps, __ATOMIC_RELAXED);
	if (cached || !controller || !callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32)
		return cached;
	const auto value = callbackIGFX->readRegister32(controller, GEN6_RP_STATE_CAP);
	const auto rp0 = value & 0xFF;
	const auto rpn = (value >> 16) & 0xFF;
	if (value == UINT32_MAX || !rpn || rp0 < rpn || rp0 > (UINT32_MAX >> GEN9_FREQUENCY_SHIFT) / GEN9_FREQ_SCALER)
		return 0;
	__atomic_compare_exchange_n(&rpCaps, &cached, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
	return cached ? cached : value;
}

bool IGFX::RPSControlPatch::sustainedSubmissions(uint64_t now) {
	// A pair of submissions in the same frame is common even at idle.
	// Require short gaps spanning a full sampling window. Out-of-order
	// observations must not move the clock backward or create a boost.
	if (__atomic_exchange_n(&samplingLoad, true, __ATOMIC_ACQUIRE))
		return false;
	bool boost = false;
	if (now > lastNotify) {
		if (!lastNotify || now - lastNotify > busyInterval)
			burstStart = now;
		else
			boost = now - burstStart >= 2 * busyInterval;
		lastNotify = now;
	}
	__atomic_store_n(&samplingLoad, false, __ATOMIC_RELEASE);
	return boost;
}

void IGFX::RPSControlPatch::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if ((rc6Fix || diagnostics) && !rc6LogCall) {
		logLock = IOSimpleLockAlloc();
		if (logLock)
			rc6LogCall = thread_call_allocate(logRC6, this);
		if (!rc6LogCall) {
			if (logLock) {
				IOSimpleLockFree(logLock);
				logLock = nullptr;
			}
			if (rc6Fix)
				publish("weg-rc6-status", "disabled: log allocation failed");
			if (diagnostics)
				publish("weg-power-status", "disabled: log allocation failed");
			rc6Fix = diagnostics = false;
		}
	}
	if (mode != 0 || rc6Fix || diagnostics) {
		KernelPatcher::RouteRequest routeRequest = {
			"__ZL15pmNotifyWrapperjjPyPj",
			wrapPmNotifyWrapper,
			orgPmNotifyWrapper
		};

		if (!patcher.routeMultiple(index, &routeRequest, 1, address, size)) {
			SYSLOG(log, "Failed to route pmNotifyWrapper.");
			if (rc6LogCall && !orgPmNotifyWrapper) {
				thread_call_free(static_cast<thread_call_t>(rc6LogCall));
				rc6LogCall = nullptr;
				IOSimpleLockFree(logLock);
				logLock = nullptr;
				if (rc6Fix)
					publish("weg-rc6-status", "disabled: pmNotifyWrapper not found");
				if (diagnostics)
					publish("weg-power-status", "disabled: pmNotifyWrapper not found");
				rc6Fix = diagnostics = false;
			}
		}
	}

	// The doPMWork command numbers were verified on the KBL and CFL framebuffers only
	auto framebuffer = callbackIGFX->getRealFramebuffer(index);
	if (gtFloorCap && framebuffer != &kextIntelKBLFb && framebuffer != &kextIntelCFLFb) {
		SYSLOG(log, "GT floor cap is only supported with the KBL and CFL framebuffers, disabled.");
		publish("weg-gt-floor-status", "disabled: not the KBL or CFL framebuffer");
		gtFloorCap = 0;
	}

	if (gtFloorCap) {
		KernelPatcher::RouteRequest routeRequest = {
			"__ZN31AppleIntelFramebufferController8doPMWorkEjPvS0_S0_",
			wrapDoPMWork,
			orgDoPMWork
		};

		if (!patcher.routeMultiple(index, &routeRequest, 1, address, size)) {
			SYSLOG(log, "Failed to route doPMWork, GT floor cap disabled.");
			publish("weg-gt-floor-status", "disabled: doPMWork not found");
			gtFloorCap = 0;
		}
	}
	if (!gtFloorCap && !rc6Fix && !diagnostics)
		OSSafeReleaseNULL(igpu);
}

IOReturn IGFX::RPSControlPatch::wrapDoPMWork(void *controller, uint32_t cmd, void *arg1, void *arg2, void *arg3) {
	auto &self = callbackIGFX->modRPSControlPatch;

	if ((cmd == PMWORK_GT2_FLOOR || cmd == PMWORK_GT3_FLOOR) && arg1 && self.gtFloorCap) {
		uint32_t requested = *static_cast<uint32_t *>(arg1);

		// Never go below the hardware minimum (RPn) or above the maximum (RP0)
		uint32_t cap = self.readCaps(controller);
		if (!cap)
			return self.orgDoPMWork(controller, cmd, arg1, arg2, arg3);
		uint32_t rp0 = cap & 0xFF;
		uint32_t rpn = (cap >> 16) & 0xFF;
		uint32_t floor = self.gtFloorCap;
		if (floor < rpn)
			floor = rpn;
		if (rp0 && floor > rp0)
			floor = rp0;

		// doPMWork runs on the framebuffer's command gate, so publishing (registry lock) is fine here
		const auto diagnosticMHz = min(requested, UINT32_MAX / 50) * 50;
		if (self.gtFloorLogged != diagnosticMHz + 1) {
			self.gtFloorLogged = diagnosticMHz + 1;
			self.publish("weg-gt-floor-agpm", diagnosticMHz);
			self.publish("weg-gt-floor-applied", (requested > floor ? floor : requested) * 50);
			self.publish("weg-gt-floor-rpn", rpn * 50);
			self.publish("weg-gt-floor-status", requested > floor ? "lowered AGPM floor" : "AGPM floor already low enough");
			SYSLOG(log, "GT%u floor: AGPM requested %u MHz, using %u MHz (RPn %u MHz)",
				   cmd == PMWORK_GT2_FLOOR ? 2 : 3, diagnosticMHz, (requested > floor ? floor : requested) * 50, rpn * 50);
		}

		// Only lower the floor AGPM asks for, so a lower request still works
		if (requested > floor) {
			// Pass a copy: the argument points into the caller's stack frame
			uint32_t value = floor;
			return self.orgDoPMWork(controller, cmd, &value, arg2, arg3);
		}
	}

	return self.orgDoPMWork(controller, cmd, arg1, arg2, arg3);
}

void IGFX::RPSControlPatch::processGraphicsKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	// Some supported GPUs use the original Apple ForceWake implementation.
	if ((callbackIGFX->powerOptions & (Diagnose | RespectEPP)) && !callbackIGFX->modForceWakeWorkaround.enabled) {
		KernelPatcher::RouteRequest route {
			"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", observeWake, orgObservedWake
		};
		if (patcher.routeMultiple(index, &route, 1, address, size))
			wakeHookReady();
		else
			patcher.clearError();
	}
	// The RC6 monitor alone needs no RPS changes
	if (mode == 0)
		return;

	// Address of `IGHardwareCommandStreamer2::submitExecList`
	mach_vm_address_t orgSubmitExecList;
	
	KernelPatcher::SolveRequest request = {
		getKernelVersion() >= KernelVersion::Catalina ? "__ZN26IGHardwareCommandStreamer514submitExecListEj" : "__ZN26IGHardwareCommandStreamer214submitExecListEj",
		orgSubmitExecList
	};
	
	if (!patcher.solveMultiple(index, &request, 1, address, size)) {
		SYSLOG(log, "Failed to solve the symbol for submitExecList.");
		return;
	}
	
	// `submitExecList()` only controls RPS for RCS type streamers
	// Patch it to enable control for any kind of streamer
	if (!patchRCSCheck(orgSubmitExecList))
		SYSLOG(log, "Failed to patch RCS check.");
}

int IGFX::RPSControlPatch::wrapPmNotifyWrapper(unsigned int a0, unsigned int a1, unsigned long long *a2, unsigned int *freq) {
	auto &self = callbackIGFX->modRPSControlPatch;

	int ret = self.orgPmNotifyWrapper(a0, a1, a2, freq);
	if (self.rc6Fix || self.diagnostics)
		self.checkRC6();
	if (self.mode == 0 || !freq || ret != 0)
		return ret;
	if (self.mode == 1) {
		auto now = mach_absolute_time();
		if (!self.sustainedSubmissions(now) || now < __atomic_load_n(&self.deferBoostUntil, __ATOMIC_RELAXED))
			return ret;
	}

	const uint32_t rp0 = self.readCaps(callbackIGFX->defaultController()) & 0xFF;
	if (rp0)
		*freq = (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * rp0;
	return ret;
}

void IGFX::RPSControlPatch::postRC6Event(uint32_t event, uint32_t v0, uint32_t v1, uint32_t v2) {
	auto irq = IOSimpleLockLockDisableInterrupt(logLock);
	if (logCount < arrsize(logQueue)) {
		logQueue[(logHead + logCount) % arrsize(logQueue)] = {event, v0, v1, v2};
		++logCount;
	}
	bool schedule = !logScheduled;
	logScheduled = true;
	IOSimpleLockUnlockEnableInterrupt(logLock, irq);
	if (schedule)
		thread_call_enter(static_cast<thread_call_t>(rc6LogCall));
}

void IGFX::RPSControlPatch::logRC6(void *param0, void *) {
	auto &self = *static_cast<RPSControlPatch *>(param0);
	for (;;) {
		auto irq = IOSimpleLockLockDisableInterrupt(self.logLock);
		if (!self.logCount) {
			self.logScheduled = false;
			IOSimpleLockUnlockEnableInterrupt(self.logLock, irq);
			return;
		}
		auto event = self.logQueue[self.logHead];
		self.logHead = (self.logHead + 1) % arrsize(self.logQueue);
		--self.logCount;
		IOSimpleLockUnlockEnableInterrupt(self.logLock, irq);
		self.reportEvent(event);
	}
}

void IGFX::RPSControlPatch::reportEvent(const Event &event) {
	const auto events = event.kind;
	const auto v0 = event.v0, v1 = event.v1, v2 = event.v2;
	if (events == RC6EventTelemetry) {
		publishTelemetry(event);
		return;
	}
	if (events & RC6EventSnapshot) {
		SYSLOG(log, "RC6: recorded the driver's RC6 configuration, RC_CONTROL 0x%08x PG_ENABLE 0x%08x", v0, v1);
		publish("weg-rc6-control", v0);
		publish("weg-rc6-status", "enabled");
	}
	if (events & RC6EventRestored) {
		SYSLOG(log, "RC6: RC6 was disabled (RC_CONTROL 0x%08x), restored RC_CONTROL 0x%08x (total %u)", v0, v1, v2);
		publish("weg-rc6-restores", v2);
		publish("weg-rc6-status", "restored after it was disabled");
	}
	if (events & RC6EventGiveUp) {
		SYSLOG(log, "RC6: the driver keeps RC6 disabled (RC_CONTROL 0x%08x), no more restores until RC6 is enabled again", v0);
		publish("weg-rc6-status", "driver keeps RC6 disabled, restores stopped");
	}
	if (events & RC6EventNever) {
		SYSLOG(log, "RC6: the driver never enabled RC6 (RC_CONTROL 0x%08x), nothing to restore", v0);
		publish("weg-rc6-control", v0);
		publish("weg-rc6-status", "never enabled by the driver");
	}
	if (events & RC6EventStall) {
		SYSLOG(log, "RC6: RC6 is enabled but residency has not advanced for 60 s. Forcewake render ack 0x%08x media ack 0x%08x "
			   "(bits held all the time mean a leaked forcewake reference keeps the GT out of RC6)", v0, v1);
		publish("weg-rc6-forcewake-render", v0);
		publish("weg-rc6-forcewake-media", v1);
		publish("weg-rc6-status", "enabled but no residency for 60 s (forcewake held?)");
	}
}

void IGFX::RPSControlPatch::checkRC6() {
	const uint64_t now = mach_absolute_time();
	if (now < __atomic_load_n(&rc6NextCheck, __ATOMIC_RELAXED))
		return;
	uint32_t idle = 0;
	if (!__atomic_compare_exchange_n(&rc6Sampler, &idle, 1U, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	if (now >= __atomic_load_n(&rc6NextCheck, __ATOMIC_RELAXED)) {
		__atomic_store_n(&rc6NextCheck, now + rc6CheckInterval, __ATOMIC_RELAXED);
		sampleRC6(now);
	}
	__atomic_fetch_and(&rc6Sampler, ~1U, __ATOMIC_RELEASE);
}

void IGFX::RPSControlPatch::sampleTelemetry(void *controller) {
	uint32_t ticks = callbackIGFX->readRegister32(controller, GEN6_GT_GFX_RC6);
	const uint64_t now = mach_absolute_time();
	uint64_t uptime = 0, elapsed = 0;
	absolutetime_to_nanoseconds(now, &uptime);
	if (sampleTime && now > sampleTime)
		absolutetime_to_nanoseconds(now - sampleTime, &elapsed);
	uint32_t permille = UINT32_MAX;
	clock_sec_t sec = 0;
	clock_usec_t usec = 0;
	clock_get_calendar_microtime(&sec, &usec);
	// Absolute time may stop across sleep. Discard windows spanning sleep or a clock step.
	uint64_t wall = sec * 1000000000ULL + usec * 1000ULL;
	uint64_t wallElapsed = wall >= sampleWall ? wall - sampleWall : UINT64_MAX;
	uint64_t skew = wallElapsed > elapsed ? wallElapsed - elapsed : elapsed - wallElapsed;
	if (ticks != UINT32_MAX && elapsed && elapsed <= 60000000000ULL && skew < 100000000ULL) {
		// KBL/CFL/CML GT residency has a 32-bit counter with 1.28 us ticks.
		uint64_t resident = static_cast<uint32_t>(ticks - sampleTicks) * 1280ULL;
		// Allow up to 1 ms for MMIO and timestamp acquisition skew.
		if (resident <= elapsed + 1000000)
			permille = static_cast<uint32_t>(min(resident, elapsed) * 1000 / elapsed);
	}
	sampleTicks = ticks;
	sampleTime = ticks == UINT32_MAX ? 0 : now;
	sampleWall = wall;
	postRC6Event(RC6EventTelemetry, permille,
		permille == UINT32_MAX ? 0 : static_cast<uint32_t>(elapsed / 1000000),
		static_cast<uint32_t>(uptime / 1000000000));
}

void IGFX::RPSControlPatch::countWake(bool set, uint32_t domains, uint32_t context) {
	for (unsigned i = 0; i < arrsize(wakeCalls); ++i) {
		if (!(domains & (1U << i)))
			continue;
		__atomic_fetch_add(&wakeCalls[i][set ? 1 : 0], 1U, __ATOMIC_RELAXED);
		// This is the last observed request for each context bit, before any ACK.
		if (context < 15) {
			if (set)
				__atomic_fetch_or(&wakeContexts[i], 1U << context, __ATOMIC_RELAXED);
			else
				__atomic_fetch_and(&wakeContexts[i], ~(1U << context), __ATOMIC_RELAXED);
		}
	}
}

void IGFX::RPSControlPatch::observeWake(void *accelerator, bool set, uint32_t domains, uint32_t context) {
	auto &self = callbackIGFX->modRPSControlPatch;
	self.countWake(set, domains, context);
	self.orgObservedWake(accelerator, set, domains, context);
}

namespace {
// KBL/CFL eDP transcoder and display engine. Observe only Apple framebuffer reads.
struct DisplayObservation { uint32_t address; const char *property; };
constexpr DisplayObservation displayRegisters[] = {
	{0x6F840, "weg-power-psr1-status-raw"}, {0x6F940, "weg-power-psr2-status-raw"},
	{0x45504, "weg-power-dc-policy-raw"}, {0x80030, "weg-power-dc3-dc5-count"},
	{0x8002C, "weg-power-dc5-dc6-count"}, {0x43208, "weg-power-fbc-control-raw"},
	{0x46000, "weg-power-cdclk-control-raw"}
};
}

void IGFX::RPSControlPatch::observeDisplayRead(uint32_t address, uint32_t value) {
	static_assert(arrsize(displayRegisters) == DisplayRegisterCount, "display register cache size");
	if (value == UINT32_MAX)
		return;
	for (unsigned i = 0; i < arrsize(displayRegisters); ++i) {
		if (address == displayRegisters[i].address) {
			__atomic_store_n(&displayValues[i], value, __ATOMIC_RELAXED);
			__atomic_fetch_or(&seenDisplay, 1U << i, __ATOMIC_RELEASE);
			return;
		}
	}
}

void IGFX::RPSControlPatch::sampleEPP() {
	uint32_t epp = UINT32_MAX, lo = 0, hi = 0;
	// Sample the worker CPU. Respect package delegation when its local EPP is not valid.
	if (readMsr && !readMsr(0x770, &lo, &hi) && (lo & 1) &&
		!readMsr(0x774, &lo, &hi)) {
		if (!(hi & (1U << 10)) || (hi & (1U << 28)) || !readMsr(0x772, &lo, &hi))
			epp = lo >> 24;
	}
	const bool defer = mode == 1 && epp != UINT32_MAX && epp >= 128;
	__atomic_store_n(&deferBoostUntil, defer ? mach_absolute_time() + 2 * telemetryInterval : 0, __ATOMIC_RELAXED);
	if (epp == UINT32_MAX)
		igpu->removeProperty("weg-power-hwp-epp");
	else
		publish("weg-power-hwp-epp", epp);
	publish("weg-power-defer-rp0", static_cast<uint32_t>(defer));
}

void IGFX::RPSControlPatch::publishTelemetry(const Event &event) {
	if (!igpu)
		return;
	if (event.v0 == UINT32_MAX)
		igpu->removeProperty("weg-power-rc6-permille");
	else
		publish("weg-power-rc6-permille", event.v0);
	publish("weg-power-window-ms", event.v1);
	publish("weg-power-uptime-s", event.v2);
	publish("weg-power-status", event.v0 == UINT32_MAX ? "waiting for a valid window" : "sampled");
	const uint32_t seen = __atomic_exchange_n(&seenDisplay, 0U, __ATOMIC_ACQUIRE);
	for (unsigned i = 0; i < arrsize(displayRegisters); ++i)
		if (seen & (1U << i))
			publish(displayRegisters[i].property, __atomic_load_n(&displayValues[i], __ATOMIC_RELAXED));

	const bool hooked = __atomic_load_n(&wakeHookInstalled, __ATOMIC_ACQUIRE);
	publish("weg-power-forcewake-hook", static_cast<uint32_t>(hooked));
	if (hooked) {
		const char *domains[] = {"render", "media", "blitter"};
		for (unsigned i = 0; i < arrsize(domains); ++i) {
			char key[48];
			snprintf(key, sizeof(key), "weg-power-fw-%s-set", domains[i]);
			publish(key, __atomic_load_n(&wakeCalls[i][1], __ATOMIC_RELAXED));
			snprintf(key, sizeof(key), "weg-power-fw-%s-clear", domains[i]);
			publish(key, __atomic_load_n(&wakeCalls[i][0], __ATOMIC_RELAXED));
			snprintf(key, sizeof(key), "weg-power-fw-%s-contexts", domains[i]);
			publish(key, __atomic_load_n(&wakeContexts[i], __ATOMIC_RELAXED));
		}
	}
	if (callbackIGFX->powerOptions & RespectEPP)
		sampleEPP();
}

void IGFX::RPSControlPatch::sampleRC6(uint64_t now) {
	auto controller = callbackIGFX->defaultController();
	if (!controller || !callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32)
		return;
	if (diagnostics && now >= nextTelemetry) {
		nextTelemetry = now + telemetryInterval;
		sampleTelemetry(controller);
	}
	if (!rc6Fix || !callbackIGFX->modMMIORegistersWriteSupport.orgWriteRegister32)
		return;

	// Read GT registers only while the render well is awake. Otherwise they read as 0, which would look
	// like a lost configuration.
	uint32_t ackRender = callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_RENDER_GEN9);
	if ((ackRender & 0xFFFF) == 0 || ackRender == 0xFFFFFFFF)
		return;

	uint32_t rcControl = callbackIGFX->readRegister32(controller, GEN6_RC_CONTROL);
	if (rcControl == UINT32_MAX)
		return;
	static_assert(sizeof(rc6Snapshot) / sizeof(rc6Snapshot[0]) == arrsize(RC6_REGISTERS), "RC6 snapshot layout");
	constexpr size_t controlIndex = arrsize(RC6_REGISTERS) - 2;
	constexpr size_t pgIndex = arrsize(RC6_REGISTERS) - 1;

	if (rcControl & GEN6_RC_CTL_RC6_ENABLE) {
		rc6LostCount = 0;
		rc6NeverCount = 0;
		if (!rc6EnabledSince)
			rc6EnabledSince = now;

		if (!rc6HaveSnapshot) {
			// Wait until the driver's configuration is stable
			if (++rc6GoodCount >= 3) {
				for (size_t i = 0; i < arrsize(RC6_REGISTERS); i++) {
					rc6Snapshot[i] = callbackIGFX->readRegister32(controller, RC6_REGISTERS[i]);
					if (rc6Snapshot[i] == UINT32_MAX)
						return;
				}
				rc6LastResidency = callbackIGFX->readRegister32(controller, GEN6_GT_GFX_RC6);
				const auto ack = callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_RENDER_GEN9);
				if (rc6Snapshot[controlIndex] != rcControl || rc6LastResidency == UINT32_MAX ||
					ack == UINT32_MAX || !(ack & 0xFFFF))
					return;
				rc6HaveSnapshot = true;
				rc6ResidencyChanged = now;
				postRC6Event(RC6EventSnapshot, rc6Snapshot[controlIndex], rc6Snapshot[pgIndex]);
			}
			return;
		}

		// RC6 that stays enabled for a minute resets the restore limit
		if (rc6FailedRestores && now - rc6EnabledSince >= 60 * rc6CheckInterval)
			rc6FailedRestores = 0;

		// Residency should advance between submissions on an idle desktop. When it does not, something
		// (usually a held forcewake reference) keeps the GT awake: report it, it is not safe to fix here.
		uint32_t residency = callbackIGFX->readRegister32(controller, GEN6_GT_GFX_RC6);
		if (residency == UINT32_MAX)
			return;
		if (residency != rc6LastResidency) {
			rc6LastResidency = residency;
			rc6ResidencyChanged = now;
		} else if (now - rc6ResidencyChanged >= 60 * rc6CheckInterval && now >= rc6NextStallReport) {
			const auto media = callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_MEDIA_GEN9);
			if (media != UINT32_MAX) {
				rc6NextStallReport = now + 600 * rc6CheckInterval;
				postRC6Event(RC6EventStall, ackRender, media);
			}
		}
		return;
	}

	rc6GoodCount = 0;
	rc6EnabledSince = 0;
	rc6ResidencyChanged = now;

	if (!rc6HaveSnapshot) {
		if (++rc6NeverCount == 60)
			postRC6Event(RC6EventNever, rcControl);
		return;
	}

	// RC6 has to stay disabled for 5 checks before restoring it, so the driver's own reprogramming
	// (wake, GPU reset) is not interrupted
	if (++rc6LostCount < 5 || rc6FailedRestores >= 3)
		return;
	rc6LostCount = 0;

	// Read every dependent register before modifying the first one. Do not
	// enable RC6 over a partial configuration or replace a new driver setup.
	uint32_t live[arrsize(RC6_REGISTERS)];
	for (size_t i = 0; i < arrsize(RC6_REGISTERS); ++i) {
		live[i] = callbackIGFX->readRegister32(controller, RC6_REGISTERS[i]);
		if (live[i] == UINT32_MAX)
			return;
	}
	if (live[controlIndex] != rcControl)
		return;

	// A restore that did not hold for 30 s means the driver may be
	// disabling RC6 deliberately. Stop after three such attempts.
	if (rc6LastRestore && now - rc6LastRestore < 30 * rc6CheckInterval && ++rc6FailedRestores >= 3) {
		postRC6Event(RC6EventGiveUp, rcControl);
		return;
	}
	rc6LastRestore = now;
	for (size_t i = 0; i < arrsize(RC6_REGISTERS); ++i) {
		if (live[i] == rc6Snapshot[i] || (i == pgIndex && !rc6Snapshot[i]))
			continue;
		const auto ack = callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_RENDER_GEN9);
		if (ack == UINT32_MAX || !(ack & 0xFFFF))
			return;
		if (i == controlIndex && callbackIGFX->readRegister32(controller, GEN6_RC_CONTROL) != rcControl)
			return;
		callbackIGFX->writeRegister32(controller, RC6_REGISTERS[i], rc6Snapshot[i]);
		if (callbackIGFX->readRegister32(controller, RC6_REGISTERS[i]) != rc6Snapshot[i]) {
			rc6LastRestore = 0; // This failed write is accounted for immediately.
			if (++rc6FailedRestores >= 3)
				postRC6Event(RC6EventGiveUp, rcControl);
			return;
		}
	}

	postRC6Event(RC6EventRestored, rcControl, rc6Snapshot[controlIndex], ++rc6Restores);
}

bool IGFX::RPSControlPatch::patchRCSCheck(mach_vm_address_t& start) {
	constexpr unsigned ninsts_max {256};
	
	hde64s dis;
	
	bool found_cmp = false;
	bool found_jmp = false;

	for (size_t i = 0; i < ninsts_max; i++) {
		auto sz = Disassembler::hdeDisasm(start, &dis);

		if (dis.flags & F_ERROR) {
			SYSLOG(log, "Error disassembling submitExecList");
			break;
		}

		/* cmp byte ptr [rcx], 0 */
		if (!found_cmp && dis.opcode == 0x80 && dis.modrm_reg == 7 && dis.modrm_rm == 1)
			found_cmp = true;
		/* jnz rel32 */
		if (found_cmp && dis.opcode == 0x0f && dis.opcode2 == 0x85) {
			found_jmp = true;
			break;
		}

		start += sz;
	}
	
	if (found_jmp) {
		auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
		if (status == KERN_SUCCESS) {
			constexpr uint8_t nop6[] {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			lilu_os_memcpy(reinterpret_cast<void*>(start), nop6, arrsize(nop6));
			MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			DBGLOG(log, "Patched submitExecList");
			return true;
		} else {
			DBGLOG(log, "Failed to set kernel writing");
			return false;
		}
	} else {
		SYSLOG(log, "jnz in submitExecList not found");
		return false;
	}
}

// MARK: - Force Wake Workaround

static uint64_t forceWakeDeadline() {
	uint64_t deadline;
	clock_interval_to_deadline(FORCEWAKE_ACK_TIMEOUT_MS, kMillisecondScale, &deadline);
	return deadline;
}

// SafeForceWakeMultithreaded can run in an IRQ or under a spinlock. Never
// sleep here. Space out slow MMIO reads while keeping the common ACK fast.
bool IGFX::ForceWakeWorkaround::pollRegister(void *controller, uint32_t reg, uint32_t val, uint32_t mask, uint64_t deadline) {
	unsigned delay = 0;
	do {
		auto rd = callbackIGFX->readRegister32(controller, reg);
		if (rd == UINT32_MAX)
			return false;
		if ((rd & mask) == val)
			return true;
		if (delay)
			IODelay(delay);
		delay = delay ? min(16U, delay * 2) : 1;
	} while (mach_absolute_time() < deadline);
	return false;
}

bool IGFX::ForceWakeWorkaround::forceWakeWaitAckFallback(void *controller, uint32_t d, uint32_t val, uint32_t mask) {
	auto &self = callbackIGFX->modForceWakeWorkaround;
	// All contexts share bit 15 on a domain. Never clear another caller's pulse.
	if (__atomic_fetch_or(&self.fallbackDomains, d, __ATOMIC_ACQUIRE) & d)
		return false;
	const auto deadline = forceWakeDeadline();
	bool ack = false;
	for (unsigned pass = 1; pass <= 10 && mach_absolute_time() < deadline; ++pass) {
		if (!pollRegister(controller, ackForDom(d), 0, FORCEWAKE_KERNEL_FALLBACK, deadline))
			break;
		callbackIGFX->writeRegister32(controller, regForDom(d), fw_set(FORCEWAKE_KERNEL_FALLBACK));
		IODelay(10 * pass);
		const bool pulse = pollRegister(controller, ackForDom(d), FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, deadline);
		auto rd = callbackIGFX->readRegister32(controller, ackForDom(d));
		callbackIGFX->writeRegister32(controller, regForDom(d), fw_clear(FORCEWAKE_KERNEL_FALLBACK));
		ack = pulse && rd != UINT32_MAX && (rd & mask) == val;
		if (!pulse || ack)
			break;
	}
	__atomic_fetch_and(&self.fallbackDomains, ~d, __ATOMIC_RELEASE);
	return ack;
}

void IGFX::ForceWakeWorkaround::forceWake(void *accelerator, bool set, uint32_t dom, uint32_t ctx) {
	auto &self = callbackIGFX->modForceWakeWorkaround;
	if (callbackIGFX->powerOptions & (Diagnose | RespectEPP))
		callbackIGFX->modRPSControlPatch.countWake(set, dom, ctx);
	auto controller = callbackIGFX->defaultController();
	if (!controller || !callbackIGFX->modMMIORegistersWriteSupport.orgWriteRegister32 ||
		!callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32 ||
		ctx >= 15 || (dom & ~(DOM_RENDER | DOM_MEDIA | DOM_BLITTER))) {
		self.orgForceWake(accelerator, set, dom, ctx);
		return;
	}

	const uint32_t mask = 1U << ctx;
	const uint32_t expected = set ? mask : 0;
	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
		if (!(dom & d))
			continue;
		callbackIGFX->writeRegister32(controller, regForDom(d), masked_field(mask, expected));
		IOPause(100);
		if (!pollRegister(controller, ackForDom(d), expected, mask, forceWakeDeadline()) &&
			!forceWakeWaitAckFallback(controller, d, expected, mask) &&
			!pollRegister(controller, ackForDom(d), expected, mask, forceWakeDeadline())) {
			// The ABI has no error return. Hand the unresolved request to
			// Apple's original routine, with its own recovery/failure policy.
			self.orgForceWake(accelerator, set, d, ctx);
		}
	}
}

void IGFX::ForceWakeWorkaround::init() {
	// We only need to patch the acceleration driver
	requiresPatchingGraphics = true;
	
	// Requires access to global framebuffer controllers
	requiresGlobalFramebufferControllersAccess = true;
	
	// Requires read and write access to MMIO registers
	requiresMMIORegistersReadAccess = true;
	requiresMMIORegistersWriteAccess = true;
}

void IGFX::ForceWakeWorkaround::processGraphicsKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest request = {
		"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj",
		forceWake,
		orgForceWake
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "Failed to route SafeForceWake.");
	else if (callbackIGFX->powerOptions & (Diagnose | RespectEPP))
		callbackIGFX->modRPSControlPatch.wakeHookReady();
}
