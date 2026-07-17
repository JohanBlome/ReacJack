# ReacJack

ReacJack captures Roland REAC Ethernet audio (up to 40 channels of 24-bit
audio at 48 kHz) and exposes the decoded channels to ordinary recording
applications. One shared, unit-tested decoder feeds three frontends:

- **macOS native**: `reacjackd` captures and decodes REAC into a shared
  memory ring, and `ReacJack.driver` (a CoreAudio HAL plug-in) publishes it
  as a 40-channel input device named `ReacJack REAC`. No JACK required.
- **Linux native**: `reacjack-pw` exposes the channels as a PipeWire source
  node named `ReacJack REAC`.
- **JACK bridge (optional)**: the original `reacjack` client exposes the
  channels as JACK output ports. It builds on both macOS and Linux when the
  JACK development files are installed, and is skipped with a notice when
  they are not.

## Build

### macOS

Xcode command-line tools are enough for the native path:

```sh
make          # reacjackd, reacjackctl, ReacJack.driver (+ reacjack if JACK is installed)
make test     # decoder, shared ring, and HAL driver test suites
```

Install the full stack — binaries to `/usr/local/bin`, the CoreAudio device
to `/Library/Audio/Plug-Ins/HAL` (briefly restarts `coreaudiod`, which
interrupts system audio for a moment):

```sh
make install-macos             # manual capture: run reacjackd yourself
make install-macos IFACE=en5   # also installs a launchd daemon that
                               # captures from en5 at boot and logs to
                               # /var/log/reacjackd.log
make uninstall-macos           # removes daemon, binaries, and driver
```

(`make install-driver` / `make uninstall-driver` still exist for iterating
on the HAL plug-in alone.)

Then start the daemon (unless launchd already did) and record the device in
any CoreAudio app (QuickTime, Reaper, Logic, ...):

```sh
sudo ./reacjackd -i en5   # channel count detected from the REAC stream
./reacjackctl -w          # watch ring fill, counters, and clock
```

Capturing raw Ethernet frames may require elevated privileges; if you
prefer not to run the daemon as root, adjust BPF capture permissions
instead (`reacjackd` prints a hint when capture is denied). If `reacjackd`
is restarted, stop and restart recording so the device reattaches to the
new ring.

To build the optional JACK bridge as well: `brew install jack`, then `make`.

### Verifying the install without REAC hardware

The daemon's `-tone` mode substitutes a synthetic 440 Hz signal for
capture, which exercises the whole chain (daemon, shared ring, CoreAudio
driver) with no REAC device attached:

```sh
make && make test        # all three suites must pass
make install-macos
./reacjackd -tone -c 2   # leave running
./reacjackctl -w         # in another terminal
```

Expected while recording the `ReacJack REAC` device in QuickTime/Reaper:

1. The device appears in Audio MIDI Setup (40 channels, 48 kHz).
2. The recording carries the 440 Hz tone on channels 1-2 and silence on
   channels 3-40; without the daemon running it records pure silence.
3. In `reacjackctl -w`: `fill_frames` hovers near 4800 (the 100 ms
   target) while recording, `clock_sample_pos` advances steadily, and
   `drift_inserted_frames`/`drift_dropped_frames` stay at or near zero -
   the device clock is slaved to the daemon's observed rate, so audio
   corrections should be rare.
4. Stopping the daemon mid-recording degrades to silence (underruns
   count up; nothing crashes); after restarting it, stop and start the
   recording to reattach.

For long-session confidence, leave the tone recording running for a few
hours and confirm the drift counters stay near zero and the recording has
no audible glitches. `make uninstall-macos` removes everything afterwards.

### Linux

```sh
make            # reacjackctl (+ reacjack if JACK development files are installed)
make test
make pipewire   # reacjack-pw, needs libpipewire-0.3 development files
```

Run the native PipeWire source:

```sh
sudo ./reacjack-pw -i eth0
```

`reacjack-pw` appears in the PipeWire graph as `ReacJack REAC`, producing
48 kHz float audio for capture/recording clients. It defaults to 40
channels; use `-c` for smaller REAC configurations (`-c 2`).

For non-root packet capture, the install targets grant capabilities:

```sh
make install            # reacjack (JACK bridge)
make install-pipewire   # reacjack-pw
```

The JACK build also works through PipeWire's JACK support (`pipewire-jack`)
on modern systems.

## Clocking and resilience

REAC hardware and the recording machine run on separate clocks. The daemon
publishes clock observations (host time, sample position) into the shared
ring, and the CoreAudio device slaves its timestamps to the observed REAC
rate, so CoreAudio rate-matches the device the same way it does a hardware
interface. On top of that, the ring reader keeps its fill inside a target
band by inserting or dropping at most a few frames per cycle - with clock
slaving active this acts only as a safety net for packet loss.

Malformed packets, packet loss, underruns, overruns, and drift corrections
never crash the process: affected periods become silence and counters are
updated. `reacjackctl` (macOS and Linux) shows the ring format, fill, and
all counters; `reacjackd` prints a status line every two seconds.

The source lives in `src/`, tests in `tests/`, and the CoreAudio migration
plan with per-milestone status in `docs/COREAUDIO_PLAN.md`.
