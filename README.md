# LW39X0 Signal Capture V4.0 — Release Baseline

V4.0 is the release baseline built strictly from the validated V3.9 feature set. RX acquisition, timed capture, multilingual UI, responsive layout, PCIe Stream Guard, adaptive IQ buffering, O_DIRECT recording, spectrum/waterfall/time-domain display, graceful Stop, and persistent diagnostics are retained.

## V4.0 release changes

- Restored the LuoWave logo and made it release-safe. The logo is embedded in Qt resources so it remains visible even if the external `assets/` directory is not copied next to the executable. An external `assets/company_logo.png` still acts as an optional override.
- The logo is rescaled from the original pixmap whenever the responsive layout changes, so it remains correctly sized on compact/high-DPI screens instead of relying on a one-time fixed 220×62 render.
- Hardened PCIe preflight on hosts containing more than one Xilinx PCIe endpoint. Because the current LW SDK exposes `pcie_0` but not a reliable URI→BDF mapping, V4.0 no longer hard-blocks a capture using a guessed endpoint. A clear log entry is emitted and the best detected endpoint is retained only for developer diagnostics. Single-Xilinx systems retain the V3.9 Stream Guard behavior.
- Resize handling now genuinely coalesces rapid resize events, reducing repeated layout/font measurements on lower-performance hosts.
- Application and session-log version identifiers are updated to V4.0.

## Preserved release behavior

- Chinese / English / Russian runtime language switching.
- Responsive side panels and scroll fallbacks for low-resolution/high-DPI displays.
- Continuous or timed capture.
- Multi-channel spectrum, waterfall, time-domain, independent Average/Max Hold/Min Hold traces, markers, screenshot.
- Host-aware IQ RAM buffer tiers and Auto mode.
- O_DIRECT synchronous writer with buffered fallback, no-silent-drop policy, queue monitoring, NVMe temperature diagnostics and file verification.
- Detailed log stored in `<IQ directory>/log/`.
- One driver context per Start/Stop session; Connect remains a logical ready action.

## Release validation focus

Before customer distribution, compile on the target Ubuntu + Qt 6.11 + LW39X0 SDK environment and run at least:

1. Start/Stop repeatedly while changing channel combinations and sample rates.
2. 8-channel 122.88 MS/s continuous recording on the validated high-performance host.
3. Timed recording with IQ save enabled and verify exact normal shutdown/file validation.
4. Chinese → English → Russian switching at 1920×1080 and at a compact window size.
5. Confirm the LuoWave logo is visible both with and without an external `assets/company_logo.png` beside the executable.
6. Confirm an unsupported PCIe throughput configuration is blocked on a single-Xilinx host.

The release keeps the acquisition and storage hot paths unchanged from V3.9.
