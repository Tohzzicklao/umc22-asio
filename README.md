# Low Latency ASIO for UMC22

A Windows (x64) ASIO driver for the **Behringer UMC22** (USB Audio CODEC / PCM2902 chip, which has no official ASIO driver). It works with any ASIO host — Neural DSP standalone plugins (Archetype series, etc.), other amp sims, and DAWs such as REAPER or Cubase — giving **low latency** while **mixing Windows audio (YouTube, Spotify, etc.) into your headphones at the same time**.

## Download

Grab the latest build from the [**Releases**](../../releases/latest) page:

1. Download `umc22-asio-vX.Y.Z-win64.zip` and extract it anywhere permanent (e.g. `C:\Program Files\UMC22ASIO` or a folder in your user directory).
2. Close any ASIO host (Nolly X, your DAW…).
3. Run **`install.bat`** and accept the administrator prompt.
4. In your host, choose **ASIO → "Low Latency ASIO for UMC22"**, 48000 Hz, buffer size 144.

To uninstall, run `install.bat /u`.

> No Visual C++ runtime is needed: the DLL is statically linked.

## Why

| Option | Problem with the UMC22 |
|---|---|
| FlexASIO / WASAPI shared mode | The UMC22 has a **fixed 10 ms period** in shared mode → high latency |
| ASIO4ALL / exclusive mode | Low latency, but **no other app can play audio** (YouTube goes silent) |
| **Low Latency ASIO for UMC22** | Exclusive mode at **3 ms** + Windows audio mixed inside the driver |

## How it works

```
Guitar ─► UMC22 input (WASAPI exclusive, 3 ms) ─┐
                                                ├─► Nolly X (ASIO) ─┐
                                                │                   ▼
Browser/YouTube ─► "High Definition Audio Device" ─► loopback ─► resampler ─► mix ─► limiter ─► UMC22 output (exclusive) ─► headphones
```

- **UMC22 input and output in WASAPI exclusive mode**, event-driven, at the device's minimum period (144 frames @ 48 kHz = 3 ms). Typical latency with a 144 buffer: 3 ms in, 6 ms out.
- **Windows audio mix**: Windows plays to a different device (by default the motherboard output, "High Definition Audio Device"). The driver captures it via WASAPI *loopback*, resamples it (cubic Hermite) with smoothed clock-drift correction, and adds it to the output with short fades to avoid clicks.
- **Automatic default device switching**: when the driver opens, if Windows uses the UMC22 as its default output, it is moved to the loopback device. It's restored when the driver closes.
- **Peak limiter** (−0.2 dBFS) instead of hard clipping, and **TPDF dither** to 16-bit.
- If the host only sends the guitar on the left channel, it is copied to the right (`monoAuto`).
- The ASIO interface is implemented at the ABI level: **no Steinberg SDK required** (see [ASIO SDK](#asio-sdk)).

## Requirements

- Windows 10/11 x64
- Behringer UMC22 (or another USB Audio Class interface: adjust the device names in `config.ini`)
- A second Windows output device for the loopback mix (the motherboard's onboard audio works; nothing needs to be plugged into it)
- To build from source: Visual Studio 2022 or later with "Desktop development with C++"

## Configuration

`%APPDATA%\UMC22ASIO\config.ini` is created on first use. Open it with the driver's *Control Panel* button in your host.

| Key | Default | Description |
|---|---|---|
| `render` / `capture` | `USB Audio CODEC` | Substring of the output / input device name |
| `loopback` | `High Definition Audio Device` | Device whose audio is mixed into the headphones (empty = disabled) |
| `ytVolume` | `1.0` | Volume of the Windows/YouTube mix (0–2) |
| `ytLatencyMs` | `40` | Buffer for the Windows audio; raise it if YouTube drops out |
| `preferredBuffer` | `144` | Preferred ASIO buffer size (frames) |
| `monoAuto` | `1` | Copy L→R if the host only sends the left channel |
| `autoDefault` | `1` | Automatically switch/restore the Windows default output |
| `debugMeter` | `0` | Log levels, phase correlation and limiter activity every 5 s |

Log file: `%APPDATA%\UMC22ASIO\log.txt` (rotated to `log.old.txt` above 512 KB).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| "The UMC22 is in use by another app" | An app has the UMC22 open and doesn't follow the default device. Change its output in *Volume mixer* |
| "Exclusive mode is disabled in the UMC22 properties" | Sound settings → UMC22 → *More sound settings* → *Advanced* → enable "Allow applications to take exclusive control" |
| "UMC22 (USB Audio CODEC) not found" | Interface unplugged, or a different device name: adjust `render` / `capture` in `config.ini` |
| No YouTube audio | Make sure the browser/Windows plays to the `loopback` device and that `log.txt` shows `loopback active` |
| YouTube sounds odd / weak vocals | Disable *Audio enhancements* and *Spatial sound* on the loopback device. If it also happens without the driver, check the headphone plug: a TRRS headset plug in a TRS jack, or a half-inserted plug, produces L−R cancellation |
| YouTube dropouts | Raise `ytLatencyMs` to 60–80 |
| Guitar crackles | Increase the buffer size in your host (192, 256) |

## Building from source

```bat
git clone https://github.com/Tohzzicklao/umc22-asio.git
cd umc22-asio
build.bat          :: builds umc22asio.dll and tools\bin\*.exe
install.bat        :: copies to bin\ and registers the driver (admin)
install.bat /u     :: uninstalls
```

`build.bat` locates Visual Studio automatically through `vswhere`.

> To update without closing the host: Windows lets you **rename** a loaded DLL. Rename `bin\umc22asio.dll`, copy the new one in, and the host picks it up on its next start (the registered path doesn't change).

### Making a release

```bat
build.bat
package.bat 1.0.0  :: creates dist\umc22-asio-v1.0.0-win64.zip
gh release create v1.0.0 dist\umc22-asio-v1.0.0-win64.zip --title "v1.0.0" --notes-file CHANGELOG.md
```

## Tools (`tools/`)

- `probe` — lists audio devices with their mix format, shared/exclusive periods and supported exclusive formats.
- `testhost` — minimal ASIO host: loads `umc22asio.dll` without registering it and counts callbacks for 3 s (`testhost.exe [buffer]`, run from the folder containing the DLL).
- `phasetest` — plays a mono test tone on the loopback device and measures L/R level and phase with synchronous detection (detects Windows effects that alter the stereo image). The endpoint ID is hard-coded at the top of the file; change it for your machine.

## Limitations

- 44.1 / 48 kHz only, 2 inputs / 2 outputs.
- The UMC22 only accepts 16-bit in exclusive mode.
- The onboard output also plays the Windows audio (only audible if something is plugged into it).
- `IPolicyConfig` (used to switch the default device) is an undocumented Windows interface.

## ASIO SDK

This project **does not include, use, or require the Steinberg ASIO SDK**. No SDK source files or headers are part of this repository, and none are needed to build it.

An ASIO driver is an in-process COM object that a host loads and calls through a fixed binary interface (a table of virtual methods plus a few plain C structs). `umc22asio.cpp` declares that interface itself — the method order, calling convention and struct layouts needed for binary compatibility with ASIO hosts — and implements it with original code on top of Windows WASAPI. This is done purely for interoperability, so that existing ASIO hosts can talk to the driver.

For context: Steinberg historically distributed the ASIO SDK under a proprietary license that does not allow redistributing its files, which is why other open-source drivers download it at build time. Since October 2025 Steinberg also offers the ASIO SDK under the GPLv3, so a future version of this project could adopt the official headers without any licensing conflict, as this project is GPLv3 as well.

## License

Copyright (C) 2026 Tohzzicklao

This program is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0** or (at your option) any later version. It is distributed in the hope that it will be useful, but **without any warranty**. See [`LICENSE`](LICENSE) for the full text.

## Trademarks and disclaimer

This is an independent, unofficial project. It is **not affiliated with, endorsed by, or supported by** Steinberg Media Technologies GmbH, Behringer / Music Tribe, or Neural DSP Technologies.

- ASIO is a trademark and software of Steinberg Media Technologies GmbH.
- Behringer and UMC22 are trademarks of Music Tribe Global Brands Ltd.
- Archetype: Nolly X is a product of Neural DSP Technologies.
- All other trademarks belong to their respective owners and are used only to describe compatibility.

Use at your own risk. Keep your monitoring volume at a safe level, especially when testing a new configuration.
