# Intel graphics power experiments

These boot arguments target a display-connected Kaby Lake, Coffee Lake or Comet Lake iGPU using the Apple KBL/CFL framebuffer. The default is off. Remove the experimental arguments and reboot to return to your previous settings. Keep a bootable copy of your previous EFI when testing display policy.

## Daily use

A Release build contains no observation or measurement code: `igfxpwr` bits 1 and 256, `igfxrpsc=3`, `-igfxfreqprofile`, the `-igfxrc6fix` report and the `-igfxpmlifecycle` and FBC wake diagnostics (`weg-gpu-power-generation`, `weg-gpu-ready-generation`, `weg-fbc-surface-valid`, `weg-fbc-wake-*`) are compiled only into Debug builds and ignored by Release builds. Release keeps the options that change behaviour, the boot-time confirmation properties and the FBC refresh after a hibernate restore. Build Debug to use the observation described below.

The options that change behaviour publish nothing at runtime: only a few properties set once at boot confirm the configuration. Observation (`igfxpwr=1` or `256`) and the measure-only mode (`igfxrpsc=3`) are for testing. For daily use, leave bit 1 out of `igfxpwr`, for example `igfxpwr=2 igfxrpsc=5` instead of `igfxpwr=3 igfxrpsc=5`.

| Boot argument | Behaviour |
| --- | --- |
| `igfxpwr=1` | Observe: GT RC6 residency and the display registers the Apple framebuffer already reads. No new timer. Changes nothing. |
| `igfxpwr=2` | Request native FBC (`0x4`) and reserve its compressed buffer, after the same DVMT check Apple's driver makes (see below). |
| `igfxpwr=4` | Request native dynamic CDCLK (`0x200000`). |
| `igfxpwr=8` | With `igfxrpsc=1`, `4` or `5`, follow the CPU's HWP EPP (see below). |
| `igfxpwr=16` | PSR off: the enable bit is cleared from the framebuffer's PSR1/PSR2 control writes. |
| `igfxpwr=32` | DC states limited to DC5: a DC6 request is written as DC5. |
| `igfxpwr=64` | DC5/DC6 off (overrides `32`). |
| `igfxpwr=128` | Native modeset: WhateverGreen's forced complete modeset is turned off so the Apple driver decides. |
| `igfxpwr=256` | Observe with dynamic FBC left under native policy. Bit 256 no longer forces dynamic FBC: that path panicked in testing and its cause is unresolved. |
| `igfxrpsc=3` | Measure GT load only (`weg-rps-busy-permille`). Never changes the frequency. |
| `igfxrpsc=4` | Boost to RP0 only while the measured hardware load stays high. |
| `igfxrpsc=5` | Three-tier boost limited by AC/battery and CPU temperature. |
| `-igfxvblboost` | With `igfxrpsc=5`, promote a middle-tier request to RP0 near the next predicted VBL. |
| `-igfxdynfbc` | With `igfxpwr=2`: also request native dynamic FBC (`0x10`) when the FBC check passes and connector 0 is the built-in port. |
| `-igfxfbctimerguard` | 24H232 CFL framebuffer only: skip FBC timer arming when the controller has no timer object. Optional safety net for dynamic FBC. |
| `-igfxfreqprofile` | With observation (`igfxpwr=1`), a 50 MHz GT frequency histogram from Apple's own RPSTAT1 reads. Testing only. |
| `igfxfcms=0` | Existing independent option with the same modeset effect as `igfxpwr=128`; `igfxfcms` and `complete-modeset` take precedence. |

Add the `igfxpwr` values to combine them. Try one policy change per boot. The display options merge flags into the selected platform entry only; an explicit enabled `framebuffer-flags` patch takes precedence. Existing platform capabilities, panel support, and driver policy still apply. CDCLK additionally depends on Apple's `SupportDynamicCDClk=1` and `dc6config=3`. This experiment manipulates neither PLLs nor display watermarks.

## Properties set once at boot

* `weg-power-framebuffer-matched` and `weg-power-framebuffer-flags`: the Apple controller found the selected platform entry, and its flags after the request.
* With bit 2 set, WEG checks the final FBC flags, including explicit platform overrides. On insufficient or unknown DVMT, an invalid memory slot count or an undersized reservation, it clears both base and dynamic FBC. The 24H232 model requires the stolen units, compressed buffer, 1 MiB, 8 MiB headroom and 4 KiB to total strictly less than DVMT. Dynamic FBC also requires base FBC and a valid internal port.
* An empty buffer gets the largest whole MiB the memory check still allows (DVMT minus the stolen units, 9 MiB and 4 KiB), at most 40 MiB (a 4096x2560 Gen9 compressed buffer) and at least 9 MiB and the boot console. The boot console is only a lower bound: a scaled (HiDPI) desktop scans out a larger surface, e.g. 2560x1440 needs 15 MiB. With `0x3E9B0000` (57 MiB) and a 96 MiB DVMT that is 29 MiB, enough up to 3200x1800. Existing and explicit `framebuffer-fbmem` reservations keep their size.
* On the matched CFL UUID, `-igfxfbctimerguard` also guards the native timer callback before it can access a missing framebuffer table or rearm a missing timer. `weg-fbc-timer-callback-guard` reports the second route separately. Apple retains ownership of timer allocation and teardown.
* RPS modes 3–5 can sample after a BLITTER-domain ForceWake acquire and before release, subject to the existing rate limit and ACK checks. In mode 5, battery operation withdraws the extra frequency floor at moderate load. Sustained high load can still request a midrange floor, subject to thermal and EPP limits. Apple retains control of its own frequency request.
* `weg-fbc-timer-guard` (`installed`, `UUID mismatch` or `route failed`) confirms `-igfxfbctimerguard`.
* `weg-dynamic-fbc-result` confirms `-igfxdynfbc`.
* `weg-rps-control-mode`, `weg-power-psr-policy`, `weg-power-dc-policy` and `weg-power-native-modeset` confirm the selected modes.
* `-igfxrc6fix` keeps its `weg-rc6-*` status properties, which change only on RC6 events.

## Observation (`igfxpwr=1` or `256`)

Run this after using the desktop for a few seconds:

```sh
ioreg -lw0 -p IOService | grep '"weg-power-'
```

* `weg-power-rc6-permille` is the share of the measured interval spent in GT RC6, from 0 through 1000 (900 = 90%). Check `weg-power-window-ms`, `weg-power-status`, and the uptime of the last sample in `weg-power-uptime-s`. GPU notifications drive sampling at most once every five seconds; a fully idle GPU may leave an old sample visible. The residency counter (0x138108) is outside the GT ForceWake ranges, so it is sampled whether or not the render well is awake. The first sample, a zero or all-ones read and each wake from sleep start a new baseline; a long gap or a wall-clock discontinuity invalidates the window.
* PSR1/PSR2 status, FBC control and status, CDCLK control, DC policy and the DC3-to-DC5 / DC5-to-DC6 counters are the **last observed** native reads (`weg-power-*-raw`, `weg-power-dc3-dc5-count`, `weg-power-dc5-dc6-count`). `weg-power-fbc-enabled` is bit 31 of FBC control, `weg-power-fbc-compressing` whether the FBC status showed compressed segments, and `weg-power-cdclk-transition-observed` is `1` once two CDCLK reads in the same wake cycle differed. These appear only if Apple's driver reads the register; an absent property means no read was observed. Values are cleared at each wake from sleep.
* With `igfxpwr=8`, `weg-power-hwp-epp`, `weg-power-defer-rp0` and `weg-power-boost-bias` show the EPP decision.

For an A/B test, use the same brightness, panel mode, AC/battery state and workload. Compare multiple equal-duration idle and video intervals; include wake, lid open/close and external-display reconnect after changing display flags. Use independent power measurements (`powermetrics`, battery drain) for watts or battery life.

## EPP (`igfxpwr=8`)

Every five seconds a worker samples a CPU's HWP request, including package delegation when selected. EPP 128–255 (power saving) defers WhateverGreen's extra RP0 request, EPP 0–63 (performance) lets `igfxrpsc=1` boost after one busy interval instead of two, and 64–127 keeps the default. Each hint lasts two intervals, expires on a read failure and is dropped at wake. Apple's requested frequency is preserved while the boost is deferred. Mode 2 retains its existing behaviour. No MSR or package power limit is written.

## Display policy (`igfxpwr=16/32/64/128`)

PSR, DC and modeset policies only lower what Apple's driver requests. They cannot turn on PSR the panel or driver did not enable, or allow a deeper DC state than the driver asked for. Turning PSR or DC states off normally costs power; use it to check whether a flicker, wake or black-screen problem comes from them, or as the baseline of an A/B test. If the Apple driver waits for a PSR or DC state it requested, the wait can time out; remove the option if wake or display changes become slow.

## GT load and boost (`igfxrpsc=3/4/5`)

Modes 3–5 sample the RP busy or down-idle counters immediately before a native Gen9 GT ForceWake release, with the matching context bit acknowledged. Valid samples require a plausible new evaluation window. Without a usable native hold, PM notifications can sample the uncore RC6 counter. Mode 3 reports its upper bound in `weg-rps-busy-upper-bound-permille` and clears the measured busy property. Only a recent measured window can raise the frequency. A reset request, a physically implausible backward counter jump, or a known wake invalidates the history. A direct reset that preserves the counter can go undetected.

* `3` publishes `weg-rps-busy-permille` (1000 = 100%), `weg-rps-high-load` and `weg-rps-busy-status`, which names the reason when the counters could not be read, about once a second.
* `4` boosts to RP0 after three consecutive samples at 85% busy or more and keeps the boost while the load stays above 60%; a gap in samples or a wake from sleep resets it. With `igfxpwr=8`, an EPP of 128–255 defers the boost.
* `5` has three tiers. Below 60% measured busy, Apple's request is kept. At 60% or more, the request can be raised to the midpoint between the fused RPn and RP0. Three consecutive samples at 85% or more permit RP0. Mode 5 only ever raises Apple's request. Battery power, or a core or package thermal margin of 15–24 °C, limits it to the midpoint; a smaller margin, active thermal throttling, missing power data or an unavailable sensor leaves the driver in control. The AC state comes from the first `IOPMPowerSource` that reports `ExternalConnected`, looked up at most every 10 s and after each wake, and battery change notifications refresh it sooner. Sampling backs off to 80 ms below 30% busy or after an invalid read. With `igfxpwr=8`, an EPP of 128–255 defers the boost. These thresholds are experimental.
* `-igfxvblboost` (mode 5) promotes an existing middle-tier request to RP0 in the last tenth of the predicted VBL period of framebuffer zero, using projections younger than six periods. During eligible load, the worker is asked to refresh after two periods. Heat, AC/battery policy and EPP keep precedence. VBL timing is a display deadline proxy, not a per-command completion deadline.

## RC6 repair and backlight

`-igfxrc6fix` now reports RC_CONTROL as observed through native framebuffer reads. Recovery writes belong to the Apple driver. The readback is tagged with a reset generation and invalidated at known wake boundaries. A missing native read leaves the status pending. With explicit observation, `weg-power-last-gt-release-ack` records the last observed GT ACK before release. It does not measure hold duration.

DC5/DC6 properties count transitions. No verified DC dwell-time source is available. With explicit observation and `-igfxfreqprofile`, native RPSTAT1 reads feed 32 little-endian UInt64 buckets in `weg-gt-50mhz-bins-ms` (50 MHz each, the last includes 1550 MHz and above). No extra GT read is issued. Equal-band endpoints within 250 ms contribute to `weg-gt-frequency-covered-ms`. Compare that value with `weg-gt-frequency-window-ms`, the elapsed time between native observations within recorded wake/reset epochs. Reads made outside the framebuffer hook are invisible, so an empty histogram is possible. Dynamic FBC stays under native policy pending a symbolicated panic trace.

With `-igfxbls`, a one-shot timer schedules each fade step on the existing workloop, identical pending nonzero requests are coalesced, and screen-off is executed immediately.

The PWM start value is sampled by the submitter outside the request spinlock. Ordered commits and immediate zero-duty writes still use that lock.

### CFL 24H232 sleep and resume experiment

Add `-igfxpmlifecycle` to enable the controller power hook and native FBC timer guards on the verified framebuffer UUID. For the 96 MiB experiment use `igfxpwr=3 igfxrpsc=5`; retain the native 8 MiB FBC headroom. The switch is independent of CPU hibernation configuration. Test S3 and an actual hibernate-image restore separately.

The experimental path starts closed. Complete routes admit native CDCLK observation. A controller power transition increments the generation, drains plugin reads and ForceWake pulses, cancels queued fades and invalidates PM samples. An acknowledged power-up at ordinal 2 requires a subsequent native CDCLK read with a valid frequency field. Until then WhateverGreen's own register accesses, ForceWake handling, fades and PM work stay closed. MMIO injectors that run synchronously inside Apple's own register writes, such as `-igfxblr` and the PSR/DC policies, keep working during transitions as without the lifecycle option. The first clock in a new generation is a baseline. PSR/DC values come from fresh Apple reads; an absent observation means unknown. No cached display register is replayed.

Dynamic FBC additionally needs pixel information for the current internal mode, a fitting native reservation and a live timer. After a modeset, sleep or incomplete hooks, missing evidence keeps new FBC arming closed. A delayed native callback must match the current arm generation. Native timer ownership and release stay with Apple. Other graphics kexts and access paths outside the routed CFL functions are outside the scope of this experiment.

`weg-gpu-lifecycle` reports hook installation. With diagnostics enabled, compare `weg-gpu-power-generation` on the IGPU with the asynchronously published `weg-gpu-ready-generation` and `weg-fbc-surface-valid`. Check QuickTime playback, brightness changes during sleep entry, internal-panel modesets and repeated wakes. Timer creation or a valid surface alone does not confirm compression.

With `-igfxpmlifecycle` and observation (`igfxpwr` bit 1), `weg-fbc-wake-diagnostics` confirms a read-only FBC check after each ready generation. 3 s and 30 s after the native CDCLK read reopens the GPU, WhateverGreen reads DPFC_CONTROL (0x43208), DPFC_STATUS2 (0x43214), ILK_DPFC_CHICKEN (0x43224), DISP_ARB_CTL (0x45000), CHICKEN_MISC_4 (0x4208C) and DPFC_CB_BASE (0x43200) once each and publishes them on the IGPU as 64-bit numbers: `weg-fbc-boot-3s-*` and `weg-fbc-boot-30s-*` for the first generation after boot, `weg-fbc-wake-3s-*` and `weg-fbc-wake-30s-*` for the latest wake, with `-generation` and, when the kernel symbol resolves, `-hibernate` (1 for a hibernate image restore). Apple's `hwFBCSet` writes `0x800100` into ILK_DPFC_CHICKEN and `0x80008000` into DISP_ARB_CTL when it enables FBC. Compare those bits after a normal boot, S3 and S4 to tell missing workarounds from stale compressed data. Nothing is written.

Small display glitches remained after a hibernate (S4) restore with FBC on a Latitude 5410, while S3 and normal boots were clean. With `-igfxpmlifecycle`, WhateverGreen therefore turns FBC off and on once 3 s and once 30 s after the GPU is ready again following a hibernate restore (`_gIOHibernateState` captured at controller power-up; every wake if the symbol is unavailable). It runs only while Apple's driver has FBC enabled (its +0x38A8 state and DPFC_CONTROL bit 31), calls Apple's own `WriteFBCControl` so the driver's FBC state stays consistent, and runs on the controller's work loop, where Apple's FBC timer and modesets also run. Two 60 Hz frames pass between the writes so that the hardware restarts compression from the current frame. A power transition in between leaves FBC off for Apple to set again. The boot and S3 wakes are left alone. `weg-fbc-wake-refresh` reports `after hibernate restore`, `after every wake` or `unavailable`; with observation, `weg-fbc-wake-refreshes` counts the off/on cycles.

### FBC supervision (`-igfxpmlifecycle`)

With `-igfxpmlifecycle`, WhateverGreen checks Apple's FBC against the Gen9 rules of Linux `intel_fbc.c` when Apple's driver turns FBC on (`WriteFBCControl`, after `hwFBCSet` programmed the buffer and workarounds; a passing result is reused for the same power generation and surface, so dynamic FBC toggling is checked once per generation), and 3 s and 30 s after every ready generation (boot, S3, S4) and 3 s after every internal-panel modeset on the controller's work loop, where FBC is currently on or held off. `weg-fbc-supervision` confirms the check at boot. The check covers:

* Surface (`intel_fbc_check_plane`): primary plane of pipe A (while it is off, e.g. inside a modeset where `hwFBCSet` enables FBC before the plane, nothing is compressed and the check waits for the next one instead of turning FBC off), 8:8:8:8 or RGB565, linear (512-byte stride, Display WA #1105), X or Y tiling, no 90/270 degree rotation, a 64-byte stride multiple of at least the plane width, at most 4096x4096. i915 also excludes formats with alpha; that rule is not applied, because Apple's driver compresses its primary plane without glitches.
* Compressed buffer size (`intel_fbc_cfb_size`): 4 bytes per pixel, a 512-byte aligned stride (`skl_fbc_min_cfb_stride`), at most 2560 lines, divided by the compression limit Apple selected (DPFC_CONTROL bits 7:6). It must fit the buffer Apple reserved (`framebuffer-fbmem`, or what `igfxpwr=2` reserves).
* Compressed buffer base: DPFC_CB_BASE 4 KiB aligned, past the first stolen page, and equal to what Apple's `hwFBCSet` derives from its compressed buffer slot. After a hibernate restore this also catches a base lost with the register state.
* Stolen memory (`intel_fbc_stolen_end`): the buffer must end below the top 8 MiB of stolen memory, which the Gen9 FBC hardware treats as reserved. It must not overlap the hardware reserved region (GEN6_STOLEN_RESERVED) or the scanout surface when that lives in stolen memory.
* GTT: every page of the scanout surface must be mapped. Every 16th page and the last page of the stolen framebuffer memory are compared. The first check outside a hibernate restore records whether the driver maps its stolen framebuffer memory linearly; if it does, a later mapping that is no longer linear (a GTT not rebuilt after a restore) fails the check.
* Gen9 workarounds: WaFbcNukeOnHostModify and WaFbcHighMemBwCorruptionAvoidance (ILK_DPFC_CHICKEN `0x800100`), WaFbcWakeMemOn and WaFbcTurnOffFbcWatermark (DISP_ARB_CTL `0x80008000`).

A failed check turns FBC off through Apple's `WriteFBCControl` and keeps Apple's later enables off (its FBC state then says off as well, and dynamic FBC arming stays closed) until a check passes; then the enable Apple asked for is applied. Unreadable registers skip the check instead. `weg-fbc-validation` changes only with the verdict: `ok` or `FBC off: <reason>`. A buffer that is too small also names the plane size and the MiB it needs, e.g. `FBC off: compressed buffer smaller than the plane needs (3200x1800 plane needs 22 MiB, reserved 9 MiB)`. A scaled (HiDPI) mode scans out a larger surface than the panel, so it needs a larger `framebuffer-fbmem`; the FBC memory check then needs stolen + fbmem + 9 MiB below the DVMT.

After a hibernate restore, or when the workaround bits are missing, the compression state is re-initialised: FBC off; the disable must complete (DPFC_CONTROL reads back disabled and pipe A's frame counter advances twice, or does not move for 50 ms while the pipe is idle; otherwise FBC stays off); the missing workaround bits are set as Apple's `hwFBCSet` sets them; FBC on again only for the same power and surface generation (plane format, tiling, stride, size, buffer base and limit unchanged). A new surface keeps the enable for the next check. The boot reservation of `igfxpwr=2` uses the same i915 buffer size for the boot console, with the 9 MiB minimum. Debug builds publish the checked values as `weg-fbc-check-*`.
