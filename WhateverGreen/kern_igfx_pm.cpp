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
#include <IOKit/graphics/IOFramebuffer.h>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_disasm.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <mach/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
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

[[maybe_unused]] constexpr uint32_t GEN6_RC_CTL_RC6_ENABLE = 1U << 18;
constexpr uint32_t GEN6_GT_GFX_RC6 = 0x138108;
enum RC6Event : uint32_t {
	NativeRC6State = 1U << 14, RefreshVBL = 1U << 13,
	RPSEventEPP = 1U << 9, RPSEventBusy = 1U << 8, RC6EventTelemetry = 1U << 5
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

void IGFX::RPSControlPatch::unpublish(const char *key) {
	if (igpu)
		igpu->removeProperty(key);
}

void IGFX::RPSControlPatch::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	uint32_t rpsc = 0;
	if (PE_parse_boot_argn("igfxrpsc", &rpsc, sizeof(rpsc)) ||
		WIOKit::getOSDataValue(info->videoBuiltin, "rps-control", rpsc)) {
		// The measure-only mode 3 is observation: Release builds leave the driver alone
		if (!ObservationBuild && rpsc == 3)
			rpsc = 0;
		enabled = rpsc > 0 && available;
		mode = min(rpsc, 2U);
		if (rpsc >= 3 && rpsc <= 5) {
			auto cpu = BaseDeviceInfo::get().cpuGeneration;
			mode = available && (cpu == CPUInfo::CpuGeneration::KabyLake ||
				cpu == CPUInfo::CpuGeneration::CoffeeLake || cpu == CPUInfo::CpuGeneration::CometLake) ? rpsc : 0;
			nanoseconds_to_absolutetime(20000000ULL, &busySamplePeriod);
		}
		DBGLOG("weg", "RPS control patch overriden (%u) availabile %d", rpsc, available);
	}

	if (mode == 5) {
		deadlineBoost = checkKernelArgument("-igfxvblboost");
		nanoseconds_to_absolutetime(10000000000ULL, &acCheckInterval);
		uint32_t features = 0, ecx = 0;
		if (CPUInfo::getCpuid(1, 0, nullptr, nullptr, &ecx) && !(ecx & (1U << 31)) &&
			CPUInfo::getCpuid(6, 0, &features) && (features & 1)) {
			packageSensor = (features & (1U << 6)) != 0;
			thermalMsr = reinterpret_cast<decltype(thermalMsr)>(patcher.solveSymbol(KernelPatcher::KernelID, "_rdmsr_carefully"));
			if (!thermalMsr)
				patcher.clearError();
		}
	}
	// The RC6 monitor runs in the same pmNotifyWrapper hook, with or without RPS control
	// -igfxrc6fix only reports what Apple's driver read, so it is part of the DEBUG observation
	rc6Fix = ObservationBuild && available && (checkKernelArgument("-igfxrc6fix") ||
		(info->videoBuiltin && info->videoBuiltin->getProperty("enable-rc6-fix") != nullptr));
	observe = ObservationBuild && (callbackIGFX->powerOptions & (Diagnose | RequestDynamicFBC)) != 0;
	frequencyProfile = observe && checkKernelArgument("-igfxfreqprofile");
	diagnostics = observe || (callbackIGFX->powerOptions & RespectEPP);
	if (rc6Fix || diagnostics || mode >= 3) {
		lastWake = reinterpret_cast<const uint64_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_gIOLastWakeAbsTime"));
		if (!lastWake)
			patcher.clearError();
	}
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
	// igfxpwr=16/32/64 filter the framebuffer's own PSR and DC state writes, so they need the write hook
	displayPolicy = (callbackIGFX->powerOptions & (DisablePSR | LimitDC5 | DisableDC)) != 0;
	if (rc6Fix || diagnostics) {
		enabled = true;
		if (rc6Fix)
			nanoseconds_to_absolutetime(1000ULL * 1000 * 1000, &rc6CheckInterval);
		else
			rc6CheckInterval = telemetryInterval;
	}
	if (displayPolicy)
		enabled = true;
	// Only ever set here, as on master: without -igfxrc6fix or an igfxpwr display policy nothing changes
	if (rc6Fix || displayPolicy || mode >= 3)
		requiresMMIORegistersWriteAccess = true;

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
		if ((mode >= 3 || gtFloorCap || rc6Fix || diagnostics || displayPolicy) && info->videoBuiltin && !igpu) {
			igpu = info->videoBuiltin;
			igpu->retain();
		}
		if (gtFloorCap) {
			publish("weg-gt-floor-cap", gtFloorCap * 50);
			publish("weg-gt-floor-status", "waiting for AGPM floor request");
		}
		if (info->videoBuiltin)
			info->videoBuiltin->setProperty("weg-rps-control-mode", mode, 32);
		if (observe)
			publish("weg-power-status", "waiting for GPU notifications");
		if (callbackIGFX->powerOptions & DisablePSR)
			publish("weg-power-psr-policy", "PSR1/PSR2 enable writes cleared");
		if (callbackIGFX->powerOptions & DisableDC)
			publish("weg-power-dc-policy", "DC5/DC6 disabled");
		else if (callbackIGFX->powerOptions & LimitDC5)
			publish("weg-power-dc-policy", "limited to DC5");
		if (rc6Fix)
			publish("weg-rc6-status", "waiting for native readback");
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
	if (acNotifier) {
		acNotifier->remove();
		acNotifier = nullptr;
	}
	OSSafeReleaseNULL(acProvider);
	OSSafeReleaseNULL(timingFramebuffer);
	OSSafeReleaseNULL(igpu);
}

void IGFX::RPSControlPatch::invalidatePower() {
	__atomic_fetch_add(&resetGeneration, 1U, __ATOMIC_ACQ_REL);
	__atomic_fetch_add(&acGeneration, 1U, __ATOMIC_ACQ_REL);
	__atomic_store_n(&rpCaps, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&policyAt, 0ULL, __ATOMIC_RELEASE);
	__atomic_store_n(&seenDisplay, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&lastGTReleaseAck, UINT32_MAX, __ATOMIC_RELEASE);
	__atomic_store_n(&deferBoostUntil, 0ULL, __ATOMIC_RELEASE);
	__atomic_store_n(&fastBoostUntil, 0ULL, __ATOMIC_RELEASE);
}

uint32_t IGFX::RPSControlPatch::readCaps(void *controller) {
	const uint32_t generation = currentGeneration();
	const auto cached = __atomic_load_n(&rpCaps, __ATOMIC_ACQUIRE);
	if ((cached >> 32) == generation && static_cast<uint32_t>(cached))
		return static_cast<uint32_t>(cached);
	if (!controller || !callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32)
		return 0;
	const auto value = callbackIGFX->readRegister32(controller, GEN6_RP_STATE_CAP);
	const auto rp0 = value & 0xFF;
	const auto rpn = (value >> 16) & 0xFF;
	if (value == UINT32_MAX || !rpn || rp0 < rpn || rp0 > (UINT32_MAX >> GEN9_FREQUENCY_SHIFT) / GEN9_FREQ_SCALER)
		return 0;
	__atomic_store_n(&rpCaps, (static_cast<uint64_t>(generation) << 32) | value, __ATOMIC_RELEASE);
	return generation == currentGeneration() ? value : 0;
}

bool IGFX::RPSControlPatch::sustainedSubmissions(uint64_t now) {
	// A pair of submissions in the same frame is common even at idle.
	// Require short gaps spanning a full sampling window. Out-of-order
	// observations must not move the clock backward or create a boost.
	if (__atomic_exchange_n(&samplingLoad, true, __ATOMIC_ACQUIRE))
		return false;
	bool boost = false;
	if (submissionEpoch != callbackIGFX->token()) {
		submissionEpoch = callbackIGFX->token();
		lastNotify = burstStart = 0;
	}
	if (now > lastNotify) {
		if (!lastNotify || now - lastNotify > busyInterval)
			burstStart = now;
		else
			// Sustained load normally needs two busy intervals; one while the CPU prefers performance
			boost = now - burstStart >= (currentEPP() && now < __atomic_load_n(&fastBoostUntil, __ATOMIC_RELAXED) ? 1 : 2) * busyInterval;
		lastNotify = now;
	}
	__atomic_store_n(&samplingLoad, false, __ATOMIC_RELEASE);
	return boost;
}

void IGFX::RPSControlPatch::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if ((mode >= 3 || rc6Fix || diagnostics) && !rc6LogCall) {
		logLock = IOSimpleLockAlloc();
		if (logLock)
			rc6LogCall = thread_call_allocate(logRC6, this);
		if (!rc6LogCall) {
			if (mode >= 3) {
				mode = 0;
				publish("weg-rps-busy-status", "disabled: worker unavailable");
			}
			if (logLock) {
				IOSimpleLockFree(logLock);
				logLock = nullptr;
			}
			if (rc6Fix)
				publish("weg-rc6-status", "disabled: log allocation failed");
			if (diagnostics)
				publish("weg-power-status", "disabled: log allocation failed");
			rc6Fix = observe = frequencyProfile = diagnostics = false;
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
			patcher.clearError();
			mode = 0;
			if (rc6LogCall && !orgPmNotifyWrapper) {
				thread_call_free(static_cast<thread_call_t>(rc6LogCall));
				rc6LogCall = nullptr;
				IOSimpleLockFree(logLock);
				logLock = nullptr;
				if (rc6Fix)
					publish("weg-rc6-status", "disabled: pmNotifyWrapper not found");
				if (diagnostics)
					publish("weg-power-status", "disabled: pmNotifyWrapper not found");
				rc6Fix = observe = frequencyProfile = diagnostics = false;
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
	if (displayPolicy && !displayPolicyRegistered) {
		displayPolicyRegistered = true;
		if (callbackIGFX->powerOptions & DisablePSR) {
			callbackIGFX->modMMIORegistersWriteSupport.replacerList.add(&dPSR1);
			callbackIGFX->modMMIORegistersWriteSupport.replacerList.add(&dPSR2);
		}
		if (callbackIGFX->powerOptions & (LimitDC5 | DisableDC))
			callbackIGFX->modMMIORegistersWriteSupport.replacerList.add(&dDCState);
	}
	if (mode < 3 && !gtFloorCap && !rc6Fix && !diagnostics && !displayPolicy)
		OSSafeReleaseNULL(igpu);
}

IOReturn IGFX::RPSControlPatch::wrapDoPMWork(void *controller, uint32_t cmd, void *arg1, void *arg2, void *arg3) {
	auto &self = callbackIGFX->modRPSControlPatch;

	if (callbackIGFX->admitted() && (cmd == PMWORK_GT2_FLOOR || cmd == PMWORK_GT3_FLOOR) && arg1 && self.gtFloorCap) {
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
	if (wantsWakeHook() && !callbackIGFX->modForceWakeWorkaround.enabled) {
		KernelPatcher::RouteRequest route {
			"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", observeWake, orgObservedWake
		};
		if (!patcher.routeMultiple(index, &route, 1, address, size))
			patcher.clearError();
	}
	// The RC6 monitor alone needs no RPS changes
	if (mode == 0 || mode >= 3)
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
	if (!callbackIGFX->admitted())
		return ret;
	if (self.rc6Fix || self.diagnostics)
		self.checkRC6();
	if (self.mode == 0 || !freq || ret != 0)
		return ret;
	unsigned tier = 2;
	if (self.mode >= 3) {
		__atomic_store_n(&self.measurementStarted, true, __ATOMIC_RELEASE);
		tier = self.hardwareBusy(mach_absolute_time());
		if (self.mode == 3 || !tier || (self.currentEPP() && mach_absolute_time() < __atomic_load_n(&self.deferBoostUntil, __ATOMIC_RELAXED)))
			return ret;
	}
	if (self.mode == 1) {
		auto now = mach_absolute_time();
		if (!self.sustainedSubmissions(now) || (self.currentEPP() && now < __atomic_load_n(&self.deferBoostUntil, __ATOMIC_RELAXED)))
			return ret;
	}

	const auto caps = self.readCaps(callbackIGFX->defaultController());
	uint32_t rp0 = caps & 0xFF;
	if (self.mode == 5) {
		auto sample = __atomic_load_n(&self.policyAt, __ATOMIC_ACQUIRE);
		auto now = mach_absolute_time();
		if (__atomic_load_n(&self.acGeneration, __ATOMIC_ACQUIRE) != __atomic_load_n(&self.policyGeneration, __ATOMIC_ACQUIRE) || !sample || now < sample || now - sample > 150 * self.busySamplePeriod ||
			(self.lastWake && sample < __atomic_load_n(self.lastWake, __ATOMIC_RELAXED)))
			return ret;
		auto policy = __atomic_load_n(&self.policyLimit, __ATOMIC_RELAXED);
		auto ceiling = policy & ~BatteryPolicy;
		if (tier == 1 && ceiling == 2 && self.deadlineBoost && self.nearVBL(now)) {
			tier = 2;
		}
		const unsigned requestedTier = tier;
		tier = min(tier, ceiling);
		if (!tier || !rp0)
			return ret;
		if (tier == 1) {
			auto minRatio = (caps >> 16) & 0xff;
			// On battery withdraw the supplemental floor as load drops from
			// tier 2 to tier 1. Sustained high load retains the midrange floor.
			if ((policy & BatteryPolicy) && requestedTier == 1)
				return ret;
			rp0 = minRatio + (rp0 - minRatio) / 2;
		}
		// Only ever raise Apple's request
		*freq = max(*freq, (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * rp0);
		return ret;
	}
	if (rp0)
		*freq = (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * rp0;
	return ret;
}

namespace {
// Result of one igfxrpsc=3/4/5 sample, published as weg-rps-busy-status in measure-only mode 3
enum BusyResult : uint32_t {
	BusySampled, BusyNoController, BusyGTAsleep, BusyRPMode, BusyImplausible, BusyUnchanged,
	BusyRC6Bound, BusyDownSampled, BusyResultCount
};
[[maybe_unused]] constexpr const char *busyStatus[BusyResultCount] {
	"sampled",
	"counter unavailable: no framebuffer controller or register read hook",
	"counter unavailable: no ForceWake ACK and the GT may be in RC6",
	"counter unavailable: RP_CONTROL not in RP-enabled busy-average mode",
	"counter unavailable: implausible or changing counter values",
	"counter unavailable: counters unchanged since the last sample",
	"busy unavailable: only an RC6 upper bound was observed",
	"sampled from down idle average",
};
}

// Serialized with the busy and telemetry samplers. A backwards jump can be a
// 32-bit wrap. Invalidate history only if the modulo delta cannot fit in time.
void IGFX::RPSControlPatch::trackCounter(uint32_t ticks, uint64_t now) {
	if (!ticks || ticks == UINT32_MAX)
		return;
	auto generation = currentGeneration();
	if (counterGeneration != generation)
		sampleTime = sampleWall = 0;
	auto wake = lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
	if (counterTime && counterTime >= wake && counterGeneration == generation &&
		now > counterTime && ticks < counterTicks) {
		uint64_t elapsed = 0;
		absolutetime_to_nanoseconds(now - counterTime, &elapsed);
		uint64_t delta = static_cast<uint32_t>(ticks - counterTicks) * 1280ULL;
		if (elapsed <= 60000000000ULL && delta > elapsed + 1000000 &&
			__atomic_compare_exchange_n(&resetGeneration, &generation, generation + 1, false,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			++generation;
			__atomic_fetch_add(&discontinuities, 1U, __ATOMIC_RELAXED);
			sampleTime = sampleWall = 0;
		}
	}
	counterTicks = ticks;
	counterTime = now;
	counterGeneration = generation;
}

// Sample during native notifications. No timer or ForceWake request is issued.
unsigned IGFX::RPSControlPatch::hardwareBusy(uint64_t now, uint32_t heldMask) {
	uint32_t idle = 0;
	if (!__atomic_compare_exchange_n(&rc6Sampler, &idle, 1U, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return false;
	auto generation = __atomic_load_n(&resetGeneration, __ATOMIC_ACQUIRE);
	if (generation != busyGeneration || (lastWake && busyNext && previousBusySample < __atomic_load_n(lastWake, __ATOMIC_RELAXED))) {
		busyGeneration = generation;
		lastHeldSample = nextVBLRefresh = previousBusySample = 0;
		lastDownWindow = lastIdle = 0;
		busyNext = busyUntil = busyPublish = 0;
		busyStreak = lastWindow = lastBusy = measuredTier = 0;
		busyRc6 = 0;
		busyRc6At = 0;
	}
	uint64_t sinceHeld = now >= lastHeldSample ? now - lastHeldSample : 0;
	const bool heldDue = heldMask && (!lastHeldSample || sinceHeld >= busySamplePeriod);
	const bool idleDue = !heldMask && now >= busyNext && (!lastHeldSample || sinceHeld >= 4 * busySamplePeriod);
	if (heldDue || idleDue) {
		if (heldMask)
			lastHeldSample = now;
		const bool stale = !previousBusySample || now < previousBusySample || now - previousBusySample > 3 * busySamplePeriod;
		auto controller = callbackIGFX->defaultController();
		uint32_t load = UINT32_MAX;
		uint32_t result = BusyNoController, bound = UINT32_MAX;
		if (controller && callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32) {
			auto read = [&](uint32_t reg) { return callbackIGFX->readRegister32(controller, reg); };
			auto ack = heldMask ? read(FORCEWAKE_ACK_BLITTER_GEN9) : UINT32_MAX;
			const bool forcewake = heldMask && ack != UINT32_MAX && (ack & heldMask);
			// An ACK of 0 only means nobody holds ForceWake: the GT can still be awake and running work.
			// The RC6 residency counter is uncore, so it tells whether the GT was awake since the last sample.
			const uint32_t rc6 = read(GEN6_GT_GFX_RC6);
			trackCounter(rc6, now);
			const bool rc6Valid = rc6 && rc6 != UINT32_MAX;
			uint32_t idlePermille = UINT32_MAX;
			if (rc6Valid && busyRc6 && busyRc6At && now > busyRc6At) {
				uint64_t elapsed = 0;
				absolutetime_to_nanoseconds(now - busyRc6At, &elapsed);
				const uint64_t resident = static_cast<uint32_t>(rc6 - busyRc6) * 1280ULL;
				if (elapsed && elapsed <= 60000000000ULL && resident <= elapsed + 1000000)
					idlePermille = static_cast<uint32_t>(min(resident, elapsed) * 1000 / elapsed);
			}
			busyRc6 = rc6Valid ? rc6 : 0;
			busyRc6At = rc6Valid ? now : 0;
			// Observing somebody else's ACK does not give this sampler a reference.
			auto stillAwake = [&]() {
				auto current = read(FORCEWAKE_ACK_BLITTER_GEN9);
				return current != UINT32_MAX && (current & heldMask);
			};
			uint32_t ctl = UINT32_MAX;
			if (forcewake) {
				ctl = read(0xa024);
				result = ctl == UINT32_MAX ? BusyGTAsleep : BusyRPMode;
			} else {
				if (idlePermille != UINT32_MAX) bound = 1000 - idlePermille;
				result = idlePermille == UINT32_MAX ? BusyGTAsleep : BusyRC6Bound;
			}
			if (ctl != UINT32_MAX && (ctl & 0xb8) == 0x90) { // RP enabled, UP_BUSY_AVG
				auto begin = read(0xa050);
				auto busy = read(0xa054);
				auto end = read(0xa050);
				result = BusyImplausible;
				if (end <= 0xffffff && begin >= 128 && end >= begin && busy <= end &&
					end - begin <= 1 + begin / 50 && read(0xa024) == ctl && stillAwake()) {
					result = BusyUnchanged;
					if (begin != lastWindow || busy != lastBusy) {
						load = static_cast<uint32_t>(1000ULL * busy / end);
						result = BusySampled;
					}
				}
				lastWindow = begin;
				lastBusy = busy;
			}
			if (load == UINT32_MAX && ctl != UINT32_MAX && (ctl & 0x87) == 0x82) {
				auto first = read(0xa05c), idleTicks = read(0xa060), end = read(0xa05c);
				if (first >= 128 && end <= 0xffffff && first <= end && idleTicks <= end &&
					end - first <= 1 + first / 50 && read(0xa024) == ctl &&
					stillAwake() && (first != lastDownWindow || idleTicks != lastIdle)) {
					load = 1000 - static_cast<uint32_t>(1000ULL * idleTicks / end);
					result = BusyDownSampled;
				}
				lastDownWindow = first;
				lastIdle = idleTicks;
			}
		}
		if (__atomic_load_n(&resetGeneration, __ATOMIC_ACQUIRE) != generation) {
			load = UINT32_MAX;
			result = BusyImplausible;
		}
		if (stale || load == UINT32_MAX || load < 600) {
			busyStreak = 0;
			busyUntil = 0;
		}
		if (load != UINT32_MAX && load >= 850) {
			if (busyStreak < 3)
				++busyStreak;
		} else if (!busyUntil || now >= busyUntil) {
			busyStreak = 0;
		}
		if (load != UINT32_MAX && busyStreak == 3)
			busyUntil = now + 3 * busySamplePeriod;
		measuredTier = now < busyUntil ? 2 : load != UINT32_MAX && load >= 600 ? 1 : 0;
		previousBusySample = now;
		unsigned periods = mode == 5 && (load == UINT32_MAX || load < 300) ? 4 : 1;
		busyNext = now + periods * busySamplePeriod;
		if (deadlineBoost && measuredTier && now >= nextVBLRefresh) {
			auto period = __atomic_load_n(&vblPeriod, __ATOMIC_RELAXED);
			nextVBLRefresh = now + (period ? 2 * period : 3 * busySamplePeriod);
			postRC6Event(RefreshVBL, 0);
		}
		if (mode != 4 && now >= busyPublish) {
			postRC6Event(RPSEventBusy, load, (now < busyUntil ? 1U << 16 : 0) |
				(bound == UINT32_MAX ? 0xffffU : bound), result);
			busyPublish = now + 50 * busySamplePeriod;
		}
	}
	const bool fresh = lastHeldSample && now >= lastHeldSample && now - lastHeldSample <= 3 * busySamplePeriod;
	const unsigned boost = fresh ? (mode == 5 ? measuredTier : static_cast<unsigned>(now < busyUntil)) : 0;
	__atomic_fetch_and(&rc6Sampler, ~1U, __ATOMIC_RELEASE);
	return boost;
}

bool IGFX::RPSControlPatch::postRC6Event(uint32_t event, uint32_t v0, uint32_t v1, uint32_t v2) {
	auto irq = IOSimpleLockLockDisableInterrupt(logLock);
	const bool queued = logCount < arrsize(logQueue);
	if (queued) {
		logQueue[(logHead + logCount) % arrsize(logQueue)] = {event, v0, v1, v2,
			lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0,
			__atomic_load_n(&resetGeneration, __ATOMIC_ACQUIRE)};
		++logCount;
	}
	bool schedule = !logScheduled;
	logScheduled = true;
	IOSimpleLockUnlockEnableInterrupt(logLock, irq);
	if (schedule)
		thread_call_enter(static_cast<thread_call_t>(rc6LogCall));
	return queued;
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
		if (callbackIGFX->admitted() && (!self.lastWake || event.epoch == __atomic_load_n(self.lastWake, __ATOMIC_RELAXED)) &&
			event.generation == __atomic_load_n(&self.resetGeneration, __ATOMIC_ACQUIRE))
			self.reportEvent(event);
	}
}

// Look up the AC state at most every 10 s and after each wake: the registry walk is the expensive part.
// The first power source that reports ExternalConnected wins (other IOPMPowerSource services may not).
IOReturn IGFX::RPSControlPatch::powerChanged(void *target, void *, UInt32 type, IOService *, void *, vm_size_t) {
	if (type == kIOPMMessageBatteryStatusHasChanged || type == kIOMessageServiceIsTerminated) {
		auto self = static_cast<RPSControlPatch *>(target);
		__atomic_fetch_add(&self->acGeneration, 1U, __ATOMIC_RELEASE);
	}
	return kIOReturnSuccess;
}

int IGFX::RPSControlPatch::externalPower(uint64_t now) {
	const uint64_t wake = lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
	const auto change = __atomic_load_n(&acGeneration, __ATOMIC_ACQUIRE);
	if (change == acConsumed && acCheckedAt && now - acCheckedAt < acCheckInterval && acEpoch == wake)
		return acState;
	acConsumed = change;
	acCheckedAt = now;
	acEpoch = wake;
	acState = -1;
	auto dict = IOService::serviceMatching("IOPMPowerSource");
	auto iter = dict ? IOService::getMatchingServices(dict) : nullptr;
	OSSafeReleaseNULL(dict);
	while (iter && acState < 0) {
		auto object = iter->getNextObject();
		if (!object) break;
		auto source = OSDynamicCast(IOService, object);
		if (!source || source->isInactive())
			continue;
		auto property = source->copyProperty(kIOPMPSExternalConnectedKey);
		if (auto ac = OSDynamicCast(OSBoolean, property)) {
			acState = ac->isTrue() ? 1 : 0;
			if (source != acProvider || !acNotifier) {
				if (acNotifier)
					acNotifier->remove();
				OSSafeReleaseNULL(acProvider);
				source->retain();
				acProvider = source;
				acNotifier = source->registerInterest(gIOGeneralInterest, powerChanged, this);
				auto fresh = source->copyProperty(kIOPMPSExternalConnectedKey);
				auto connected = OSDynamicCast(OSBoolean, fresh);
				acState = connected ? static_cast<int>(connected->isTrue()) : -1;
				OSSafeReleaseNULL(fresh);
			}
		}
		OSSafeReleaseNULL(property);
	}
	OSSafeReleaseNULL(iter);
	return acState;
}

// Cache a short projection of framebuffer zero's VBL timing outside the PM callback.
void IGFX::RPSControlPatch::observeFrequency(uint32_t value, uint32_t generation) {
#ifdef DEBUG
	if (!frequencyProfile || __atomic_exchange_n(&recordingFrequency, true, __ATOMIC_ACQUIRE))
		return;
	auto now = mach_absolute_time();
	auto wake = lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
	uint32_t ratio = value == UINT32_MAX ? 0 : value >> 23;
	unsigned band = ratio ? min(ratio / 3, 31U) : 32;
	if (generation == currentGeneration()) {
		if (previousFrequencyAt && previousFrequencyAt >= wake && now > previousFrequencyAt &&
			frequencyGeneration == generation) {
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(now - previousFrequencyAt, &ns);
			__atomic_fetch_add(&frequencyWindow, ns, __ATOMIC_RELAXED);
			if (band < 32 && band == previousBand && ns <= 250000000)
				__atomic_fetch_add(&frequencyBins[band], ns, __ATOMIC_RELAXED);
		}
		frequencyGeneration = generation;
		previousFrequencyAt = now;
		previousBand = band;
	}
	__atomic_store_n(&recordingFrequency, false, __ATOMIC_RELEASE);
#endif
}

void IGFX::RPSControlPatch::publishFrequency() {
#ifdef DEBUG
	uint64_t bins[32], covered = 0;
	for (unsigned i = 0; i < arrsize(bins); ++i) {
		auto ns = __atomic_load_n(&frequencyBins[i], __ATOMIC_RELAXED);
		covered += ns;
		bins[i] = ns / 1000000;
	}
	igpu->setProperty("weg-gt-50mhz-bins-ms", bins, sizeof(bins));
	igpu->setProperty("weg-gt-frequency-covered-ms", covered / 1000000, 64);
	igpu->setProperty("weg-gt-frequency-window-ms", __atomic_load_n(&frequencyWindow, __ATOMIC_RELAXED) / 1000000, 64);
#endif
}

void IGFX::RPSControlPatch::sampleVBL() {
	auto generation = currentGeneration();
	uint64_t anchor = 0, period = 0;
	if (discoveryGeneration != generation) {
		discoveryGeneration = generation;
		OSSafeReleaseNULL(timingFramebuffer);
		nextVBLDiscovery = 0;
	}
	if (timingFramebuffer && timingFramebuffer->isInactive())
		OSSafeReleaseNULL(timingFramebuffer);
	auto now = mach_absolute_time();
	auto iter = igpu && !timingFramebuffer && now >= nextVBLDiscovery ? IORegistryIterator::iterateOver(igpu, gIOServicePlane, kIORegistryIterateRecursively) : nullptr;
	if (iter) {
		while (auto obj = iter->getNextObject()) {
			auto fb = static_cast<IOFramebuffer *>(obj->metaCast("IOFramebuffer"));
			uint32_t index;
			if (fb && !fb->isInactive() && AppleIntelFramebufferExplorer::getIndex(fb, index) && !index) {
				fb->retain();
				timingFramebuffer = fb;
				break;
			}
		}
		iter->release();
		nextVBLDiscovery = now + 50 * busySamplePeriod;
	}
	if (timingFramebuffer)
		static_cast<IOFramebuffer *>(timingFramebuffer)->getVBLTime(&anchor, &period);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(period, &ns);
	if (ns < 4000000 || ns > 50000000)
		period = 0;
	__atomic_fetch_add(&vblSequence, 1U, __ATOMIC_SEQ_CST);
	__atomic_store_n(&vblGeneration, generation, __ATOMIC_SEQ_CST);
	__atomic_store_n(&vblAnchor, anchor, __ATOMIC_SEQ_CST);
	__atomic_store_n(&vblPeriod, period, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&vblSequence, 1U, __ATOMIC_SEQ_CST);
}

bool IGFX::RPSControlPatch::nearVBL(uint64_t now) {
	auto seq = __atomic_load_n(&vblSequence, __ATOMIC_SEQ_CST);
	auto anchor = __atomic_load_n(&vblAnchor, __ATOMIC_SEQ_CST);
	auto period = __atomic_load_n(&vblPeriod, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&vblGeneration, __ATOMIC_SEQ_CST) != currentGeneration() || (seq & 1) || seq != __atomic_load_n(&vblSequence, __ATOMIC_SEQ_CST) ||
		!period || !anchor || now < anchor || now - anchor >= 6 * period ||
		(lastWake && anchor < __atomic_load_n(lastWake, __ATOMIC_RELAXED)))
		return false;
	return period - (now - anchor) % period <= period / 10;
}

void IGFX::RPSControlPatch::refreshPolicy() {
	unsigned limit = 0;
	const int ac = externalPower(mach_absolute_time());
	uint32_t lo = 0, hi = 0;
	// Read the executing core's digital thermal sensor. Unknown input leaves Apple's request intact.
	if (ac >= 0 && thermalMsr && !thermalMsr(0x19c, &lo, &hi) && (lo & (1U << 31))) {
		unsigned margin = (lo >> 16) & 0x7f;
		if (!(lo & 1) && margin >= 15)
			limit = ac == 1 && margin >= 25 ? 2 : 1;
	}
	if (packageSensor) {
		if (!thermalMsr || thermalMsr(0x1b1, &lo, &hi) || lo == UINT32_MAX) {
			limit = 0;
		} else {
			unsigned margin = (lo >> 16) & 127;
			if ((lo & 1) || margin < 15)
				limit = 0;
			else if (margin < 25)
				limit = min(1U, limit);
		}
	}
	// Publish the power source with its ceiling as one snapshot.
	__atomic_store_n(&policyLimit, limit | (ac == 0 ? BatteryPolicy : 0U), __ATOMIC_RELAXED);
	__atomic_store_n(&policyGeneration, acConsumed, __ATOMIC_RELEASE);
	__atomic_store_n(&policyAt, mach_absolute_time(), __ATOMIC_RELEASE);
}

void IGFX::RPSControlPatch::reportEvent(const Event &event) {
	if (event.kind == RefreshVBL) {
		sampleVBL();
		return;
	}
	if (event.kind == RPSEventBusy) {
		if (mode == 5) {
			refreshPolicy();
		}
#ifdef DEBUG
		else if (mode == 3) {
			// Measure-only mode: the measurement is its whole output
			if (event.v0 == UINT32_MAX)
				unpublish("weg-rps-busy-permille");
			else
				publish("weg-rps-busy-permille", event.v0);
			publish("weg-rps-busy-status", busyStatus[event.v2 < BusyResultCount ? event.v2 : BusyNoController]);
			publish("weg-rps-high-load", event.v1 >> 16);
			if ((event.v1 & 0xffff) != 0xffff)
				publish("weg-rps-busy-upper-bound-permille", event.v1 & 0xffff);
			else
				unpublish("weg-rps-busy-upper-bound-permille");
		}
#endif
		return;
	}
	if (event.kind == RPSEventEPP) {
		sampleEPP();
		return;
	}
#ifdef DEBUG
	const auto events = event.kind;
	const auto v0 = event.v0;
	if (events == RC6EventTelemetry) {
		publishTelemetry(event);
		if (igpu && frequencyProfile) publishFrequency();
		auto ack = __atomic_load_n(&lastGTReleaseAck, __ATOMIC_RELAXED);
		if (ack != UINT32_MAX) publish("weg-power-last-gt-release-ack", ack);
		else unpublish("weg-power-last-gt-release-ack");
		return;
	}
	if (events == NativeRC6State && event.v1 == currentGeneration()) {
		publish("weg-gt-observed-reset-generation", __atomic_load_n(&resetRequests, __ATOMIC_RELAXED));
		publish("weg-gt-counter-discontinuities", __atomic_load_n(&discontinuities, __ATOMIC_RELAXED));
		publish("weg-gt-invalidation-epoch", currentGeneration());
		if (v0 == UINT32_MAX) unpublish("weg-rc6-control");
		else publish("weg-rc6-control", v0);
		publish("weg-rc6-status", v0 == UINT32_MAX ? "waiting for native readback" :
			(v0 & GEN6_RC_CTL_RC6_ENABLE) ? "enabled by Apple driver" : "disabled by Apple driver");
	}
#endif
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
#ifdef DEBUG
	// Gen9 mainstream RC6 residency is an uncore register
	uint32_t ticks = callbackIGFX->readRegister32(controller, GEN6_GT_GFX_RC6);
	// 0x138108 lies outside the Gen9 GT ForceWake ranges. Sample it independently
	// of the advisory ACK snapshot and reject ambiguous zero / all-ones baselines.
	if (!ticks)
		ticks = UINT32_MAX;
	const uint64_t now = mach_absolute_time();
	trackCounter(ticks, now);
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
#endif
}

// igfxpwr=16: the framebuffer enables PSR1/PSR2 with bit 31 of EDP_PSR_CTL/EDP_PSR2_CTL. Clearing it keeps
// the rest of the configuration the driver wrote, so the link stays as the driver set it up; PSR is never
// entered. Writes that disable PSR go through unchanged.
void IGFX::RPSControlPatch::filterPSR(void *controller, uint32_t address, uint32_t value) {
	// Clearing the enable bit keeps the rest of the configuration
	value &= ~(1U << 31);
	callbackIGFX->writeRegister32(controller, address, value);
}

// igfxpwr=32/64: DC_STATE_EN bits 1:0 select the deepest display C-state the DMC may enter (1 = DC5,
// 2 = DC6). Only lower what the driver asks for: 32 turns a DC6 request into DC5, 64 clears both. The
// other bits (e.g. DC9) are left alone.
void IGFX::RPSControlPatch::filterDCState(void *controller, uint32_t address, uint32_t value) {
	const uint32_t requested = value & 3U;
	uint32_t allowed = requested;
	if (callbackIGFX->powerOptions & DisableDC)
		allowed = 0;
	else if ((callbackIGFX->powerOptions & LimitDC5) && requested == 2)
		allowed = 1;
	if (allowed != requested)
		value = (value & ~3U) | allowed;
	callbackIGFX->writeRegister32(controller, address, value);
}

namespace {
// KBL/CFL eDP transcoder and display engine. Observe only Apple framebuffer reads.
struct DisplayObservation { uint32_t address; const char *property; };
[[maybe_unused]] constexpr DisplayObservation displayRegisters[] = {
	{0x6F840, "weg-power-psr1-status-raw"}, {0x6F940, "weg-power-psr2-status-raw"},
	{0x45504, "weg-power-dc-policy-raw"}, {0x80030, "weg-power-dc3-dc5-count"},
	{0x8002C, "weg-power-dc5-dc6-count"}, {0x43208, "weg-power-fbc-control-raw"},
	{0x46000, "weg-power-cdclk-control-raw"}, {0x43214, "weg-power-fbc-status2-raw"}
};
}

void IGFX::RPSControlPatch::observeDisplayRead(uint32_t address, uint32_t value, uint64_t epoch) {
#ifdef DEBUG
	static_assert(arrsize(displayRegisters) == DisplayRegisterCount, "display register cache size");
	if (value == UINT32_MAX)
		return;
	for (unsigned i = 0; i < arrsize(displayRegisters); ++i) {
		if (address == displayRegisters[i].address) {
			__atomic_store_n(&displayValues[i], (epoch << 32) | value, __ATOMIC_RELAXED);
			__atomic_fetch_or(&seenDisplay, 1U << i, __ATOMIC_RELEASE);
			return;
		}
	}
#endif
}

void IGFX::RPSControlPatch::sampleEPP() {
	uint32_t epp = UINT32_MAX, lo = 0, hi = 0;
	// Sample the worker CPU. Respect package delegation when its local EPP is not valid.
	auto irq = IOSimpleLockLockDisableInterrupt(logLock);
	auto epoch = lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
	if (callbackIGFX->lifecycle) epoch = callbackIGFX->token();
	if (readMsr && !readMsr(0x770, &lo, &hi) && (lo & 1) &&
		!readMsr(0x774, &lo, &hi)) {
		if (!(hi & (1U << 10)) || (hi & (1U << 28)) || !readMsr(0x772, &lo, &hi))
			epp = lo >> 24;
	}
	IOSimpleLockUnlockEnableInterrupt(logLock, irq);
	// Both directions follow the CPU's own preference: EPP 128-255 (power saving) defers the extra RP0
	// boost, EPP 0-63 (performance) boosts after one busy interval instead of two, 64-127 keeps the default.
	// Each hint lasts two sampling intervals and expires on a read failure. No MSR is written.
	const bool known = (mode == 1 || mode == 4 || mode == 5) && epp != UINT32_MAX;
	const bool defer = known && epp >= 128;
	const bool fast = mode == 1 && known && epp <= 63;
	const uint64_t until = mach_absolute_time() + 2 * telemetryInterval;
	__atomic_store_n(&deferBoostUntil, defer ? until : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&fastBoostUntil, fast ? until : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&eppEpoch, epoch, __ATOMIC_RELEASE);
#ifdef DEBUG
	if (!observe)
		return;
	if (epp == UINT32_MAX)
		unpublish("weg-power-hwp-epp");
	else
		publish("weg-power-hwp-epp", epp);
	publish("weg-power-defer-rp0", static_cast<uint32_t>(defer));
	publish("weg-power-boost-bias", defer ? "power saving: RP0 boost deferred" :
			fast ? "performance: RP0 boost after one busy interval" : "balanced: default RP0 boost");
#endif
}

void IGFX::RPSControlPatch::publishTelemetry(const Event &event) {
#ifdef DEBUG
	if (!igpu)
		return;
	if (event.v0 == UINT32_MAX)
		unpublish("weg-power-rc6-permille");
	else
		publish("weg-power-rc6-permille", event.v0);
	publish("weg-power-window-ms", event.v1);
	publish("weg-power-uptime-s", event.v2);
	if (callbackIGFX->lifecycle) {
		publish("weg-gpu-ready-generation", static_cast<uint32_t>(callbackIGFX->token() >> 3));
		publish("weg-fbc-surface-valid", static_cast<uint32_t>(__atomic_load_n(&callbackIGFX->surfaceToken, __ATOMIC_ACQUIRE) == callbackIGFX->token() &&
			__atomic_load_n(&callbackIGFX->surfaceMode, __ATOMIC_ACQUIRE) == __atomic_load_n(&callbackIGFX->modeGeneration, __ATOMIC_ACQUIRE)));
	}
	publish("weg-power-status", event.v0 == UINT32_MAX ? "waiting for a valid window" : "sampled");
	auto wake = callbackIGFX->lifecycle ? callbackIGFX->token() : lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
	if (wake != publishedEpoch) {
		publishedEpoch = wake;
		lastClock = UINT32_MAX;
		for (const auto &reg : displayRegisters)
			unpublish(reg.property);
		unpublish("weg-power-fbc-enabled");
		unpublish("weg-power-fbc-compressing");
		unpublish("weg-power-cdclk-transition-observed");
	}
	const uint32_t seen = __atomic_exchange_n(&seenDisplay, 0U, __ATOMIC_ACQUIRE);
	for (unsigned i = 0; i < arrsize(displayRegisters); ++i)
		if (seen & (1U << i)) {
			const auto observation = __atomic_load_n(&displayValues[i], __ATOMIC_ACQUIRE);
			if (callbackIGFX->lifecycle && static_cast<uint32_t>(observation >> 32) != static_cast<uint32_t>(wake))
				continue;
			auto value = static_cast<uint32_t>(observation);
			publish(displayRegisters[i].property, value);
			if (displayRegisters[i].address == 0x43208)
				publish("weg-power-fbc-enabled", value >> 31);
			if (displayRegisters[i].address == 0x43214)
				publish("weg-power-fbc-compressing", static_cast<uint32_t>((value & 0xfff) != 0));
			if (displayRegisters[i].address == 0x46000) {
				auto clock = value & 0x7ff;
				if (lastClock != UINT32_MAX && lastClock != clock)
					publish("weg-power-cdclk-transition-observed", 1U);
				lastClock = clock;
			}
		}
#endif
}

void IGFX::RPSControlPatch::sampleRC6(uint64_t now) {
#ifdef DEBUG
	auto controller = callbackIGFX->defaultController();
#endif
	if (diagnostics && now >= nextTelemetry) {
		nextTelemetry = now + telemetryInterval;
		auto epoch = lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
		if (callbackIGFX->lifecycle) epoch = callbackIGFX->token();
		if (epoch != telemetryEpoch) {
			telemetryEpoch = epoch;
			__atomic_store_n(&lastGTReleaseAck, UINT32_MAX, __ATOMIC_RELAXED);
			sampleTime = sampleWall = 0;
			if (!callbackIGFX->lifecycle)
				__atomic_store_n(&seenDisplay, 0U, __ATOMIC_RELEASE);
			__atomic_store_n(&deferBoostUntil, 0ULL, __ATOMIC_RELAXED);
			__atomic_store_n(&fastBoostUntil, 0ULL, __ATOMIC_RELAXED);
		}
		if (callbackIGFX->powerOptions & RespectEPP)
			postRC6Event(RPSEventEPP, 0);
#ifdef DEBUG
		if (observe && controller && callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32)
			sampleTelemetry(controller);
#endif
	}
#ifdef DEBUG
	if (rc6Fix) {
		auto wake = lastWake ? __atomic_load_n(lastWake, __ATOMIC_RELAXED) : 0;
		if (wake != nativeWake) {
			nativeWake = wake;
			__atomic_fetch_add(&resetGeneration, 1U, __ATOMIC_ACQ_REL);
		}
		if (!observe && controller && callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32)
			trackCounter(callbackIGFX->readRegister32(controller, GEN6_GT_GFX_RC6), now);
		auto generation = currentGeneration();
		auto sample = __atomic_load_n(&nativeRC6Sample, __ATOMIC_ACQUIRE);
		auto value = static_cast<uint32_t>(sample);
		if (generation != sample >> 32)
			value = UINT32_MAX;
		if ((value != lastReportedControl || generation != snapshotGeneration) &&
			postRC6Event(NativeRC6State, value, generation)) {
			lastReportedControl = value;
			snapshotGeneration = generation;
		}
	}
#endif
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

static uint64_t forceWakeDeadline(uint32_t milliseconds = FORCEWAKE_ACK_TIMEOUT_MS) {
	uint64_t deadline;
	clock_interval_to_deadline(milliseconds, kMillisecondScale, &deadline);
	return deadline;
}

// SafeForceWakeMultithreaded can run in an IRQ or under a spinlock. Never
// sleep here. Space out slow MMIO reads while keeping the common ACK fast.
bool IGFX::ForceWakeWorkaround::pollRegister(void *controller, uint32_t reg, uint32_t val, uint32_t mask, uint64_t deadline) {
	unsigned delay = 0;
	while (mach_absolute_time() < deadline && callbackIGFX->admitted()) {
		auto rd = callbackIGFX->readRegister32(controller, reg);
		if (rd == UINT32_MAX)
			return false;
		if ((rd & mask) == val)
			return true;
		if (delay)
			IODelay(delay);
		delay = delay ? min(16U, delay * 2) : 1;
	}
	return false;
}

bool IGFX::ForceWakeWorkaround::forceWakeWaitAckFallback(void *controller, uint32_t d, uint32_t val, uint32_t mask, uint64_t deadline) {
	auto &self = callbackIGFX->modForceWakeWorkaround;
	// All contexts share bit 15 on a domain. Never clear another caller's pulse.
	if (__atomic_fetch_or(&self.fallbackDomains, d, __ATOMIC_ACQUIRE) & d)
		return false;
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

void IGFX::RPSControlPatch::wakeRequest(bool set, uint32_t domains, uint32_t context) {
	if (!callbackIGFX->admitted() || !(domains & DOM_BLITTER) || context >= 15)
		return;
#ifdef DEBUG
	if (!set && observe && callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32) {
		auto controller = callbackIGFX->defaultController();
		if (controller)
			__atomic_store_n(&lastGTReleaseAck, callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_BLITTER_GEN9), __ATOMIC_RELAXED);
	}
#endif
	if (__atomic_load_n(&measurementStarted, __ATOMIC_ACQUIRE))
		hardwareBusy(mach_absolute_time(), 1U << context);
}

void IGFX::RPSControlPatch::observeWake(void *accelerator, bool set, uint32_t domains, uint32_t context) {
	auto &self = callbackIGFX->modRPSControlPatch;
	if (!set)
		self.wakeRequest(false, domains, context);
	self.orgObservedWake(accelerator, set, domains, context);
	if (set)
		self.wakeRequest(true, domains, context);
}

void IGFX::ForceWakeWorkaround::forceWake(void *accelerator, bool set, uint32_t dom, uint32_t ctx) {
	auto &self = callbackIGFX->modForceWakeWorkaround;
	if (!callbackIGFX->admitted())
		return self.orgForceWake(accelerator, set, dom, ctx);
	auto &rps = callbackIGFX->modRPSControlPatch;
	if (!set && rps.wantsWakeHook())
		rps.wakeRequest(false, dom, ctx);
	auto controller = callbackIGFX->defaultController();
	if (!controller || !callbackIGFX->modMMIORegistersWriteSupport.orgWriteRegister32 ||
		!callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32 ||
		ctx >= 15 || (dom & ~(DOM_RENDER | DOM_MEDIA | DOM_BLITTER))) {
		self.orgForceWake(accelerator, set, dom, ctx);
		if (set && rps.wantsWakeHook())
			rps.wakeRequest(true, dom, ctx);
		return;
	}

	const uint32_t mask = 1U << ctx;
	const uint32_t expected = set ? mask : 0;
	const auto deadline = forceWakeDeadline();
	const auto epoch = callbackIGFX->token();
	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
		if (!(dom & d))
			continue;
		if (mach_absolute_time() >= deadline || !callbackIGFX->beginAccess(controller, epoch)) {
			self.orgForceWake(accelerator, set, dom & ~(d - 1U), ctx);
			break;
		}
		callbackIGFX->writeRegister32(controller, regForDom(d), masked_field(mask, expected));
		IOPause(100);
		const bool failed = !pollRegister(controller, ackForDom(d), expected, mask, min(deadline, forceWakeDeadline(1))) &&
			!forceWakeWaitAckFallback(controller, d, expected, mask, deadline) &&
			!pollRegister(controller, ackForDom(d), expected, mask, deadline);
		callbackIGFX->endAccess();
		if (failed) {
			// The ABI has no error return. Hand the unresolved request to
			// Apple's original routine, with its own recovery/failure policy.
			self.orgForceWake(accelerator, set, d, ctx);
		}
	}
	if (set && rps.wantsWakeHook())
		rps.wakeRequest(true, dom, ctx);
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
}
