# Steam Frame Dongle — `set_region`

[![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
[![cpp](https://img.shields.io/badge/C++-23-purple)](https://img.shields.io/badge/C++-23-purple)
[![msvc](https://img.shields.io/badge/build-MSVC%202022%2B-orange)](https://img.shields.io/badge/build-MSVC%202022%2B-orange)
[![single-file](https://img.shields.io/badge/single--file-yes-brightgreen)](https://img.shields.io/badge/single--file-yes-brightgreen)
[![license](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

A small, safe, single-file Windows CLI tool to **show or set the country code of the
Steam Frame USB dongle's 6 GHz Wi-Fi adapter** — the same operation the Steam Frame
app performs, exposed as a one-liner.

```
C:\> set_region
US

C:\> set_region PL
Adapter:  Steam Frame
Device:   USB\VID_28DE&PID_2432\...
Driver:   verified 5.32.908.2026
Before:   US
Driver:   Country code has changed to PL
After:    PL
    6G Info
    6G Support (domain:05), due to REGU_RSN_MANUAL
SUCCESS: country set to PL with 6 GHz support
```

## Why

The Steam Frame dongle ships with a fixed country code for its 6 GHz radio, which
limits available channels/bands in your region. Changing it normally requires the
Steam Frame app; this tool performs the same driver operation directly — with
strict safety checks so it never talks to the wrong device or driver.

## Features

- **Read or write** the adapter country code (`ISO 3166-1 alpha-2`: `PL`, `US`, `DE`, …)
- **Hardware-identified adapter** — matched by USB ID `VID_28DE&PID_2432`, never by
  display name or "first interface", so it cannot target a wrong/named device
- **Driver integrity checks** — verifies the `rtwlanuval` service path and SHA-256
  of `rtwlanuval.sys`; warns on unverified driver versions
- **6 GHz support verification** — after a change, confirms the driver actually
  reports `REGU_RSN_MANUAL` 6 GHz support for the new country (exit code `2` if not)
- **Concurrency-safe** — takes a `Global\` mutex shared with the Rust tool to avoid
  corrupting concurrent diagnostics (the driver uses shared buffers)
- **No blind retries** — on uncertain driver state the request is *not* resent;
  success is always confirmed by re-reading the driver, never inferred from the reply
- **Zero runtime dependencies** — single `.cpp` file, C++23, all pure logic
  `constexpr` and verified at compile time with `static_assert` test suites

## Requirements

- Windows 10/11 x64
- Steam Frame USB adapter plugged in (driver installed, service `rtwlanuval` running)
- [Visual Studio 2022+](https://visualstudio.microsoft.com/downloads/) with the
  *"Desktop development with C++"* workload (build only)
- **Administrator** privileges at runtime

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

or manually, from the *x64 Native Tools Command Prompt*:

```bat
cl /std:c++latest /EHsc /O2 set_region.cpp wlanapi.lib advapi32.lib ole32.lib crypt32.lib /Fe:set_region.exe
```

Output: `set_region.exe` in the project root.

## Usage

```
set_region [CC]
  (no argument)  print the adapter's current country code
  CC             set the country code (ISO 3166-1 alpha-2, e.g. PL, US, DE)
```

| Exit code | Meaning |
|:---------:|---------|
| `0` | OK (read succeeded, or country set **with** 6 GHz support) |
| `1` | Error (no adapter, driver mismatch, protocol failure, bad argument, …) |
| `2` | Country was set, but the driver reports **no manual 6 GHz support** for it |

## How it works

The tool speaks the driver's private IHV protocol over `WlanIhvControl`:

1. Enumerates WLAN interfaces + setup-class devices, matches the Steam Frame by USB
   hardware ID, and resolves its interface GUID.
2. Validates the `rtwlanuval` service `ImagePath` and hashes the driver binary.
3. Acquires the shared diagnostic mutex.
4. Exchanges fixed-size packets (40-byte header + body) for:
   - `country` (OID `0xff81521f`) — read current code,
   - `diagnostic` subcommand `29` with tokens `67` + `<CC>` — set the code,
   - `diagnostic` subcommand `0` (`echo core 6g_info`) — read 6 GHz status,
   - `done` / `output` OIDs — completion flag and chunked text output.
5. Re-reads the status to confirm the change, and checks 6 GHz support.

All packet framing, parsing, and state logic is `constexpr` and covered by
compile-time tests.

## Project layout

```
.
├── set_region.cpp   # the entire tool (protocol, driver checks, CLI, tests)
├── build.ps1        # MSVC build via vswhere + DevShell
├── LICENSE
└── README.md
```

## Safety notes

- Run **only** with the Steam Frame dongle plugged in — other adapters are ignored
  by design, but keep the environment clean.
- Setting a country code the driver does not support is rejected by the driver
  (`Invalid country code!`); the tool surfaces this as an error, not a success.
- Country-code rules (e.g. which codes your ISP/regulator allows) are your
  responsibility.

## Credits

Protocol is based on the Steam Frame 6 GHz tool.

## License

[MIT](LICENSE)
