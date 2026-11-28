//
//  kern_igfx.cpp
//  WhateverGreen
//
//  Copyright © 2018 vit9696. All rights reserved.
//

#include "kern_igfx.hpp"
#include "kern_fb.hpp"
#include "kern_guc.hpp"
#include "kern_agdc.hpp"
#include "kern_igfx_kexts.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_file.hpp>
#include <Headers/kern_iokit.hpp>

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <kern/thread_call.h>

IGFX *IGFX::callbackIGFX;

void IGFX::init() {
	callbackIGFX = this;
	// Initialize each submodule
	for (auto submodule : submodules)
		submodule->init();
	for (auto submodule : sharedSubmodules)
		submodule->init();
	auto &bdi = BaseDeviceInfo::get();
	auto generation = bdi.cpuGeneration;
	auto family = bdi.cpuFamily;
	auto model = bdi.cpuModel;
	switch (generation) {
		case CPUInfo::CpuGeneration::Penryn:
		case CPUInfo::CpuGeneration::Nehalem:
			// Do not warn about legacy processors (e.g. Xeon).
			break;
		case CPUInfo::CpuGeneration::Westmere:
			currentGraphics = &kextIntelHD;
			currentFramebuffer = &kextIntelHDFb;
			break;
		case CPUInfo::CpuGeneration::SandyBridge: {
			int tmp = 1;
			PE_parse_boot_argn("igfxsnb", &tmp, sizeof(tmp));
			moderniseAccelerator = tmp == 1;
			currentGraphics = &kextIntelHD3000;
			currentFramebuffer = &kextIntelSNBFb;
			break;
		}
		case CPUInfo::CpuGeneration::IvyBridge:
			currentGraphics = &kextIntelHD4000;
			currentFramebuffer = &kextIntelCapriFb;
			break;
		case CPUInfo::CpuGeneration::Haswell:
			currentGraphics = &kextIntelHD5000;
			currentFramebuffer = &kextIntelAzulFb;
			break;
		case CPUInfo::CpuGeneration::Broadwell:
			currentGraphics = &kextIntelBDW;
			currentFramebuffer = &kextIntelBDWFb;
			break;
		case CPUInfo::CpuGeneration::Skylake:
			// Fake SKL as KBL on 13.0+ due to the removal of SKL kexts
			// Or KBL kext can be used on SKL with older versions as well with KBL `device-id' and `ig-platform-id' injected.
			forceSKLAsKBL = getKernelVersion() >= KernelVersion::Ventura || checkKernelArgument("-igfxsklaskbl");
			if (forceSKLAsKBL) {
				DBGLOG("igfx", "enforcing KBL kexts and patches on Skylake");
				supportsGuCFirmware = true;
				currentGraphics = &kextIntelKBL;
				currentFramebuffer = &kextIntelKBLFb;
				modForceCompleteModeset.supported = modForceCompleteModeset.enabled = true;
				modRPSControlPatch.available = true;
				modForceWakeWorkaround.enabled = true;
				modTypeCCheckDisabler.enabled = getKernelVersion() >= KernelVersion::BigSur;
				modBlackScreenFix.available = true;
			} else {
				supportsGuCFirmware = true;
				currentGraphics = &kextIntelSKL;
				currentFramebuffer = &kextIntelSKLFb;
				modForceCompleteModeset.supported = modForceCompleteModeset.legacy = true; // not enabled, as on legacy operating systems it casues crashes.
				modTypeCCheckDisabler.enabled = getKernelVersion() >= KernelVersion::BigSur;
				modBlackScreenFix.available = true;
			}
			break;
		case CPUInfo::CpuGeneration::KabyLake:
			supportsGuCFirmware = true;
			currentGraphics = &kextIntelKBL;
			currentFramebuffer = &kextIntelKBLFb;
			modForceCompleteModeset.supported = modForceCompleteModeset.enabled = true;
			modRPSControlPatch.available = true;
			modForceWakeWorkaround.enabled = true;
			modTypeCCheckDisabler.enabled = getKernelVersion() >= KernelVersion::BigSur;
			modBlackScreenFix.available = true;
			break;
		case CPUInfo::CpuGeneration::CoffeeLake:
			supportsGuCFirmware = true;
			currentGraphics = &kextIntelKBL;
			currentFramebuffer = &kextIntelCFLFb;
			// Allow faking ask KBL
			currentFramebufferOpt = &kextIntelKBLFb;
			// Note, several CFL GPUs are completely broken. They freeze in IGMemoryManager::initCache due to incompatible
			// configuration, supposedly due to Apple not supporting new MOCS table and forcing Skylake-based format.
			// See: https://github.com/torvalds/linux/blob/135c5504a600ff9b06e321694fbcac78a9530cd4/drivers/gpu/drm/i915/intel_mocs.c#L181
			modForceCompleteModeset.supported = modForceCompleteModeset.enabled = true;
			modRPSControlPatch.available = true;
			modForceWakeWorkaround.enabled = true;
			modTypeCCheckDisabler.enabled = true;
			modBlackScreenFix.available = true;
			break;
		case CPUInfo::CpuGeneration::CannonLake:
			supportsGuCFirmware = true;
			currentGraphics = &kextIntelCNL;
			currentFramebuffer = &kextIntelCNLFb;
			modForceCompleteModeset.supported = modForceCompleteModeset.enabled = true;
			modTypeCCheckDisabler.enabled = true;
			break;
		case CPUInfo::CpuGeneration::IceLake:
			supportsGuCFirmware = true;
			currentGraphics = &kextIntelICL;
			currentFramebuffer = &kextIntelICLLPFb;
			currentFramebufferOpt = &kextIntelICLHPFb;
			modDVMTCalcFix.available = true;
			break;
		case CPUInfo::CpuGeneration::CometLake:
			supportsGuCFirmware = true;
			currentGraphics = &kextIntelKBL;
			currentFramebuffer = &kextIntelCFLFb;
			// Allow faking ask KBL
			currentFramebufferOpt = &kextIntelKBLFb;
			// Note, several CFL GPUs are completely broken. They freeze in IGMemoryManager::initCache due to incompatible
			// configuration, supposedly due to Apple not supporting new MOCS table and forcing Skylake-based format.
			// See: https://github.com/torvalds/linux/blob/135c5504a600ff9b06e321694fbcac78a9530cd4/drivers/gpu/drm/i915/intel_mocs.c#L181
			modForceCompleteModeset.supported = modForceCompleteModeset.enabled = true;
			modRPSControlPatch.available = true;
			modTypeCCheckDisabler.enabled = true;
			modBlackScreenFix.available = true;
			break;
		case CPUInfo::CpuGeneration::RocketLake:
		case CPUInfo::CpuGeneration::AlderLake:
		case CPUInfo::CpuGeneration::RaptorLake:
		case CPUInfo::CpuGeneration::ArrowLake:
			gPlatformGraphicsSupported = false;
			break;
		default:
			SYSLOG("igfx", "found an unsupported processor 0x%X:0x%X, please report this!", family, model);
			break;
	}

	if (currentGraphics)
		lilu.onKextLoadForce(currentGraphics);

	if (currentFramebuffer)
		lilu.onKextLoadForce(currentFramebuffer);

	if (currentFramebufferOpt)
		lilu.onKextLoadForce(currentFramebufferOpt);
}

void IGFX::deinit() {
	if (fbcRefreshCall) {
		auto call = static_cast<thread_call_t>(fbcRefreshCall);
		thread_call_cancel(call);
		while (!thread_call_free(call))
			IOSleep(1);
		fbcRefreshCall = nullptr;
	}
	if (fbcWakeCall) {
		auto call = static_cast<thread_call_t>(fbcWakeCall);
		thread_call_cancel(call);
		while (!thread_call_free(call))
			IOSleep(1);
		fbcWakeCall = nullptr;
	}
	OSSafeReleaseNULL(fbcGuardDevice);
	// Deinitialize each submodule
	for (auto submodule : submodules)
		submodule->deinit();
	for (auto submodule : sharedSubmodules)
		submodule->deinit();
}

void IGFX::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	bool switchOffGraphics = false;
	bool switchOffFramebuffer = false;
	framebufferPatch.framebufferId = info->reportedFramebufferId;

	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;

	if (info->videoBuiltin && !gPlatformGraphicsSupported) {
		switchOffGraphics = switchOffFramebuffer = true;
	} else if (info->videoBuiltin) {
		lifecycle = checkKernelArgument("-igfxpmlifecycle");
		guardFBCTimer = lifecycle || checkKernelArgument("-igfxfbctimerguard");
		if (guardFBCTimer && !fbcGuardDevice) {
			fbcGuardDevice = info->videoBuiltin;
			fbcGuardDevice->retain();
			fbcGuardDevice->setProperty("weg-fbc-timer-guard", "waiting for CFL framebuffer");
		}
		applyFramebufferPatch = loadPatchesFromDevice(info->videoBuiltin, info->reportedFramebufferId);

#ifdef DEBUG
		dumpFramebufferToDisk = checkKernelArgument("-igfxdump");
		dumpPlatformTable = checkKernelArgument("-igfxfbdump");
#endif

		if (supportsGuCFirmware && getKernelVersion() >= KernelVersion::HighSierra) {
			if (!PE_parse_boot_argn("igfxfw", &fwLoadMode, sizeof(fwLoadMode)))
				WIOKit::getOSDataValue<int32_t>(info->videoBuiltin, "igfxfw", fwLoadMode);
			if (fwLoadMode == FW_AUTO)
				fwLoadMode = info->firmwareVendor == DeviceInfo::FirmwareVendor::Apple ? FW_APPLE : FW_DISABLE;
		} else {
			fwLoadMode = FW_APPLE; /* Do nothing, GuC is either unsupported due to low OS or Apple */
		}
		
		disableAccel = checkKernelArgument("-igfxvesa");

		disableIGTelemetry = info->videoBuiltin->getProperty("disable-telemetry-load") != nullptr || checkKernelArgument("-igfxnotelemetryload");

		bool connectorLessFrame = info->reportedFramebufferIsConnectorLess;
		uint32_t options = 0;
		if (!disableAccel && !connectorLessFrame &&
			(cpuGeneration == CPUInfo::CpuGeneration::KabyLake ||
			 cpuGeneration == CPUInfo::CpuGeneration::CoffeeLake ||
			 cpuGeneration == CPUInfo::CpuGeneration::CometLake) &&
			PE_parse_boot_argn("igfxpwr", &options, sizeof(options)) && !(options & ~AllPowerOptions)) {
			// Release builds have no observation: bit 1 and bit 256 are ignored
			if (!ObservationBuild)
				options &= ~(Diagnose | RequestDynamicFBC);
			powerOptions = options;
			powerFramebufferId = info->reportedFramebufferId;
			if (options & RequestFBC) {
				if (WIOKit::awaitPublishing(info->videoBuiltin)) {
					auto raw = WIOKit::readPCIConfigValue(info->videoBuiltin, WIOKit::kIOPCIConfigGraphicsControl, 0, 16);
					auto gms = (raw >> 8) & 0xff;
					// Gen9 GGC GMS: 0x01-0x10 in 32 MB units, 0x20/0x30/0x40 = 1/1.5/2 GB, 0xF0-0xFE = 4-60 MB
					if ((raw & 0xffff) == 0xffff)
						gms = UINT32_MAX;
					else if (gms >= 1 && gms <= 16)
						stolenMB = gms * 32;
					else if (gms == 0x20 || gms == 0x30 || gms == 0x40)
						stolenMB = gms / 0x10 * 512;
					else if (gms >= 0xf0 && gms <= 0xfe)
						stolenMB = (gms - 0xef) * 4;
					info->videoBuiltin->setProperty("weg-fbc-gms-raw", gms, 32);
				}
				// The compressed buffer is reserved even when an explicit framebuffer-flags patch sets FBC.
				reserveFBCMemory = true;
				if (!framebufferPatchFlags.bits.FPFFlags) {
					powerFlagMask |= 0x4; // FBFramebufferCompression
					// Dynamic FBC rides on the same check and buffer; bit 256 stays observation only
					if (checkKernelArgument("-igfxdynfbc"))
						powerFlagMask |= 0x10; // FBDynamicFBCEnable
				}
			}
			if (!framebufferPatchFlags.bits.FPFFlags) {
				if (options & RequestDynamicFBC)
					info->videoBuiltin->setProperty("weg-dynamic-fbc-policy", "native only");
				if (options & RequestCDCLK)
					powerFlagMask |= 0x200000; // FBEnableDynamicCDCLK
			}
		}

		int gl = info->videoBuiltin->getProperty("disable-metal") != nullptr;
		PE_parse_boot_argn("igfxgl", &gl, sizeof(gl));
		forceOpenGL = gl == 1;

		int metal = info->videoBuiltin->getProperty("enable-metal") != nullptr;
		PE_parse_boot_argn("igfxmetal", &metal, sizeof(metal));
		forceMetal = metal == 1;

		// Automatically enable HDMI -> DP patches
		bool nohdmi = info->videoBuiltin->getProperty("disable-hdmi-patches") != nullptr;
		hdmiAutopatch = !applyFramebufferPatch && !connectorLessFrame && getKernelVersion() >= Yosemite &&
			cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge && !checkKernelArgument("-igfxnohdmi") && !nohdmi;
		
		// Set AAPL00,DualLink to zero on Westmere if applying single link patches.
		if (cpuGeneration == CPUInfo::CpuGeneration::Westmere && framebufferWestmerePatches.SingleLink) {
			uint8_t dualLinkBytes[] { 0x00, 0x00, 0x00, 0x00 };
			info->videoBuiltin->setProperty("AAPL00,DualLink", dualLinkBytes, sizeof(dualLinkBytes));
		}
		
		// Note that the order does matter
		// We first iterate through each submodule and redirect the request
		for (auto submodule : submodules)
			submodule->processKernel(patcher, info);
		
		// Then process shared submodules
		// A shared submodule will disable itself if no active submodule depends on it
		for (auto submodule : sharedSubmodules)
			submodule->processKernel(patcher, info);
		
		// Iterate through each submodule and see if we need to patch the graphics and the framebuffer kext
		auto submodulesRequiresFramebufferPatch = false;
		auto submodulesRequiresGraphicsPatch = false;
		for (auto submodule : submodules) {
			submodulesRequiresFramebufferPatch |= submodule->requiresPatchingFramebuffer;
			submodulesRequiresGraphicsPatch |= submodule->requiresPatchingGraphics;
		}
		
		// Ideally, we could get rid of these two lambda expressions
		auto requiresFramebufferPatches = [this]() {
			if (applyFramebufferPatch || hdmiAutopatch || powerFlagMask || reserveFBCMemory || guardFBCTimer)
				return true;
			if (dumpFramebufferToDisk || dumpPlatformTable)
				return true;
			return false;
		};

		auto requiresGraphicsPatches = [this]() {
			if (forceOpenGL)
				return true;
			if (forceMetal)
				return true;
			if (moderniseAccelerator)
				return true;
			if (fwLoadMode != FW_APPLE)
				return true;
			if (disableAccel)
				return true;
			if (disableIGTelemetry)
				return true;
			return false;
		};

		// Disable kext patching if we have nothing to do.
		switchOffFramebuffer = !requiresFramebufferPatches() && !submodulesRequiresFramebufferPatch;
		switchOffGraphics = !requiresGraphicsPatches() && !submodulesRequiresGraphicsPatch;
	} else {
		switchOffGraphics = switchOffFramebuffer = true;
	}

	if (switchOffGraphics && currentGraphics)
		currentGraphics->switchOff();

	if (switchOffFramebuffer) {
		if (currentFramebuffer)
			currentFramebuffer->switchOff();
		if (currentFramebufferOpt)
			currentFramebufferOpt->switchOff();
	}

	if (moderniseAccelerator) {
		KernelPatcher::RouteRequest request("__ZN9IOService20copyExistingServicesEP12OSDictionaryjj", wrapCopyExistingServices, orgCopyExistingServices);
		patcher.routeMultiple(KernelPatcher::KernelID, &request, 1);
	}
}

// The two object offsets below are private to the CFL driver in build 24H232.
static bool matchesFBCTimerLayout(mach_vm_address_t address, size_t size) {
	static constexpr uint8_t uuid[] = {
		0x6A, 0x12, 0x9C, 0x83, 0x8F, 0xE9, 0x34, 0xFD,
		0x92, 0x47, 0x7C, 0x48, 0x88, 0xAA, 0xEB, 0x31
	};
	if (!address || size < sizeof(mach_header_64))
		return false;
	auto *header = reinterpret_cast<const mach_header_64 *>(address);
	if (header->magic != MH_MAGIC_64 || header->sizeofcmds > size - sizeof(*header))
		return false;
	auto cursor = reinterpret_cast<const uint8_t *>(header + 1);
	size_t remaining = header->sizeofcmds;
	for (uint32_t i = 0; i < header->ncmds && remaining >= sizeof(load_command); ++i) {
		auto cmd = reinterpret_cast<const load_command *>(cursor);
		if (cmd->cmdsize < sizeof(*cmd) || cmd->cmdsize > remaining)
			return false;
		if (cmd->cmd == LC_UUID)
			return cmd->cmdsize == sizeof(uuid_command) &&
				memcmp(reinterpret_cast<const uuid_command *>(cmd)->uuid, uuid, sizeof(uuid)) == 0;
		cursor += cmd->cmdsize;
		remaining -= cmd->cmdsize;
	}
	return false;
}

void IGFX::installLifecycle(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	systemWake = reinterpret_cast<const uint64_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_gIOLastWakeAbsTime"));
	if (!systemWake)
		patcher.clearError();
	if (!systemWake || !matchesFBCTimerLayout(address, size) || !orgUpdateFBC || !orgFBCTimerFired ||
		!modMMIORegistersReadSupport.orgReadRegister32) {
		fbcGuardDevice->setProperty("weg-gpu-lifecycle", "disabled: missing prerequisite");
		return;
	}
	KernelPatcher::RouteRequest hooks[] {
		{"__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation", wrapPixels, orgPixels},
		{"__ZN21AppleIntelFramebuffer14setDisplayModeEii", wrapMode, orgMode},
		{"__ZN31AppleIntelFramebufferController13setPowerStateEmP9IOService", wrapControllerPower, orgControllerPower}
	};
	if (!patcher.routeMultiple(index, hooks, arrsize(hooks), address, size)) {
		patcher.clearError();
		fbcGuardDevice->setProperty("weg-gpu-lifecycle", "disabled: incomplete hooks");
		return;
	}
	__atomic_store_n(&powerToken, static_cast<uint64_t>(AwaitClock), __ATOMIC_RELEASE);
	fbcGuardDevice->setProperty("weg-gpu-lifecycle", "installed");
	hibernateState = reinterpret_cast<const uint32_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_gIOHibernateState"));
	if (!hibernateState)
		patcher.clearError();
	// The route keeps Apple from turning FBC back on while a check failed; WhateverGreen's own
	// off/on cycle calls the original function.
	KernelPatcher::RouteRequest writeControl("__ZN31AppleIntelFramebufferController15WriteFBCControlEj", wrapWriteFBCControl, orgWriteFBCControl);
	if (patcher.routeMultiple(index, &writeControl, 1, address, size) && orgWriteFBCControl)
		fbcWriteControl = orgWriteFBCControl;
	else
		patcher.clearError();
	if (fbcWriteControl)
		fbcRefreshCall = thread_call_allocate(reinterpret_cast<thread_call_func_t>(refreshFBC), nullptr);
	fbcGuardDevice->setProperty("weg-fbc-wake-refresh", !fbcRefreshCall ? "unavailable" :
		hibernateState ? "after hibernate restore" : "after every wake");
	fbcGuardDevice->setProperty("weg-fbc-supervision", fbcRefreshCall ? "installed" : "unavailable");
#ifdef DEBUG
	if (powerOptions & Diagnose) {
		fbcWakeCall = thread_call_allocate(reinterpret_cast<thread_call_func_t>(sampleFBCWake), nullptr);
		fbcGuardDevice->setProperty("weg-fbc-wake-diagnostics", fbcWakeCall ? "installed" : "unavailable");
	}
#endif
}

void IGFX::scheduleFBCWakeSample(void *controller, uint64_t epoch) {
#ifdef DEBUG
	if (!fbcWakeCall)
		return;
	__atomic_store_n(&fbcWakeController, controller, __ATOMIC_RELAXED);
	__atomic_store_n(&fbcWakeStep, 0U, __ATOMIC_RELAXED);
	// The first ready generation after boot is the baseline for later wakes.
	__atomic_store_n(&fbcWakeBoot, !__atomic_exchange_n(&fbcWakeScheduled, true, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
	__atomic_store_n(&fbcWakeEpoch, epoch, __ATOMIC_RELEASE);
	uint64_t deadline;
	clock_interval_to_deadline(3, kSecondScale, &deadline);
	thread_call_enter_delayed(static_cast<thread_call_t>(fbcWakeCall), deadline);
#endif
}

void IGFX::scheduleFBCRefresh(void *controller, uint64_t epoch, bool restart) {
	if (!fbcRefreshCall)
		return;
	__atomic_store_n(&fbcRefreshController, controller, __ATOMIC_RELAXED);
	__atomic_store_n(&fbcRefreshStep, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&fbcRestartPending, restart, __ATOMIC_RELAXED);
	__atomic_store_n(&fbcRefreshEpoch, epoch, __ATOMIC_RELEASE);
	uint64_t deadline;
	clock_interval_to_deadline(3, kSecondScale, &deadline);
	thread_call_enter_delayed(static_cast<thread_call_t>(fbcRefreshCall), deadline);
}

namespace {
// Gen9 display registers, as named in Linux i915_reg.h / intel_fbc_regs.h
constexpr uint32_t FBCCBBase = 0x43200;         // ILK_DPFC_CB_BASE: stolen memory offset of the compressed buffer
constexpr uint32_t FBCControl = 0x43208;        // ILK_DPFC_CONTROL
constexpr uint32_t FBCChicken = 0x43224;        // ILK_DPFC_CHICKEN
constexpr uint32_t DisplayArbiter = 0x45000;    // DISP_ARB_CTL
constexpr uint32_t ChickenMisc4 = 0x4208C;      // CHICKEN_MISC_4 (FBC stride override)
constexpr uint32_t StolenReserved = 0x1082C0;   // GEN6_STOLEN_RESERVED
constexpr uint32_t PlaneControl = 0x70180;      // PLANE_CTL(PIPE_A, PLANE_1)
constexpr uint32_t PlaneStride = 0x70188;       // PLANE_STRIDE
constexpr uint32_t PlaneSize = 0x70190;         // PLANE_SIZE
constexpr uint32_t PlaneSurface = 0x7019C;      // PLANE_SURF
constexpr uint32_t PipeFrameCount = 0x70040;    // PIPE_FRMCOUNT_G4X(PIPE_A)
constexpr uint32_t FBCEnable = 1U << 31;
// WaFbcNukeOnHostModify (bit 23) and WaFbcHighMemBwCorruptionAvoidance (bit 8): skl, kbl, cfl
constexpr uint32_t FBCChickenWorkarounds = 0x800100;
// WaFbcWakeMemOn (bit 31) and WaFbcTurnOffFbcWatermark (bit 15): skl, kbl, cfl
constexpr uint32_t ArbiterWorkarounds = 0x80008000;
// AppleIntelFramebufferController 24H232: memory slots of 0x28 bytes (+0 type, +4 offset, +0x18 first page
// physical address, +0x20 size), slot 4 is the compressed buffer; +0x37A8 is the offset Apple subtracts
// for ILK_DPFC_CB_BASE in hwFBCSet; +0x38A0 FBC feature, +0x38A8 FBC state recorded by WriteFBCControl.
constexpr uint32_t SlotTable = 0x37B8, SlotStride = 0x28, CFBSlot = 4;
constexpr uint64_t MiB = 1ULL << 20;
// Not a verdict: the plane FBC tracks is off, e.g. inside a modeset where hwFBCSet enables FBC before the
// plane. Nothing is compressed then; the next check of an enabled plane decides.
const char FBCPlaneOff[] = "primary plane off";
}

uint64_t IGFX::fbcSignature(const FBCCheck &c) {
	// Geometry identity of the surface FBC tracks, without its flipping address
	return (static_cast<uint64_t>(c.planeCtl & 0x8F001C33) << 32) ^ (static_cast<uint64_t>(c.planeStride) << 16) ^
		c.planeSize ^ (static_cast<uint64_t>(c.cbBase) << 8) ^ ((c.control >> 6) & 3);
}

const char *IGFX::readFBCState(IOService *controller, FBCCheck &c, bool native) {
	auto pci = OSDynamicCast(IOPCIDevice, fbcGuardDevice);
	if (!pci)
		return "GPU PCI device unavailable";
	// Stolen memory from the GPU's config space: GGC mirror (0x50, GMS) and BSM (0x5C), as i915 reads it
	const uint16_t ggc = pci->configRead16(WIOKit::kIOPCIConfigGraphicsControl);
	const uint32_t bsm = pci->configRead32(0x5C);
	const uint32_t gms = (ggc >> 8) & 0xFF;
	if (ggc != 0xFFFF && gms >= 1 && gms <= 16)
		c.stolenSize = gms * 32 * MiB;
	else if (gms == 0x20 || gms == 0x30 || gms == 0x40)
		c.stolenSize = gms / 0x10 * 512 * MiB;
	else if (gms >= 0xF0 && gms <= 0xFE)
		c.stolenSize = (gms - 0xEF) * 4 * MiB;
	c.stolenBase = bsm == UINT32_MAX ? 0 : bsm & 0xFFF00000;
	if (!c.stolenSize || !c.stolenBase)
		return "stolen memory size or base unknown";
	const struct { uint32_t reg; uint32_t *value; } registers[] {
		{FBCControl, &c.control}, {FBCCBBase, &c.cbBase}, {FBCChicken, &c.chicken}, {DisplayArbiter, &c.arbCtl},
		{ChickenMisc4, &c.misc4}, {StolenReserved, &c.reserved}, {PlaneControl, &c.planeCtl},
		{PlaneStride, &c.planeStride}, {PlaneSize, &c.planeSize}, {PlaneSurface, &c.planeSurf}
	};
	// Inside Apple's own register path the lifecycle gate does not apply (see nativeReadRegister32)
	for (auto &r : registers)
		if ((*r.value = native ? nativeReadRegister32(controller, r.reg) : readRegister32(controller, r.reg)) == UINT32_MAX &&
			r.reg != ChickenMisc4)
			return "display registers unreadable";
	c.signature = fbcSignature(c);
	return nullptr;
}

const char *IGFX::validateFBC(IOService *controller, FBCCheck &c) {
	// Linux intel_fbc_check_plane, Gen9 (DISPLAY_VER 9, not BXT/GLK)
	if (!(c.planeCtl & (1U << 31)))
		return FBCPlaneOff;
	const uint32_t format = (c.planeCtl >> 24) & 0xF, tiling = (c.planeCtl >> 10) & 7, rotation = c.planeCtl & 3;
	// pixel_format_is_valid: 8:8:8:8 or RGB565. i915 also leaves out formats with alpha; Apple's driver
	// compresses its primary plane without glitches, so the alpha mode (bits 5:4) is not a reason here.
	uint32_t cpp = 0;
	if (format == 4)
		cpp = 4;
	else if (format == 14)
		cpp = 2;
	if (!cpp)
		return "pixel format not supported by FBC";
	// tiling_is_valid (linear, X, Y on Gen9) and rotation_is_valid (no 90/270)
	uint32_t unit;
	switch (tiling) {
		case 0: unit = 64; break;
		case 1: unit = 512; break;
		case 4: unit = 128; break;
		default: return "surface tiling not supported by FBC";
	}
	if (rotation & 1)
		return "90/270 degree rotation not supported by FBC";
	// stride_is_valid: 64-byte multiple; Display WA #1105 (skl..cfl): linear needs a 512-byte multiple
	c.strideBytes = (c.planeStride & 0x3FF) * unit;
	c.width = (c.planeSize & 0x1FFF) + 1;
	c.height = ((c.planeSize >> 16) & 0x1FFF) + 1;
	if (!c.strideBytes || (c.strideBytes & 63) || (tiling == 0 && (c.strideBytes & 511)))
		return "plane stride not supported by FBC";
	if (c.strideBytes < c.width * cpp)
		return "plane stride below plane width";
	// intel_fbc_max_plane_size: 4096x4096 on Gen8/9
	if (c.width > 4096 || c.height > 4096)
		return "plane larger than 4096x4096";
	// intel_fbc_cfb_size / intel_fbc_cfb_stride: 4 bytes per pixel, 512-byte aligned, at most 2560 lines on Gen8+,
	// and skl_fbc_min_cfb_stride; divided by the compression limit Apple selected (ILK_DPFC_CONTROL bits 7:6)
	const uint32_t limitBits = (c.control >> 6) & 3;
	if (limitBits == 3)
		return "reserved FBC compression limit";
	c.limit = 1U << limitBits;
	auto align512 = [](uint64_t v) { return (v + 511) & ~511ULL; };
	const uint64_t cfbStride = max(align512(static_cast<uint64_t>(c.strideBytes / cpp) * 4), align512(static_cast<uint64_t>(c.width) * 4));
	c.cfbBytes = cfbStride * min(c.height, 2560U) / c.limit;

	// Compressed buffer base: 4 KiB aligned, past the first stolen page (WaSkipStolenMemoryFirstPage),
	// and exactly what Apple's hwFBCSet derives from its own compressed buffer slot
	const uint32_t slot = SlotTable + CFBSlot * SlotStride;
	const uint32_t appleBase = getMember<uint32_t>(controller, slot + 4) - getMember<uint32_t>(controller, 0x37A8);
	const uint32_t appleSize = getMember<uint32_t>(controller, slot + 0x20);
	if (!getMember<uint32_t>(controller, slot) || !appleSize)
		return "driver reserved no compressed buffer";
	if ((c.cbBase & 0xFFF) || c.cbBase < 4096)
		return "compressed buffer base misaligned";
	if (c.cbBase != appleBase)
		return "compressed buffer base differs from the driver's reservation";
	// The compressed data of this plane must fit what the driver reserved (fbmem 0 caused glitches)
	if (c.cfbBytes > appleSize) {
		__atomic_store_n(&fbcNeededBytes, c.cfbBytes, __ATOMIC_RELAXED);
		__atomic_store_n(&fbcReservedBytes, static_cast<uint64_t>(appleSize), __ATOMIC_RELAXED);
		__atomic_store_n(&fbcNeededWidth, c.width, __ATOMIC_RELAXED);
		__atomic_store_n(&fbcNeededHeight, c.height, __ATOMIC_RELAXED);
		return "compressed buffer smaller than the plane needs";
	}
	// intel_fbc_stolen_end: Gen9 FBC assumes the maximum 8 MiB stolen reserved range at the top
	const uint64_t cfbEnd = static_cast<uint64_t>(c.cbBase) + c.cfbBytes;
	if (c.stolenSize < 8 * MiB || cfbEnd > c.stolenSize - 8 * MiB)
		return "compressed buffer reaches the top 8 MiB of stolen memory";

	// Overlap with the scanout surface when it lives in stolen memory (located through its GTT entry).
	// Apple's slots are sized generously and its compressed buffer starts 4 KiB before the end of the first
	// slot (it skips the first stolen page), so the surface in use is checked rather than the slot sizes.
	{
		auto gtt = getMember<volatile uint64_t *>(controller, 0x1BE0);
		const uint64_t entries = getMember<uint32_t>(controller, 0x1BB8) / 8;
		const uint64_t index = (c.planeSurf & 0xFFFFF000) / 4096;
		const uint64_t phys = gtt && index + 1 < entries ? gtt[index] & 0x7FFFFFF000ULL : 0;
		if (phys >= c.stolenBase && phys < c.stolenBase + c.stolenSize) {
			const uint64_t start = phys - c.stolenBase, end = start + static_cast<uint64_t>(c.strideBytes) * c.height;
			if (c.cbBase < end && start < cfbEnd)
				return "compressed buffer overlaps the scanout surface";
		}
	}
	// Overlap with the hardware reserved region (GEN6_STOLEN_RESERVED, 1-8 MiB)
	if (c.reserved & 1) {
		const uint64_t rbase = c.reserved & 0xFFF00000, rsize = (1ULL << ((c.reserved >> 7) & 3)) * MiB;
		if (rbase < c.stolenBase + cfbEnd && c.stolenBase + c.cbBase < rbase + rsize)
			return "compressed buffer overlaps the hardware reserved region";
	}
	c.workaroundsMissing = (c.chicken & FBCChickenWorkarounds) != FBCChickenWorkarounds ||
		(c.arbCtl & ArbiterWorkarounds) != ArbiterWorkarounds;
	return validateGTT(controller, c);
}

const char *IGFX::validateGTT(IOService *controller, const FBCCheck &c) {
	// The GTT lives in stolen memory and is rebuilt after a hibernate restore. The scanout surface must be
	// mapped by present entries, and the stolen-backed framebuffer memory must map its own stolen pages.
	auto gtt = getMember<volatile uint64_t *>(controller, 0x1BE0);
	const uint64_t gttBytes = getMember<uint32_t>(controller, 0x1BB8);
	if (!gtt || gttBytes < 16)
		return "GTT unavailable";
	const uint64_t entries = gttBytes / 8 - 1;
	auto pte = [&](uint64_t index) -> uint64_t { return index < entries ? gtt[index] : 0; };
	const uint64_t surface = c.planeSurf & 0xFFFFF000;
	const uint64_t pages = (static_cast<uint64_t>(c.strideBytes) * c.height + 4095) / 4096;
	for (uint64_t i = 0; i < pages; i++)
		if (!(pte(surface / 4096 + i) & 1))
			return "scanout surface has unmapped GTT entries";
	bool linear = true;
	const uint32_t slots = min(static_cast<uint32_t>(getMember<uint8_t>(controller, 0x2C59)), CFBSlot);
	for (uint32_t i = 0; i < slots && linear; i++) {
		const uint32_t entry = SlotTable + i * SlotStride;
		const uint64_t phys = getMember<uint64_t>(controller, entry + 0x18);
		const uint64_t offset = getMember<uint32_t>(controller, entry + 4);
		const uint64_t count = getMember<uint32_t>(controller, entry + 0x20) / 4096;
		if (getMember<uint32_t>(controller, entry) != 1 || !phys || phys < c.stolenBase || phys >= c.stolenBase + c.stolenSize)
			continue;
		// Every 16th page and the last one keep the check short inside Apple's FBC enable
		for (uint64_t k = 0; k < count && linear; k = (k + 16 < count || k + 1 == count) ? k + 16 : count - 1)
			linear = (pte(offset / 4096 + k) & 0x7FFFFFF000ULL) == phys + k * 4096;
	}
	// The first check records how the driver maps its stolen framebuffer memory. Only a linear mapping
	// that later stops being linear (a GTT not rebuilt after a hibernate restore) turns FBC off.
	// A hibernate restore is not a baseline: its GTT is the mapping under test.
	uint32_t baseline = __atomic_load_n(&fbcGTTBaseline, __ATOMIC_ACQUIRE);
	if (!baseline && !__atomic_load_n(&fbcRestartPending, __ATOMIC_RELAXED))
		__atomic_compare_exchange_n(&fbcGTTBaseline, &baseline, linear ? 1U : 2U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (baseline == 1 && !linear)
		return "framebuffer GTT mapping not restored";
	return nullptr;
}

bool IGFX::waitFBCDisabled(IOService *controller) {
	// The disable latches at the next vblank; i915 also activates FBC only a vblank after a plane update.
	// Complete when the control reads back disabled and pipe A's frame counter advanced twice. A counter
	// that does not move (pipe idle, e.g. in PSR) completes after 50 ms, when no frame can still be fetched
	// with the old state. A control that still reads enabled after 100 ms does not complete.
	const uint32_t first = readRegister32(controller, PipeFrameCount);
	for (unsigned ms = 2; ms <= 100; ms += 2) {
		IOSleep(2);
		const uint32_t control = readRegister32(controller, FBCControl);
		if (control == UINT32_MAX)
			return false;
		if (control & FBCEnable)
			continue;
		const uint32_t frame = readRegister32(controller, PipeFrameCount);
		if (first != UINT32_MAX && frame != UINT32_MAX && frame - first >= 2)
			return true;
		if (ms >= 50 && (first == UINT32_MAX || frame == first))
			return true;
	}
	return false;
}

void IGFX::setFBCVeto(const char *reason) {
	const bool veto = reason != nullptr;
	__atomic_store_n(&fbcVeto, veto, __ATOMIC_RELEASE);
	// Published only when the verdict changes
	const char *text = reason ? reason : "ok";
	if (fbcGuardDevice && __atomic_exchange_n(&fbcVerdict, text, __ATOMIC_ACQ_REL) != text) {
		char status[160];
		const uint64_t needed = __atomic_load_n(&fbcNeededBytes, __ATOMIC_RELAXED);
		if (reason && needed && !strcmp(reason, "compressed buffer smaller than the plane needs"))
			// Round up to whole MiB, as framebuffer-fbmem takes them
			snprintf(status, sizeof(status), "FBC off: %s (%ux%u plane needs %llu MiB, reserved %llu MiB)", text,
				__atomic_load_n(&fbcNeededWidth, __ATOMIC_RELAXED), __atomic_load_n(&fbcNeededHeight, __ATOMIC_RELAXED),
				(needed + MiB - 1) / MiB, __atomic_load_n(&fbcReservedBytes, __ATOMIC_RELAXED) / MiB);
		else
			snprintf(status, sizeof(status), "%s%s", reason ? "FBC off: " : "", text);
		fbcGuardDevice->setProperty("weg-fbc-validation", status);
		if (reason)
			SYSLOG("igfx", "FBC turned off: %s", reason);
	}
}

void IGFX::wrapWriteFBCControl(void *controller, uint32_t value) {
	auto &s = *callbackIGFX;
	// Check when Apple turns FBC on: dynamic FBC is off most of the time, so the delayed checks can miss it.
	// Apple's hwFBCSet programs the buffer base and workarounds before this write. A passing result is
	// reused for the same power generation and surface; reads go straight to Apple's accessor.
	if ((value & FBCEnable) && !__atomic_load_n(&s.fbcVeto, __ATOMIC_ACQUIRE)) {
		auto service = static_cast<IOService *>(controller);
		FBCCheck check;
		if (!s.readFBCState(service, check, true)) {
			check.control = value;
			check.signature = fbcSignature(check);
			const auto epoch = s.token();
			if (epoch != __atomic_load_n(&s.fbcValidEpoch, __ATOMIC_ACQUIRE) ||
				check.signature != __atomic_load_n(&s.fbcValidSignature, __ATOMIC_ACQUIRE)) {
				const char *reason = s.validateFBC(service, check);
				if (!reason) {
					__atomic_store_n(&s.fbcValidSignature, check.signature, __ATOMIC_RELEASE);
					__atomic_store_n(&s.fbcValidEpoch, epoch, __ATOMIC_RELEASE);
				}
				if (reason != FBCPlaneOff)
					s.setFBCVeto(reason);
			}
		}
	}
	// While a check failed, Apple's FBC enables become disables, so its +0x38A8 state stays consistent.
	if (__atomic_load_n(&s.fbcVeto, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&s.fbcBlockedControl, value, __ATOMIC_RELEASE);
		value &= ~FBCEnable;
	} else {
		__atomic_store_n(&s.fbcBlockedControl, 0U, __ATOMIC_RELEASE);
	}
	s.orgWriteFBCControl(controller, value);
}

void IGFX::restoreAppleFBC(IOService *controller) {
	// Apple asked to enable FBC while it was held off: give it the request now that a check passed.
	const uint32_t blocked = __atomic_exchange_n(&fbcBlockedControl, 0U, __ATOMIC_ACQ_REL);
	if ((blocked & FBCEnable) && getMember<uint8_t>(controller, 0x38A0))
		fbcWriteControl(controller, blocked);
}

void IGFX::refreshFBC(void *, void *) {
	auto &s = *callbackIGFX;
	const auto epoch = __atomic_load_n(&s.fbcRefreshEpoch, __ATOMIC_ACQUIRE);
	auto controller = static_cast<IOService *>(__atomic_load_n(&s.fbcRefreshController, __ATOMIC_RELAXED));
	if (!controller || s.token() != epoch)
		return;
	auto workLoop = controller->getWorkLoop();
	if (workLoop) {
		// Gated with Apple's FBC timer and modesets, which run on the controller's work loop.
		workLoop->runAction([](OSObject *target, void *arg, void *, void *, void *) -> IOReturn {
			auto &s = *callbackIGFX;
			auto controller = static_cast<IOService *>(target);
			const auto epoch = *static_cast<const uint64_t *>(arg);
			const auto mode = __atomic_load_n(&s.modeGeneration, __ATOMIC_ACQUIRE);
			if (s.token() != epoch || !s.admitted() || (mode & 1) || controller->isInactive() || !getMember<uint8_t>(controller, 0x38A0))
				return kIOReturnSuccess;
			FBCCheck check;
			// Unreadable state is no verdict: skip this check rather than turn FBC off
			if (s.readFBCState(controller, check))
				return kIOReturnSuccess;
			const bool enabled = (check.control & FBCEnable) != 0;
			const bool held = __atomic_load_n(&s.fbcVeto, __ATOMIC_ACQUIRE) || __atomic_load_n(&s.fbcBlockedControl, __ATOMIC_ACQUIRE);
			// Nothing compresses now and nothing is held off: nothing to check.
			if (!enabled && !held)
				return kIOReturnSuccess;
			const char *reason = s.validateFBC(controller, check);
			if (reason == FBCPlaneOff)
				return kIOReturnSuccess;
#ifdef DEBUG
			if (s.fbcGuardDevice) {
				s.fbcGuardDevice->setProperty("weg-fbc-check-plane-ctl", check.planeCtl, 64);
				s.fbcGuardDevice->setProperty("weg-fbc-check-stride-bytes", check.strideBytes, 32);
				s.fbcGuardDevice->setProperty("weg-fbc-check-plane-size", check.planeSize, 64);
				s.fbcGuardDevice->setProperty("weg-fbc-check-cb-base", check.cbBase, 64);
				s.fbcGuardDevice->setProperty("weg-fbc-check-cfb-bytes", check.cfbBytes, 64);
				s.fbcGuardDevice->setProperty("weg-fbc-check-limit", check.limit, 32);
				s.fbcGuardDevice->setProperty("weg-fbc-check-stolen-mb", check.stolenSize / MiB, 64);
				s.fbcGuardDevice->setProperty("weg-fbc-check-workarounds-missing", static_cast<uint32_t>(check.workaroundsMissing), 32);
				s.fbcGuardDevice->setProperty("weg-fbc-check-result", reason ? reason : "valid");
				s.fbcGuardDevice->setProperty("weg-fbc-check-gtt-baseline", __atomic_load_n(&s.fbcGTTBaseline, __ATOMIC_RELAXED), 32);
				s.fbcGuardDevice->setProperty("weg-fbc-check-chicken", check.chicken, 64);
				s.fbcGuardDevice->setProperty("weg-fbc-check-arb-ctl", check.arbCtl, 64);
			}
#endif
			const bool appleOn = getMember<uint8_t>(controller, 0x38A8) == 1;
			if (reason) {
				// Off through Apple's own path, then keep Apple from enabling it again
				s.setFBCVeto(reason);
				if (enabled && appleOn) {
					s.fbcWriteControl(controller, check.control & ~FBCEnable);
					__atomic_store_n(&s.fbcBlockedControl, check.control, __ATOMIC_RELEASE);
					s.waitFBCDisabled(controller);
				}
				return kIOReturnSuccess;
			}
			s.setFBCVeto(nullptr);
			if (enabled)
				__atomic_store_n(&s.fbcBlockedControl, 0U, __ATOMIC_RELEASE); // Apple enabled FBC itself
			if (!enabled) {
				// The state Apple programmed is valid again: hand back an enable the veto held off.
				s.restoreAppleFBC(controller);
				return kIOReturnSuccess;
			}
			const bool restart = __atomic_load_n(&s.fbcRestartPending, __ATOMIC_RELAXED) || check.workaroundsMissing;
			if (!restart || !appleOn)
				return kIOReturnSuccess;
			// Re-initialise the compression state: FBC off until the disable completed, the Gen9 workaround
			// bits Apple's hwFBCSet sets, then on again for the same surface generation only.
			s.fbcWriteControl(controller, check.control & ~FBCEnable);
			if (!s.waitFBCDisabled(controller)) {
				// Unconfirmed disable: keep FBC off and remember Apple's enable for the next passing check.
				s.setFBCVeto("FBC disable did not complete");
				__atomic_store_n(&s.fbcBlockedControl, check.control, __ATOMIC_RELEASE);
				return kIOReturnSuccess;
			}
			if (check.workaroundsMissing) {
				s.writeRegister32(controller, FBCChicken, check.chicken | FBCChickenWorkarounds);
				s.writeRegister32(controller, DisplayArbiter, check.arbCtl | ArbiterWorkarounds);
			}
			FBCCheck after;
			// A power transition: Apple programs FBC again after power-up
			if (s.token() != epoch || !s.admitted())
				return kIOReturnSuccess;
			// A new surface generation: keep Apple's enable for the next check of that surface
			if (__atomic_load_n(&s.modeGeneration, __ATOMIC_ACQUIRE) != mode || s.readFBCState(controller, after) ||
				after.signature != check.signature) {
				__atomic_store_n(&s.fbcBlockedControl, check.control, __ATOMIC_RELEASE);
				return kIOReturnSuccess;
			}
			s.fbcWriteControl(controller, check.control);
			__atomic_fetch_add(&s.fbcRefreshes, 1ULL, __ATOMIC_RELAXED);
			return kIOReturnSuccess;
		}, controller, const_cast<uint64_t *>(&epoch));
#ifdef DEBUG
		if ((s.powerOptions & Diagnose) && s.fbcGuardDevice)
			s.fbcGuardDevice->setProperty("weg-fbc-wake-refreshes", __atomic_load_n(&s.fbcRefreshes, __ATOMIC_RELAXED), 64);
#endif
	}
	const auto step = __atomic_load_n(&s.fbcRefreshStep, __ATOMIC_RELAXED);
	if (!step && s.token() == epoch) {
		// Dynamic FBC can turn FBC on only later; check once more.
		__atomic_store_n(&s.fbcRefreshStep, 1U, __ATOMIC_RELAXED);
		uint64_t deadline;
		clock_interval_to_deadline(27, kSecondScale, &deadline);
		thread_call_enter_delayed(static_cast<thread_call_t>(s.fbcRefreshCall), deadline);
	}
}

void IGFX::sampleFBCWake(void *, void *) {
#ifdef DEBUG
	auto &s = *callbackIGFX;
	const auto epoch = __atomic_load_n(&s.fbcWakeEpoch, __ATOMIC_ACQUIRE);
	auto controller = __atomic_load_n(&s.fbcWakeController, __ATOMIC_RELAXED);
	if (!controller || !s.fbcGuardDevice || s.token() != epoch)
		return;
	static constexpr struct { uint32_t reg; const char *name; } registers[] {
		{0x43208, "dpfc-control"}, {0x43214, "dpfc-status2"}, {0x43224, "dpfc-chicken"},
		{0x45000, "disp-arb-ctl"}, {0x4208C, "chicken-misc4"}, {0x43200, "cb-base"}
	};
	uint32_t values[arrsize(registers)];
	for (size_t i = 0; i < arrsize(registers); i++)
		values[i] = s.readRegister32(controller, registers[i].reg);
	// A power transition during the reads makes the set inconsistent.
	if (s.token() != epoch)
		return;
	const auto step = __atomic_load_n(&s.fbcWakeStep, __ATOMIC_RELAXED);
	char prefix[24], key[64];
	snprintf(prefix, sizeof(prefix), "weg-fbc-%s-%s", __atomic_load_n(&s.fbcWakeBoot, __ATOMIC_RELAXED) ? "boot" : "wake", step ? "30s" : "3s");
	for (size_t i = 0; i < arrsize(registers); i++) {
		snprintf(key, sizeof(key), "%s-%s", prefix, registers[i].name);
		// 64-bit numbers: ioreg shows a 32-bit value with bit 31 set sign-extended
		s.fbcGuardDevice->setProperty(key, values[i], 64);
	}
	snprintf(key, sizeof(key), "%s-generation", prefix);
	s.fbcGuardDevice->setProperty(key, epoch >> 3, 64);
	if (s.hibernateState) {
		snprintf(key, sizeof(key), "%s-hibernate", prefix);
		s.fbcGuardDevice->setProperty(key, static_cast<uint32_t>(__atomic_load_n(&s.fbcWakeHibernate, __ATOMIC_RELAXED)), 32);
	}
	// A newer ready generation reschedules its own 3 s sample; only this generation queues its 30 s one.
	if (!step && s.token() == epoch) {
		__atomic_store_n(&s.fbcWakeStep, 1U, __ATOMIC_RELAXED);
		uint64_t deadline;
		clock_interval_to_deadline(27, kSecondScale, &deadline);
		thread_call_enter_delayed(static_cast<thread_call_t>(s.fbcWakeCall), deadline);
	}
#endif
}

IOReturn IGFX::wrapControllerPower(IOService *controller, unsigned long state, IOService *device) {
	auto &s = *callbackIGFX;
	auto old = s.token();
	if ((old & 7) == Closed)
		return s.orgControllerPower(controller, state, device);
	void *owner = nullptr;
	if (!__atomic_compare_exchange_n(&s.powerController, &owner, static_cast<void *>(controller), false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && owner != controller)
		return s.orgControllerPower(controller, state, device);
	uint64_t transition;
	do {
		transition = ((old & ~7ULL) + 8) | Transition;
	} while (!__atomic_compare_exchange_n(&s.powerToken, &old, transition, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
	// Drain bounded register accesses and ForceWake pulses before Apple powers down.
	while (__atomic_load_n(&s.activeAccesses, __ATOMIC_SEQ_CST))
		IOSleep(1);
	s.modBacklightSmoother.cancelPowerRequests();
	s.modRPSControlPatch.invalidatePower();
#ifdef DEBUG
	if (s.fbcGuardDevice)
		s.fbcGuardDevice->setProperty("weg-gpu-power-generation", transition >> 3, 64);
#endif
	// kIOHibernateStateWakingFromHibernate (2) lasts until hibernate post-wake, after the controller powers up.
	if (state == 2 && s.hibernateState)
		__atomic_store_n(&s.fbcWakeHibernate, __atomic_load_n(s.hibernateState, __ATOMIC_RELAXED) == 2, __ATOMIC_RELAXED);
	auto result = s.orgControllerPower(controller, state, device);
	// CFL 24H232 performs its power-up path at ordinal 2 and acknowledges synchronously.
	auto next = (transition & ~7ULL) | (state == 2 && !result ? AwaitClock : Off);
	__atomic_compare_exchange_n(&s.powerToken, &transition, next, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	return result;
}

IOReturn IGFX::wrapPixels(IOFramebuffer *fb, int mode, int depth, int aperture, IOPixelInformation *pixels) {
	auto &s = *callbackIGFX;
	auto epoch = s.token();
	auto serial = __atomic_load_n(&s.modeGeneration, __ATOMIC_ACQUIRE);
	auto result = s.orgPixels(fb, mode, depth, aperture, pixels);
	uint32_t index;
	if ((epoch & 7) != Ready || aperture != kIOFBSystemAperture || !AppleIntelFramebufferExplorer::getIndex(fb, index) || index ||
		getMember<void *>(fb, 0x1D0) != __atomic_load_n(&s.powerController, __ATOMIC_ACQUIRE))
		return result;
	// 24H232 getCurrentDisplayMode returns these two fields. A nested
	// getCurrentDisplayMode here could call back into display enumeration.
	if (!getMember<uint8_t>(fb, 0x1E0) || mode != getMember<int>(fb, 0xA1C0) || depth != getMember<int>(fb, 0xA1C4))
		return result;
	auto controller = getMember<void *>(fb, 0x1D0);
	auto platform = getMember<void *>(controller, 0x1C70);
	const uint32_t pool = getMember<uint32_t>(controller, 0x2C48);
	const uint32_t unit = getMember<uint32_t>(controller, 0x2C4C);
	const uint32_t count = getMember<uint8_t>(controller, 0x2C59);
	const uint32_t flags = platform ? getMember<uint32_t>(platform, 0x58) : 0;
	const uint32_t reserved = getMember<uint8_t>(controller, 0x38A2) == 1 ?
		getMember<uint32_t>(controller, 0x38A4) : (flags & 4U) ? getMember<uint32_t>(platform, 0x18) : 0;
	const uint64_t required = static_cast<uint64_t>(unit) * ((flags & 2U) ? 1 : count) + reserved + (9ULL << 20);
	bool fits = count && unit && reserved && required < pool &&
		reserved >= __atomic_load_n(&s.surfaceReservation, __ATOMIC_ACQUIRE) && !result && pixels && pixels->activeWidth && pixels->activeHeight && pixels->bytesPerRow &&
		pixels->bitsPerPixel && static_cast<uint64_t>(pixels->activeWidth) * pixels->bitsPerPixel <= 8ULL * pixels->bytesPerRow &&
		static_cast<uint64_t>(pixels->bytesPerRow) * pixels->activeHeight <= reserved;
	__atomic_store_n(&s.surfaceMode, serial, __ATOMIC_RELEASE);
	__atomic_store_n(&s.surfaceToken, fits ? epoch : UINT64_MAX, __ATOMIC_RELEASE);
	return result;
}

IOReturn IGFX::wrapMode(IOFramebuffer *fb, int mode, int depth) {
	auto &s = *callbackIGFX;
	uint32_t index;
	if (!AppleIntelFramebufferExplorer::getIndex(fb, index) || index)
		return s.orgMode(fb, mode, depth);
	__atomic_fetch_add(&s.modeGeneration, 1ULL, __ATOMIC_ACQ_REL);
	auto result = s.orgMode(fb, mode, depth);
	__atomic_fetch_add(&s.modeGeneration, 1ULL, __ATOMIC_ACQ_REL);
	if (!result && s.admitted()) {
		IOPixelInformation pixels {};
		fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &pixels);
		// Check the new surface against the FBC rules; no restart
		s.scheduleFBCRefresh(getMember<void *>(fb, 0x1D0), s.token(), false);
	}
	return result;
}

bool IGFX::fbcReady(void *controller) const {
	if (!lifecycle)
		return true;
	auto epoch = token();
	auto mode = __atomic_load_n(&modeGeneration, __ATOMIC_ACQUIRE);
	return controller && admitted() && !__atomic_load_n(&fbcVeto, __ATOMIC_ACQUIRE) && (epoch & 7) == Ready && !(mode & 1) &&
		controller == __atomic_load_n(&powerController, __ATOMIC_ACQUIRE) &&
		__atomic_load_n(&surfaceToken, __ATOMIC_ACQUIRE) == epoch &&
		__atomic_load_n(&surfaceMode, __ATOMIC_ACQUIRE) == mode && getMember<uint8_t>(controller, 0x38A0);
}

void IGFX::wrapFBCTimerFired(IOService *controller) {
	if (callbackIGFX->lifecycle && getMember<uint32_t>(controller, 0x38AC) != 0xFFFF &&
		(getMember<uint32_t>(controller, 0x38AC) != 0 ||
		 __atomic_load_n(&callbackIGFX->armedTimer, __ATOMIC_ACQUIRE) != getMember<void *>(controller, 0x1B60) ||
		 __atomic_load_n(&callbackIGFX->timerEpoch, __ATOMIC_ACQUIRE) != callbackIGFX->token()))
		return;
	// 24H232 may rearm the timer from the callback itself. Read the current
	// controller fields on each invocation, including the first one after wake.
	if (getMember<uint32_t>(controller, 0x38AC) != 0xFFFF &&
		(!callbackIGFX->fbcReady(controller) || controller->isInactive() || !getMember<void *>(controller, 0x1B60) || !getMember<void *>(controller, 0x2C00)))
		return;
	callbackIGFX->orgFBCTimerFired(controller);
}

void IGFX::wrapUpdateFBC(void *framebuffer, uint32_t event) {
	const auto epoch = callbackIGFX->token();
	auto controller = getMember<void *>(framebuffer, 0x1D0);
	if (!controller)
		return;
	// Events 1 and 4 can arm the transaction timer. Other events may cancel
	// it when a transition is pending. Preserve native idle cleanup otherwise.
	if ((event == 1 || event == 4) && (!callbackIGFX->fbcReady(controller) || static_cast<IOService *>(controller)->isInactive() || !getMember<uint8_t>(controller, 0x38A0)))
		return;
	if ((event == 1 || event == 4 || getMember<uint8_t>(controller, 0x1B68) == 1) && !getMember<void *>(controller, 0x1B60))
		return;
	if (callbackIGFX->lifecycle && (event == 1 || event == 4)) {
		auto slots = getMember<void **>(controller, 0x2C00);
		if (!slots || slots[0] != framebuffer)
			return;
		if (!getMember<uint8_t>(controller, 0x1B68)) {
			__atomic_store_n(&callbackIGFX->armedTimer, getMember<void *>(controller, 0x1B60), __ATOMIC_RELEASE);
			__atomic_store_n(&callbackIGFX->timerEpoch, epoch, __ATOMIC_RELEASE);
		}
	}
	callbackIGFX->orgUpdateFBC(framebuffer, event);
}

bool IGFX::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;

	if (currentGraphics && currentGraphics->loadIndex == index) {
		if (disableIGTelemetry) {
			auto symTelemetry = patcher.solveSymbol(index, "__ZN18IGTelemetryManager16prepareTelemetryEj");
			if (symTelemetry) {
				uint8_t ret[] {0xC3};
				patcher.routeBlock(symTelemetry, ret, sizeof(ret));
				if (patcher.getError() == KernelPatcher::Error::NoError) {
					DBGLOG("igfx", "disabled __ZN18IGTelemetryManager16prepareTelemetryEj");
				} else {
					SYSLOG("igfx", "failed to disable __ZN18IGTelemetryManager16prepareTelemetryEj with code %d", patcher.getError());
					patcher.clearError();
				}
			} else {
				SYSLOG("igfx", "failed to resolve __ZN18IGTelemetryManager16prepareTelemetryE code %d", patcher.getError());
				patcher.clearError();
			}
		}

		if (forceOpenGL || forceMetal || forceSKLAsKBL || moderniseAccelerator || fwLoadMode != FW_APPLE || disableAccel) {
			KernelPatcher::RouteRequest request("__ZN16IntelAccelerator5startEP9IOService", wrapAcceleratorStart, orgAcceleratorStart);
			patcher.routeMultiple(index, &request, 1, address, size);

			if (fwLoadMode == FW_GENERIC && getKernelVersion() <= KernelVersion::Mojave)
				loadIGScheduler4Patches(patcher, index, address, size);
		}
		
		// Note that the order is reversed at this stage
		// We first process shared submodules
		// If a shared submodule fails to process the acceleration driver,
		// all subdmoules that depend on it will be disabled automatically.
		for (auto submodule : sharedSubmodules)
			if (submodule->enabled)
				submodule->processGraphicsKext(patcher, index, address, size);
		
		// Then iterate through each submodule and redirect the request if and only if it is enabled
		for (auto submodule : submodules)
			if (submodule->enabled)
				submodule->processGraphicsKext(patcher, index, address, size);

		return true;
	}

	if ((currentFramebuffer && currentFramebuffer->loadIndex == index) ||
		(currentFramebufferOpt && currentFramebufferOpt->loadIndex == index)) {
		
		if (guardFBCTimer && getRealFramebuffer(index) == &kextIntelCFLFb) {
			if (matchesFBCTimerLayout(address, size)) {
				KernelPatcher::RouteRequest route("__ZN21AppleIntelFramebuffer9UpdateFBCEj", wrapUpdateFBC, orgUpdateFBC);
				if (patcher.routeMultiple(index, &route, 1, address, size)) {
					KernelPatcher::RouteRequest fired("__ZN31AppleIntelFramebufferController26TransactionCountTimerFiredEv",
						wrapFBCTimerFired, orgFBCTimerFired);
					bool guarded = patcher.routeMultiple(index, &fired, 1, address, size);
					if (!guarded)
						patcher.clearError();
					fbcGuardDevice->setProperty("weg-fbc-timer-callback-guard", guarded ? "installed" : "route failed");
					fbcGuardDevice->setProperty("weg-fbc-timer-guard", "installed");
					SYSLOG("igfx", "FBC timer guard installed");
				} else {
					fbcGuardDevice->setProperty("weg-fbc-timer-guard", "route failed");
					SYSLOG("igfx", "FBC timer guard unavailable: failed to route UpdateFBC");
					patcher.clearError();
				}
			} else {
				fbcGuardDevice->setProperty("weg-fbc-timer-guard", "UUID mismatch");
				SYSLOG("igfx", "FBC timer guard unavailable: unrecognized framebuffer UUID");
			}
		}

		// Note that the order is reversed at this stage
		// We first process shared submodules
		// If a shared submodule fails to process the framebuffer driver,
		// all subdmoules that depend on it will be disabled automatically.
		for (auto submodule : sharedSubmodules)
			if (submodule->enabled)
				submodule->processFramebufferKext(patcher, index, address, size);
		
		if (lifecycle && getRealFramebuffer(index) == &kextIntelCFLFb)
			installLifecycle(patcher, index, address, size);

		// Then iterate through each submodule and redirect the request if and only if it is enabled
		for (auto submodule : submodules)
			if (submodule->enabled)
				submodule->processFramebufferKext(patcher, index, address, size);

		if (applyFramebufferPatch || dumpFramebufferToDisk || dumpPlatformTable || hdmiAutopatch || powerFlagMask || reserveFBCMemory) {
			powerKblLayout = getRealFramebuffer(index) == &kextIntelKBLFb;
			framebufferStart = reinterpret_cast<uint8_t *>(address);
			framebufferSize = size;
			
			if (cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge) {
				if (cpuGeneration == CPUInfo::CpuGeneration::SandyBridge) {
					gPlatformListIsSNB = true;
					gPlatformInformationList = patcher.solveSymbol<void *>(index, "_PlatformInformationList", address, size);
				} else {
					gPlatformListIsSNB = false;
					gPlatformInformationList = patcher.solveSymbol<void *>(index, "_gPlatformInformationList", address, size);
				}

				DBGLOG("igfx", "platform is snb %d and list " PRIKADDR, gPlatformListIsSNB, CASTKADDR(gPlatformInformationList));
			}

			if ((gPlatformInformationList && cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge) || cpuGeneration == CPUInfo::CpuGeneration::Westmere) {
				auto fbGetOSInformation = "__ZN31AppleIntelFramebufferController16getOSInformationEv";
				if (cpuGeneration == CPUInfo::CpuGeneration::Westmere)
					fbGetOSInformation = "__ZN22AppleIntelHDGraphicsFB16getOSInformationEv";
				else if (cpuGeneration == CPUInfo::CpuGeneration::SandyBridge)
					fbGetOSInformation = "__ZN23AppleIntelSNBGraphicsFB16getOSInformationEv";
				else if (cpuGeneration == CPUInfo::CpuGeneration::IvyBridge)
					fbGetOSInformation = "__ZN25AppleIntelCapriController16getOSInformationEv";
				else if (cpuGeneration == CPUInfo::CpuGeneration::Haswell)
					fbGetOSInformation = "__ZN24AppleIntelAzulController16getOSInformationEv";
				else if (cpuGeneration == CPUInfo::CpuGeneration::Broadwell)
					fbGetOSInformation = "__ZN22AppleIntelFBController16getOSInformationEv";

				KernelPatcher::RouteRequest request(fbGetOSInformation, wrapGetOSInformation, orgGetOSInformation);
				patcher.routeMultiple(index, &request, 1, address, size);
			} else if (cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge) {
				SYSLOG("igfx", "failed to obtain gPlatformInformationList pointer with code %d", patcher.getError());
				patcher.clearError();
			}
			
			if (applyFramebufferPatch && cpuGeneration == CPUInfo::CpuGeneration::Westmere) {
				KernelPatcher::RouteRequest requestWestmere("__ZN22AppleIntelHDGraphicsFB8TrainFDIEiPNS_10CRTCParamsE", wrapTrainFDI, orgTrainFDI);
				patcher.routeMultiple(index, &requestWestmere, 1, address, size);
				
				applyWestmerePatches(patcher);
			}
		}
		return true;
	}

	return false;
}

// MARK: - Global Framebuffer Controller Access Support

void IGFX::FramebufferControllerAccessSupport::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::FramebufferControllerAccessSupport::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	// Enable if at least one active submodule relies on this shared module
	for (auto submodule : callbackIGFX->submodules)
		if (submodule->enabled)
			enabled |= submodule->requiresGlobalFramebufferControllersAccess;
	
	DBGLOG("igfx", "FCA: Enabled = %d.", enabled);
}

void IGFX::FramebufferControllerAccessSupport::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::SolveRequest request("_gController", controllers);
	if (!patcher.solveMultiple(index, &request, 1, address, size)) {
		SYSLOG("igfx", "FCA: Failed to resolve the symbol of global controllers. Will disable all submodules that rely on this one.");
		disableDependentSubmodules();
	}
}

void IGFX::FramebufferControllerAccessSupport::disableDependentSubmodules() {
	for (auto submodule : callbackIGFX->submodules)
		if (submodule->requiresGlobalFramebufferControllersAccess)
			submodule->enabled = false;
}

// MARK: - MMIO Registers Read Support

void IGFX::MMIORegistersReadSupport::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::MMIORegistersReadSupport::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	// Enable the verbose output if the boot argument is found
	verbose = checkKernelArgument("-igfxvamregs");
	enabled |= verbose || callbackIGFX->lifecycle;
	
	for (auto submodule : callbackIGFX->submodules)
		if (submodule->enabled)
			enabled |= submodule->requiresMMIORegistersReadAccess;
	
	DBGLOG("igfx", "RRS: Enabled = %d.", enabled);
}

void IGFX::MMIORegistersReadSupport::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	// Symbol Table:
	// IVB*: "__ZN25AppleIntelCapriController14ReadRegister32Em"
	// HSW*: "__ZN24AppleIntelAzulController14ReadRegister32Em"
	// BDW*: "__ZNK22AppleIntelFBController14ReadRegister32Em"
	// SKL+: "__ZN31AppleIntelFramebufferController14ReadRegister32Em"
	const char* symbol = nullptr;
	
	auto framebuffer = Value::of(callbackIGFX->getRealFramebuffer(index));
	if (framebuffer.isOneOf(&kextIntelCapriFb)) {
		symbol = "__ZN25AppleIntelCapriController14ReadRegister32Em";
		DBGLOG("igfx", "RRS: Will setup the read register module for IVB platform.");
	} else if (framebuffer.isOneOf(&kextIntelAzulFb)) {
		symbol = "__ZN24AppleIntelAzulController14ReadRegister32Em";
		DBGLOG("igfx", "RRS: Will setup the read register module for HSW platform.");
	} else if (framebuffer.isOneOf(&kextIntelBDWFb)) {
		symbol = "__ZNK22AppleIntelFBController14ReadRegister32Em";
		DBGLOG("igfx", "RRS: Will setup the read register module for BDW platform.");
	} else if (framebuffer.isOneOf(&kextIntelSKLFb, &kextIntelKBLFb, &kextIntelCFLFb, &kextIntelICLLPFb)) {
		symbol = "__ZN31AppleIntelFramebufferController14ReadRegister32Em";
		DBGLOG("igfx", "RRS: Will setup the read register module for SKL/KBL/CFL/ICL platform.");
	} else {
		SYSLOG("igfx", "RRS: Found an unsupported platform. Will disable all submodules that depend on RRS.");
		return disableDependentSubmodules();
	}
	
	KernelPatcher::RouteRequest request(symbol, wrapReadRegister32, orgReadRegister32);
	if (!patcher.routeMultiple(index, &request, 1, address, size)) {
		SYSLOG("igfx", "RRS: Failed to resolve the symbol of ReadRegister32. Will disable all submodules that rely on this one.");
		disableDependentSubmodules();
	}
}

void IGFX::MMIORegistersReadSupport::disableDependentSubmodules() {
	for (auto submodule : callbackIGFX->submodules)
		if (submodule->requiresMMIORegistersReadAccess)
			submodule->enabled = false;
}

uint32_t IGFX::MMIORegistersReadSupport::wrapReadRegister32(void *controller, uint32_t address) {
	auto &s = *callbackIGFX;
	auto epoch = s.token();
	if (s.lifecycle && (epoch & 7) == Ready && !s.wakeCurrent()) {
		// A restore can change the kernel wake stamp after controller PM returned.
		// Invalidate producer generations without sleeping in the MMIO hook.
		auto closed = ((epoch & ~7ULL) + 8) | Transition;
		if (__atomic_compare_exchange_n(&s.powerToken, &epoch, closed, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			s.modRPSControlPatch.invalidatePower();
			__atomic_compare_exchange_n(&s.powerToken, &closed, (closed & ~7ULL) | AwaitClock, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
		}
		epoch = s.token();
	}
	if (s.lifecycle && (epoch & 7) != Ready) {
		auto wake = s.systemWake ? __atomic_load_n(s.systemWake, __ATOMIC_ACQUIRE) : 0;
		auto value = s.modMMIORegistersReadSupport.orgReadRegister32(controller, address);
		if ((epoch & 7) == AwaitClock && address == 0x46000 && value != UINT32_MAX && (value & 0x7ff)) {
			void *owner = nullptr;
			if (__atomic_compare_exchange_n(&s.powerController, &owner, controller, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || owner == controller) {
				__atomic_store_n(&s.readyWake, wake, __ATOMIC_RELEASE);
				if (__atomic_compare_exchange_n(&s.powerToken, &epoch, (epoch & ~7ULL) | Ready, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
#ifdef DEBUG
					s.modRPSControlPatch.observeDisplayRead(address, value, (epoch & ~7ULL) | Ready);
					s.scheduleFBCWakeSample(controller, (epoch & ~7ULL) | Ready);
#endif
					// Every ready generation is checked; compression restarts after a hibernate restore.
					const bool boot = !__atomic_exchange_n(&s.fbcRefreshSeen, true, __ATOMIC_RELAXED);
					s.scheduleFBCRefresh(controller, (epoch & ~7ULL) | Ready, !boot &&
						(!s.hibernateState || __atomic_load_n(&s.fbcWakeHibernate, __ATOMIC_RELAXED)));
				}
			}
		}
		return value;
	}
	// Guard: Perform prologue injections
	auto prologueInjector = callbackIGFX->modMMIORegistersReadSupport.prologueList.getInjector(address);
	if (prologueInjector) {
		DBGLOG("igfx", "RRS: Found a prologue injector triggered by the register address 0x%x.", address);
		prologueInjector(controller, address);
	}
	
	// Guard: Perform replacer injections
	auto replacerInjector = callbackIGFX->modMMIORegistersReadSupport.replacerList.getInjector(address);
	if (replacerInjector) {
		DBGLOG("igfx", "RRS: Found a replacer injector triggered by the register value 0x%x.", address);
		return replacerInjector(controller, address);
	}
	
	// Invoke the original function
#ifdef DEBUG
	auto generation = (address == 0xA090 || address == 0xA01C) ? callbackIGFX->modRPSControlPatch.currentGeneration() : 0;
#endif
	uint32_t retVal = callbackIGFX->modMMIORegistersReadSupport.orgReadRegister32(controller, address);
	if (s.lifecycle && s.token() != epoch)
		return retVal;
#ifdef DEBUG
	// Observation of Apple's own reads: frequency profile, RC6 report and display registers
	switch (address) {
		case 0xA01C:
			callbackIGFX->modRPSControlPatch.observeFrequency(retVal, generation);
			break;
		case 0xA090:
			callbackIGFX->modRPSControlPatch.observeNativeRC6(retVal, generation);
			break;
		case 0x6F840: case 0x6F940: case 0x45504: case 0x80030:
		case 0x8002C: case 0x43208: case 0x43214: case 0x46000:
			if (callbackIGFX->powerOptions & (Diagnose | RequestDynamicFBC))
				callbackIGFX->modRPSControlPatch.observeDisplayRead(address, retVal, epoch);
			break;
	}
#endif
	if (callbackIGFX->modMMIORegistersReadSupport.verbose)
		DBGLOG("igfx", "RRS: Read MMIO Register = 0x%x; Value = 0x%x.", address, retVal);
	
	// Guard: Perform epilogue injections
	auto epilogueInjector = callbackIGFX->modMMIORegistersReadSupport.epilogueList.getInjector(address);
	if (epilogueInjector) {
		DBGLOG("igfx", "RRS: Found a epilogue injector triggered by the register value 0x%x.", address);
		return epilogueInjector(controller, address, retVal);
	}
	
	// Return the register value retrieved from the original function
	return retVal;
}

// MARK: - MMIO Registers Write Support

void IGFX::MMIORegistersWriteSupport::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::MMIORegistersWriteSupport::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	// Enable the verbose output if the boot argument is found
	verbose = checkKernelArgument("-igfxvamregs");
	enabled |= verbose;
	
	// Enable if at least one active submodule relies on this shared module
	for (auto submodule : callbackIGFX->submodules)
		if (submodule->enabled)
			enabled |= submodule->requiresMMIORegistersWriteAccess;
	
	DBGLOG("igfx", "RWS: Enabled = %d.", enabled);
}

void IGFX::MMIORegistersWriteSupport::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	// Symbol Table:
	// IVB*: "__ZN25AppleIntelCapriController15WriteRegister32Emj"
	// HSW*: "__ZN24AppleIntelAzulController15WriteRegister32Emj"
	// BDW*: "__ZN22AppleIntelFBController15WriteRegister32Emj"
	// SKL+: "__ZN31AppleIntelFramebufferController15WriteRegister32Emj"
	const char* symbol = nullptr;
	
	auto framebuffer = Value::of(callbackIGFX->getRealFramebuffer(index));
	if (framebuffer.isOneOf(&kextIntelCapriFb)) {
		symbol = "__ZN25AppleIntelCapriController15WriteRegister32Emj";
		DBGLOG("igfx", "RWS: Will setup the read register module for IVB platform.");
	} else if (framebuffer.isOneOf(&kextIntelAzulFb)) {
		symbol = "__ZN24AppleIntelAzulController15WriteRegister32Emj";
		DBGLOG("igfx", "RWS: Will setup the read register module for HSW platform.");
	} else if (framebuffer.isOneOf(&kextIntelBDWFb)) {
		symbol = "__ZN22AppleIntelFBController15WriteRegister32Emj";
		DBGLOG("igfx", "RWS: Will setup the read register module for BDW platform.");
	} else if (framebuffer.isOneOf(&kextIntelSKLFb, &kextIntelKBLFb, &kextIntelCFLFb, &kextIntelICLLPFb)) {
		symbol = "__ZN31AppleIntelFramebufferController15WriteRegister32Emj";
		DBGLOG("igfx", "RWS: Will setup the read register module for SKL/KBL/CFL/ICL platform.");
	} else {
		SYSLOG("igfx", "RWS: Found an unsupported platform. Will disable all submodules that depend on RWS.");
		return disableDependentSubmodules();
	}
	
	KernelPatcher::RouteRequest request(symbol, wrapWriteRegister32, orgWriteRegister32);
	if (!patcher.routeMultiple(index, &request, 1, address, size)) {
		SYSLOG("igfx", "RWS: Failed to resolve the symbol of WriteRegister32. Will disable all submodules that rely on this one.");
		disableDependentSubmodules();
	}
}

void IGFX::MMIORegistersWriteSupport::disableDependentSubmodules() {
	for (auto submodule : callbackIGFX->submodules)
		if (submodule->requiresMMIORegistersWriteAccess)
			submodule->enabled = false;
}

void IGFX::MMIORegistersWriteSupport::wrapWriteRegister32(void *controller, uint32_t address, uint32_t value) {
	// The injectors run synchronously inside Apple's own write, also during a power transition, so
	// the backlight registers fix and the PSR/DC policies apply as without -igfxpmlifecycle. Only
	// WhateverGreen's own asynchronous work (backlight fades, ForceWake, PM samples) is gated.
	// Guard: Perform prologue injections
	auto prologueInjector = callbackIGFX->modMMIORegistersWriteSupport.prologueList.getInjector(address);
	if (prologueInjector) {
		DBGLOG("igfx", "RWS: Found a prologue injector triggered by the register address 0x%x.", address);
		prologueInjector(controller, address, value);
	}
	
	// Guard: Perform replacer injections
	auto replacerInjector = callbackIGFX->modMMIORegistersWriteSupport.replacerList.getInjector(address);
	if (replacerInjector) {
		DBGLOG("igfx", "RWS: Found a replacer injector triggered by the register value 0x%x.", address);
		return replacerInjector(controller, address, value);
	}
	
	// Native framebuffer GT reset request. Direct accelerator MMIO can bypass this hook.
	if (address == 0x941c && value && value != UINT32_MAX)
		callbackIGFX->modRPSControlPatch.observedReset();
	// Invoke the original function
	callbackIGFX->modMMIORegistersWriteSupport.orgWriteRegister32(controller, address, value);
	if (callbackIGFX->modMMIORegistersWriteSupport.verbose)
		DBGLOG("igfx", "RWS: Write MMIO Register = 0x%x; Value = 0x%x.", address, value);
	
	// Guard: Perform epilogue injections
	auto epilogueInjector = callbackIGFX->modMMIORegistersWriteSupport.epilogueList.getInjector(address);
	if (epilogueInjector) {
		DBGLOG("igfx", "RWS: Found a epilogue injector triggered by the register value 0x%x.", address);
		return epilogueInjector(controller, address, value);
	}
}

// MARK: - Force Complete Modeset

void IGFX::ForceCompleteModeset::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::ForceCompleteModeset::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	uint32_t forceCompleteModeSet = 0;
	if (PE_parse_boot_argn("igfxfcms", &forceCompleteModeSet, sizeof(forceCompleteModeSet))) {
		enabled = supported && forceCompleteModeSet != 0;
		DBGLOG("weg", "force complete-modeset overriden by boot-argument %u -> %d", forceCompleteModeSet, enabled);
	} else if (WIOKit::getOSDataValue(info->videoBuiltin, "complete-modeset", forceCompleteModeSet)) {
		enabled = supported && forceCompleteModeSet != 0;
		DBGLOG("weg", "force complete-modeset overriden by device property %u -> %d", forceCompleteModeSet, enabled);
	} else if (info->firmwareVendor == DeviceInfo::FirmwareVendor::Apple) {
		enabled = false; // may interfere with FV2
		DBGLOG("weg", "force complete-modeset overriden by Apple firmware -> %d", enabled);
	} else if (callbackIGFX->powerOptions & NativeModeset) {
		// igfxpwr=128: let the Apple driver decide when a complete modeset is needed, avoiding the extra
		// full modesets the forced mode causes (blink, link retraining). igfxfcms or complete-modeset win.
		enabled = false;
		if (info->videoBuiltin)
			info->videoBuiltin->setProperty("weg-power-native-modeset", 1U, 32);
		DBGLOG("weg", "force complete-modeset disabled by igfxpwr -> %d", enabled);
	}
	
	if (enabled) {
		uint64_t fbs;
		if (PE_parse_boot_argn("igfxfcmsfbs", &fbs, sizeof(fbs)) ||
			WIOKit::getOSDataValue(info->videoBuiltin, "complete-modeset-framebuffers", fbs)) {
			for (size_t i = 0; i < arrsize(this->fbs); i++)
				this->fbs[i] = (fbs >> (8 * i)) & 0xffU;
			customised = true;
		}
	}
}

void IGFX::ForceCompleteModeset::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest request = {
		legacy ?
		"__ZN31AppleIntelFramebufferController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathPNS_10CRTCParamsE" :
		"__ZN31AppleIntelFramebufferController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathPNS_10CRTCParamsEPK29IODetailedTimingInformationV2",
		wrapHwRegsNeedUpdate,
		orgHwRegsNeedUpdate
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "FCM: Failed to route the function hwRegsNeedUpdate.");
}

bool IGFX::ForceCompleteModeset::wrapHwRegsNeedUpdate(void *controller, IORegistryEntry *framebuffer, void *displayPath, void *crtParams, void *detailedInfo) {
	// The framebuffer controller can perform panel fitter, partial, or a
	// complete modeset (see AppleIntelFramebufferController::hwSetMode).
	// In a dual-monitor CFL DVI+HDMI setup, only HDMI output was working after
	// boot: it was observed that for HDMI framebuffer a complete modeset
	// eventually occured, but for DVI it never did until after sleep and wake
	// sequence.
	//
	// Function AppleIntelFramebufferController::hwRegsNeedUpdate checks
	// whether a complete modeset needs to be issued. It does so by comparing
	// sets of pipes and transcoder parameters. For some reason, the result was
	// never true in the above scenario, so a complete modeset never occured.
	// Consequently, AppleIntelFramebufferController::LightUpTMDS was never
	// called for that framebuffer.
	//
	// Patching hwRegsNeedUpdate to always return true seems to be a rather
	// safe solution to that. Note that the root cause of the problem is
	// somewhere deeper.

	// On older Skylake versions this function has no detailedInfo and does not use framebuffer argument.
	// As a result the compiler does not pass framebuffer to the target function. Since the fix is disabled
	// by default for Skylake, just force complete modeset on all framebuffers when actually requested.
	if (callbackIGFX->modForceCompleteModeset.legacy)
		return true;

	// Either this framebuffer is in override list
	if (callbackIGFX->modForceCompleteModeset.customised) {
		return callbackIGFX->modForceCompleteModeset.inList(framebuffer) || callbackIGFX->modForceCompleteModeset.orgHwRegsNeedUpdate(controller, framebuffer, displayPath, crtParams, detailedInfo);
	}

	// Or it is not built-in, as indicated by AppleBacklightDisplay setting property "built-in" for
	// this framebuffer.
	// Note we need to check this at every invocation, as this property may reappear
	return !framebuffer->getProperty("built-in") || callbackIGFX->modForceCompleteModeset.orgHwRegsNeedUpdate(controller, framebuffer, displayPath, crtParams, detailedInfo);
}

// MARK: - Force Online Display

void IGFX::ForceOnlineDisplay::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::ForceOnlineDisplay::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	uint32_t forceOnline = 0;
	if (PE_parse_boot_argn("igfxonln", &forceOnline, sizeof(forceOnline))) {
		enabled = forceOnline != 0;
		DBGLOG("weg", "force online overriden by boot-argument %u", forceOnline);
	} else if (WIOKit::getOSDataValue(info->videoBuiltin, "force-online", forceOnline)) {
		enabled = forceOnline != 0;
		DBGLOG("weg", "force online overriden by device property %u", forceOnline);
	}

	if (enabled) {
		uint64_t fbs;
		if (PE_parse_boot_argn("igfxonlnfbs", &fbs, sizeof(fbs)) ||
			WIOKit::getOSDataValue(info->videoBuiltin, "force-online-framebuffers", fbs)) {
			for (size_t i = 0; i < arrsize(this->fbs); i++)
				this->fbs[i] = (fbs >> (8 * i)) & 0xffU;
			customised = true;
		}
	}
}

void IGFX::ForceOnlineDisplay::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest request = {
		"__ZN21AppleIntelFramebuffer16getDisplayStatusEP21AppleIntelDisplayPath",
		wrapGetDisplayStatus,
		orgGetDisplayStatus
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "FOD: Failed to route the function getDisplayStatus.");
}

uint32_t IGFX::ForceOnlineDisplay::wrapGetDisplayStatus(IORegistryEntry *framebuffer, void *displayPath) {
	// 0 - offline, 1 - online, 2 - empty dongle.
	uint32_t ret = callbackIGFX->modForceOnlineDisplay.orgGetDisplayStatus(framebuffer, displayPath);
	if (ret != 1) {
		if (callbackIGFX->modForceOnlineDisplay.customised)
			ret = callbackIGFX->modForceOnlineDisplay.inList(framebuffer) ? 1 : ret;
		else
			ret = 1;
	}

	DBGLOG("igfx", "getDisplayStatus forces %u", ret);
	return ret;
}

// MARK: - AGDC Disabler

void IGFX::AGDCDisabler::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::AGDCDisabler::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	int agdc = info->videoBuiltin->getProperty("disable-agdc") != nullptr ? 0 : 1;
	PE_parse_boot_argn("igfxagdc", &agdc, sizeof(agdc));
	enabled = agdc == 0;
}

void IGFX::AGDCDisabler::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest request = {
		"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",
		wrapFBClientDoAttribute,
		orgFBClientDoAttribute
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "AGDCD: Failed to route the function FBClientControl::doAttribute.");
}

IOReturn IGFX::AGDCDisabler::wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4, void *externalMethodArguments) {
	if (attribute == kAGDCRegisterCallback) {
		DBGLOG("igfx", "AGDCD: Ignoring AGDC registration in FBClientControl::doAttribute.");
		return kIOReturnUnsupported;
	}
	
	return callbackIGFX->modAGDCDisabler.orgFBClientDoAttribute(fbclient, attribute, unk1, unk2, unk3, unk4, externalMethodArguments);
}

// MARK: - Type-C Check Disabler

void IGFX::TypeCCheckDisabler::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::TypeCCheckDisabler::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	enabled &= !checkKernelArgument("-igfxtypec");
}

void IGFX::TypeCCheckDisabler::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (callbackIGFX->getRealFramebuffer(index) == &kextIntelCFLFb || getKernelVersion() >= KernelVersion::BigSur) {
		KernelPatcher::RouteRequest request = {
			"__ZN31AppleIntelFramebufferController17IsTypeCOnlySystemEv",
			wrapIsTypeCOnlySystem
		};
		if (!patcher.routeMultiple(index, &request, 1, address, size))
			SYSLOG("igfx", "TCCD: Failed to route the function IsTypeCOnlySystem.");
	}
}

bool IGFX::TypeCCheckDisabler::wrapIsTypeCOnlySystem(void *controller) {
	DBGLOG("igfx", "TCCD: Forcing IsTypeCOnlySystem false.");
	return false;
}

// MARK: - Black Screen Fix (HDMI/DVI Displays)

void IGFX::BlackScreenFix::init() {
	// We only need to patch the framebuffer driver
	requiresPatchingFramebuffer = true;
}

void IGFX::BlackScreenFix::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	// Black screen (ComputeLaneCount) happened from 10.12.4
	// It only affects SKL, KBL, and CFL drivers with a frame with connectors.
	if (!info->reportedFramebufferIsConnectorLess && available &&
		((getKernelVersion() == KernelVersion::Sierra && getKernelMinorVersion() >= 5) ||
		 getKernelVersion() >= KernelVersion::HighSierra)) {
		enabled = info->firmwareVendor != DeviceInfo::FirmwareVendor::Apple;
	}
}

void IGFX::BlackScreenFix::processFramebufferKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	bool foundSymbol = false;

	// Currently it is 10.14.1 and Kaby+...
	if (getKernelVersion() >= KernelVersion::Mojave &&
		BaseDeviceInfo::get().cpuGeneration >= CPUInfo::CpuGeneration::KabyLake) {
		KernelPatcher::RouteRequest request = {
			"__ZN31AppleIntelFramebufferController16ComputeLaneCountEPK29IODetailedTimingInformationV2jPj",
			wrapComputeLaneCountNouveau,
			orgComputeLaneCountNouveau
		};
		
		foundSymbol = patcher.routeMultiple(index, &request, 1, address, size);
	}

	if (!foundSymbol) {
		KernelPatcher::RouteRequest request = {
			"__ZN31AppleIntelFramebufferController16ComputeLaneCountEPK29IODetailedTimingInformationV2jjPj",
			wrapComputeLaneCount,
			orgComputeLaneCount
		};
		
		if (!patcher.routeMultiple(index, &request, 1, address, size))
			SYSLOG("igfx", "BSF: Failed to route the function ComputeLaneCount.");
	}
}

bool IGFX::BlackScreenFix::wrapComputeLaneCount(void *controller, void *detailedTiming, uint32_t bpp, int availableLanes, int *laneCount) {
	DBGLOG("igfx", "BSF: ComputeLaneCount: bpp = %u, available lanes = %d", bpp, availableLanes);

	// It seems that AGDP fails to properly detect external boot monitors. As a result computeLaneCount
	// is mistakengly called for any boot monitor (e.g. HDMI/DVI), while it is only meant to be used for
	// DP (eDP) displays. More details could be found at:
	// https://github.com/vit9696/Lilu/issues/27#issuecomment-372103559
	// Since the only problematic function is AppleIntelFramebuffer::validateDetailedTiming, there are
	// multiple ways to workaround it.
	// 1. In 10.13.4 Apple added an additional extended timing validation call, which happened to be
	// guardded by a HDMI 2.0 enable boot-arg, which resulted in one bug fixing the other, and 10.13.5
	// broke it again.
	// 2. Another good way is to intercept AppleIntelFramebufferController::RegisterAGDCCallback and
	// make sure AppleGraphicsDevicePolicy::_VendorEventHandler returns mode 2 (not 0) for event 10.
	// 3. Disabling AGDC by nopping AppleIntelFramebufferController::RegisterAGDCCallback is also fine.
	// Simply returning true from computeLaneCount and letting 0 to be compared against zero so far was
	// least destructive and most reliable. Let's stick with it until we could solve more problems.
	bool r = callbackIGFX->modBlackScreenFix.orgComputeLaneCount(controller, detailedTiming, bpp, availableLanes, laneCount);
	if (!r && *laneCount == 0) {
		DBGLOG("igfx", "BSF: Reporting worked lane count (legacy)");
		r = true;
	}

	return r;
}

bool IGFX::BlackScreenFix::wrapComputeLaneCountNouveau(void *controller, void *detailedTiming, int availableLanes, int *laneCount) {
	bool r = callbackIGFX->modBlackScreenFix.orgComputeLaneCountNouveau(controller, detailedTiming, availableLanes, laneCount);
	if (!r && *laneCount == 0) {
		DBGLOG("igfx", "reporting worked lane count (nouveau)");
		r = true;
	}

	return r;
}

// MARK: - PAVP Disabler

void IGFX::PAVPDisabler::init() {
	// We only need to patch the accelerator driver
	requiresPatchingGraphics = true;
}

void IGFX::PAVPDisabler::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	int pavpMode = info->reportedFramebufferIsConnectorLess || info->firmwareVendor == DeviceInfo::FirmwareVendor::Apple;
	if (!PE_parse_boot_argn("igfxpavp", &pavpMode, sizeof(pavpMode)))
		WIOKit::getOSDataValue(info->videoBuiltin, "igfxpavp", pavpMode);
	enabled = pavpMode == 0 && BaseDeviceInfo::get().cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge;
}

void IGFX::PAVPDisabler::processGraphicsKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	const char* symbol;
	switch (BaseDeviceInfo::get().cpuGeneration) {
		case CPUInfo::CpuGeneration::SandyBridge:
			symbol = "__ZN15Gen6Accelerator19PAVPCommandCallbackE22PAVPSessionCommandID_t18PAVPSessionAppID_tPjb";
			break;
			
		case CPUInfo::CpuGeneration::IvyBridge:
			symbol = "__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_t18PAVPSessionAppID_tPjb";
			break;
			
		default:
			symbol = "__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb";
			break;
	}

	KernelPatcher::RouteRequest request = {
		symbol,
		wrapPavpSessionCallback,
		orgPavpSessionCallback
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "PAVP: Failed to route the function PAVPCommandCallback.");
}

IOReturn IGFX::PAVPDisabler::wrapPavpSessionCallback(void *intelAccelerator, int32_t sessionCommand, uint32_t sessionAppId, uint32_t *a4, bool flag) {
	//DBGLOG("igfx, "pavpCallback: cmd = %d, flag = %d, app = %u, a4 = %s", sessionCommand, flag, sessionAppId, a4 == nullptr ? "null" : "not null");

	if (sessionCommand == 4) {
		DBGLOG("igfx", "PAVP: PavpSessionCallback: Enforcing error on cmd 4 (send to ring?)!");
		return kIOReturnTimeout; // or kIOReturnSuccess
	}

	return callbackIGFX->modPAVPDisabler.orgPavpSessionCallback(intelAccelerator, sessionCommand, sessionAppId, a4, flag);
}

// MARK: - Read Descriptors Patch

void IGFX::ReadDescriptorPatch::init() {
	// We only need to patch the accelerator driver
	requiresPatchingGraphics = true;
}

void IGFX::ReadDescriptorPatch::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
	// Starting from 10.14.4b1 Skylake+ graphics randomly kernel panics on GPU usage
	enabled = BaseDeviceInfo::get().cpuGeneration >= CPUInfo::CpuGeneration::Skylake && getKernelVersion() >= KernelVersion::Mojave;
}

void IGFX::ReadDescriptorPatch::processGraphicsKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	KernelPatcher::RouteRequest request = {
		"__ZNK25IGHardwareGlobalPageTable4readEyRyS0_",
		globalPageTableRead
	};
	
	if (!patcher.routeMultiple(index, &request, 1, address, size))
		SYSLOG("igfx", "RDP: Failed to route the function IGHardwareGlobalPageTable::read.");
}

bool IGFX::ReadDescriptorPatch::globalPageTableRead(void *hardwareGlobalPageTable, uint64_t address, uint64_t &physAddress, uint64_t &flags) {
	uint64_t pageNumber = address >> PAGE_SHIFT;
	uint64_t pageEntry = getMember<uint64_t *>(hardwareGlobalPageTable, 0x28)[pageNumber];
	// PTE: Page Table Entry for 4KB Page, page 82:
	// https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-kbl-vol05-memory_views.pdf.
	physAddress = pageEntry & 0x7FFFFFF000ULL; // HAW-1:12, where HAW is 39.
	flags = pageEntry & PAGE_MASK; // 11:0
	// Relevant flag bits are as follows:
	// 2 Ignored          Ignored (h/w does not care about values behind ignored registers)
	// 1 R/W: Read/Write  Write permission rights. If 0, write permission not granted for requests with user-level privilege
	//                    (and requests with supervisor-level privilege, if WPE=1 in the relevant extended-context-entry) to
	//                    the memory region controlled by this entry. See a later section for access rights.
	//                    GPU does not support Supervisor mode contexts.
	// 0 P: Present       This bit must be “1” to point to a valid Page.
	//
	// In 10.14.4b1 the return condition changed from (flags & 7U) != 0 to (flags & 1U) != 0. The change makes good sense to me, but
	// it results in crashes on 10.14.4b1 on many ASUS Z170 and Z270 boards with connector-less IGPU framebuffer.
	// The reason for that is that __ZNK31IGHardwarePerProcessPageTable6422readDescriptorForRangeERK14IGAddressRangePPN10IGPagePool14PageDescriptorE
	// unconditionally returns true but actually returns NULL PageDescriptor, which subsequently results in OSAddAtomic64(&this->counter)
	// __ZN31IGHardwarePerProcessPageTable6421mapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE writing to invalid address.
	//
	// Fault CR2: 0x0000000000000028, Error code: 0x0000000000000002, Fault CPU: 0x1, PL: 0, VF: 0
	// 0xffffff821885b8f0 : 0xffffff801d3c7dc4 mach_kernel : _OSAddAtomic64 + 0x4
	// 0xffffff821885b9e0 : 0xffffff7f9f3f1845 com.apple.driver.AppleIntelKBLGraphics :
	//                      __ZN31IGHardwarePerProcessPageTable6421mapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE + 0x6b
	// 0xffffff821885ba20 : 0xffffff7f9f40a3c3 com.apple.driver.AppleIntelKBLGraphics :
	//                      __ZN29IGHardwarePerProcessPageTable25synchronizePageDescriptorEPKS_RK14IGAddressRangeb + 0x51
	// 0xffffff821885ba50 : 0xffffff7f9f40a36b com.apple.driver.AppleIntelKBLGraphics :
	//                      __ZN29IGHardwarePerProcessPageTable15synchronizeWithIS_EEvPKT_RK14IGAddressRangeb + 0x37
	// 0xffffff821885ba70 : 0xffffff7f9f3b1716 com.apple.driver.AppleIntelKBLGraphics : __ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask + 0x98
	// 0xffffff821885baa0 : 0xffffff7f9f40a716 com.apple.driver.AppleIntelKBLGraphics : __ZN11IGAccelTask15initWithOptionsEP16IntelAccelerator + 0x108
	// 0xffffff821885bad0 : 0xffffff7f9f40a5f7 com.apple.driver.AppleIntelKBLGraphics : __ZN11IGAccelTask11withOptionsEP16IntelAccelerator + 0x31
	// 0xffffff821885baf0 : 0xffffff7f9f3bfaf0 com.apple.driver.AppleIntelKBLGraphics : __ZN16IntelAccelerator17createUserGPUTaskEv + 0x30
	// 0xffffff821885bb10 : 0xffffff7f9f2f7520 com.apple.iokit.IOAcceleratorFamily2 : __ZN14IOAccelShared24initEP22IOGraphicsAccelerator2P4task + 0x2e
	// 0xffffff821885bb50 : 0xffffff7f9f30c157 com.apple.iokit.IOAcceleratorFamily2 : __ZN22IOGraphicsAccelerator212createSharedEP4task + 0x33
	// 0xffffff821885bb80 : 0xffffff7f9f2faa95 com.apple.iokit.IOAcceleratorFamily2 : __ZN24IOAccelSharedUserClient211sharedStartEv + 0x2b
	// 0xffffff821885bba0 : 0xffffff7f9f401ca2 com.apple.driver.AppleIntelKBLGraphics : __ZN23IGAccelSharedUserClient11sharedStartEv + 0x16
	// 0xffffff821885bbc0 : 0xffffff7f9f2f8aaa com.apple.iokit.IOAcceleratorFamily2 : __ZN24IOAccelSharedUserClient25startEP9IOService + 0x9c
	// 0xffffff821885bbf0 : 0xffffff7f9f30ba3c com.apple.iokit.IOAcceleratorFamily2 : __ZN22IOGraphicsAccelerator213newUserClientEP4taskPvjPP12IOUserClient + 0x43e
	// 0xffffff821885bc80 : 0xffffff801d42a040 mach_kernel : __ZN9IOService13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient + 0x30
	// 0xffffff821885bcd0 : 0xffffff801d48ef9a mach_kernel : _is_io_service_open_extended + 0x10a
	// 0xffffff821885bd30 : 0xffffff801ce96b52 mach_kernel : _iokit_server_routine + 0x58d2
	// 0xffffff821885bd80 : 0xffffff801cdb506c mach_kernel : _ipc_kobject_server + 0x12c
	// 0xffffff821885bdd0 : 0xffffff801cd8fa91 mach_kernel : _ipc_kmsg_send + 0xd1
	// 0xffffff821885be50 : 0xffffff801cda42fe mach_kernel : _mach_msg_overwrite_trap + 0x3ce
	// 0xffffff821885bef0 : 0xffffff801cec3e87 mach_kernel : _mach_call_munger64 + 0x257
	// 0xffffff821885bfa0 : 0xffffff801cd5d2c6 mach_kernel : _hndl_mach_scall64 + 0x16
	//
	// Even so the change makes good sense to me, and most likely the real bug is elsewhere. The change workarounds the issue by also checking
	// for the W (writeable) bit in addition to P (present). Presumably this works because some code misuses ::read method to iterate
	// over page table instead of obtaining valid mapped physical address.
	return (flags & 3U) != 0;
}

// MARK: - TODO

OSObject *IGFX::wrapCopyExistingServices(OSDictionary *matching, IOOptionBits inState, IOOptionBits options) {
	if (matching && inState == kIOServiceMatchedState && options == 0) {
		auto name = OSDynamicCast(OSString, matching->getObject(gIONameMatchKey));
		if (name) {
			DBGLOG("igfx", "found matching request by name %s", name->getCStringNoCopy());
			if (name->isEqualTo("Gen6Accelerator")) {
				DBGLOG("igfx", "found and fixed Gen6Accelerator request");
				auto accel = OSString::withCString("IntelAccelerator");
				if (accel) {
					matching->setObject(gIONameMatchKey, accel);
					accel->release();
				}
			}
		}
	}

	return FunctionCast(wrapCopyExistingServices, callbackIGFX->orgCopyExistingServices)(matching, inState, options);
}

bool IGFX::applyDevelopmentPatches(IOService *that) {
	// skip if not requested
	if (callbackIGFX->fwLoadMode == FW_APPLE && !callbackIGFX->modForceWakeWorkaround.enabled) {
		return true;
	}

	auto devDict = OSDynamicCast(OSDictionary, that->getProperty("Development"));
	if (!devDict)
		return false;

	auto newDevDict = OSDictionary::withDictionary(devDict);
	if (!newDevDict)
		return false;

	// By default Apple drivers load Apple-specific firmware, which is incompatible.
	// On KBL they do it unconditionally, which causes infinite loop.
	// On 10.13 there is an option to ignore/load a generic firmware, which we set here.
	// On 10.12 it is not necessary.
	// On 10.15 an option is differently named but still there.
	// There are some laptops that support Apple firmware, for them we want it to be loaded explicitly.
	// REF: https://github.com/acidanthera/bugtracker/issues/748
	if (callbackIGFX->fwLoadMode != FW_APPLE) {
		// 1 - Automatic scheduler (Apple -> fallback to disabled)
		// 2 - Force disable via plist (removed as of 10.15)
		// 3 - Apple Scheduler
		// 4 - Reference Scheduler
		// 5 - Host Preemptive (as of 10.15)
		uint32_t scheduler;
		if (callbackIGFX->fwLoadMode == FW_GENERIC)
			scheduler = 4;
		else if (getKernelVersion() >= KernelVersion::Catalina)
			scheduler = 5;
		else
			scheduler = 2;
		auto num = OSNumber::withNumber(scheduler, 32);
		if (num) {
			newDevDict->setObject("GraphicsSchedulerSelect", num);
			num->release();
		}
	}

	// 0: Framebuffer's SafeForceWake
	// 1: IntelAccelerator::SafeForceWakeMultithreaded (or ForceWakeWorkaround when enabled)
	// The default is 1. Forcing 0 will result in hangs (due to misbalanced number of calls?)
	if (callbackIGFX->modForceWakeWorkaround.enabled) {
		auto num = OSNumber::withNumber(1ull, 32);
		if (num) {
			newDevDict->setObject("MultiForceWakeSelect", num);
			num->release();
		}
	}

	that->setProperty("Development", newDevDict);
	newDevDict->release();

	return true;
}

bool IGFX::applySklAsKblPatches(IOService *that) {
	DBGLOG("igfx", "disabling VP9 hw decode support on Skylake with KBL kexts");
	that->removeProperty("IOGVAXDecode");

	// SKL does not support 10-bit hardware encoding/decoding, and this causes freezing when attempting to do so.
	// The removal of profile 2 under VTSupportedProfileArray allows fallback to software encoding/decoding.
	// Thanks dhinakg and aben for finding this.
	const char *hevcCapProps[] = { "IOGVAHEVCDecodeCapabilities", "IOGVAHEVCEncodeCapabilities" };
	bool found = false;
	for (auto prop : hevcCapProps) {
		auto hevcCap = OSDynamicCast(OSDictionary, that->getProperty(prop));
		if (!hevcCap)
			continue;

		auto newHevcCap = OSDictionary::withDictionary(hevcCap);
		if (!newHevcCap)
			continue;

		auto vtSuppProf = OSDynamicCast(OSArray, newHevcCap->getObject("VTSupportedProfileArray"));
		if (!vtSuppProf) {
			newHevcCap->release();
			continue;
		}

		auto newVtSuppProf = OSArray::withArray(vtSuppProf);
		if (!newVtSuppProf) {
			newHevcCap->release();
			continue;
		}

		unsigned int count = newVtSuppProf->getCount();
		for (unsigned int i = 0; i < count; i++) {
			auto num = OSDynamicCast(OSNumber, newVtSuppProf->getObject(i));
			if (!num)
				continue;

			if (num->unsigned8BitValue() == 2) {
				DBGLOG("igfx", "removing profile 2 from VTSupportedProfileArray/%s index %u on Skylake with KBL kexts", prop, i);
				newVtSuppProf->removeObject(i);
				found = true;
				break;
			}
		}

		newHevcCap->setObject("VTSupportedProfileArray", newVtSuppProf);
		newVtSuppProf->release();

		that->setProperty(prop, newHevcCap);
		newHevcCap->release();
	}

	// NOTE: The patches/overriding above may not be sufficient.
	//       `AAPL,GfxYTile` with value `01` may be injected for the glitch fix.
	// Ref: https://github.com/acidanthera/bugtracker/issues/2088#issuecomment-1381357651

	return found;
}

bool IGFX::wrapAcceleratorStart(IOService *that, IOService *provider) {
	if (callbackIGFX->disableAccel)
		return false;
	
	if (!applyDevelopmentPatches(that))
		SYSLOG("igfx", "failed to apply dict Development patches");

	OSObject *metalPluginName = that->getProperty("MetalPluginName");
	if (metalPluginName) {
		metalPluginName->retain();
	}

	if (callbackIGFX->forceOpenGL) {
		DBGLOG("igfx", "disabling metal support");
		that->removeProperty("MetalPluginClassName");
		that->removeProperty("MetalPluginName");
		that->removeProperty("MetalStatisticsName");
	}

	if (callbackIGFX->moderniseAccelerator)
		that->setName("IntelAccelerator");
	
	if (callbackIGFX->forceSKLAsKBL && !applySklAsKblPatches(that)) {
		SYSLOG("igfx", "failed to apply patches for Skylake with KBL kexts");
	}

	bool ret = FunctionCast(wrapAcceleratorStart, callbackIGFX->orgAcceleratorStart)(that, provider);

	if (metalPluginName) {
		if (callbackIGFX->forceMetal) {
			DBGLOG("igfx", "enabling metal support");
			that->setProperty("MetalPluginName", metalPluginName);
		}
		metalPluginName->release();
	}

	return ret;
}

void IGFX::setFBCGeometry(uint32_t width, uint32_t height, uint32_t stride) {
	// The boot console can use a lower depth than the accelerated scanout. Size the compressed buffer as
	// Linux intel_fbc_cfb_size does on Gen9 at a 1:1 limit: 4 bytes per pixel, a 512-byte aligned stride
	// (skl_fbc_min_cfb_stride) and at most 2560 lines.
	if (width && height && stride) {
		const uint64_t pitch = (max(static_cast<uint64_t>(stride), static_cast<uint64_t>(width) * 4) + 511) & ~511ULL;
		fbcSurfaceBytes = pitch > UINT32_MAX ? UINT64_MAX : pitch * min(height, 2560U);
	}
}

void IGFX::applyPowerFlags(IOService *controller) {
	auto table = reinterpret_cast<uintptr_t>(gPlatformInformationList);
	auto start = reinterpret_cast<uintptr_t>(framebufferStart);
	if (!table || !powerFramebufferId || table < start || table - start >= framebufferSize)
		return;
	auto bytes = min(static_cast<size_t>(64 * 1024), framebufferSize - static_cast<size_t>(table - start));
	// Both tables start with a framebuffer ID, but their element strides and flags differ.
	auto apply = [&](auto *frame) -> bool {
		decltype(frame) match = nullptr;
		for (size_t offset = 0; offset + sizeof(uint32_t) * 2 <= bytes; offset += sizeof(*frame), ++frame) {
			// Require a terminator on an entry boundary before changing any flags.
			if (frame->framebufferId == UINT32_MAX && frame->pad == 0) {
				if (match) {
					uint32_t mask = powerFlagMask;
					if (reserveFBCMemory) {
						// FBMemMgr_Init (24H232 CFL): the framebuffer memory is one stolen unit with flag 0x2, otherwise
						// one per framebuffer memory slot. FBC stays on only while unit + fbmem + 1 MiB is below the stolen
						// pool (DVMT - 4 KiB) minus 8 MiB. The tables run after framebuffer-stolenmem/-fbmem patches.
						const bool wantFBC = ((match->flags.value | mask) & 4) != 0;
						const uint64_t count = match->fFBMemoryCount ? match->fFBMemoryCount : 1;
						const uint64_t unit = (match->flags.value & 2) ? match->fStolenMemorySize : match->fStolenMemorySize * count;
						// The boot mode is a lower bound. Keep the existing 9 MiB fallback for
						// smaller boot consoles; explicit/native reservations retain their size.
						const bool representable = fbcSurfaceBytes <= UINT32_MAX - 0xfffffU;
						const uint32_t rounded = representable ? static_cast<uint32_t>((fbcSurfaceBytes + 0xfffff) & ~0xfffffULL) : 0;
						const uint32_t buffer = match->fFramebufferMemorySize ? match->fFramebufferMemorySize :
							max(rounded, static_cast<uint32_t>(FBCBufferBytes));
						const uint64_t required = unit + buffer + (9ULL << 20) + 4096;
						const bool budgetOK = representable && buffer >= fbcSurfaceBytes && match->fFBMemoryCount && match->fStolenMemorySize && stolenMB && required < (static_cast<uint64_t>(stolenMB) << 20);
						if (!budgetOK) {
							match->flags.value &= ~4U;
							mask &= ~4U;
						}
						if ((match->flags.value | mask) & 0x10) {
							// Apple's timer only exists while FBC passes its memory check: never request dynamic FBC without it
							const auto &panel = match->connectors[0];
							const char *reason = nullptr;
							if (lifecycle && (token() & 7) == Closed)
								reason = "skipped: incomplete lifecycle hooks";
							else if (!budgetOK)
								reason = "skipped: invalid FBC reservation or DVMT";
							else if (!((match->flags.value | mask) & 4))
								reason = "skipped: FBC off";
							// MapFBToPort treats the port with index 0 as built-in whatever its type
							else if (match->fPortCount > arrsize(match->connectors) || !match->fPortCount || !match->fPipeCount ||
								!match->fFBMemoryCount)
								reason = "skipped: unexpected port, pipe or memory count";
							else if (panel.index != 0)
								reason = "skipped: connector 0 is not the built-in port (index 0)";
							if (reason) {
								match->flags.value &= ~0x10U;
								mask &= ~0x10U;
							}
							controller->setProperty("weg-dynamic-fbc-result", reason ? reason :
								(match->flags.value & 0x10) ? "already set in platform flags" : "requested");
						}
						// The table value (or an explicit framebuffer-fbmem) is kept; only a missing buffer is reserved.
						if (budgetOK && wantFBC && !match->fFramebufferMemorySize)
							match->fFramebufferMemorySize = buffer;
						__atomic_store_n(&surfaceReservation, budgetOK && wantFBC ? buffer : 0U, __ATOMIC_RELEASE);
						controller->setProperty("weg-fbc-surface-bytes", fbcSurfaceBytes, 64);
						controller->setProperty("weg-fbc-dvmt-mb", stolenMB, 32);
						controller->setProperty("weg-fbc-required-mb", static_cast<uint32_t>((required + 0xfffff) >> 20), 32);
						controller->setProperty("weg-fbc-platform-budget-ok", static_cast<uint32_t>(budgetOK), 32);
						controller->setProperty("weg-fbc-buffer-mb", budgetOK && wantFBC ? match->fFramebufferMemorySize >> 20 : 0, 32);
					}
					uint32_t flags = match->flags.value | mask;
					match->flags.value = flags;
					controller->setProperty("weg-power-framebuffer-flags", flags, 32);
					return true;
				}
				return false;
			}
			if (offset + sizeof(*frame) > bytes)
				return false;
			if (frame->framebufferId == powerFramebufferId && frame->fPortCount) {
				if (match)
					return false;
				match = frame;
			}
		}
		return false;
	};
	bool applied = powerKblLayout ? apply(static_cast<FramebufferSKL *>(gPlatformInformationList)) :
		apply(static_cast<FramebufferCFL *>(gPlatformInformationList));
	controller->setProperty("weg-power-framebuffer-matched", static_cast<uint32_t>(applied), 32);
}

bool IGFX::wrapGetOSInformation(IOService *that) {
	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;
	
#ifdef DEBUG
	if (callbackIGFX->dumpFramebufferToDisk) {
		char name[64];
		snprintf(name, sizeof(name), "/var/log/AppleIntelFramebuffer_%d_%d.%d", cpuGeneration, getKernelVersion(), getKernelMinorVersion());
		FileIO::writeBufferToFile(name, callbackIGFX->framebufferStart, callbackIGFX->framebufferSize);
		SYSLOG("igfx", "dumping framebuffer information to %s", name);
		uint32_t delay = 20000;
		PE_parse_boot_argn("igfxdumpdelay", &delay, sizeof(delay));
		IOSleep(delay);
	}
#endif

#ifdef DEBUG
	if (callbackIGFX->dumpPlatformTable)
		callbackIGFX->writePlatformListData("platform-table-native");
#endif

	if (callbackIGFX->applyFramebufferPatch && cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge)
		callbackIGFX->applyFramebufferPatches();
	else if (callbackIGFX->applyFramebufferPatch && cpuGeneration == CPUInfo::CpuGeneration::Westmere)
		callbackIGFX->applyWestmereFeaturePatches(that);
	else if (callbackIGFX->hdmiAutopatch)
		callbackIGFX->applyHdmiAutopatch();
	if (callbackIGFX->powerFlagMask || callbackIGFX->reserveFBCMemory)
		callbackIGFX->applyPowerFlags(that);

#ifdef DEBUG
	if (callbackIGFX->dumpPlatformTable)
		callbackIGFX->writePlatformListData("platform-table-patched");
#endif

	return FunctionCast(wrapGetOSInformation, callbackIGFX->orgGetOSInformation)(that);
}

bool IGFX::wrapLoadGuCBinary(void *that, bool flag) {
	bool r = false;
	DBGLOG("igfx", "attempting to load firmware for %d scheduler for cpu gen %d",
		   callbackIGFX->fwLoadMode, BaseDeviceInfo::get().cpuGeneration);

	if (callbackIGFX->firmwareSizePointer)
		callbackIGFX->performingFirmwareLoad = true;

	r = FunctionCast(wrapLoadGuCBinary, callbackIGFX->orgLoadGuCBinary)(that, flag);
	DBGLOG("igfx", "loadGuCBinary returned %d", r);

	callbackIGFX->performingFirmwareLoad = false;

	return r;
}

bool IGFX::wrapLoadFirmware(IOService *that) {
	DBGLOG("igfx", "load firmware setting sleep overrides %d", BaseDeviceInfo::get().cpuGeneration);

	// We have to patch the virtual table, because the original methods are very short.
	// See __ZN12IGScheduler415systemWillSleepEv and __ZN12IGScheduler413systemDidWakeEv
	// Note, that other methods are also not really implemented, so we may have to implement them ourselves sooner or later.
	(*reinterpret_cast<uintptr_t **>(that))[52] = reinterpret_cast<uintptr_t>(wrapSystemWillSleep);
	(*reinterpret_cast<uintptr_t **>(that))[53] = reinterpret_cast<uintptr_t>(wrapSystemDidWake);
	return FunctionCast(wrapLoadFirmware, callbackIGFX->orgLoadFirmware)(that);
}

void IGFX::wrapSystemWillSleep(IOService *that) {
	DBGLOG("igfx", "systemWillSleep GuC callback");
	// Perhaps we want to send a message to GuC firmware like Apple does for its own implementation?
}

void IGFX::wrapSystemDidWake(IOService *that) {
	DBGLOG("igfx", "systemDidWake GuC callback");

	// This is IGHardwareGuC class instance.
	auto &GuC = (reinterpret_cast<OSObject **>(that))[76];
	DBGLOG("igfx", "reloading firmware on wake discovered IGHardwareGuC %d", GuC != nullptr);
	if (GuC) {
		GuC->release();
		GuC = nullptr;
	}

	FunctionCast(wrapLoadFirmware, callbackIGFX->orgLoadFirmware)(that);
}

bool IGFX::wrapInitSchedControl(void *that, void *ctrl) {
	// This function is caled within loadGuCBinary, and it also uses shared buffers.
	// To avoid any issues here we ensure that the filtering is off.
	DBGLOG("igfx", "attempting to init sched control with load %d", callbackIGFX->performingFirmwareLoad);
	bool perfLoad = callbackIGFX->performingFirmwareLoad;
	callbackIGFX->performingFirmwareLoad = false;
	bool r = FunctionCast(wrapInitSchedControl, callbackIGFX->orgInitSchedControl)(that, ctrl);

#ifdef DEBUG
	if (callbackIGFX->fwLoadMode == FW_GENERIC) {
		struct ParamRegs {
			uint32_t bak[35];
			uint32_t params[10];
		};

		auto v = &static_cast<ParamRegs *>(that)->params[0];
		DBGLOG("igfx", "fw params1 %08X %08X %08X %08X %08X", v[0], v[1], v[2], v[3], v[4]);
		DBGLOG("igfx", "fw params2 %08X %08X %08X %08X %08X", v[5], v[6], v[7], v[8], v[9]);
	}
#endif

	callbackIGFX->performingFirmwareLoad = perfLoad;
	return r;
}

void *IGFX::wrapIgBufferWithOptions(void *accelTask, unsigned long size, unsigned int type, unsigned int flags) {
	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;
	void *r = nullptr;

	if (callbackIGFX->performingFirmwareLoad) {
		// Allocate a dummy buffer
		callbackIGFX->dummyFirmwareBuffer = Buffer::create<uint8_t>(size);
		// Select the latest firmware to upload
		DBGLOG("igfx", "preparing firmware for cpu gen %d with range 0x%lX", cpuGeneration, size);

		const void *fw = nullptr;
		const void *fwsig = nullptr;
		size_t fwsize = 0;
		size_t fwsigsize = 0;

		if (cpuGeneration == CPUInfo::CpuGeneration::Skylake) {
			fw = GuCFirmwareSKL;
			fwsig = GuCFirmwareSKLSignature;
			fwsize = GuCFirmwareSKLSize;
			fwsigsize = GuCFirmwareSignatureSize;
		} else {
			fw = GuCFirmwareKBL;
			fwsig = GuCFirmwareKBLSignature;
			fwsize = GuCFirmwareKBLSize;
			fwsigsize = GuCFirmwareSignatureSize;
		}

		// Allocate enough memory for the new firmware (should be 64K-aligned)
		unsigned long newsize = fwsize > size ? ((fwsize + 0xFFFF) & (~0xFFFF)) : size;
		r = FunctionCast(wrapIgBufferWithOptions, callbackIGFX->orgIgBufferWithOptions)(accelTask, newsize, type, flags);
		// Replace the real buffer with a dummy buffer
		if (r && callbackIGFX->dummyFirmwareBuffer) {
			// Copy firmware contents, update the sizes and signature
			auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
			if (status == KERN_SUCCESS) {
				// Upload the firmware ourselves
				callbackIGFX->realFirmwareBuffer = static_cast<uint8_t **>(r)[7];
				static_cast<uint8_t **>(r)[7] = callbackIGFX->dummyFirmwareBuffer;
				lilu_os_memcpy(callbackIGFX->realFirmwareBuffer, fw, fwsize);
				lilu_os_memcpy(callbackIGFX->signaturePointer, fwsig, fwsigsize);
				callbackIGFX->realBinarySize = static_cast<uint32_t>(fwsize);
				// Update the firmware size for IGScheduler4
				*callbackIGFX->firmwareSizePointer = static_cast<uint32_t>(fwsize);
				MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			} else {
				SYSLOG("igfx", "ig buffer protection upgrade failure %d", status);
			}
		} else if (callbackIGFX->dummyFirmwareBuffer) {
			SYSLOG("igfx", "ig shared buffer allocation failure");
			Buffer::deleter(callbackIGFX->dummyFirmwareBuffer);
			callbackIGFX->dummyFirmwareBuffer = nullptr;
		} else {
			SYSLOG("igfx", "dummy buffer allocation failure");
		}
	} else {
		r = FunctionCast(wrapIgBufferWithOptions, callbackIGFX->orgIgBufferWithOptions)(accelTask, size, type, flags);
	}

	return r;
}

uint64_t IGFX::wrapIgBufferGetGpuVirtualAddress(void *that) {
	if (callbackIGFX->performingFirmwareLoad && callbackIGFX->realFirmwareBuffer) {
		// Restore the original firmware buffer
		static_cast<uint8_t **>(that)[7] = callbackIGFX->realFirmwareBuffer;
		callbackIGFX->realFirmwareBuffer = nullptr;
		// Free the dummy framebuffer which is no longer used
		Buffer::deleter(callbackIGFX->dummyFirmwareBuffer);
		callbackIGFX->dummyFirmwareBuffer = nullptr;
	}

	return FunctionCast(wrapIgBufferGetGpuVirtualAddress, callbackIGFX->orgIgBufferGetGpuVirtualAddress)(that);
}

void IGFX::loadIGScheduler4Patches(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	gKmGen9GuCBinary = patcher.solveSymbol<uint8_t *>(index, "__ZL17__KmGen9GuCBinary", address, size);
	if (gKmGen9GuCBinary) {
		DBGLOG("igfx", "obtained __KmGen9GuCBinary");

		auto loadGuC = patcher.solveSymbol(index, "__ZN13IGHardwareGuC13loadGuCBinaryEv", address, size);
		if (loadGuC) {
			DBGLOG("igfx", "obtained IGHardwareGuC::loadGuCBinary");

			// Lookup the assignment to the size register.
			uint8_t sizeReg[] {0x10, 0xC3, 0x00, 0x00};
			auto pos    = reinterpret_cast<uint8_t *>(loadGuC);
			auto endPos = pos + PAGE_SIZE;
			while (memcmp(pos, sizeReg, sizeof(sizeReg)) != 0 && pos < endPos)
				pos++;

			// Verify and store the size pointer
			if (pos != endPos) {
				pos += sizeof(uint32_t);
				firmwareSizePointer = reinterpret_cast<uint32_t *>(pos);
				DBGLOG("igfx", "discovered firmware size: %u bytes", *firmwareSizePointer);
				// Firmware size must not be bigger than 1 MB
				if ((*firmwareSizePointer & 0xFFFFF) == *firmwareSizePointer)
					// Firmware follows the signature
					signaturePointer = gKmGen9GuCBinary + *firmwareSizePointer;
				else
					firmwareSizePointer = nullptr;
			}

			if (firmwareSizePointer) {
				orgLoadGuCBinary = patcher.routeFunction(loadGuC, reinterpret_cast<mach_vm_address_t>(wrapLoadGuCBinary), true);
				if (patcher.getError() == KernelPatcher::Error::NoError) {
					DBGLOG("igfx", "routed IGHardwareGuC::loadGuCBinary");

					KernelPatcher::RouteRequest requests[] {
						{"__ZN12IGScheduler412loadFirmwareEv", wrapLoadFirmware, orgLoadFirmware},
						{"__ZN13IGHardwareGuC16initSchedControlEv", wrapInitSchedControl, orgInitSchedControl},
						{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj", wrapIgBufferWithOptions, orgIgBufferWithOptions},
						{"__ZNK14IGMappedBuffer20getGPUVirtualAddressEv", wrapIgBufferGetGpuVirtualAddress, orgIgBufferGetGpuVirtualAddress},
					};
					patcher.routeMultiple(index, requests, address, size);
				} else {
					SYSLOG("igfx", "failed to route IGHardwareGuC::loadGuCBinary %d", patcher.getError());
					patcher.clearError();
				}
			} else {
				SYSLOG("igfx", "failed to find GuC firmware size assignment");
			}
		} else {
			SYSLOG("igfx", "failed to resolve IGHardwareGuC::loadGuCBinary %d", patcher.getError());
			patcher.clearError();
		}
	} else {
		SYSLOG("igfx", "failed to resolve __KmGen9GuCBinary %d", patcher.getError());
		patcher.clearError();
	}
}

bool IGFX::loadPatchesFromDevice(IORegistryEntry *igpu, uint32_t currentFramebufferId) {
	bool hasFramebufferPatch = false;
	
	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;

	uint32_t framebufferPatchEnable = 0;
	if (WIOKit::getOSDataValue(igpu, "framebuffer-patch-enable", framebufferPatchEnable) && framebufferPatchEnable) {
		DBGLOG("igfx", "framebuffer-patch-enable %d", framebufferPatchEnable);
		
		if (cpuGeneration == CPUInfo::CpuGeneration::Westmere) {
			hasFramebufferPatch = true;
			
			// First generation only has link mode and width patching.
			framebufferPatchFlags.bitsWestmere.LinkWidth = WIOKit::getOSDataValue(igpu, "framebuffer-linkwidth", framebufferWestmerePatches.LinkWidth);
			framebufferPatchFlags.bitsWestmere.SingleLink = WIOKit::getOSDataValue(igpu, "framebuffer-singlelink", framebufferWestmerePatches.SingleLink);
			
			framebufferPatchFlags.bitsWestmere.FBCControlCompression =
				WIOKit::getOSDataValue(igpu, "framebuffer-fbccontrol-compression", framebufferWestmerePatches.FBCControlCompression);
			framebufferPatchFlags.bitsWestmere.FeatureControlFBC =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-fbc", framebufferWestmerePatches.FeatureControlFBC);
			framebufferPatchFlags.bitsWestmere.FeatureControlGPUInterruptHandling =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-gpuinterrupthandling", framebufferWestmerePatches.FeatureControlGPUInterruptHandling);
			framebufferPatchFlags.bitsWestmere.FeatureControlGamma =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-gamma", framebufferWestmerePatches.FeatureControlGamma);
			framebufferPatchFlags.bitsWestmere.FeatureControlMaximumSelfRefreshLevel =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-maximumselfrefreshlevel", framebufferWestmerePatches.FeatureControlMaximumSelfRefreshLevel);
			framebufferPatchFlags.bitsWestmere.FeatureControlPowerStates =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-powerstates", framebufferWestmerePatches.FeatureControlPowerStates);
			framebufferPatchFlags.bitsWestmere.FeatureControlRSTimerTest =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-rstimertest", framebufferWestmerePatches.FeatureControlRSTimerTest);
			framebufferPatchFlags.bitsWestmere.FeatureControlRenderStandby =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-renderstandby", framebufferWestmerePatches.FeatureControlRenderStandby);
			framebufferPatchFlags.bitsWestmere.FeatureControlWatermarks =
				WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-watermarks", framebufferWestmerePatches.FeatureControlWatermarks);
			
			// Settings above will override all-zero settings.
			uint32_t fbcControlAllZero = 0;
			if (WIOKit::getOSDataValue(igpu, "framebuffer-fbccontrol-allzero", fbcControlAllZero) && fbcControlAllZero) {
				framebufferPatchFlags.bitsWestmere.FBCControlCompression = 1;
			}
			
			// Settings above will override all-zero settings.
			uint32_t featureControlAllZero = 0;
			if (WIOKit::getOSDataValue(igpu, "framebuffer-featurecontrol-allzero", featureControlAllZero) && featureControlAllZero) {
				framebufferPatchFlags.bitsWestmere.FeatureControlFBC = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlGPUInterruptHandling = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlGamma = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlMaximumSelfRefreshLevel = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlPowerStates = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlRSTimerTest = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlRenderStandby = 1;
				framebufferPatchFlags.bitsWestmere.FeatureControlWatermarks = 1;
			}
		} else if (cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge) {
			// Note, the casts to uint32_t here and below are required due to device properties always injecting 32-bit types.
			framebufferPatchFlags.bits.FPFFramebufferId = WIOKit::getOSDataValue(igpu, "framebuffer-framebufferid", framebufferPatch.framebufferId);
			framebufferPatchFlags.bits.FPFFlags = WIOKit::getOSDataValue(igpu, "framebuffer-flags", framebufferPatch.flags.value);
			framebufferPatchFlags.bits.FPFCamelliaVersion = WIOKit::getOSDataValue(igpu, "framebuffer-camellia", framebufferPatch.camelliaVersion);
			framebufferPatchFlags.bits.FPFMobile = WIOKit::getOSDataValue<uint32_t>(igpu, "framebuffer-mobile", framebufferPatch.fMobile);
			framebufferPatchFlags.bits.FPFPipeCount = WIOKit::getOSDataValue<uint32_t>(igpu, "framebuffer-pipecount", framebufferPatch.fPipeCount);
			framebufferPatchFlags.bits.FPFPortCount = WIOKit::getOSDataValue<uint32_t>(igpu, "framebuffer-portcount", framebufferPatch.fPortCount);
			framebufferPatchFlags.bits.FPFFBMemoryCount = WIOKit::getOSDataValue<uint32_t>(igpu, "framebuffer-memorycount", framebufferPatch.fFBMemoryCount);
			framebufferPatchFlags.bits.FPFStolenMemorySize = WIOKit::getOSDataValue(igpu, "framebuffer-stolenmem", framebufferPatch.fStolenMemorySize);
			framebufferPatchFlags.bits.FPFFramebufferMemorySize = WIOKit::getOSDataValue(igpu, "framebuffer-fbmem", framebufferPatch.fFramebufferMemorySize);
			framebufferPatchFlags.bits.FPFUnifiedMemorySize = WIOKit::getOSDataValue(igpu, "framebuffer-unifiedmem", framebufferPatch.fUnifiedMemorySize);
			framebufferPatchFlags.bits.FPFFramebufferCursorSize = WIOKit::getOSDataValue(igpu, "framebuffer-cursormem", fPatchCursorMemorySize);

			if (framebufferPatchFlags.value != 0)
				hasFramebufferPatch = true;

			for (size_t i = 0; i < arrsize(framebufferPatch.connectors); i++) {
				char name[48];
				snprintf(name, sizeof(name), "framebuffer-con%lu-enable", i);
				uint32_t framebufferConnectorPatchEnable = 0;
				if (!WIOKit::getOSDataValue(igpu, name, framebufferConnectorPatchEnable) || !framebufferConnectorPatchEnable)
					continue;

				DBGLOG("igfx", "framebuffer-con%lu-enable %d", i, framebufferConnectorPatchEnable);

				snprintf(name, sizeof(name), "framebuffer-con%lu-%08x-alldata", i, currentFramebufferId);
				auto allData = OSDynamicCast(OSData, igpu->getProperty(name));
				if (!allData) {
					snprintf(name, sizeof(name), "framebuffer-con%lu-alldata", i);
					allData = OSDynamicCast(OSData, igpu->getProperty(name));
				}
				if (allData) {
					auto allDataSize = allData->getLength();
					auto replaceCount = allDataSize / sizeof(ConnectorInfo);
					if (0 == allDataSize % sizeof(ConnectorInfo) && i + replaceCount <= arrsize(framebufferPatch.connectors)) {
						auto replacementConnectors = reinterpret_cast<const ConnectorInfo*>(allData->getBytesNoCopy());
						for (size_t j = 0; j < replaceCount; j++) {
							framebufferPatch.connectors[i+j].index = replacementConnectors[j].index;
							framebufferPatch.connectors[i+j].busId = replacementConnectors[j].busId;
							framebufferPatch.connectors[i+j].pipe = replacementConnectors[j].pipe;
							framebufferPatch.connectors[i+j].pad = replacementConnectors[j].pad;
							framebufferPatch.connectors[i+j].type = replacementConnectors[j].type;
							framebufferPatch.connectors[i+j].flags = replacementConnectors[j].flags;
							connectorPatchFlags[i+j].bits.CPFIndex = true;
							connectorPatchFlags[i+j].bits.CPFBusId = true;
							connectorPatchFlags[i+j].bits.CPFPipe = true;
							connectorPatchFlags[i+j].bits.CPFType = true;
							connectorPatchFlags[i+j].bits.CPFFlags = true;
						}
					}
				}

				snprintf(name, sizeof(name), "framebuffer-con%lu-index", i);
				connectorPatchFlags[i].bits.CPFIndex |= WIOKit::getOSDataValue(igpu, name, framebufferPatch.connectors[i].index);
				snprintf(name, sizeof(name), "framebuffer-con%lu-busid", i);
				connectorPatchFlags[i].bits.CPFBusId |= WIOKit::getOSDataValue(igpu, name, framebufferPatch.connectors[i].busId);
				snprintf(name, sizeof(name), "framebuffer-con%lu-pipe", i);
				connectorPatchFlags[i].bits.CPFPipe |= WIOKit::getOSDataValue(igpu, name, framebufferPatch.connectors[i].pipe);
				snprintf(name, sizeof(name), "framebuffer-con%lu-type", i);
				connectorPatchFlags[i].bits.CPFType |= WIOKit::getOSDataValue(igpu, name, framebufferPatch.connectors[i].type);
				snprintf(name, sizeof(name), "framebuffer-con%lu-flags", i);
				connectorPatchFlags[i].bits.CPFFlags |= WIOKit::getOSDataValue(igpu, name, framebufferPatch.connectors[i].flags.value);

				if (connectorPatchFlags[i].value != 0)
					hasFramebufferPatch = true;
			}
		}
	}

	if (cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge) {
		size_t patchIndex = 0;
		for (size_t i = 0; i < MaxFramebufferPatchCount; i++) {
			char name[48];
			snprintf(name, sizeof(name), "framebuffer-patch%lu-enable", i);
			// Missing status means no patches at all.
			uint32_t framebufferPatchEnable = 0;
			if (!WIOKit::getOSDataValue(igpu, name, framebufferPatchEnable))
				break;

			// False status means a temporarily disabled patch, skip for next one.
			if (!framebufferPatchEnable)
				continue;

			uint32_t framebufferId = 0;
			size_t framebufferPatchCount = 0;

			snprintf(name, sizeof(name), "framebuffer-patch%lu-framebufferid", i);
			bool passedFramebufferId = WIOKit::getOSDataValue(igpu, name, framebufferId);
			snprintf(name, sizeof(name), "framebuffer-patch%lu-find", i);
			auto framebufferPatchFind = OSDynamicCast(OSData, igpu->getProperty(name));
			snprintf(name, sizeof(name), "framebuffer-patch%lu-replace", i);
			auto framebufferPatchReplace = OSDynamicCast(OSData, igpu->getProperty(name));
			snprintf(name, sizeof(name), "framebuffer-patch%lu-count", i);
			(void)WIOKit::getOSDataValue(igpu, name, framebufferPatchCount);

			if (!framebufferPatchFind || !framebufferPatchReplace)
				continue;

			framebufferPatches[patchIndex].framebufferId = (passedFramebufferId ? framebufferId : currentFramebufferId);
			framebufferPatches[patchIndex].find = framebufferPatchFind;
			framebufferPatches[patchIndex].replace = framebufferPatchReplace;
			framebufferPatches[patchIndex].count = (framebufferPatchCount ? framebufferPatchCount : 1);

			framebufferPatchFind->retain();
			framebufferPatchReplace->retain();

			hasFramebufferPatch = true;

			patchIndex++;
		}
	}

	return hasFramebufferPatch;
}

uint8_t *IGFX::findFramebufferId(uint32_t framebufferId, uint8_t *startingAddress, size_t maxSize) {
	uint32_t *startAddress = reinterpret_cast<uint32_t *>(startingAddress);
	uint32_t *endAddress = reinterpret_cast<uint32_t *>(startingAddress + maxSize);
	while (startAddress < endAddress) {
		if (*startAddress == framebufferId)
			return reinterpret_cast<uint8_t *>(startAddress);
		startAddress++;
	}

	return nullptr;
}

#ifdef DEBUG
size_t IGFX::calculatePlatformListSize(size_t maxSize) {
	// sanity check maxSize
	if (maxSize < sizeof(uint32_t)*2)
		return maxSize;
	// ig-platform-id table ends with 0xFFFFF, but to avoid false positive
	// look for FFFFFFFF 00000000
	// and Sandy Bridge is special, ending in 00000000 000c0c0c
	uint8_t * startingAddress = reinterpret_cast<uint8_t *>(gPlatformInformationList);
	uint32_t *startAddress = reinterpret_cast<uint32_t *>(startingAddress);
	uint32_t *endAddress = reinterpret_cast<uint32_t *>(startingAddress + maxSize - sizeof(uint32_t));
	while (startAddress < endAddress) {
		if ((!gPlatformListIsSNB && 0xffffffff == startAddress[0] && 0 == startAddress[1]) ||
			(gPlatformListIsSNB && 0 == startAddress[0] && 0x0c0c0c00 == startAddress[1]))
			return reinterpret_cast<uint8_t *>(startAddress) - startingAddress + sizeof(uint32_t)*2;
		startAddress++;
	}

	return maxSize; // in case of no termination, just return maxSize
}

void IGFX::writePlatformListData(const char *subKeyName) {
	if (BaseDeviceInfo::get().cpuGeneration < CPUInfo::CpuGeneration::SandyBridge) {
		DBGLOG("igfx", "writePlatformListData unsupported below Sandy bridge");
		return;
	}
	
	auto entry = IORegistryEntry::fromPath("IOService:/IOResources/WhateverGreen");
	if (entry) {
		entry->setProperty(subKeyName, gPlatformInformationList, static_cast<unsigned>(calculatePlatformListSize(PAGE_SIZE)));
		entry->release();
	}
}
#endif

bool IGFX::applyPatch(const KernelPatcher::LookupPatch &patch, uint8_t *startingAddress, size_t maxSize) {
	bool r = false;
	size_t i = 0, patchCount = 0;

	// Guard: Nothing to look for, or the pattern does not fit at all
	if (patch.size == 0 || maxSize < patch.size || framebufferSize < patch.size)
		return false;

	// endAddress is the last position where the whole pattern still fits. The framebuffer bound used to
	// be clamped without subtracting patch.size, so a match near its end read and wrote past the region.
	uint8_t *startAddress = startingAddress;
	uint8_t *endAddress = startingAddress + maxSize - patch.size;

	if (startAddress < framebufferStart)
		startAddress = framebufferStart;
	if (endAddress > framebufferStart + framebufferSize - patch.size)
		endAddress = framebufferStart + framebufferSize - patch.size;

	while (startAddress <= endAddress) {
		for (i = 0; i < patch.size; i++) {
			if (startAddress[i] != patch.find[i])
				break;
		}
		if (i == patch.size) {
			for (i = 0; i < patch.size; i++)
				startAddress[i] = patch.replace[i];

			r = true;

			if (++patchCount >= patch.count)
				break;

			startAddress += patch.size;
			continue;
		}

		startAddress++;
	}

	return r;
}

bool IGFX::setDictUInt32(OSDictionary *dict, const char *key, UInt32 value) {
    auto *num = OSNumber::withNumber(value, sizeof(UInt32));
	if (!num)
		return false;
	
	bool success = dict->setObject(key, num);
	num->release();
	return success;
}

template <>
bool IGFX::applyPlatformInformationListPatch(uint32_t framebufferId, FramebufferSNB *platformInformationList) {
	bool framebufferFound = false;

	for (size_t i = 0; i < SandyPlatformNum; i++) {
		if (sandyPlatformId[i] == framebufferId) {
			if (framebufferPatchFlags.bits.FPFMobile)
				platformInformationList[i].fMobile = framebufferPatch.fMobile;

			if (framebufferPatchFlags.bits.FPFPipeCount)
				platformInformationList[i].fPipeCount = framebufferPatch.fPipeCount;

			if (framebufferPatchFlags.bits.FPFPortCount)
				platformInformationList[i].fPortCount = framebufferPatch.fPortCount;

			if (framebufferPatchFlags.bits.FPFFBMemoryCount)
				platformInformationList[i].fFBMemoryCount = framebufferPatch.fFBMemoryCount;

			for (size_t j = 0; j < arrsize(platformInformationList[i].connectors); j++) {
				if (connectorPatchFlags[j].bits.CPFIndex)
					platformInformationList[i].connectors[j].index = framebufferPatch.connectors[j].index;

				if (connectorPatchFlags[j].bits.CPFBusId)
					platformInformationList[i].connectors[j].busId = framebufferPatch.connectors[j].busId;

				if (connectorPatchFlags[j].bits.CPFPipe)
					platformInformationList[i].connectors[j].pipe = framebufferPatch.connectors[j].pipe;

				if (connectorPatchFlags[j].bits.CPFType)
					platformInformationList[i].connectors[j].type = framebufferPatch.connectors[j].type;

				if (connectorPatchFlags[j].bits.CPFFlags)
					platformInformationList[i].connectors[j].flags = framebufferPatch.connectors[j].flags;

				if (connectorPatchFlags[j].value) {
					DBGLOG("igfx", "patching framebufferId 0x%08X connector [%d] busId: 0x%02X, pipe: %u, type: 0x%08X, flags: 0x%08X", framebufferId, platformInformationList[i].connectors[j].index, platformInformationList[i].connectors[j].busId, platformInformationList[i].connectors[j].pipe, platformInformationList[i].connectors[j].type, platformInformationList[i].connectors[j].flags.value);

					framebufferFound = true;
				}
			}

			if (framebufferPatchFlags.value) {
				DBGLOG("igfx", "patching framebufferId 0x%08X", framebufferId);
				DBGLOG("igfx", "mobile: 0x%08X", platformInformationList[i].fMobile);
				DBGLOG("igfx", "pipeCount: %u", platformInformationList[i].fPipeCount);
				DBGLOG("igfx", "portCount: %u", platformInformationList[i].fPortCount);
				DBGLOG("igfx", "fbMemoryCount: %u", platformInformationList[i].fFBMemoryCount);

				framebufferFound = true;
			}
		}
	}

	return framebufferFound;
}

// Sandy and Ivy have no flags
template <>
void IGFX::applyPlatformInformationPatchEx(FramebufferSNB *frame) {}

template <>
void IGFX::applyPlatformInformationPatchEx(FramebufferIVB *frame) {}

template <>
void IGFX::applyPlatformInformationPatchEx(FramebufferHSW *frame) {
	// fCursorMemorySize is Haswell specific
	if (framebufferPatchFlags.bits.FPFFramebufferCursorSize) {
		frame->fCursorMemorySize = fPatchCursorMemorySize;
		DBGLOG("igfx", "fCursorMemorySize: 0x%08X", frame->fCursorMemorySize);
	}

	if (framebufferPatchFlags.bits.FPFFlags)
		frame->flags.value = framebufferPatch.flags.value;

	if (framebufferPatchFlags.bits.FPFCamelliaVersion)
		frame->camelliaVersion = framebufferPatch.camelliaVersion;
}

template <typename T>
void IGFX::applyPlatformInformationPatchEx(T *frame) {
	if (framebufferPatchFlags.bits.FPFFlags)
		frame->flags.value = framebufferPatch.flags.value;


	if (framebufferPatchFlags.bits.FPFCamelliaVersion)
		frame->camelliaVersion = framebufferPatch.camelliaVersion;
}

template <typename T>
bool IGFX::applyPlatformInformationListPatch(uint32_t framebufferId, T *platformInformationList) {
	auto frame = reinterpret_cast<T *>(findFramebufferId(framebufferId, reinterpret_cast<uint8_t *>(platformInformationList), PAGE_SIZE));
	if (!frame)
		return false;

	bool r = false;

	if (framebufferPatchFlags.bits.FPFMobile)
		frame->fMobile = framebufferPatch.fMobile;

	if (framebufferPatchFlags.bits.FPFPipeCount)
		frame->fPipeCount = framebufferPatch.fPipeCount;

	if (framebufferPatchFlags.bits.FPFPortCount)
		frame->fPortCount = framebufferPatch.fPortCount;

	if (framebufferPatchFlags.bits.FPFFBMemoryCount)
		frame->fFBMemoryCount = framebufferPatch.fFBMemoryCount;

	if (framebufferPatchFlags.bits.FPFStolenMemorySize)
		frame->fStolenMemorySize = framebufferPatch.fStolenMemorySize;

	if (framebufferPatchFlags.bits.FPFFramebufferMemorySize)
		frame->fFramebufferMemorySize = framebufferPatch.fFramebufferMemorySize;

	if (framebufferPatchFlags.bits.FPFUnifiedMemorySize)
		frame->fUnifiedMemorySize = framebufferPatch.fUnifiedMemorySize;

	if (framebufferPatchFlags.value) {
		DBGLOG("igfx", "patching framebufferId 0x%08X", frame->framebufferId);
		DBGLOG("igfx", "mobile: 0x%08X", frame->fMobile);
		DBGLOG("igfx", "pipeCount: %u", frame->fPipeCount);
		DBGLOG("igfx", "portCount: %u", frame->fPortCount);
		DBGLOG("igfx", "fbMemoryCount: %u", frame->fFBMemoryCount);
		DBGLOG("igfx", "stolenMemorySize: 0x%08X", frame->fStolenMemorySize);
		DBGLOG("igfx", "framebufferMemorySize: 0x%08X", frame->fFramebufferMemorySize);
		DBGLOG("igfx", "unifiedMemorySize: 0x%08X", frame->fUnifiedMemorySize);

		r = true;
	}

	applyPlatformInformationPatchEx(frame);

	for (size_t j = 0; j < arrsize(frame->connectors); j++) {
		if (connectorPatchFlags[j].bits.CPFIndex)
			frame->connectors[j].index = framebufferPatch.connectors[j].index;

		if (connectorPatchFlags[j].bits.CPFBusId)
			frame->connectors[j].busId = framebufferPatch.connectors[j].busId;

		if (connectorPatchFlags[j].bits.CPFPipe)
			frame->connectors[j].pipe = framebufferPatch.connectors[j].pipe;

		if (connectorPatchFlags[j].bits.CPFType)
			frame->connectors[j].type = framebufferPatch.connectors[j].type;

		if (connectorPatchFlags[j].bits.CPFFlags)
			frame->connectors[j].flags = framebufferPatch.connectors[j].flags;

		if (connectorPatchFlags[j].value) {
			DBGLOG("igfx", "patching framebufferId 0x%08X connector [%d] busId: 0x%02X, pipe: %u, type: 0x%08X, flags: 0x%08X", frame->framebufferId, frame->connectors[j].index, frame->connectors[j].busId, frame->connectors[j].pipe, frame->connectors[j].type, frame->connectors[j].flags.value);

			r = true;
		}
	}

	return r;
}

template <>
bool IGFX::applyDPtoHDMIPatch(uint32_t framebufferId, FramebufferSNB *platformInformationList) {
	bool found = false;

	for (size_t i = 0; i < SandyPlatformNum; i++) {
		if (sandyPlatformId[i] == framebufferId) {
			for (size_t j = 0; j < arrsize(platformInformationList[i].connectors); j++) {
				DBGLOG("igfx", "snb connector [%lu] busId: 0x%02X, pipe: %d, type: 0x%08X, flags: 0x%08X", j, platformInformationList[i].connectors[j].busId, platformInformationList[i].connectors[j].pipe,
					   platformInformationList[i].connectors[j].type, platformInformationList[i].connectors[j].flags);

				if (platformInformationList[i].connectors[j].type == ConnectorDP) {
					platformInformationList[i].connectors[j].type = ConnectorHDMI;
					DBGLOG("igfx", "replaced snb connector %lu type from DP to HDMI", j);
					found = true;
				}
			}
		}
	}

	return found;
}

template <typename T>
bool IGFX::applyDPtoHDMIPatch(uint32_t framebufferId, T *platformInformationList) {
	auto frame = reinterpret_cast<T *>(findFramebufferId(framebufferId, reinterpret_cast<uint8_t *>(platformInformationList), PAGE_SIZE));
	if (!frame)
		return false;

	bool found = false;
	for (size_t i = 0; i < arrsize(frame->connectors); i++) {
		DBGLOG("igfx", "connector [%lu] busId: 0x%02X, pipe: %d, type: 0x%08X, flags: 0x%08X", i, platformInformationList[i].connectors[i].busId, platformInformationList[i].connectors[i].pipe,
			   platformInformationList[i].connectors[i].type, platformInformationList[i].connectors[i].flags);

		if (frame->connectors[i].type == ConnectorDP) {
			frame->connectors[i].type = ConnectorHDMI;
			DBGLOG("igfx", "replaced connector %lu type from DP to HDMI", i);
			found = true;
		}
	}

	return found;
}

void IGFX::applyFramebufferPatches() {
	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;
	uint32_t framebufferId = framebufferPatch.framebufferId;

	// Not tested prior to 10.10.5, and definitely different on 10.9.5 at least.
	if (getKernelVersion() >= KernelVersion::Yosemite) {
		bool success = false;
		if (cpuGeneration == CPUInfo::CpuGeneration::SandyBridge)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferSNB *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::IvyBridge)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferIVB *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::Haswell)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferHSW *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::Broadwell)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferBDW *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::Skylake || cpuGeneration == CPUInfo::CpuGeneration::KabyLake ||
				 (cpuGeneration == CPUInfo::CpuGeneration::CoffeeLake && static_cast<FramebufferSKL *>(gPlatformInformationList)->framebufferId == 0x591E0000))
			//FIXME: write this in a nicer way (coffee pretending to be Kaby, detecting via first kaby frame)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferSKL *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::CoffeeLake)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferCFL *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::CometLake)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferCFL *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::CannonLake)
			success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferCNL *>(gPlatformInformationList));
		else if (cpuGeneration == CPUInfo::CpuGeneration::IceLake) {
			// FIXME: Need to address possible circumstance of both ICL kexts loaded at the same time
			if (callbackIGFX->currentFramebuffer->loadIndex != KernelPatcher::KextInfo::Unloaded)
				success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferICLLP *>(gPlatformInformationList));
			else if (callbackIGFX->currentFramebufferOpt->loadIndex != KernelPatcher::KextInfo::Unloaded)
				success = applyPlatformInformationListPatch(framebufferId, static_cast<FramebufferICLHP *>(gPlatformInformationList));
		}

		if (success)
			DBGLOG("igfx", "patching framebufferId 0x%08X successful", framebufferId);
		else
			DBGLOG("igfx", "patching framebufferId 0x%08X failed", framebufferId);
	}

	uint8_t *platformInformationAddress = findFramebufferId(framebufferId, static_cast<uint8_t *>(gPlatformInformationList), PAGE_SIZE);
	if (platformInformationAddress) {
		for (size_t i = 0; i < MaxFramebufferPatchCount; i++) {
			if (!framebufferPatches[i].find || !framebufferPatches[i].replace)
				continue;

			if (framebufferPatches[i].framebufferId != framebufferId)    {
				framebufferId = framebufferPatches[i].framebufferId;
				platformInformationAddress = findFramebufferId(framebufferId, static_cast<uint8_t *>(gPlatformInformationList), PAGE_SIZE);
			}

			// Skipped patches release their data like applied ones (it used to leak)
			auto dropPatch = [this, i]() {
				OSSafeReleaseNULL(framebufferPatches[i].find);
				OSSafeReleaseNULL(framebufferPatches[i].replace);
			};

			if (!platformInformationAddress) {
				DBGLOG("igfx", "patch %lu framebufferId 0x%08X not found", i, framebufferId);
				dropPatch();
				continue;
			}

			if (framebufferPatches[i].find->getLength() != framebufferPatches[i].replace->getLength()) {
				SYSLOG("igfx", "patch %lu framebufferId 0x%08X find and replace lengths differ, skipped", i, framebufferId);
				dropPatch();
				continue;
			}

			KernelPatcher::LookupPatch patch {};
			patch.kext = currentFramebuffer;
			patch.find = static_cast<const uint8_t *>(framebufferPatches[i].find->getBytesNoCopy());
			patch.replace = static_cast<const uint8_t *>(framebufferPatches[i].replace->getBytesNoCopy());
			patch.size = framebufferPatches[i].find->getLength();
			patch.count = framebufferPatches[i].count;

			if (applyPatch(patch, platformInformationAddress, PAGE_SIZE))
				DBGLOG("igfx", "patch %lu framebufferId 0x%08X successful", i, framebufferId);
			else
				DBGLOG("igfx", "patch %lu framebufferId 0x%08X failed", i, framebufferId);

			framebufferPatches[i].find->release();
			framebufferPatches[i].find = nullptr;
			framebufferPatches[i].replace->release();
			framebufferPatches[i].replace = nullptr;
		}
	}
}

void IGFX::applyHdmiAutopatch() {
	auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;
	uint32_t framebufferId = framebufferPatch.framebufferId;

	DBGLOG("igfx", "applyHdmiAutopatch framebufferId %X cpugen %X", framebufferId, cpuGeneration);

	bool success = false;
	if (cpuGeneration == CPUInfo::CpuGeneration::SandyBridge)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferSNB *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::IvyBridge)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferIVB *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::Haswell)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferHSW *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::Broadwell)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferBDW *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::Skylake || cpuGeneration == CPUInfo::CpuGeneration::KabyLake ||
			 (cpuGeneration == CPUInfo::CpuGeneration::CoffeeLake && static_cast<FramebufferSKL *>(gPlatformInformationList)->framebufferId == 0x591E0000))
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferSKL *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::CoffeeLake)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferCFL *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::CometLake)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferCFL *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::CannonLake)
		success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferCNL *>(gPlatformInformationList));
	else if (cpuGeneration == CPUInfo::CpuGeneration::IceLake) {
		// FIXME: Need to address possible circumstance of both ICL kexts loaded at the same time
		if (callbackIGFX->currentFramebuffer->loadIndex != KernelPatcher::KextInfo::Unloaded)
			success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferICLLP *>(gPlatformInformationList));
		else if (callbackIGFX->currentFramebufferOpt->loadIndex != KernelPatcher::KextInfo::Unloaded)
			success = applyDPtoHDMIPatch(framebufferId, static_cast<FramebufferICLHP *>(gPlatformInformationList));
	}

	if (success)
		DBGLOG("igfx", "hdmi patching framebufferId 0x%08X successful", framebufferId);
	else
		DBGLOG("igfx", "hdmi patching framebufferId 0x%08X failed", framebufferId);
}

void IGFX::applyWestmerePatches(KernelPatcher &patcher) {
	auto kernelVersion = getKernelVersion();
	DBGLOG("igfx", "applyWestmerePatches kernel version %u", kernelVersion);

	//
	// Reference: https://github.com/Goldfish64/ArrandaleGraphicsHackintosh/blob/master/Patches.md
	//

	// Use 0x2000 for stride (fixes artifacts on resolutions > 1024x768). Located in AppleIntelHDGraphicsFB::hwSetCRTC.
	if (kernelVersion == KernelVersion::SnowLeopard) {
		static const uint8_t findStrideSnow64[] = { 0x0F, 0x85, 0x75, 0xFE, 0xFF, 0xFF, 0xE9, 0x6B, 0xFE, 0xFF, 0xFF };
		static const uint8_t replaceStrideSnow64[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xE9, 0x6B, 0xFE, 0xFF, 0xFF  };
		KernelPatcher::LookupPatch stridePatchSnow64 { currentFramebuffer, findStrideSnow64, replaceStrideSnow64, sizeof(findStrideSnow64), 1 };
		patcher.applyLookupPatch(&stridePatchSnow64);
		DBGLOG("igfx", "applyWestmerePatches applied stride patch for Snow Leopard");

	} else {
		static const uint8_t findStrideLionPlus64[] = { 0x0F, 0x45, 0xC8, 0x42, 0x89, 0x8C };
		static const uint8_t replaceStrideLionPlus64[] = { 0x90, 0x90, 0x90, 0x42, 0x89, 0x8C };
		KernelPatcher::LookupPatch stridePatchLionPlus64 { currentFramebuffer, findStrideLionPlus64, replaceStrideLionPlus64, sizeof(findStrideLionPlus64), 1 };
		patcher.applyLookupPatch(&stridePatchLionPlus64);
		DBGLOG("igfx", "applyWestmerePatches applied stride patch for Lion+");
	}

	if (framebufferPatchFlags.bitsWestmere.SingleLink && framebufferWestmerePatches.SingleLink) {
		// AAPL00,DualLink is set to <00000000> instead of patch 1.

		// Single link patch 2. Changes to divisor 14. Located in AppleIntelHDGraphicsFB::hwRegsNeedUpdate.
		if (kernelVersion == KernelVersion::SnowLeopard) {
			static const uint8_t findSingleWidth2Snow64[] = { 0xBE, 0x00, 0x00, 0x00, 0x09 };
			static const uint8_t replaceSingleWidth2Snow64[] = { 0xBE, 0x00, 0x00, 0x00, 0x08 };
			KernelPatcher::LookupPatch singleWidth2PatchSnow64 { currentFramebuffer, findSingleWidth2Snow64, replaceSingleWidth2Snow64, sizeof(findSingleWidth2Snow64), 1 };
			patcher.applyLookupPatch(&singleWidth2PatchSnow64);
			DBGLOG("igfx", "applyWestmerePatches applied single width patch 2 for Snow Leopard");

		} else if (kernelVersion == KernelVersion::Lion) {
			static const uint8_t findSingleWidth2Lion64[] = { 0xB8, 0x00, 0x00, 0x00, 0x09 };
			static const uint8_t replaceSingleWidth2Lion64[] = { 0xB8, 0x00, 0x00, 0x00, 0x08 };
			KernelPatcher::LookupPatch singleWidth2PatchLion64 { currentFramebuffer, findSingleWidth2Lion64, replaceSingleWidth2Lion64, sizeof(findSingleWidth2Lion64), 1 };
			patcher.applyLookupPatch(&singleWidth2PatchLion64);
			DBGLOG("igfx", "applyWestmerePatches applied single width patch 2 for Lion");

		} else if (kernelVersion == KernelVersion::MountainLion) {
			static const uint8_t findSingleWidth2Ml[] = { 0xB9, 0x00, 0x00, 0x00, 0x09 };
			static const uint8_t replaceSingleWidth2Ml[] = { 0xB9, 0x00, 0x00, 0x00, 0x08 };
			KernelPatcher::LookupPatch singleWidth2PatchMl { currentFramebuffer, findSingleWidth2Ml, replaceSingleWidth2Ml, sizeof(findSingleWidth2Ml), 1 };
			patcher.applyLookupPatch(&singleWidth2PatchMl);
			DBGLOG("igfx", "applyWestmerePatches applied single width patch 2 for Mountain Lion");

		} else if (kernelVersion == KernelVersion::Mavericks) {
			static const uint8_t findSingleWidth2Mav[] = { 0x41, 0xB9, 0x00, 0x60, 0x00, 0x09 };
			static const uint8_t replaceSingleWidth2Mav[] = { 0x41, 0xB9, 0x00, 0x60, 0x00, 0x08 };
			KernelPatcher::LookupPatch singleWidth2PatchMav { currentFramebuffer, findSingleWidth2Mav, replaceSingleWidth2Mav, sizeof(findSingleWidth2Mav), 1 };
			patcher.applyLookupPatch(&singleWidth2PatchMav);
			DBGLOG("igfx", "applyWestmerePatches applied single width patch 2 for Mavericks");

		} else if (kernelVersion >= KernelVersion::Yosemite && kernelVersion <= KernelVersion::Sierra) {
			static const uint8_t findSingleWidth2Yos[] = { 0xB8, 0x00, 0x60, 0x00, 0x09 };
			static const uint8_t replaceSingleWidth2Yos[] = { 0xB8, 0x00, 0x60, 0x00, 0x08 };
			KernelPatcher::LookupPatch singleWidth2PatchYos { currentFramebuffer, findSingleWidth2Yos, replaceSingleWidth2Yos, sizeof(findSingleWidth2Yos), 1 };
			patcher.applyLookupPatch(&singleWidth2PatchYos);
			DBGLOG("igfx", "applyWestmerePatches applied single width patch 2 for Yosemite to Sierra");

		} else if (kernelVersion >= KernelVersion::HighSierra) {
			static const uint8_t findSingleWidth2Hs[] = { 0xBA, 0x00, 0x60, 0x00, 0x09 };
			static const uint8_t replaceSingleWidth2Hs[] = { 0xBA, 0x00, 0x60, 0x00, 0x08 };
			KernelPatcher::LookupPatch singleWidth2PatchHs { currentFramebuffer, findSingleWidth2Hs, replaceSingleWidth2Hs, sizeof(findSingleWidth2Hs), 1 };
			patcher.applyLookupPatch(&singleWidth2PatchHs);
			DBGLOG("igfx", "applyWestmerePatches applied single width patch 2 for High Sierra+");
		}

		// Single link patch 3. Powers down CLKB (fixes pixelated image). Located in AppleIntelHDGraphicsFB::hwRegsNeedUpdate.
		static const uint8_t findSingleWidth3[] = { 0x3C, 0x03, 0x30, 0x80 };
		static const uint8_t replaceSingleWidth3[] = { 0x00, 0x03, 0x30, 0x80 };
		KernelPatcher::LookupPatch singleWidth3Patch { currentFramebuffer, findSingleWidth3, replaceSingleWidth3, sizeof(findSingleWidth3), 1 };
		patcher.applyLookupPatch(&singleWidth3Patch);
		DBGLOG("igfx", "applyWestmerePatches applied single width patch 3");
	}
}

void IGFX::wrapTrainFDI(IOService *that, int32_t value, void *params) {
	// Cap link width.
	if (callbackIGFX->framebufferWestmerePatches.LinkWidth == 0 || callbackIGFX->framebufferWestmerePatches.LinkWidth > 4) {
		SYSLOG("igfx", "link width of %u is invalid; using 1", callbackIGFX->framebufferWestmerePatches.LinkWidth);
		callbackIGFX->framebufferWestmerePatches.LinkWidth = 1;
	}
	
	auto pciDevice = OSDynamicCast(IOPCIDevice, that->getProvider());
	if (pciDevice) {
		auto baseMemoryMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
		if (baseMemoryMap) {
			auto baseAddr = (volatile void *) (baseMemoryMap->getVirtualAddress());

			// Set initial link width
			auto txaCtl = OSReadLittleInt32(baseAddr, WESTMERE_TXA_CTL) & WESTMERE_LINK_WIDTH_MASK;
			auto rxaCtl = OSReadLittleInt32(baseAddr, WESTMERE_RXA_CTL) & WESTMERE_LINK_WIDTH_MASK;
			auto linkWidth = (callbackIGFX->framebufferWestmerePatches.LinkWidth - 1) << WESTMERE_LINK_WIDTH_SHIFT;

			OSWriteLittleInt32(baseAddr, WESTMERE_TXA_CTL, txaCtl | linkWidth);
			OSWriteLittleInt32(baseAddr, WESTMERE_RXA_CTL, rxaCtl | linkWidth);
			// AppleIntelHDGraphicsFB has a delay after setting registers
			IODelay(100);

			baseMemoryMap->release();
		} else {
			SYSLOG("igfx", "failed to map BAR0");
		}
	} else {
		SYSLOG("igfx", "provider is not IOPCIDevice");
	}

	FunctionCast(wrapTrainFDI, callbackIGFX->orgTrainFDI)(that, value, params);
}

void IGFX::applyWestmereFeaturePatches(IOService *framebuffer) {
	bool success = true;

	uint32_t patchFeatureControl = 0;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlFBC;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlGPUInterruptHandling;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlGamma;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlMaximumSelfRefreshLevel;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlPowerStates;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlRSTimerTest;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlRenderStandby;
	patchFeatureControl |= callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlWatermarks;

	if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FBCControlCompression) {
		// Entire dictionary will always be replaced as there is only a single property.
		auto dictFBCControl = OSDictionary::withCapacity(1);
		if (dictFBCControl) {
			success &= setDictUInt32(dictFBCControl, "Compression", framebufferWestmerePatches.FBCControlCompression);

			// Replace FBCControl dictionary.
			success &= framebuffer->setProperty("FBCControl", dictFBCControl);
			dictFBCControl->release();
		} else {
			success = false;
		}
	}

	if (patchFeatureControl) {
		// Try to get existing dictionary, replace if not found.
		auto dictFeatureControlOld = OSDynamicCast(OSDictionary, framebuffer->getProperty("FeatureControl"));
		OSDictionary *dictFeatureControlNew;
		if (dictFeatureControlOld)
			dictFeatureControlNew = OSDictionary::withDictionary(dictFeatureControlOld);
		else
			dictFeatureControlNew = OSDictionary::withCapacity(8);

		// Replace any specified properties.
		if (dictFeatureControlNew) {
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlFBC)
				success &= setDictUInt32(dictFeatureControlNew, "FBC", framebufferWestmerePatches.FeatureControlFBC);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlGPUInterruptHandling)
				success &= setDictUInt32(dictFeatureControlNew, "GPUInterruptHandling", framebufferWestmerePatches.FeatureControlGPUInterruptHandling);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlGamma)
				success &= setDictUInt32(dictFeatureControlNew, "Gamma", framebufferWestmerePatches.FeatureControlGamma);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlMaximumSelfRefreshLevel)
				success &= setDictUInt32(dictFeatureControlNew, "MaximumSelfRefreshLevel", framebufferWestmerePatches.FeatureControlMaximumSelfRefreshLevel);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlPowerStates)
				success &= setDictUInt32(dictFeatureControlNew, "PowerStates", framebufferWestmerePatches.FeatureControlPowerStates);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlRSTimerTest)
				success &= setDictUInt32(dictFeatureControlNew, "RSTimerTest", framebufferWestmerePatches.FeatureControlRSTimerTest);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlRenderStandby)
				success &= setDictUInt32(dictFeatureControlNew, "RenderStandby", framebufferWestmerePatches.FeatureControlRenderStandby);
			if (callbackIGFX->framebufferPatchFlags.bitsWestmere.FeatureControlWatermarks)
				success &= setDictUInt32(dictFeatureControlNew, "Watermarks", framebufferWestmerePatches.FeatureControlWatermarks);

			// Replace FBCControl dictionary.
			success &= framebuffer->setProperty("FeatureControl", dictFeatureControlNew);
			dictFeatureControlNew->release();
		} else {
			success = false;
		}
	}

	if (success)
		DBGLOG("igfx", "applyWestmereFeaturePatches successful %X", callbackIGFX->framebufferPatchFlags.value);
	else
		DBGLOG("igfx", "applyWestmereFeaturePatches failed %X", callbackIGFX->framebufferPatchFlags.value);
}
