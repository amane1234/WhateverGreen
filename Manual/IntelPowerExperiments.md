# Intel graphics power experiments

These boot arguments target a display-connected Kaby Lake, Coffee Lake or Comet Lake iGPU using the Apple KBL/CFL framebuffer. The default is off. Remove the experimental arguments and reboot to return to your previous settings. Keep a bootable copy of your previous EFI when testing display policy.

## Daily use

The options that change behaviour publish nothing at runtime: only a few properties set once at boot confirm the configuration. Observation (`igfxpwr=1` or `256`) and the measure-only mode (`igfxrpsc=3`) are for testing. For daily use, leave bit 1 out of `igfxpwr`, for example `igfxpwr=2 igfxrpsc=5` instead of `igfxpwr=3 igfxrpsc=5`.

| Boot argument | Behaviour |
| --- | --- |
| `igfxpwr=1` | Observe: GT RC6 residency and the display registers the Apple framebuffer already reads. No new timer. Changes nothing. |
| `igfxpwr=2` | Request native FBC (`0x4`) after checking the DVMT against the platform's stolen memory. |
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
| `-igfxfreqprofile` | With observation (`igfxpwr=1`), a 50 MHz GT frequency histogram from Apple's own RPSTAT1 reads. Testing only. |
| `igfxfcms=0` | Existing independent option with the same modeset effect as `igfxpwr=128`; `igfxfcms` and `complete-modeset` take precedence. |

Add the `igfxpwr` values to combine them. Try one policy change per boot. The display options merge flags into the selected platform entry only; an explicit enabled `framebuffer-flags` patch takes precedence. Existing platform capabilities, panel support, and driver policy still apply. CDCLK additionally depends on Apple's `SupportDynamicCDClk=1` and `dc6config=3`. This experiment manipulates neither PLLs nor display watermarks.

## Properties set once at boot

* `weg-power-framebuffer-matched` and `weg-power-framebuffer-flags`: the Apple controller found the selected platform entry, and its flags after the request.
* FBC check: the request is added only when the DVMT (GMS field of the GPU's GGC register) holds the selected entry's stolen memory plus its boot framebuffer memory, the FAQ's TOTAL STOLEN (slightly conservative). Stolen memory times `FBMemoryCount` is the all-pipes MAX STOLEN and is not required. `weg-fbc-gms-raw`, `weg-fbc-dvmt-mb`, `weg-fbc-required-mb` and `weg-fbc-platform-budget-ok` report the check; a DVMT of 0 means the GMS value was not recognized and the FBC request was dropped. Dynamic FBC stays under native policy (`weg-dynamic-fbc-policy`).
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
