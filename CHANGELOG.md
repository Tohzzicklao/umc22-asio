# Changelog

## v1.2.1 — 2026-11-19

- Settings window: **Report a problem...** button. It opens a new GitHub issue prefilled with the driver and Windows versions, the host, the settings, the status and the system check (user profile paths are replaced by `%USERPROFILE%`), and opens the log folder so `log.txt` can be attached.
- Settings window: **Apply** stays disabled until a setting changes; OK without changes no longer rewrites `config.ini`.
- Driver version reported to hosts: 1.2.1.

## v1.2.0 — 2026-11-19

- **Lower output latency**: the host output goes straight into the UMC22 without an extra buffer, one buffer less output latency. At 144 samples the reported latency drops from 3 / 6 ms to **3 / 3 ms**. New `lowLatency` setting (on by default) to turn it off.
- **Survives unplugging**: if the UMC22 is disconnected while playing, the driver waits for it and resumes audio automatically when it is plugged back in, even on another USB port. The host no longer has to reload the driver. Also detected when Windows simply stops sending audio events.
- **Settings window** (Control Panel button in the host, Start menu shortcut, or `rundll32 umc22asio.dll,ControlPanel`): device lists, buffer size, low latency output, Windows audio volume (applied live) and buffer, options with help on the less obvious ones, a system check (UMC22 connected, exclusive mode allowed, Windows audio device, driver registration), status and level meters. Settings that need the device reopened trigger an ASIO reset request.
- **Installer** (`umc22-asio-vX.Y.Z-setup.exe`): installs to Program Files, registers the driver, adds a Start menu shortcut and an uninstaller. The portable zip with `install.bat` is still available.
- When the UMC22 was plugged in a moment before the host opened the driver, `init` now waits up to 1 s for Windows to list it instead of failing with "not found".
- When no exclusive format can be opened, the driver reports the real WASAPI error (device in use, exclusive mode disabled...) instead of "unsupported format". Rejected formats are logged only in that case.
- `testhost` accepts a duration and a `panel` option.
- Driver version reported to hosts: 1.2.0.

## v1.1.0 — 2026-11-10

- **Renamed** to "Low Latency ASIO for UMC22". Installing this version removes the old "UMC22 Low Latency" entry; re-select the driver in your host.
- Licensed under the **GNU GPL v3.0 or later** (`LICENSE` included in the release zip).
- README: ASIO SDK clarification, license and trademark/disclaimer sections.
- Driver version reported to hosts: 1.1.0.

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

Close your ASIO host and run **`umc22-asio-vX.Y.Z-setup.exe`**, then select **"Low Latency ASIO for UMC22"** in your host. Portable alternative: extract the zip to a permanent folder and run `install.bat` (admin). Upgrading from the zip version: run `install.bat /u` from the old folder first. See the README for details.
