# Intel graphics power experiments

These boot arguments target a display-connected Kaby Lake, Coffee Lake or Comet Lake iGPU using the Apple KBL/CFL framebuffer. The default is off. Remove the experimental arguments and reboot to return to your previous settings. Keep a bootable copy of your previous EFI when testing display policy.

| Boot argument | Behaviour |
| --- | --- |
| `igfxpwr=1` | Collect RC6 residency, observed ForceWake requests, and display registers already read by the Apple framebuffer. No new timer. |
| `igfxpwr=3` | Diagnostics plus a request for native FBC and dynamic FBC. |
| `igfxpwr=5` | Diagnostics plus a request for native dynamic CDCLK. |
| `igfxpwr=9 igfxrpsc=1` | Diagnostics plus an HWP-aware limit on WhateverGreen's adaptive RP0 boost. |
| `igfxpwr=17` | Diagnostics plus PSR off: the enable bit is cleared from the framebuffer's PSR1/PSR2 control writes. |
| `igfxpwr=33` | Diagnostics plus DC states limited to DC5: a DC6 request is written as DC5. |
| `igfxpwr=65` | Diagnostics plus DC5/DC6 off. |
| `igfxpwr=129` | Diagnostics plus native modeset: WhateverGreen's forced complete modeset is turned off so the Apple driver decides. |
| `igfxfcms=0` | Existing independent option with the same modeset effect; `igfxfcms` and `complete-modeset` take precedence over `igfxpwr=128`. |

The decimal mask accepts bits 1 (observe), 2 (FBC), 4 (CDCLK), 8 (EPP, includes observation), 16 (PSR off), 32 (DC5 limit), 64 (DC5/DC6 off) and 128 (native modeset). Values 2 and 4 can be used without diagnostics. Try one policy change per boot. The display options merge flags into the selected platform entry only; an explicit enabled `framebuffer-flags` patch takes precedence. The Apple controller publishes `weg-power-framebuffer-flags` when a matching entry is found. Existing platform capabilities, panel support, and driver policy still apply. CDCLK additionally depends on Apple's `SupportDynamicCDClk=1` and `dc6config=3`. This experiment manipulates neither PLLs nor display watermarks.

## Reading a sample

Run this after using the desktop for a few seconds:

```sh
ioreg -lw0 -p IOService | grep '"weg-power-'
```

* `weg-power-rc6-permille` is the share of the measured interval spent in GT RC6, from 0 through 1000. A value of 900 means 90%. Check `weg-power-window-ms`, `weg-power-status`, and the uptime of the last sample in `weg-power-uptime-s`. GPU notifications drive sampling at most once every five seconds. A fully idle GPU may leave an old sample visible. The first sample establishes a baseline. A notification that arrives while the GT render well is asleep is skipped without touching the baseline, because the residency counter reads as 0 then. An all-ones register read, incompatible counter delta, long gap or wall-clock discontinuity invalidates the window.
* With `weg-power-forcewake-hook=1`, the render, media and blitter counters show the observed set/clear calls to Apple's SafeForceWakeMultithreaded entry. The context masks show the last observed request per context, before hardware acknowledgement. Values are approximate concurrent snapshots. Repeated requests for the same bit, driver paths outside the hook, and in-flight transitions prevent deriving a driver's ownership count or a confirmed leak.
* PSR1/PSR2 status, FBC control, CDCLK control, DC policy, and DC3-to-DC5 / DC5-to-DC6 transition counters are **last observed** native reads. An absent property means no usable read has been captured and published. A zero policy value or unchanged counter alone cannot establish time spent in a state. PSR and DC transitions continue to be managed by Apple and the panel/DMC.
* The EPP option samples a worker CPU's HWP request, including package delegation when selected. An available EPP of 128–255 defers the extra adaptive RP0 request for up to ten seconds after each sample. Apple's requested frequency is preserved during that interval. Read failures expire the hint. Mode 2 retains its existing behaviour. This option makes no MSR or package power-limit writes.
* ForceWake holds: `weg-power-fw-<domain>-longest-hold-ms` is how long the oldest context bit has been requested on that domain without a clear, and `weg-power-fw-<domain>-longest-holder` is that context bit. `weg-power-fw-<domain>-ack` is the hardware ACK of the context bits, read while the GT was awake. `weg-power-fw-leak-suspect` has one bit per domain (1 render, 2 media, 4 blitter) whose longest hold reached 30 s; such a hold keeps the GT out of RC6. Holds are per requested context bit, not Apple's internal reference count, so a short-lived nested request can end a hold early.
* With the EPP option, the boost follows the CPU's preference in both directions: EPP 128–255 defers WhateverGreen's extra RP0 request, EPP 0–63 lets `igfxrpsc=1` boost after one busy interval instead of two, and 64–127 keeps the default. `weg-power-boost-bias` shows the current choice.
* PSR, DC and modeset policies only lower what Apple's driver requests. They cannot turn on PSR the panel or driver did not enable, or allow a deeper DC state than the driver asked for. `weg-power-psr1-writes-cleared`, `weg-power-psr2-writes-cleared` and `weg-power-dc-writes-limited` count the changed writes (published with diagnostics); 0 means the driver did not request that state through WriteRegister32. `weg-power-psr-policy`, `weg-power-dc-policy` and `weg-power-native-modeset` confirm the selected policy.
* Turning PSR or DC states off normally costs power; use it to check whether a flicker, wake or black-screen problem comes from them, or as the baseline of an A/B test. If the Apple driver waits for a PSR or DC state it requested, the wait can time out; remove the option if wake or display changes become slow.
* `-igfxrc6fix` is a separate existing control for RC6 repair. It is unnecessary for observation.

For an A/B test, use the same brightness, panel mode, AC/battery state and workload. Compare multiple equal-duration idle and video intervals; include wake, lid open/close and external-display reconnect after changing display flags. Use independent power measurements for watts or battery life.
