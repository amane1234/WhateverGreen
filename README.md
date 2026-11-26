WhateverGreen
=============

[![Build Status](https://github.com/acidanthera/WhateverGreen/actions/workflows/main.yml/badge.svg?branch=master)](https://github.com/acidanthera/WhateverGreen/actions) [![Scan Status](https://scan.coverity.com/projects/16177/badge.svg?flat=1)](https://scan.coverity.com/projects/16177)

[Lilu](https://github.com/acidanthera/Lilu) plugin providing patches to select GPUs on macOS. Requires Lilu 1.5.6 or newer.

#### Features

- Fixes boot to black screen on AMD and NVIDIA
- Fixes sleep wake to black screen on AMD
- Fixes boot screen distortion in certain cases
- Fixes transmitter/encoder in autodetected connectors for multimonitor support (`-raddvi`)
- Fixes HD 7730/7750/7770/R7 250/R7 250X initialisation (`radpg=15`)
- Allows tuning of aty_config, aty_properties, cail_properties via ACPI
- Allows enforcing 24-bit mode on unsupported displays (`-rad24`)
- Allows booting without video acceleration (`-radvesa`)
- Allows automatically setting GPU model name or providing it manually for RadeonFramebuffer
- Allows specifying custom connectors via device properties for RadeonFramebuffer
- Allows tuning autodetected connector priority via device properties (HD 7xxx or newer)
- Fixes an issue in AppleGraphicsDevicePolicy.kext so that we could use a MacPro6,1 board-id/model combination,  without the usual hang with a black screen. [Patching AppleGraphicsDevicePolicy.kext](https://pikeralpha.wordpress.com/2015/11/23/patching-applegraphicsdevicepolicy-kext)
- Modifies macOS to recognize NVIDIA's web drivers as platform binaries. This resolves the issue with transparent windows without content, which appear for applications that use Metal and have Library Validation enabled. Common affected applications are iBooks and Little Snitch Network Monitor, though this patch is universal and fixes them all. [NVWebDriverLibValFix](https://github.com/mologie/NVWebDriverLibValFix)
- Injects IOVARendererID into GPU properties (required for Shiki-based solution for non-freezing Intel and/or any discrete GPU)
- For Intel HD digital audio HDMI, DP, Digital DVI (Patches connector-type DP -> HDMI)
- Fixes NVIDIA GPU interface stuttering on 10.13 (official and web drivers)
- Fixes the kernel panic caused by an invalid link rate reported by DPCD on some laptops with Intel IGPU.
- Fixes the infinite loop on establishing Intel HDMI connections with a higher pixel clock rate on Skylake, Kaby Lake and Coffee Lake platforms.
- Implements the driver support for onboard LSPCON chips to enable DisplayPort to HDMI 2.0 output on some platforms with Intel IGPU.
- Enforces complete modeset on non-built-in displays on Kaby Lake and newer to fix booting to black screen.
- Allows non-supported cards to use HW video encoder (`-radcodec`)
- Fixes choppy video playback on Intel Kaby Lake and newer.
- Fixes black screen on Intel HD since 10.15.5.
- Adds workaround for rare force wake timeout panics on Intel KBL and CFL.
- Supports all valid Core Display Clock (CDCLK) freqencies on Intel ICL platforms.
- Fixes the kernel panic caused by an incorrectly calculated amount of DVMT pre-allocated memory on Intel ICL platforms.
- Makes brightness transitions smoother on Intel IVB+ platforms.
- Fixes the short period garbled screen issue after the system boots on Intel ICL platforms.
- Fixes the PWM backlight control of the built-in display that is directly wired to AMD Radeon RX 5000 series graphic cards.
- Fixes the freeze during iGPU initialization that may occur on certain laptops such as Chromebooks on macOS 10.15 and later.

#### Documentation

Read [FAQs](./Manual/) and avoid asking any questions. No support is provided for the time being.

#### Boot arguments

##### Global

| Boot argument 	| DeviceProperties 	| Description 	|
|---	|---	|---	|
| `-cdfon` 			  | `enable-hdmi20`  | Enable HDMI 2.0 patches on iGPU and dGPU (Not implemented for macOS 11+)  |
| `-wegbeta` 		  | N/A 	| Enable WhateverGreen on unsupported macOS versions (26 and below are enabled by default) 	|
| `-wegdbg` 		  | N/A 	| Enable debug printing (available in DEBUG binaries) 	|
| `-wegoff` 		  | N/A 	| Disable WhateverGreen 	|

##### Switch GPU

| Boot argument 	| DeviceProperties 	| Description 	|
|---	|---	|---	|
| `-wegnoegpu` 		| `disable-gpu` property to each GFX0 	| Disable all external GPUs 	|
| `-wegnoigpu` 		| `disable-gpu` property to IGPU 	| Disable internal GPU 	|
| `-wegswitchgpu` | `switch-to-external-gpu` property to IGPU 	| Disable internal GPU when external GPU is installed 	|

##### AMD Radeon

| Boot argument 	| DeviceProperties 	| Description 	|
|---	|---	|---	|
| `-rad24` 			  | N/A 	| Enforce 24-bit display mode 	|
| `-radcodec` 		| N/A 	| Force the spoofed PID to be used in AMDRadeonVADriver 	|
| `-raddvi` 		  | N/A 	| Enable DVI transmitter correction (required for 290X, 370, etc.) 	|
| `-radvesa` 		  | N/A 	| Disable ATI/AMD video acceleration completely 	|
| `radpg=15` 		  | N/A 	| Disable several power-gating modes (see [FAQ Radeon](./Manual/FAQ.Radeon.en.md), required for Cape Verde GPUs: Radeon HD 7730/7750/7770/R7 250/R7 250X) 	|

##### Board-id

| Boot argument 	  | DeviceProperties 	| Description 	|
|---	|---	|---	  |
| `agdpmod=ignore` 	| `agdpmod` property to external GPU 	| Disables AGDP patches (`vit9696,pikera` value is implicit default for external GPUs) 	|
| `agdpmod=pikera` 	| `agdpmod` property to external GPU 	| Replaces `board-id` with `board-ix` 	|
| `agdpmod=vit9696` | `agdpmod` property to external GPU 	| Disable check for `board-id` 	|

##### Nvidia

| Boot argument 	  | DeviceProperties 	| Description 	|
|---	|---	|---	  |
| `-ngfxdbg` 		    | N/A 	| Enable NVIDIA driver error logging 	|
| `ngfxcompat=1` 	  | `force-compat` 	| Ignore compatibility check in NVDAStartupWeb 	|
| `ngfxgl=1` 		    | `disable-metal` 	| Disable Metal support on NVIDIA 	|
| `ngfxsubmit=0` 	  | `disable-gfx-submit` 	| Disable interface stuttering fix on 10.13 	|

##### Intel HD Graphics

| Boot argument 	  | DeviceProperties 	| Description 	|
|---	|---	|---	  |
| `-igfxblr` 		    | `enable-backlight-registers-fix` property on IGPU 	| Fix backlight registers on KBL, CFL and ICL platforms 	|
| `-igfxbls` 		    | `enable-backlight-smoother` property on IGPU 	| Make brightness transitions smoother on IVB+ platforms. [Read the manual](./Manual/FAQ.IntelHD.en.md#customize-the-behavior-of-the-backlight-smoother-to-improve-your-experience) 	|
| `-igfxblt` | `enable-backlight-registers-alternative-fix` property on IGPU  | An alternative to the Backlight Registers Fix and make Backlight Smoother work on KBL/CFL platforms running macOS 13.4 or later. [Read the manual](./Manual/FAQ.IntelHD.en.md#fix-the-3-minute-black-screen-issue-on-cfl-platforms-running-macos-134-or-later) |
| `-igfxcdc` 		    | `enable-cdclk-frequency-fix` property on IGPU 	| Support all valid Core Display Clock (CDCLK) frequencies on ICL platforms. [Read the manual](./Manual/FAQ.IntelHD.en.md#support-all-possible-core-display-clock-cdclk-frequencies-on-icl-platforms) 	 |
| `-igfxdbeo` 		  | `enable-dbuf-early-optimizer` property on IGPU 	| Fix the Display Data Buffer (DBUF) issues on ICL+ platforms. [Read the manual](./Manual/FAQ.IntelHD.en.md#fix-the-issue-that-the-builtin-display-remains-garbled-after-the-system-boots-on-icl-platforms) 	|
| `-igfxdump` 		  | N/A 	| Dump IGPU framebuffer kext to `/var/log/AppleIntelFramebuffer_X_Y` (available in DEBUG binaries) 	|
| `-igfxdvmt` 		  | `enable-dvmt-calc-fix` property on IGPU 	| Fix the kernel panic caused by an incorrectly calculated amount of DVMT pre-allocated memory on Intel ICL platforms 	|
| `-igfxfbdump` 		| N/A 	| Dump native and patched framebuffer table to ioreg at `IOService:/IOResources/WhateverGreen` 	|
| `-igfxhdmidivs` 	| `enable-hdmi-dividers-fix` property on IGPU 	| Fix the infinite loop on establishing Intel HDMI connections with a higher pixel clock rate on SKL, KBL and CFL platforms 	|
| `-igfxi2cdbg` 	  | N/A 	| Enable verbose output in I2C-over-AUX transactions (only for debugging purposes) 	|
| `-igfxlspcon` 	  | `enable-lspcon-support` property on IGPU 	| Enable the driver support for onboard LSPCON chips.<br> [Read the manual](./Manual/FAQ.IntelHD.en.md#lspcon-driver-support-to-enable-displayport-to-hdmi-20-output-on-igpu) 	|
| `-igfxmlr` 		    | `enable-dpcd-max-link-rate-fix` property on IGPU 	| Apply the maximum link rate fix 	|
| `-igfxmpc` 		    | `enable-max-pixel-clock-override` and `max-pixel-clock-frequency` properties on IGPU 	| Increase max pixel clock (as an alternative to patching `CoreDisplay.framework` 	|
| `-igfxnohdmi` 	  | `disable-hdmi-patches` 	| Disable DP to HDMI conversion patches for digital sound 	|
| `-igfxnotelemetryload` | `disable-telemetry-load` property on IGPU  | Disables iGPU telemetry loading that may cause a freeze during startup on certain laptops such as Chromebooks
| `-igfxsklaskbl` 	| N/A 	| Enforce Kaby Lake (KBL) graphics kext being loaded and used on Skylake models (KBL `device-id` and `ig-platform-id` are required. Not required on macOS 13 and above) 	|
| `-igfxtypec` 		 	| N/A 	| Force DP connectivity for Type-C platforms 	|
| `-igfxvesa` 		  | N/A 	| Disable Intel Graphics acceleration 	|
| `igfxagdc=0` 		  | `disable-agdc` property on IGPU 	| Disable AGDC 	|
| `igfxfcms=1` 		  | `complete-modeset` property on IGPU 	| Force complete modeset on Skylake or Apple firmwares 	|
| `igfxfcmsfbs=` 	  | `complete-modeset-framebuffers` property on IGPU 	| Specify indices of connectors for which complete modeset must be enforced. Each index is a byte in a 64-bit word; for example, value `0x010203` specifies connectors 1, 2, 3. If a connector is not in the list, the driver's logic is used to determine whether complete modeset is needed. Pass `-1` to disable.  	|
| `igfxframe=frame` | `AAPL,ig-platform-id` or `AAPL,snb-platform-id` property on IGPU 	| Inject a dedicated framebuffer identifier into IGPU (only for TESTING purposes) 	|
| `igfxfw=2` 		    | `igfxfw` property on IGPU 	| Force loading of Apple GuC firmware 	|
| `igfxgl=1` 		    | `disable-metal` 	| Disable Metal support on Intel 	|
| `igfxmetal=1` 	  | `enable-metal` 	| Force enable Metal support on Intel for offline rendering 	|
| `igfxonln=1` 		  | `force-online` property on IGPU 	| Force online status on all displays 	|
| `igfxonlnfbs=MASK`| `force-online-framebuffers` property on IGPU 	| Specify indices of connectors for which online status is enforced. Format is similar to `igfxfcmsfbs` 	|
| `igfxpavp=1` 		  | `igfxpavp` property on IGPU 	| Force enable PAVP output 	|
| `igfxrpsc=1` 		 	| `rps-control` property on IGPU 	| Enable RPS control patch (improves IGPU performance). `1` boosts to the maximum frequency only under sustained load, `2` always requests the maximum frequency (old behaviour, can keep the GPU at maximum clock after S3/S4 sleep). **Experimental branch only**, KBL/CFL/CML: `3` measures GT load from the hardware RP busy counters without boosting (`weg-rps-busy-permille`), `4` boosts only while that measured load stays high, `5` three-tier boost limited by AC/battery and CPU temperature 	|
| `igfxgtfloor=MHz` 		| `gt-floor-freq` property on IGPU 	| With Apple GuC (`igfxfw=2`), cap the GPU minimum frequency that AGPM requests (e.g. 500 MHz) at the given value. Values at or below the hardware minimum give the hardware minimum (e.g. `igfxgtfloor=300`). KBL and CFL framebuffers only 	|
| `-igfxrc6fix` 		 	| `enable-rc6-fix` property on IGPU 	| Restore the RC6 (GPU idle power state) configuration when it is lost, e.g. after S3/S4 sleep, and log RC6 problems (`igfx_pm` in the kernel log). SKL/KBL/CFL/CML only 	|
| `igfxpwr=MASK` 		 	| N/A 	| **Experimental.** Power diagnostics and display power policy requests for a display-connected KBL/CFL/CML iGPU, off by default. Bits: `1` observe (RC6 residency, ForceWake requests and holds, display registers the framebuffer already reads, published as `weg-power-*` IGPU properties), `2` request native FBC, `256` observe native dynamic FBC, `4` request native dynamic CDCLK (`2`, `4` and `256` also turn on observation), `8` HWP-EPP-aware `igfxrpsc=1` boost in both directions (includes observation), `16` PSR off, `32` DC states limited to DC5, `64` DC5/DC6 off, `128` native modeset (no forced complete modeset). Try one policy change per boot. [Read the manual](./Manual/IntelPowerExperiments.md) 	|
| `igfxsnb=0` 		  | N/A 	| Disable IntelAccelerator name fix for Sandy Bridge CPUs 	|

##### Backlight

| Boot argument 	| DeviceProperties 	| Description 	|
|---	|---	|---	|
| `applbkl=3` 		| `applbkl` property 	| Enable PWM backlight control of AMD Radeon RX 5000 series graphic cards [read here.](./Manual/FAQ.Radeon.en.md) 	|
| `applbkl=0` 		| `applbkl` property on IGPU 	| Disable AppleBacklight.kext patches for IGPU. <br>In case of custom AppleBacklight profile [read here](./Manual/FAQ.OldPlugins.en.md) 	|

##### 2nd Boot stage

| Boot argument 	| DeviceProperties 	| Description 	|
|---	|---	|---	|
| `gfxrst=1` 		  | N/A 	| Prefer drawing Apple logo at 2nd boot stage instead of framebuffer copying 	|
| `gfxrst=4` 		  | N/A 	| Disable framebuffer init interaction during 2nd boot stage 	|

##### Misc

| Boot argument 	| DeviceProperties 	| Description 	|
|---	|---	|---	|
| `wegtree=1` 		| `rebuild-device-tree` property 	| Force device renaming on Apple FW 	|

#### Credits

- [Apple](https://www.apple.com) for macOS
- [AMD](https://www.amd.com) for ATOM VBIOS parsing code
- [The PCI ID Repository](http://pci-ids.ucw.cz) for multiple GPU model names
- [Andrey1970AppleLife](https://github.com/Andrey1970AppleLife) for [FAQs](./Manual/)
- [FireWolf](https://github.com/0xFireWolf/) for the DPCD maximum link rate fix, infinite loop fix for Intel HDMI connections, LSPCON driver support, Core Display Clock frequency fix for ICL platforms, DVMT pre-allocated memory calculation fix for ICL platforms, Backlight Smoother for IVB+ platforms, Display Data Buffer fix for ICL platforms, and Backlight Registers Alternative Fix.
- [Floris497](https://github.com/Floris497) for the CoreDisplay [patches](https://github.com/Floris497/mac-pixel-clock-patch-v2)
- [Fraxul](https://github.com/Fraxul) for original CFL backlight patch
- [headkaze](https://github.com/headkaze) for Intel framebuffer patching code and CFL backlight patch improvements
- [hieplpvip](https://github.com/hieplpvip) for initial AppleBacklight patching plugin
- [igork](https://applelife.ru/members/igork.564/) for power-gating patch discovery and various FP research
- [lvs1974](https://applelife.ru/members/lvs1974.53809) for continuous implementation of Intel and NVIDIA fixing code
- [mologie](https://github.com/mologie/NVWebDriverLibValFix) for creating NVWebDriverLibValFix.kext which forces macOS to recognize NVIDIA's web drivers as platform binaries
- [PMheart](https://github.com/PMheart) for CoreDisplay patching code and Intel fix backporting
- [RehabMan](https://github.com/RehabMan) for various enhancements
- [RemB](https://applelife.ru/members/remb.8064/) for continuing sleep-wake research and finding the right register for AMD issues
- [Vandroiy](https://applelife.ru/members/vandroiy.83653/) for maintaining the GPU model detection database
- [YungRaj](https://github.com/YungRaj) and [syscl](https://github.com/syscl) for Intel fix backporting
- [vit9696](https://github.com/vit9696) for writing the software and maintaining it

#### Experimental branch

This `experimental` branch is `master` plus an **experimental** Intel graphics power option. **Default: off.** Without `igfxpwr` in `boot-args` (or with `igfxpwr=0`) none of the features below run, and the kext behaves exactly like `master`; each bit has to be added explicitly. There is no DeviceProperties equivalent, so nothing can turn them on by accident. Removing the boot argument restores the `master` behaviour. Keep a bootable copy of your current EFI while testing.

##### What is added

`igfxpwr=MASK` for a display-connected Kaby Lake, Coffee Lake or Comet Lake iGPU on the Apple KBL/CFL framebuffer. `MASK` is the sum of these bits:

| Bit | Value | Effect |
|---|---|---|
| Observe | `1` | Publishes GT RC6 residency, observed ForceWake set/clear calls with per-context hold times and the hardware ACK, and the last PSR, DC state, FBC and CDCLK registers the Apple framebuffer read, as `weg-power-*` properties on IGPU. Samples ride on GPU notifications at most every 5 s; no new timer. Changes nothing. |
| FBC | `2` | Asks the Apple framebuffer for native FBC (adds `FBFramebufferCompression`, 0x4, to the selected platform entry). Turns on observation, so `weg-power-fbc-enabled` and `weg-power-fbc-compressing` show whether FBC is on and compressing. |
| CDCLK | `4` | Asks for native dynamic CDCLK. Also needs Apple's `SupportDynamicCDClk=1` and `dc6config=3`. Turns on observation; `weg-power-cdclk-transition-observed` is `1` once the CDCLK frequency field was seen to change. |
| EPP | `8` | With `igfxrpsc=1` (or `igfxrpsc=4`, defer only), follows the CPU's HWP EPP in both directions: power saving (128–255) defers WhateverGreen's extra RP0 boost, performance (0–63) boosts after one busy interval instead of two. Reads MSRs only, never writes them. Includes Observe. |
| PSR off | `16` | Clears the enable bit from the framebuffer's PSR1/PSR2 control writes, so PSR is never entered. |
| DC5 limit | `32` | Writes a DC6 request as DC5. |
| DC off | `64` | Clears DC5/DC6 from DC state writes (overrides `32`). |
| Dynamic FBC | `256` | Observes native policy. The forced dynamic FBC path is suspended pending panic diagnosis. |
| Native modeset | `128` | Turns off WhateverGreen's forced complete modeset so the Apple driver decides when one is needed, like `igfxfcms=0` (an explicit `igfxfcms` or `complete-modeset` wins). |

Observation values are reset at each wake from sleep, so samples, ForceWake holds and EPP hints from before sleep are never mixed with new ones.

The branch also adds two `igfxrpsc` modes (KBL/CFL/CML only, otherwise RPS control stays off). They read the GT's own RP busy counters during Apple's PM notifications only; no timer and no ForceWake request is added:

| `igfxrpsc` | Effect |
|---|---|
| `3` | Measures only: `weg-rps-busy-permille` is the GT busy share of the last hardware evaluation interval (1000 = 100%), `weg-rps-high-load` is `1` while the load counts as high, `weg-rps-busy-status` whether the counter could be read. Never changes the frequency. |
| `4` | Boosts to RP0 only after three 20 ms samples in a row at 85% busy or more, and keeps the boost while the load stays above 60%. With `igfxpwr=8` an EPP of 128–255 defers the boost. |
| `5` | Adaptive three-tier policy with AC/battery and core/package thermal limits, adaptive sampling, a frequency residency estimate and a 50 MHz histogram (`weg-gt-50mhz-bins-ms`). Add `-igfxvblboost` to test promoting the middle tier to RP0 near the next VBL. |

An enabled explicit `framebuffer-flags` patch takes precedence over bits `2`, `4` and `256`. Bits `16`, `32` and `64` only lower what Apple's driver requests: they cannot enable PSR or a deeper DC state the driver did not ask for. Turning PSR or DC states off normally costs power; it is meant for A/B baselines and for finding the cause of flicker, wake or black-screen problems. Full details: [Manual/IntelPowerExperiments.md](./Manual/IntelPowerExperiments.md).

##### How to test it

1. Build this branch: `bash build-whatevergreen.sh Release experimental`, or take the artifact of the GitHub Actions run for this branch. Replace `EFI/OC/Kexts/WhateverGreen.kext`.
2. Start with observation only. Add `igfxpwr=1` to `boot-args` and reboot.
3. Use the desktop for a few seconds, then read the values:

   ```sh
   ioreg -lw0 -p IOService | grep '"weg-power-'
   ```

   `weg-power-rc6-permille` is the share of the last window spent in GT RC6 (900 = 90%), `weg-power-window-ms` the window length and `weg-power-status` whether the window was valid. A fully idle GPU may leave an old sample visible.
4. Then test **one** policy change per boot, comparing with the `igfxpwr=1` readings:
   - `igfxpwr=3`: FBC only (check that `weg-power-framebuffer-matched` is `1` and `weg-power-framebuffer-flags` has 0x4)
   - Use `igfxpwr=3` to test ordinary FBC. Bit 256 leaves dynamic FBC under native driver control.
   - `igfxpwr=5`: dynamic CDCLK
   - `igfxpwr=9 igfxrpsc=1`: EPP-aware boost (`weg-power-hwp-epp`, `weg-power-defer-rp0`, `weg-power-boost-bias`)
   - `igfxpwr=1 igfxrpsc=3`: GT load measurement only (`ioreg -lw0 -p IOService | grep '"weg-rps-'`); then `igfxrpsc=4` to boost on measured load and compare with `igfxrpsc=1`. `igfxrpsc=4` and `5` can only boost once `weg-rps-busy-status` shows `sampled`
   - `igfxrpsc=5`: tiered boost (`weg-rps-policy-tier-limit`, `weg-rps-power-source`, `weg-rps-core-thermal-margin-c`, `weg-gt-*-ms`)
   - `igfxpwr=17`: PSR off (`weg-power-psr1-writes-cleared`, `weg-power-psr2-writes-cleared` count the changed writes)
   - `igfxpwr=33` or `igfxpwr=65`: DC states limited to DC5, or off (`weg-power-dc-writes-limited`, `weg-power-dc3-dc5-count`, `weg-power-dc5-dc6-count`)
   - `igfxpwr=129`: native modeset (`weg-power-native-modeset` is `1`)
   - ForceWake leaks need no extra bit: with `igfxpwr=1`, check `weg-power-fw-leak-suspect` and `weg-power-fw-<render|media|blitter>-longest-hold-ms`
5. For each setting, keep brightness, panel mode, AC/battery state and workload the same, and compare several equal idle and video intervals. After changing display flags, also check wake from S3/S4, lid open/close and external display reconnect. Use an independent power measurement (for example `powermetrics` or battery drain) for watts or battery life.
6. To go back, remove `igfxpwr` (and the extra `igfxrpsc`) from `boot-args` and reboot.
