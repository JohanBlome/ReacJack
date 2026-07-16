# ReacJack CoreAudio Migration Plan

## Goal

Make ReacJack usable as a native macOS recording source by exposing REAC audio
as a CoreAudio input device, while preserving the existing JACK bridge and
improving reliability for long unattended recordings.

## Principles

- Keep packet capture and REAC decoding out of the CoreAudio HAL plug-in.
- Keep all real-time callbacks non-blocking and allocation-free.
- Treat malformed packets, packet loss, underruns, and overruns as recoverable
  events with counters.
- Build each layer with tests before connecting it to the next layer.
- Keep Linux support working while adding macOS-native paths.

## Milestones

### 1. Shared REAC Decoder

Create a reusable decoder used by every frontend.

Files:

- `src/reac_decode.h`
- `src/reac_decode.c`
- `tests/test_reac_decode.c`

Responsibilities:

- Validate Ethernet EtherType `0x8819`.
- Validate REAC packet overhead and ending marker.
- Extract packet counter.
- Detect channel count from payload size.
- Reject odd/zero/unsupported channel counts.
- Decode packed REAC 24-bit samples into `float32` channel buffers.

Tests:

- Parses a valid synthetic 2-channel REAC packet.
- Rejects non-REAC Ethernet frames.
- Rejects short packets.
- Rejects malformed payload lengths.
- Rejects unsupported channel counts.
- Decodes positive, negative, maximum, and minimum-ish 24-bit sample values.

### 2. Existing JACK Bridge Uses Shared Decoder

Refactor `src/reacjack.c` to use the shared decoder.

Tests/checks:

- `make test` passes without JACK installed.
- `make` builds once JACK headers are installed.
- Manual Linux smoke test with existing REAC hardware path.
- Manual macOS smoke test with `libpcap` capture path.

### 3. macOS Capture Daemon

Create `reacjackd`, a standalone capture/decode process.

Responsibilities:

- Select and open an Ethernet interface with `libpcap`.
- Decode REAC packets to non-interleaved `float32`.
- Maintain packet and buffer health counters.
- Write decoded audio into shared memory.
- Continue on packet loss, malformed packets, capture timeouts, and overrun.

Tests:

- Synthetic packet feeder writes deterministic audio into the shared ring.
- Daemon status counters change predictably under malformed/lost packet tests.
- Long synthetic run keeps memory and file descriptors stable.

### 3b. Native Linux PipeWire Source

Create `reacjack-pw`, a native PipeWire source client for modern Linux systems.

Responsibilities:

- Capture REAC packets from a Linux raw packet socket.
- Decode packets through `src/reac_decode.c`.
- Buffer decoded `float32` audio in a single-writer/single-reader ring.
- Expose a PipeWire source node named `ReacJack REAC`.
- Produce 48 kHz interleaved `float32` audio into `pw_stream`.
- Continue on malformed packets, packet loss, overrun, and underrun.

Tests/checks:

- `make pipewire` builds on a Linux host with `libpipewire-0.3` development
  files installed.
- Source node appears in `pw-cli`, `qpwgraph`, or `Helvum`.
- `pw-record --target reacjack.reac out.wav` records synthetic or live audio.
- Overrun and underrun counters increase without process failure.

### 4. Shared Audio Ring

Implement a single-writer/single-reader shared memory audio ring.

Files:

- `shared_audio.h`
- `shared_audio.c`

Responsibilities:

- Metadata: ABI version, sample rate, channels, frame capacity, positions.
- Audio: `float32` non-interleaved or channel-strided ring frames.
- Counters: underruns, overruns, dropped packets, resets.
- Reader never blocks.
- Writer never corrupts partially written frames.

Tests:

- Basic write/read.
- Wraparound write/read.
- Underrun returns silence.
- Overrun policy is deterministic.
- Header version/size mismatch is rejected.

### 5. Silent CoreAudio HAL Plug-in

Create `ReacJack.driver` as an Audio Server Driver plug-in.

Behavior:

- Appears in Audio MIDI Setup as `ReacJack REAC`.
- Input-only.
- 40 channels.
- 48 kHz.
- `float32`, non-interleaved.
- Records silence reliably.

Tests/checks:

- Plug-in loads under `coreaudiod`.
- Device appears in Audio MIDI Setup.
- Reaper/Logic/QuickTime can open the device.
- Recording silence does not log callback errors.

### 6. HAL Plug-in Reads Shared Ring

Connect the CoreAudio device to `reacjackd`.

Behavior:

- `StartIO` opens shared memory.
- `ReadInput` pulls frames from shared memory.
- Missing frames are silence.
- Driver keeps counters but does not perform network I/O.

Tests/checks:

- Synthetic tone from daemon records through CoreAudio.
- Underrun produces silence, not failure.
- Restarting daemon recovers without rebooting the Mac.
- Killing `coreaudiod` reloads the driver cleanly.

### 7. Clock Drift Handling

Long recordings need drift control because REAC hardware and CoreAudio are
separate clocks.

Initial policy:

- Track target ring fill.
- If fill drifts low, insert tiny silence/duplicate correction.
- If fill drifts high, drop tiny frame ranges at zero-ish crossings when
  possible.

Later policy:

- Add a lightweight drift-aware resampler.

Tests:

- Synthetic source running slightly fast keeps ring fill bounded.
- Synthetic source running slightly slow keeps ring fill bounded.
- Corrections are counted and visible.
- Multi-hour soak test does not drift into underrun/overrun.

### 8. Packaging And Operations

Build deliverables:

- `reacjackd`
- `reacjackctl`
- `ReacJack.driver`
- install/uninstall scripts

Tests/checks:

- Fresh install works.
- Uninstall removes the HAL plug-in and shared resources.
- Clear error messages for missing capture permissions.
- Clear status for no REAC packets, packet loss, and clock drift.

## Status

- Milestones 1, 2, and 3b are done: the decoder is tested, the JACK bridge and
  the PipeWire client consume it.
- Milestone 4 is done: `src/shared_audio.{h,c}` implements the shared memory
  ring with tests in `tests/test_shared_audio.c`.
- Milestone 3 is done for the capture/decode/ring path: `reacjackd` captures
  via libpcap (or generates a `-tone` test signal) into the shared ring, and
  `reacjackctl` reports ring health. Long-run soak testing is still pending.
- Milestone 5 is code-complete: `src/coreaudio/ReacJackDriver.c` builds into
  `ReacJack.driver`, an input-only 40-channel 48 kHz float32 device that
  records silence. `tests/test_hal_driver.c` loads the bundle the way
  `coreaudiod` does (factory, QueryInterface) and exercises properties, IO
  start/stop, zero timestamps, and silent ReadInput. Still pending: manual
  verification under `coreaudiod` (`make install-driver`, then check Audio
  MIDI Setup and record in QuickTime/Reaper).
- Next: milestone 6, connecting the HAL plug-in to the reacjackd shared ring.
