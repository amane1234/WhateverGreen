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

constexpr const char* const strForDom(unsigned d) {
	if (d == DOM_RENDER)
		return "Render";
	if (d == DOM_MEDIA)
		return "Media";
	if (d == DOM_BLITTER)
		return "Blitter";
	return "(unk)";
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
	if (available && info->videoBuiltin && !igpu) {
		igpu = info->videoBuiltin;
		igpu->retain();
	}

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
	if (rc6Fix) {
		rc6LogCall = thread_call_allocate(reinterpret_cast<thread_call_func_t>(logRC6), this);
		if (rc6LogCall) {
			enabled = true;
			requiresMMIORegistersWriteAccess = true;
			nanoseconds_to_absolutetime(1000ULL * 1000 * 1000, &rc6CheckInterval);
			SYSLOG(log, "RC6 monitor enabled (RPS control mode %u)", mode);
		} else {
			rc6Fix = false;
			SYSLOG(log, "Failed to allocate the RC6 monitor log call");
		}
	}

	// Submissions closer together than this mean the GPU is under sustained load
	nanoseconds_to_absolutetime(8 * 1000 * 1000, &busyInterval);

	// With Apple GuC (igfxfw=2) AGPM sets a minimum frequency (e.g. 500 MHz) that the host driver ignores
	// otherwise. igfxgtfloor=MHz caps it; any value at or below the hardware minimum (RPn) gives RPn.
	uint32_t floorMHz = 0;
	if (available && (PE_parse_boot_argn("igfxgtfloor", &floorMHz, sizeof(floorMHz)) ||
		WIOKit::getOSDataValue(info->videoBuiltin, "gt-floor-freq", floorMHz)) && floorMHz > 0) {
		gtFloorCap = (floorMHz + 49) / 50;
		enabled = true;
		SYSLOG(log, "GT floor capped at %u MHz", gtFloorCap * 50);
		publish("weg-gt-floor-cap", gtFloorCap * 50);
		publish("weg-gt-floor-status", "waiting for AGPM floor request");
	}

	if (enabled) {
		publish("weg-rps-control-mode", mode);
		if (rc6Fix)
			publish("weg-rc6-status", "monitoring");
	}
}

void IGFX::RPSControlPatch::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (mode != 0 || rc6Fix) {
		KernelPatcher::RouteRequest routeRequest = {
			"__ZL15pmNotifyWrapperjjPyPj",
			wrapPmNotifyWrapper,
			orgPmNotifyWrapper
		};

		if (!patcher.routeMultiple(index, &routeRequest, 1, address, size))
			SYSLOG(log, "Failed to route pmNotifyWrapper.");
	}

	// The doPMWork command numbers were verified on the KBL framebuffer only
	if (gtFloorCap && callbackIGFX->getRealFramebuffer(index) != &kextIntelKBLFb) {
		SYSLOG(log, "GT floor cap is only supported with the KBL framebuffer, disabled.");
		publish("weg-gt-floor-status", "disabled: not the KBL framebuffer");
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
}

IOReturn IGFX::RPSControlPatch::wrapDoPMWork(void *controller, uint32_t cmd, void *arg1, void *arg2, void *arg3) {
	auto &self = callbackIGFX->modRPSControlPatch;

	if ((cmd == PMWORK_GT2_FLOOR || cmd == PMWORK_GT3_FLOOR) && arg1 && self.gtFloorCap) {
		uint32_t requested = *static_cast<uint32_t *>(arg1);

		// Never go below the hardware minimum (RPn) or above the maximum (RP0)
		uint32_t cap = callbackIGFX->readRegister32(controller, GEN6_RP_STATE_CAP);
		uint32_t rp0 = cap & 0xFF;
		uint32_t rpn = (cap >> 16) & 0xFF;
		uint32_t floor = self.gtFloorCap;
		if (floor < rpn)
			floor = rpn;
		if (rp0 && floor > rp0)
			floor = rp0;

		// doPMWork runs on the framebuffer's command gate, so publishing (registry lock) is fine here
		if (self.gtFloorLogged != requested + 1) {
			self.gtFloorLogged = requested + 1;
			self.publish("weg-gt-floor-agpm", requested * 50);
			self.publish("weg-gt-floor-applied", (requested > floor ? floor : requested) * 50);
			self.publish("weg-gt-floor-rpn", rpn * 50);
			self.publish("weg-gt-floor-status", requested > floor ? "lowered AGPM floor" : "AGPM floor already low enough");
			SYSLOG(log, "GT%u floor: AGPM requested %u MHz, using %u MHz (RPn %u MHz)",
				   cmd == PMWORK_GT2_FLOOR ? 2 : 3, requested * 50, (requested > floor ? floor : requested) * 50, rpn * 50);
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

	if (self.mode == 0) {
		// RC6 monitor only
		int ret = self.orgPmNotifyWrapper(a0, a1, a2, freq);
		if (self.rc6Fix)
			self.checkRC6();
		return ret;
	}

	// Submissions only happen with the GT awake, which is when the RC6 registers can be read
	if (self.rc6Fix)
		self.checkRC6();

	if (self.mode == 2) {
		// Legacy mode: request the maximum RPS at every exec list submission.
		// This relies on RC6 (force wake clears) to save power. When RC6 is not entered again after
		// S3/S4, the GPU stays at RP0 for the rest of the session.
		uint32_t cfreq = 0;
		self.orgPmNotifyWrapper(a0, a1, a2, &cfreq);

		if (!self.freq_max) {
			self.freq_max = callbackIGFX->readRegister32(callbackIGFX->defaultController(), GEN6_RP_STATE_CAP) & 0xFF;
			DBGLOG(log, "Read RP0 %d", self.freq_max);
		}

		// Without RP0 (register not readable yet) keep the frequency the driver asked for: 0 would request
		// the lowest frequency instead of the highest one
		*freq = self.freq_max ? (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * self.freq_max : cfreq;
		return 0;
	}

	// Adaptive mode: let the driver (AGPM) pick the frequency, and only boost to RP0 while submissions
	// arrive back to back. Every request used to be RP0, so the frequency only dropped through RC6.
	// After S3/S4 sleep, the GPU was then pinned at the maximum clock. Now the next light submission
	// (cursor, compositor) restores the driver's own choice.
	int ret = self.orgPmNotifyWrapper(a0, a1, a2, freq);

	uint64_t now = mach_absolute_time();
	uint64_t last = __atomic_exchange_n(&self.lastNotify, now, __ATOMIC_RELAXED);
	// last > now: the timestamp came from another CPU a moment ago, treat it as busy
	bool busy = last != 0 && (now < last || now - last <= self.busyInterval);
	if (!busy)
		return ret;

	if (!self.freq_max) {
		self.freq_max = callbackIGFX->readRegister32(callbackIGFX->defaultController(), GEN6_RP_STATE_CAP) & 0xFF;
		DBGLOG(log, "Read RP0 %d", self.freq_max);
	}

	if (self.freq_max == 0)
		return ret;

	*freq = (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * self.freq_max;
	return 0;
}

void IGFX::RPSControlPatch::postRC6Event(uint32_t event, uint32_t v0, uint32_t v1, uint32_t v2) {
	// Called from the submission path, which may run in interrupt context: only record the event here
	// and log it from a thread call
	rc6LogValue[0] = v0;
	rc6LogValue[1] = v1;
	rc6LogValue[2] = v2;
	__atomic_fetch_or(&rc6Events, event, __ATOMIC_RELEASE);
	thread_call_enter(static_cast<thread_call_t>(rc6LogCall));
}

void IGFX::RPSControlPatch::logRC6(void *param0, void *) {
	auto &self = *static_cast<RPSControlPatch *>(param0);
	uint32_t events = __atomic_exchange_n(&self.rc6Events, 0, __ATOMIC_ACQUIRE);
	uint32_t v0 = self.rc6LogValue[0], v1 = self.rc6LogValue[1], v2 = self.rc6LogValue[2];

	if (events & RC6EventSnapshot) {
		SYSLOG(log, "RC6: recorded the driver's RC6 configuration, RC_CONTROL 0x%08x PG_ENABLE 0x%08x", v0, v1);
		self.publish("weg-rc6-control", v0);
		self.publish("weg-rc6-status", "enabled");
	}
	if (events & RC6EventRestored) {
		SYSLOG(log, "RC6: RC6 was disabled (RC_CONTROL 0x%08x), restored RC_CONTROL 0x%08x (attempt %u)", v0, v1, v2);
		self.publish("weg-rc6-restores", ++self.rc6Restores);
		self.publish("weg-rc6-status", "restored after it was disabled");
	}
	if (events & RC6EventGiveUp) {
		SYSLOG(log, "RC6: the driver keeps RC6 disabled (RC_CONTROL 0x%08x), no more restores until RC6 is enabled again", v0);
		self.publish("weg-rc6-status", "driver keeps RC6 disabled, restores stopped");
	}
	if (events & RC6EventNever) {
		SYSLOG(log, "RC6: the driver never enabled RC6 (RC_CONTROL 0x%08x), nothing to restore", v0);
		self.publish("weg-rc6-control", v0);
		self.publish("weg-rc6-status", "never enabled by the driver");
	}
	if (events & RC6EventStall) {
		SYSLOG(log, "RC6: RC6 is enabled but residency has not advanced for 60 s. Forcewake render ack 0x%08x media ack 0x%08x "
			   "(bits held all the time mean a leaked forcewake reference keeps the GT out of RC6)", v0, v1);
		self.publish("weg-rc6-forcewake-render", v0);
		self.publish("weg-rc6-forcewake-media", v1);
		self.publish("weg-rc6-status", "enabled but no residency for 60 s (forcewake held?)");
	}
}

void IGFX::RPSControlPatch::checkRC6() {
	// Check at most once per second. The compare-exchange also keeps concurrent submissions out.
	uint64_t now = mach_absolute_time();
	uint64_t next = __atomic_load_n(&rc6NextCheck, __ATOMIC_RELAXED);
	if (now < next || !__atomic_compare_exchange_n(&rc6NextCheck, &next, now + rc6CheckInterval, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;

	auto controller = callbackIGFX->defaultController();
	if (!controller)
		return;

	// Read GT registers only while the render well is awake. Otherwise they read as 0, which would look
	// like a lost configuration.
	uint32_t ackRender = callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_RENDER_GEN9);
	if ((ackRender & 0xFFFF) == 0 || ackRender == 0xFFFFFFFF)
		return;

	uint32_t rcControl = callbackIGFX->readRegister32(controller, GEN6_RC_CONTROL);
	constexpr size_t controlIndex = arrsize(RC6_REGISTERS) - 2;
	constexpr size_t pgIndex = arrsize(RC6_REGISTERS) - 1;

	if (rcControl & GEN6_RC_CTL_RC6_ENABLE) {
		rc6LostCount = 0;
		if (!rc6EnabledSince)
			rc6EnabledSince = now;

		if (!rc6HaveSnapshot) {
			// Wait until the driver's configuration is stable
			if (++rc6GoodCount >= 3) {
				for (size_t i = 0; i < arrsize(RC6_REGISTERS); i++)
					rc6Snapshot[i] = callbackIGFX->readRegister32(controller, RC6_REGISTERS[i]);
				rc6HaveSnapshot = true;
				rc6LastResidency = callbackIGFX->readRegister32(controller, GEN6_GT_GFX_RC6);
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
		if (residency != rc6LastResidency) {
			rc6LastResidency = residency;
			rc6ResidencyChanged = now;
		} else if (now - rc6ResidencyChanged >= 60 * rc6CheckInterval && now >= rc6NextStallReport) {
			rc6NextStallReport = now + 600 * rc6CheckInterval;
			postRC6Event(RC6EventStall, ackRender, callbackIGFX->readRegister32(controller, FORCEWAKE_ACK_MEDIA_GEN9));
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

	// A restore that did not hold for 30 s means the driver disables RC6 on purpose: stop after 3
	if (rc6LastRestore && now - rc6LastRestore < 30 * rc6CheckInterval && ++rc6FailedRestores >= 3) {
		postRC6Event(RC6EventGiveUp, rcControl);
		return;
	}
	rc6LastRestore = now;

	for (size_t i = 0; i < arrsize(RC6_REGISTERS); i++) {
		if (RC6_REGISTERS[i] == GEN9_PG_ENABLE && rc6Snapshot[i] == 0)
			continue;
		if (callbackIGFX->readRegister32(controller, RC6_REGISTERS[i]) != rc6Snapshot[i])
			callbackIGFX->writeRegister32(controller, RC6_REGISTERS[i], rc6Snapshot[i]);
	}

	postRC6Event(RC6EventRestored, rcControl, rc6Snapshot[controlIndex], rc6FailedRestores + 1);
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

bool IGFX::ForceWakeWorkaround::pollRegister(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeout) {
	AbsoluteTime now, deadline;

	clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
	
	for (clock_get_uptime(&now); now < deadline; clock_get_uptime(&now)) {
		auto rd = callbackIGFX->readRegister32(callbackIGFX->defaultController(), reg);

//		DBGLOG(log, "Rd 0x%x = 0x%x, expected 0x%x", reg, rd, val);

		if ((rd & mask) == val)
			return true;
	}

	return false;
}

bool IGFX::ForceWakeWorkaround::forceWakeWaitAckFallback(uint32_t d, uint32_t val, uint32_t mask) {
	unsigned pass = 1;
	bool ack = false;
	auto controller = callbackIGFX->defaultController();
	
	do {
		pollRegister(ackForDom(d), 0, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);
		callbackIGFX->writeRegister32(controller, regForDom(d), fw_set(FORCEWAKE_KERNEL_FALLBACK));
		
		IODelay(10 * pass);
		pollRegister(ackForDom(d), FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);
		
		ack = (callbackIGFX->readRegister32(controller, ackForDom(d)) & mask) == val;

		callbackIGFX->writeRegister32(controller, regForDom(d), fw_clear(FORCEWAKE_KERNEL_FALLBACK));
	} while (!ack && pass++ < 10);
	
//	DBGLOG(log, "Force wake fallback used to %s %s in %u passes", set ? "set" : "clear", strForDom(d), pass);
	
	return ack;
}

/**
 * Port of i915 force wake. The difference with Apple code is as follows:
 * 1. 50 ms ACK timeouts, see https://patchwork.kernel.org/patch/7057561/
 * Apple code uses 90 ms.
 * 2. Use reserve bit as a fallback at primary ACK timeout, see https://patchwork.kernel.org/patch/10029821/
 */

// NOTE: We are either in IRQ context, or in a spinlock critical section
void IGFX::ForceWakeWorkaround::forceWake(void*, uint8_t set, uint32_t dom, uint32_t ctx) {
	// ctx 2: IRQ, 1: normal
	
	uint32_t ack_exp = set << ctx;
	uint32_t mask = 1 << ctx;
	uint32_t wr = ack_exp | (1 << ctx << 16);
	
	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1)
	if (dom & d) {
		callbackIGFX->writeRegister32(callbackIGFX->defaultController(), regForDom(d), wr);
		IOPause(100);
		if (!pollRegister(ackForDom(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
			!forceWakeWaitAckFallback(d, ack_exp, mask) &&
			!pollRegister(ackForDom(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
			PANIC(log, "ForceWake timeout for domain %s, expected 0x%x", strForDom(d), ack_exp);
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
		forceWake
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "Failed to route SafeForceWake.");
}
