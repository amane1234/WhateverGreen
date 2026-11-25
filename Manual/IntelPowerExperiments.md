# Intel graphics power experiments

These boot arguments target a display-connected Kaby Lake, Coffee Lake or Comet Lake iGPU using the Apple KBL/CFL framebuffer. The default is off. Remove the experimental arguments and reboot to return to your previous settings. Keep a bootable copy of your previous EFI when testing display policy.

| Boot argument | Behaviour |
| --- | --- |
| `igfxpwr=1` | Collect RC6 residency, observed ForceWake requests, and display registers already read by the Apple framebuffer. No new timer. |
| `igfxpwr=3` | Diagnostics plus a request for native FBC and dynamic FBC. |
| `igfxpwr=5` | Diagnostics plus a request for native dynamic CDCLK. |
| `igfxpwr=9 igfxrpsc=1` | Diagnostics plus an HWP-aware limit on WhateverGreen's adaptive RP0 boost. |
| `igfxfcms=0` | Existing independent option. Allow the Apple driver to decide when a complete modeset is needed. |

The decimal mask accepts bits 1 (observe), 2 (FBC), 4 (CDCLK), and 8 (EPP, includes observation). Values 2 and 4 can be used without diagnostics. Try one policy change per boot. The display options merge flags into the selected platform entry only; an explicit enabled `framebuffer-flags` patch takes precedence. The Apple controller publishes `weg-power-framebuffer-flags` when a matching entry is found. Existing platform capabilities, panel support, and driver policy still apply. CDCLK additionally depends on Apple's `SupportDynamicCDClk=1` and `dc6config=3`. This experiment manipulates neither PLLs nor display watermarks.

## Reading a sample

Run this after using the desktop for a few seconds:

```sh
ioreg -lw0 -p IOService | grep '"weg-power-'
```

* `weg-power-rc6-permille` is the share of the measured interval spent in GT RC6, from 0 through 1000. A value of 900 means 90%. Check `weg-power-window-ms`, `weg-power-status`, and the uptime of the last sample in `weg-power-uptime-s`. GPU notifications drive sampling at most once every five seconds. A fully idle GPU may leave an old sample visible. The first sample establishes a baseline. A notification that arrives while the GT render well is asleep is skipped without touching the baseline, because the residency counter reads as 0 then. An all-ones register read, incompatible counter delta, long gap or wall-clock discontinuity invalidates the window.
* With `weg-power-forcewake-hook=1`, the render, media and blitter counters show the observed set/clear calls to Apple's SafeForceWakeMultithreaded entry. The context masks show the last observed request per context, before hardware acknowledgement. Values are approximate concurrent snapshots. Repeated requests for the same bit, driver paths outside the hook, and in-flight transitions prevent deriving a driver's ownership count or a confirmed leak.
* PSR1/PSR2 status, FBC control, CDCLK control, DC policy, and DC3-to-DC5 / DC5-to-DC6 transition counters are **last observed** native reads. An absent property means no usable read has been captured and published. A zero policy value or unchanged counter alone cannot establish time spent in a state. PSR and DC transitions continue to be managed by Apple and the panel/DMC.
* The EPP option samples a worker CPU's HWP request, including package delegation when selected. An available EPP of 128–255 defers the extra adaptive RP0 request for up to ten seconds after each sample. Apple's requested frequency is preserved during that interval. Read failures expire the hint. Mode 2 retains its existing behaviour. This option makes no MSR or package power-limit writes.
* `-igfxrc6fix` is a separate existing control for RC6 repair. It is unnecessary for observation.

For an A/B test, use the same brightness, panel mode, AC/battery state and workload. Compare multiple equal-duration idle and video intervals; include wake, lid open/close and external-display reconnect after changing display flags. Use independent power measurements for watts or battery life.
