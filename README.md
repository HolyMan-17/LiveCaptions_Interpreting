# Live Captions

![Screenshot of the application](https://github.com/abb128/LiveCaptions/blob/main/screenshot.png?raw=true)

Live Captions provides live captioning for the Linux desktop, entirely locally.

This fork of [abb128/LiveCaptions](https://github.com/abb128/LiveCaptions) adds:

* A persistent, scrollable transcript instead of fading caption lines
* A resizable, draggable caption window
* An additional [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) ASR engine with support for streaming zipformer and NVIDIA Nemotron models
* sherpa-onnx model management in the settings

Only the English language is supported currently. Other languages may produce gibberish or a bad phonetic translation.

## Features

* Simple interface
* Caption desktop or microphone audio locally; audio is never sent anywhere
* Does not rely on any proprietary services/libraries
* Persistent transcript of the entire session, with scrolling
* Auto-scroll follows new captions; scrolling up pauses it, scrolling back to the bottom resumes it
* Clear the transcript with `Ctrl+K` or copy it with `Ctrl+Shift+C` (also available from the menu and side buttons)
* Click and drag anywhere on the caption text to move the window; resize from the window edges
* Optional always-on-top
* Adjust font, font size, text casing, line count, and line width
* Optional token-level confidence text fading
* Two ASR engines: april-asr (default) and sherpa-onnx
* Load any compatible streaming transducer model directory for sherpa-onnx from the settings

Running this requires a somewhat-decent CPU that can perform realtime captioning, especially if you want to be doing other tasks (such as video decode) while running Live Captions. It has been tested working on:

* Intel i7-2670QM (2011)
* Intel i7-7820HQ (2017)
* Intel i5-8265U (2018)
* AMD Ryzen 5 1600 (2017)
* Steam Deck

GPU is not required or used.

## Usage

The app starts captioning system audio when launched. Microphone captioning can be toggled from the menu.

The transcript keeps the whole session. New text is followed automatically; if you scroll up to read something, auto-scrolling pauses and resumes when you return to the bottom.

| Shortcut | Action |
| --- | --- |
| `Ctrl+K` | Clear the transcript |
| `Ctrl+Shift+C` | Copy the transcript |
| `Ctrl+,` | Preferences |
| `Ctrl+Q` | Quit |

## Accuracy

The live captions may not be accurate. It may make mistakes, including when it comes to numbers. Please do not rely on the results for anything critical or important.

## ASR engines and models

### april-asr (default engine)

The default engine is [aprilasr](https://github.com/abb128/april-asr). It requires an april model file, which can be downloaded and pointed to with `APRIL_MODEL_PATH`:

```
$ wget https://april.sapples.net/april-english-dev-01110_en.april
$ export APRIL_MODEL_PATH=`pwd`/april-english-dev-01110_en.april
```

### sherpa-onnx (recommended for accuracy)

The sherpa-onnx engine loads streaming transducer model directories. A model directory must contain `tokens.txt` and encoder/decoder/joiner `.onnx` files (fp32 or int8; int8 is preferred when both are present).

Tested models:

| Model | Size (int8) | Notes |
| --- | --- | --- |
| `sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25` | ~623 MB | Recommended. Best accuracy; outputs punctuation and capitalization. Needs roughly 1 GB of RAM while running. |
| `sherpa-onnx-streaming-zipformer-en-2023-06-26` | ~68 MB | Fast and lightweight. |
| `sherpa-onnx-streaming-zipformer-en-2023-06-21` | ~179 MB | Trained on LibriSpeech + GigaSpeech. |

Model downloads are available from the [sherpa-onnx ASR models releases](https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models).

### Installing the Nemotron model

Download and extract the model used by this fork's default setup:

```
$ mkdir -p models
$ cd models
$ wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25.tar.bz2
$ tar xf sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25.tar.bz2
$ rm sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25.tar.bz2
```

The extracted directory is `models/sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25`.

Select it in the app under **Preferences → Engine: sherpa-onnx → Load Model**, and choose the model directory.

Alternatively, configure it from the terminal before the first launch (use absolute paths):

```
$ gsettings set net.sapples.LiveCaptions sherpa-installed-models "['/absolute/path/to/models/sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25']"
$ gsettings set net.sapples.LiveCaptions sherpa-active-model '/absolute/path/to/models/sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25'
$ gsettings set net.sapples.LiveCaptions active-engine 'sherpa-onnx'
```

Note: the app loads the configured engine at startup. If you do not have an april model, configure sherpa-onnx with the commands above (or set `APRIL_MODEL_PATH`) before launching the app for the first time.

Other Nemotron variants with smaller chunk sizes (80 ms, 160 ms, 1120 ms) are also available from the same releases page. Larger chunk sizes trade latency for accuracy; 560 ms is a good balance.

## Building

### Dependencies

* Linux with PulseAudio
* GTK 4 and libadwaita >= 1.4
* meson, ninja, a C compiler, pkg-config, git, wget

On Debian/Ubuntu, the required packages can be installed with:

```
$ sudo apt install build-essential pkg-config meson ninja-build libgtk-4-dev libadwaita-1-dev libpulse-dev git wget
```

### ONNX Runtime

Download [ONNX Runtime v1.14.1 (Linux x64)](https://github.com/microsoft/onnxruntime/releases/download/v1.14.1/onnxruntime-linux-x64-1.14.1.tgz), extract it somewhere, and export the paths used by both ASR engines:

```
$ export ONNX_ROOT=/path/to/onnxruntime-linux-x64-1.14.1
$ export SHERPA_ONNXRUNTIME_INCLUDE_DIR="$ONNX_ROOT/include"
$ export SHERPA_ONNXRUNTIME_LIB_DIR="$ONNX_ROOT/lib"
$ export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$ONNX_ROOT/lib"
```

### Clone, configure, build and run

The april-asr dependency is a git submodule, so clone recursively:

```
$ git clone --recursive https://github.com/HolyMan-17/LiveCaptions_Interpreting.git
$ cd LiveCaptions_Interpreting
```

The first Meson configure downloads the sherpa-onnx subproject (via its Meson wrap file), so an internet connection is needed once:

```
$ meson setup builddir --buildtype release -Donnxruntime_libdir="$ONNX_ROOT/lib"
$ meson devenv -C builddir
$ ninja
$ src/livecaptions
```

Notes:

* `-Donnxruntime_libdir` embeds an rpath so the app finds `libonnxruntime.so` at runtime without relying on `LD_LIBRARY_PATH`.
* `meson devenv` sets `GSETTINGS_SCHEMA_DIR` so the app and `gsettings` use the schema from the build directory. When running the app outside `meson devenv`, install the schema with `meson install` first.
* Downloaded models can be placed in `models/`, which is ignored by git.

## Model credits

* april model: thanks to Fangjun Kuang for the [pretrained model](https://huggingface.co/csukuangfj/icefall-asr-librispeech-lstm-transducer-stateless2-2022-09-03/tree/main), and to the [icefall](https://github.com/k2-fsa/icefall) contributors for the model recipes.
* sherpa-onnx models: thanks to the [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) and icefall contributors.
* Nemotron streaming ASR model: thanks to NVIDIA.
