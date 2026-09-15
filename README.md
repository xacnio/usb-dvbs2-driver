# usb-dvbs2-driver

An SDK for a USB DVB-S/S2 receiver on Windows, Linux and macOS: the drivers,
an engine that runs the receiver in-process, a daemon that puts the engine
behind a local command channel, a client library for programs that start and
drive the daemon, a command-line tool on that library, diagnostic tools and
tests.

Nothing here knows about a user interface. A program links the engine, or
starts the daemon through the client library, and plays the MPEG-TS stream
it sends over UDP; `usb-dvbs2` is one such program. A project can also carry
this repository as a `git submodule` and build it with `add_subdirectory`.

## Supported hardware

The receiver the SDK is developed and tested against:

- USB id `048D:F036` (`UDTV`)
- IT9303 USB bridge
- AVL6261 DVB-S/S2/S2X demodulator
- RDA5815M tuner

Other USB receivers that look similar are not guaranteed to work. Only this
one is supported because only this one has been tested: everything above the
USB layer is ordinary DVB, the same on any DVB-S2 receiver.

What is device specific sits in one record in `core/hardware_profile.c`: USB
id, bridge, demodulator, tuner, firmware names and I²C addresses. A receiver
built on the same parts is a new row in that table; different silicon needs a
new driver and its own firmware.

## Firmware

The bridge and the demodulator keep no software of their own; the firmware
is uploaded on every start. The files are in `firmware/`, and the daemon and
the engine read them from `firmware/` beside their executable (or in the
working directory), under the names the hardware profile gives:

- `hiremco-it9303.fw`: IT9303 USB bridge
- `hiremco-avl62x1.fw`: AVL6261 demodulator

The files come from the manufacturer's Android application
(`HiremcoDTV.apk`) as they are; nothing here writes, builds, decodes or
modifies them, only transfers them to the device in the order the APK does.
Their rights belong to their respective holders; this repository's license
does not cover them.

## Layout

- `core/`: USB backend, IT9303 bridge, AVL62x1 demodulator, RDA5815M tuner,
  hardware profiles, transport stream and DVB table parsing, card server
  client and DVB-CSA descrambling (library `usbdvbs2core`), and what the rest
  needs from the operating system: `dtv_platform.h` (clocks, locks, threads),
  `dtv_net.h` (sockets) and `dtv_ipc.h` (the daemon's command channel)
- `engine/`: `usb_dvbs2_engine.h`, the receiver in-process (library
  `usbdvbs2engine`): cold init, tuning, the filtered stream on UDP and what it
  carries, reported through callbacks
- `daemon/`: `usb-dvbs2-daemon`, the engine behind its command channel, and
  `usb-dvbs2-probe`
- `sdk/`: `usb_dvbs2.h`, the client library (`usbdvbs2client`): starts the daemon,
  sends its commands, reads its events and status
- `cli/`: `usb-dvbs2`, a terminal session on the SDK
- `tools/`: diagnostic tools for development
- `packaging/linux/`: the udev rule that gives a desktop user the receiver
- `tests/`: tests of the parsing and cryptography code

## Building

CMake 3.16 and a C11 compiler. libdvbcsa is in `third_party/`; libusb comes
from the system on Linux and macOS, and as prebuilt binaries on Windows.

### Windows

Clang and MinGW Makefiles (`mingw32-make`), as in
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw). libusb's header is in
`third_party/`, but not its binaries: put `libusb-1.0.dll` and
`libusb-1.0.dll.a` from the MinGW64 build of
[libusb 1.0.27](https://github.com/libusb/libusb/releases/tag/v1.0.27) into
`third_party/libusb/lib/`. The receiver needs the WinUSB driver, which
[Zadig](https://zadig.akeo.ie/) installs.

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake --install build --prefix usb-dvbs2-driver
```

### Linux

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
sudo cp packaging/linux/60-usb-dvbs2.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

The udev rule lets the user at the desktop (and the `plugdev` group) open the
receiver without root; plug it in again after installing it. A kernel driver
that has claimed the device is detached when the daemon opens it.

### macOS

```sh
brew install cmake pkg-config libusb
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

No driver is needed: libusb talks to the receiver directly.

### Options and packages

- `-DUSB_DVBS2_CAMD=OFF` leaves out the card server client, DVB-CSA
  descrambling and libdvbcsa. Free services stream as before; `CAMD` reports
  that support is missing.
- `-DUSB_DVBS2_CSA_PORTABLE=ON` uses the plain C descrambler word that
  processors without SSE2 (ARM) use, on x86 too, to test it there.

`cmake --install` lays out a package: `bin/` with the executables and
`firmware/` (and `libusb-1.0.dll` on Windows), ready to run, `include/` and
`lib/` with the SDK, and on Linux `share/udev/` with the rule. Every push to
`master` publishes one per system (Windows x64, Linux x64 and ARM64, macOS
ARM64) as a release, tagged with the version in `CMakeLists.txt` and the
commit.

The build copies the firmware into `firmware/` beside the executables, so they
run from the build folder as they are. A parent project can `add_subdirectory`
this folder, and pass `USB_DVBS2_LIBUSB_DIR`, `USB_DVBS2_LIBDVBCSA_DIR` or
`USB_DVBS2_FIRMWARE_DIR` to use its own copies.

## Running

The daemon finds its firmware in `firmware/` beside itself; on Windows
`libusb-1.0.dll` has to be there too. It streams the MPEG-TS to
`127.0.0.1:<port>` and mirrors it to the next port up.

```text
usb-dvbs2-daemon --udp 5560 [--pipe <channel>] --profile auto
    [--device <index>]
    [--camd-proto newcamd|cs378x --camd-server <host:port>
     --camd-user <user> --camd-pass <pass> --camd-des-key <28 hex digits>]
```

The command channel is a named pipe on Windows, `\\.\pipe\usb_dvbs2` by
default, and a Unix domain socket elsewhere: `$XDG_RUNTIME_DIR/usb_dvbs2.sock`,
or `/tmp/usb_dvbs2-<uid>.sock` without that variable, readable only by the
user who started it. On Linux and macOS SIGTERM, SIGINT and SIGHUP end the
daemon as `QUIT` does, powering the LNB down.

### usb-dvbs2

```text
usb-dvbs2 [--dir <folder>] [--profile <key>] [--udp <port>]
```

`--dir` is the folder holding the daemon and `firmware/`; by default the
tool's own. It starts the daemon (or reuses a running one) and reads commands
one per line:

```text
tune 1629000 30000 18 22          lock a carrier: L-band kHz, symbol rate
                                  ksps, LNB volts, 22 kHz tone
tune_rf 11794 30000 V             the same from the RF frequency
channel 51202 6202 6202 6203      service, PMT, video and audio PIDs
status
epg on
quit
```

`help` lists the rest. Commands can also come from a pipe; `wait <seconds>`
keeps a script's session open. Ending the tool powers the LNB down.

## SDK

```c
#include "usb_dvbs2.h"

usb_dvbs2_host host = { .log = my_log, .signal_state = my_signal };
usb_dvbs2_init("/opt/usb-dvbs2-driver/bin", USB_DVBS2_UDP_PORT, &host);
if (usb_dvbs2_start() == 0) {
    usb_dvbs2_tune(1629000, 30000, 18, 22, "0 0 0 0");
    usb_dvbs2_channel(51202, 6202, 6202, 6203, 0x1fff);
    /* ... usb_dvbs2_poll(&status) for signal readings ... */
    usb_dvbs2_stop();
}
```

Link the `usbdvbs2client` target. Callbacks in `usb_dvbs2_host` run on the
library's reader thread. The library holds one daemon session per process.

## Engine

The daemon is the engine and a command channel, nothing more: every callback
prints one protocol line. A program that wants the receiver without a second
process links `usbdvbs2engine` instead:

```c
#include "usb_dvbs2_engine.h"

static void on_stage(int percent, const char *text) { /* ... */ }

usb_dvbs2_engine_host host = { .stage = on_stage };
usb_dvbs2_engine_config config = { .profile_key = "auto", .udp_port = 5560 };
if (usb_dvbs2_engine_open(&config, &host) == 0) {
    usb_dvbs2_engine_tune(1629000, 30000, 18, 22, 0, 0, 0, 0);
    usb_dvbs2_engine_set_channel(51202, 6202, 6202, 6203, 0x1fff, 0, 0x1fff);
    /* ... usb_dvbs2_engine_get_status(&status) ... */
    usb_dvbs2_engine_close();
}
```

The engine owns the receiver, so it cannot run beside a daemon, and there is
one per process. As with the daemon, `firmware/` must be beside the executable
(and `libusb-1.0.dll` on Windows). Callbacks run on the engine's threads and
must return quickly.

## Protocol

Commands go to the command channel, one line each. One client is served at a
time, and the session lasts as long as its connection does.


| Command | Meaning |
|---|---|
| `TUNE <lband_khz> <sr_ksps> <lnb_v> <tone_khz> <diseqc> [<uncommitted> <burst> <repeat>]` | Locks a carrier. Ignored when that carrier is already streaming. `sr_ksps` 0 acquires blind: the tuner's widest filter, and the demodulator finds the symbol rate itself, which `STATUS` then reports. |
| `TUNE_RF <rf_mhz> <sr_ksps> <H\|V> [<diseqc>]` | The same from the RF frequency, through a Ku-band universal LNB: LO 9750 MHz, or 10600 MHz and the 22 kHz tone from 11700 MHz; 13 V vertical, 18 V horizontal. `diseqc` is the DiSEqC 1.0 port, none by default. |
| `CHANNEL <service> <pmt> <video> <audio> [<teletext> [<ca_system> <ca_pid>]]` | Selects a service on the locked carrier. PIDs not carried are `8191`. |
| `STATUS` | Answered on the pipe, see below. |
| `CAMD OFF` / `CAMD <newcamd\|cs378x> <host> <port> <user> <pass> <des_key>` | Card server settings; take effect at once. |
| `STOP` | Powers the LNB down; the daemon stays. |
| `QUIT` | Ends the daemon. |

The `STATUS` reply is one line: five numbers, then tab-separated text fields.

```text
STATUS <locked> <fec> <frame> <snr_x100> <symbol_rate_hz>\t<date>\t<time>\t<now_start>\t<now_end>\t<now_title>\t<next_start>\t<next_title>\t<cam_state>\t<cam_message>
```

Everything else the daemon reports arrives on its standard output, one line
per event, fields separated by `|`. Lines that match none of these are log
text.

| Event | Meaning |
|---|---|
| `STAGE\|<percent>\|<text>` | Progress of the cold start. |
| `SIGNAL\|<1\|0>\|` | The stream is running, or has stopped. |
| `DEVICE\|0\|<text>` | The receiver was disconnected. |
| `CA\|<service>\|<scrambled>\|<ca_system>\|<ca_pid>` | From the PMT of the selected service. |
| `TTX\|<service>\|<pid>` | Teletext stream of the selected service. |
| `EPG\|<service>\|<event>\|<start_utc>\|<duration_s>\|<title>\|<text>` | A programme from the event table. |
| `CAMD\|<state>\|<message>` | Card server state: 0 off, 1 connecting, 2 connected, 3 decrypting, 4 rejected, 5 error; -1 for bad settings. |
| `LOSS\|<continuity_gaps>\|<send_failures>\|<overflow>` | Packets lost before the local socket. |

## License

GPL-2.0; see `LICENSE`. The files written for the project are
GPL-2.0-or-later, but `core/dtv_camd_newcamd.c` and `core/dtv_camd_cs378x.c`
derive from tsdecrypt, which grants version 2 only, so the built binaries are
bound by GPL-2.0.

| Source | Used for | License |
|---|---|---|
| [libusb](https://libusb.info/) 1.0.27 | All communication with the USB receiver | LGPL-2.1-or-later |
| [libdvbcsa](https://code.videolan.org/videolan/libdvbcsa) | DVB-CSA descrambling | GPL-2.0-or-later |
| [tsdecrypt](https://github.com/gfto/tsdecrypt) | The newcamd and cs378x clients, `core/dtv_camd_newcamd.c` and `core/dtv_camd_cs378x.c` | GPL-2.0 |
| [availink/dvb-frontends-availink](https://github.com/availink/dvb-frontends-availink) | AVL62x1 command format and registers, `core/avl62x1.c` | GPL-2.0-or-later |
| Linux `dvb-usb-v2` it930x/af9035, by way of [nxdong520/avl6381](https://github.com/nxdong520/avl6381) | The IT9300 bridge on libusb, `core/it9300.c` | GPL-2.0-or-later |

The firmware in `firmware/` is not covered by this license; see *Firmware*.
