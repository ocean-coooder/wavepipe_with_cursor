# ALSA WAV Playback + PCM/WAV Record Tool

## Features

- Play PCM WAV audio via ALSA (`play` mode)
- Record raw PCM data via ALSA (`record` mode)
- Record WAV file via ALSA (`record-wav` mode)
- Supported sample rates: `44100`, `48000`
- Supported channels: `2`, `4`, `8`
- Supported bit depth: `8`, `16`, `32`
- Auto detect first available ALSA PCM device if device is not specified (uses `plughw:N,M` instead of bare `hw` for better Raspberry Pi compatibility)

## Build (Raspberry Pi 400)

Install dependency:

```bash
sudo apt update
sudo apt install -y build-essential libasound2-dev
```

Build binary:

```bash
make
```

Output binary:

```bash
bin/alsa_tool
```

## Usage

Play WAV:

```bash
./bin/alsa_tool play input.wav
```

Play WAV with explicit ALSA device:

```bash
./bin/alsa_tool play input.wav hw:0,0
```

Play WAV on a specific **card** index (uses `plughw:N,0`; card index matches `aplay -l`):

```bash
./bin/alsa_tool play -c 1 input.wav
./bin/alsa_tool play --card 1 --pcm 0 input.wav
```

Record PCM:

```bash
./bin/alsa_tool record out.pcm 5 48000 2 16
```

Record PCM with explicit ALSA device:

```bash
./bin/alsa_tool record out.pcm 5 44100 8 32 hw:1,0
```

Record on card 1 (PCM subdevice 0):

```bash
./bin/alsa_tool record -c 1 out.pcm 5 48000 2 16
```

Record WAV:

```bash
./bin/alsa_tool record-wav out.wav 5 48000 2 16
```

Record WAV with explicit ALSA device:

```bash
./bin/alsa_tool record-wav out.wav 5 44100 4 32 hw:1,0
```

Record WAV on card 1:

```bash
./bin/alsa_tool record-wav -c 1 out.wav 5 48000 2 16
```

Do not combine `-c` with a trailing `[device]` argument; use one or the other.

## Notes

- WAV parser in this demo expects a standard 44-byte PCM WAV header.
- `record` output is raw PCM without WAV header.
- `record-wav` writes a standard PCM WAV header and updates data length after recording.

## Troubleshooting (Raspberry Pi: `snd_pcm_open` / Unknown error 524)

If playback fails with `Unknown error 524` (or similar) when using `hw:0,0`, this build automatically retries `plughw:0,0` and then `default`. You can also pick the device explicitly:

```bash
./bin/alsa_tool play input.wav plughw:0,0
./bin/alsa_tool play input.wav default
```

If audio still routes to the wrong jack (HDMI vs headphone), set the default card in `/etc/asound.conf`, for example:

```
defaults.pcm.card 1
defaults.ctl.card 1
```

Then reboot (or reload ALSA) and try again. Use `aplay -l` / `arecord -l` to see card indices.
