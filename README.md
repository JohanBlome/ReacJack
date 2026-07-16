# ReacJack

ReacJack captures Roland REAC Ethernet audio packets and exposes the decoded
channels as JACK output ports.

This tree has been updated to keep the original Linux raw packet path while
adding a macOS capture path through `libpcap`. The audio side still uses JACK,
so on macOS the practical route is:

1. Install and start a JACK server.
2. Run ReacJack and select the Ethernet interface connected to REAC.
3. Record the `ReacJack` output ports in any JACK-aware recorder or route them
   onward with your JACK patchbay.

## Build

### macOS

Install JACK headers/libraries first. With Homebrew this is typically:

```sh
brew install jack
make
```

Start JACK before running ReacJack. Homebrew's JACK formula suggests either the
service or a direct CoreAudio launch:

```sh
brew services start jack
# or
/opt/homebrew/opt/jack/bin/jackd -X coremidi -d coreaudio
```

The macOS packet capture backend uses the system `libpcap`. Capturing raw
Ethernet frames may require elevated privileges. If `sudo` cannot see your JACK
server, adjust BPF capture permissions instead of running the audio client as
root.

```sh
sudo ./reacjack
```

### Linux

Install JACK development headers, then build:

```sh
make
```

For modern PipeWire systems, you have two options.

The compatibility path is to run the JACK build through PipeWire's JACK support
(`pipewire-jack` on many distributions). The native path is the PipeWire source
client:

```sh
make pipewire
sudo ./reacjack-pw -i eth0
```

`reacjack-pw` appears in the PipeWire graph as `ReacJack REAC`, producing
48 kHz float audio for capture/recording clients. It currently defaults to 40
channels; use `-c` for smaller REAC configurations:

```sh
sudo ./reacjack-pw -i eth0 -c 2
```

To install the PipeWire client with raw-capture capabilities:

```sh
make install-pipewire
reacjack-pw -i eth0
```

For non-root packet capture after `make install`, Linux uses capabilities:

```sh
make install
```

## Resilience Notes

The current version validates packet sizes, channel counts, and REAC EtherType
before decoding. If the network stream underruns or the JACK side cannot read
enough samples, the affected JACK period is filled with silence and counters are
updated instead of crashing the process.

The source lives in `src/`, tests in `tests/`, and planning notes in `docs/`.
The next major improvement would be splitting the capture, REAC decode, and
audio output code into separate modules. That would make it much easier to add a
native CoreAudio output or a direct multichannel WAV recorder for macOS.
