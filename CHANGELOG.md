# Changelog

## v1.0.1 — 2026-11-10

- Driver error messages, log output and generated `config.ini` comments are now in English.
- Diagnostic tools and build scripts translated to English.
- README: troubleshooting entries for every driver error message.

## v1.0.0 — 2026-11-10

First release.

- ASIO driver for the Behringer UMC22: WASAPI exclusive input and output at the device's minimum period (3 ms @ 48 kHz), 2 in / 2 out, 44.1 / 48 kHz.
- Windows audio (YouTube, etc.) mixed into the output via WASAPI loopback, with a cubic Hermite resampler, smoothed clock-drift correction and click-free fades.
- Automatic switching of the Windows default output to the loopback device while the driver is open, restored on close.
- Peak limiter (−0.2 dBFS), TPDF dither to 16-bit, automatic mono L→R copy.
- `config.ini` settings, rotating log, optional level/phase meter.
- Diagnostic tools: `probe`, `testhost`, `phasetest`.

## Installation

Extract the zip to a permanent folder, close your ASIO host, run `install.bat` (admin) and select **"UMC22 Low Latency"** in your host. See the README for details.
