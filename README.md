# LW39X0 Signal Capture V1.1.1

V1.1.1 is based on V1.1. The V1.1 acquisition/storage core is retained; V1.1.1 adds user/developer log modes and post-capture offline validation. The acquisition/display architecture is retained while the storage path is adjusted after comparison with the vendor-style `recv_demo`.


## V1.1.1 changes

- Default **Normal log mode** shows only key customer-facing events and creates no log file.
- Optional **Developer log mode** displays full diagnostics and stores them under `<application>/log/`.
- Each saved-IQ run writes a small metadata JSON under `<application>/capture_meta/`.
- Added post-capture **Data Validation**. It is disabled while RX is active, automatically targets the last saved capture, reads the IQ file end-to-end on a separate thread, checks exact RX/Writer/file byte counters, and estimates per-channel sample-time coverage.
- Validation reports are stored under `<application>/capture_meta/`. Exact interior gap location still requires a future hardware/driver sample counter.
- Relative IQ paths such as the default `rx_iq.bin` are resolved against the executable directory; manually selected absolute paths remain unchanged.

## V1.1 changes

- Version numbering is normalized to the product release line: V1.0 -> V1.1.
- Added an explicit **RF calibration** checkbox. It is off by default. When enabled, only the zones containing enabled RX channels are calibrated before `lw39x0_issue_recv_start()`.
- Added **Storage Mode**: `Auto`, `Buffered`, `Direct I/O`.
  - `Auto` in V1.1 intentionally prefers buffered sequential `fwrite()` because the vendor demo uses the same family of I/O and field tests showed gentler SSD thermal behaviour.
  - `Direct I/O` remains available for validated high-throughput hosts.
- Buffered mode writes one RX block at a time, matching the vendor demo write granularity more closely.
- IQ application buffering is configured by **time window** rather than large GiB tiers: Auto, 0.25 s, 0.50 s, 1.00 s, 2.00 s.
  - Auto target: approximately 0.75 s.
  - Auto hard cap: 2 GiB.
  - Manual hard cap: 4 GiB.
  - The actual byte count is calculated from sample rate and enabled channel count and remains limited by the host-memory safety budget.
- Storage/NVMe fast diagnostics are sampled less aggressively (1 s during startup, 2 s steady-state).
- If sustained writer throughput is below RX and the application queue reaches 85%, capture stops early to preserve a continuous file prefix and reduce unnecessary backlog/drain time.
- Existing IQ validity checks, no-silent-drop policy, preview thread, graceful Stop, multilingual UI, and PCIe guard are retained. V1.1.1 moves full-file validation out of the capture-stop path and makes detailed logging opt-in.

## Storage semantics

V1.1 does **not** claim that buffered I/O makes a slow SSD capable of sustaining a higher physical media rate. Buffered mode allows the Linux page cache and writeback scheduler to shape the write workload more like the vendor demo. The application still monitors its bounded queue and NVMe temperature in normal mode. `/proc/self/io` and dirty/writeback diagnostics are collected only in Developer mode; final file validation is user-triggered after capture. On Stop, buffered mode performs a final `fflush()` + `fdatasync()` after RX has been released so the completed file is forced to storage rather than merely left in the page cache.

The storage queue remains intentionally separate from RX. A true zero-copy `lw39x0_recv()` directly into writer-owned blocks is not enabled in V1.1 because the current SDK contract does not document the ownership/lifetime guarantees needed to make that change safely.

## Calibration semantics

Calibration is a per-Start option. Disabled means no `lw39x0_set_zone_*_calibrate()` call is made. Enabled means calibration is called only for enabled zones supported by the selected model:

- LW3920: Zone A
- LW3940: Zones A/B as applicable
- LW3980: Zones A/B/C/D as applicable

Calibration can take noticeable time. A Stop request received during a blocking vendor calibration call is honored immediately after that call returns.

## Recommended V1.1 validation

1. Compare V1.1 `Auto` against the vendor demo using the same SSD, 8 channels, 122.88 MS/s, cold-start SSD temperature, and identical capture duration.
2. Record NVMe composite/max sensor temperature, application RX MiB/s, writer MiB/s, `/proc/self/io write_bytes`, Dirty/Writeback, queue percentage, and final IQ file size.
3. Repeat with `Direct I/O` to isolate the effect of the I/O backend.
4. Verify repeated Start/Stop and channel switching with calibration both off and on.
5. Validate final IQ continuity with an external known counter/test pattern when available; non-zero/RMS checks alone cannot prove sample-by-sample continuity.
