# Ruru — local speech-to-text service

Ruru (the morepork, New Zealand's listening owl) is a small resident daemon
that turns speech into text **entirely on this computer**. Audio never leaves
the machine. Any app on the box can use it over a Unix socket — AceVoiceTyper
does, and so can anything else that needs private local dictation.

| | |
|---|---|
| Model | **Underdog Bark 3.0 Clean** — 163 MB architecture, Q8_0 GGUF (706 MB), CC-BY-4.0 |
| Accuracy | 4.94% word error on the Open ASR Leaderboard's seven English sets |
| Language | **English only** |
| Speed | ~250–300 ms for 21 s of audio on an RTX 3060 (≈75× realtime) |
| Engine | [transcribe.cpp](https://github.com/handy-computer/transcribe.cpp) (`libtranscribe.so`), Vulkan backend |
| Footprint | ~64 MB engine + 706 MB model. About 200 MB resident RSS, 705 MiB in VRAM |

## Why it exists

AceVoiceTyper v11 sends recordings to xAI or Groq over HTTPS. Ruru gives it a
third option that needs no key, no network and no upload. It is a separate
service on purpose: one model load serves every app that wants it.

## Layout

Ruru is two small C programs and a Makefile. The engine and the model are
**not** in this repository — you install them once per machine.

| Piece | Path |
|---|---|
| Source | this repository |
| Daemon | `~/.local/share/ruru/rurud` |
| CLI client | `~/.local/share/ruru/ruru-cli` |
| Controller | `~/.local/bin/ruru` (→ `~/.local/share/ruru/ruru-ctl`) |
| Engine + backends | `~/.local/share/ruru/lib/` |
| Model | `~/.local/share/ruru/models/underdog-bark-3.0-clean-Q8_0.gguf` |
| Socket | `$XDG_RUNTIME_DIR/ruru.sock` (mode 600) |
| Service | `ruru.service` (systemd **user**) |
| Super-key launcher | `~/.local/share/applications/ruru.desktop` |

## Install

```bash
# 1. the engine: a transcribe.cpp release with libtranscribe.so
mkdir -p ~/.local/share/ruru/lib ~/.local/share/ruru/models
curl -L -o /tmp/tc.tar.gz https://github.com/handy-computer/transcribe.cpp/releases/download/v0.4.1/transcribe-native-0.4.1-linux-x86_64-cpu-vulkan.tar.gz
tar xzf /tmp/tc.tar.gz -C ~/.local/share/ruru/lib --strip-components=1

# 2. the model (706 MB) and its attribution
curl -L -o ~/.local/share/ruru/models/underdog-bark-3.0-clean-Q8_0.gguf \
  https://huggingface.co/joshthomasdev/underdog-bark-3.0-clean-gguf/resolve/main/underdog-bark-3.0-clean-Q8_0.gguf
curl -L -o ~/.local/share/ruru/models/NOTICE \
  https://huggingface.co/ConwayResearch/Underdog-Bark-3.0-Clean/raw/main/NOTICE

# 3. build and install ruru
make && make check
make install install-unit install-desktop
systemctl --user enable --now ruru
ruru status
```

`make check` loads the model and prints its identity — do that before enabling
the service. `make install` copies the binaries and links `~/.local/bin/ruru`;
it does **not** fetch the engine or the model.

Point `TRANSCRIBE_DIR` at a different transcribe.cpp release tree to rebuild
against another engine build, e.g. the CUDA one for an NVIDIA GPU:

```bash
make TRANSCRIBE_DIR=~/.local/share/ruru/lib-cuda install
```

## Using it

```
ruru status          service, model and backend state
ruru start|stop|restart
ruru logs [N]        last N journal lines
ruru ping            is it answering?
ruru wav FILE.wav    transcribe a 16 kHz mono 16-bit WAV (text on stdout)
ruru pcm FILE.f32    transcribe raw 16 kHz mono float32
ruru check           load the model and print its identity, then exit
```

Other apps call the socket directly. One line of JSON in, one line of JSON out:

```
-> {"cmd":"transcribe","samples":336000,"rate":16000}\n   followed by 336000 float32le samples
<- {"ok":true,"text":"…","samples":336000,"ms":293.0,"rtf":71.66,"energy":0.046,"model":"tdt-0.6b-bark-3.0-clean"}
```

Also `ping`, `status`, `shutdown`. The service is single-decoder (the engine
allows one decode in flight), so callers queue politely on its side. Input is
16 kHz mono float32, up to 10 minutes per request.

## Swap the model

Drop a different GGUF in `~/.local/share/ruru/models/`, edit `RURU_MODEL=` in
`~/.config/systemd/user/ruru.service`, then `systemctl --user restart ruru`.
`ruru check` confirms the identity before you commit.

Useful siblings (same family, all CC-BY-4.0):

| Model | Size | Notes |
|---|---|---|
| `underdog-bark-3.0-clean-Q8_0.gguf` | 706 MB | **current.** Drops "um", "uh", false starts and repeats — best for dictation |
| `underdog-bark-3.0-Q8_0.gguf` | 739 MB | Verbatim: writes every word including fillers |
| `underdog-bark-3.0-plus-*.gguf` | ~739 MB | Slightly more accurate, still 180 MB class |
| `underdog-bark-3.0-pro-*.gguf` | 1.25 GB+ | Most accurate of the family |

Community GGUF conversions (unofficial): `joshthomasdev/underdog-bark-3.0-*-gguf`,
`scholzmx/underdog-bark-3.0-*-gguf` on Hugging Face. Read their README for the
quantization layout.

## Limitations

- **English only.** For other languages use the cloud providers in AceVoiceTyper.
- **No name/jargon biasing.** xAI's `keyterm` has no local equivalent. Technical
  words can drift ("finfet" → "fin fit", "drapes" → "drips"). Hyphens are not
  written ("full-size" → "full size").
- **No word timestamps** yet (the engine can do them; this service does not ask).
- The model is 706 MB rather than 163 MB because that is Conway's packed format
  size; the GGUF is a quantization of the same weights.

## Credits

- **Underdog Bark 3.0 Clean** by [Conway Research](https://huggingface.co/ConwayResearch/Underdog-Bark-3.0-Clean), CC-BY-4.0.
- Derives from [nvidia/parakeet-tdt-0.6b-v3](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3) (NVIDIA) and
  [moondream/parakeet-ultra](https://huggingface.co/moondream/parakeet-ultra) (Moondream), both CC-BY-4.0.
- GGUF conversion by joshthomasdev, for [transcribe.cpp](https://github.com/handy-computer/transcribe.cpp).
- `include/transcribe.h` is transcribe.cpp's public C API header, vendored from
  release **v0.4.1** so this builds against a known ABI. It is transcribe.cpp's
  code, not Ruru's — see that project for its licence. The engine builds check a
  header hash; if you update the engine, update this header to match.
- Ruru itself is MIT. Keep the model's `NOTICE` alongside the weights — that is
  what CC-BY-4.0 asks for.
