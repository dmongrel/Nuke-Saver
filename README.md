# nuke-saver

A Windows screen saver, rendered in real time with Vulkan. An empty desert basin builds itself a
city. A countdown board over the rooftops runs out, and a missile comes in. The city is blown
apart into a storm of triangles that gathers back into a mushroom cloud, then lets go and rains
what is left over the ruins. Then the land is empty again and it starts over.

It is theatrical, not physical: a light show, not a model of anything real.

Source: https://github.com/dmongrel/Nuke-Saver

## Installation

Download `nuke-saver.scr` from the [latest release](https://github.com/dmongrel/Nuke-Saver/releases/latest),
right-click it and choose **Install**. You can also copy it into `C:\Windows\System32` and pick
**Nuke Saver** in Screen Saver Settings.

You only need the one file. The shaders, the preview image and the Vulkan loader are all built
in, and no other DLLs are needed. It runs on Windows 10 (1903 or later) and Windows 11, and needs
a graphics driver with Vulkan support (any current NVIDIA, AMD or Intel driver). Without Vulkan
it quietly falls back to a plain black screen saver.

## The cycle

One cycle runs about 80–115 seconds, then repeats with a new random city:

| Phase | Duration | What you see |
|---|---|---|
| Empty land | 5–8 s | A bare desert basin; the camera is already moving |
| Growth | 5–10 s | 500 buildings rise out of the ground, then the countdown board |
| Settle | 2–3 s | The finished city, still |
| Countdown | 5 s | The board lights up and counts `00:00:05` down to `00:00:00`, lighting the rooftops |
| Missile | 4–6 s | A missile comes in over the mountains, trailing a contrail |
| Flash | 0.3 s | The screen goes white |
| Blast | 4–6 s | A shock shell expands; every building it touches bursts into triangles |
| Scatter | 5 s | The triangles fly outward, tumble and skid to rest on the desert floor |
| Gather | 25–35 s | The pull reverses and the city's own triangles build a rolling mushroom cloud |
| Disperse | 15–25 s | The cloud lets go and rains back over the ruins; the embers cool and fall |
| Fade | 8 s | To black over the debris field |

The camera never stops. Each cycle it slowly circles the city, on one of four shots: a distant
ridge orbit, a low approach across the desert floor, a high oblique descent, or a rise from
street level. On more than one monitor, every monitor shows the same moment from its own camera.

## Settings

Open **Settings…** in Screen Saver Settings (or run the `.scr` with `/c`). The settings are saved
under `HKEY_CURRENT_USER\Software\nuke-saver`:

| Setting | Choices | Default |
|---|---|---|
| Quality | Automatic, Low, Medium, High | Automatic: steps down and back up to hold the frame rate |
| Time of day | Random each cycle, Morning, Noon, Twilight, Night | Twilight |
| Camera | Random each cycle, Distant ridge, Low approach, High oblique, Street level | Random each cycle |

The preview in Screen Saver Settings shows a still of the mushroom cloud rather than starting
Vulkan.

## Command-Line Arguments

| Argument | Description |
|----------|-------------|
| (none) / `/s` | Run full screen on every monitor |
| `/p <hwnd>` or `/p:<hwnd>` | Paint the preview still into the given window |
| `/c` or `/c:<hwnd>` | Open the Settings dialog |
| Any other argument | Exit immediately with code 0 |

Arguments are case-insensitive and accept both `/` and `-` as the prefix.

## Building

### Prerequisites (MSYS2 UCRT64)

- `g++`, `windres` and `make` (`mingw-w64-ucrt-x86_64-toolchain`)
- `glslc` (`mingw-w64-ucrt-x86_64-shaderc`)
- `xxd` (MSYS2 base)

### Commands

```bash
make            # build nuke-saver.scr
make selftest   # build and run the console toolchain probe
make clean      # remove everything generated
make install    # copy to %LOCALAPPDATA%\Nuke-Saver and select it as the screen saver
```

`make install` needs no administrator rights. It leaves the timeout and lock-on-resume settings
alone.

The design is in [`docs/Nuke-Saver-Spec.md`](docs/Nuke-Saver-Spec.md), and the build plan is in
[`docs/Nuke-Saver-Implementation-Plan.md`](docs/Nuke-Saver-Implementation-Plan.md).

## License

nuke-saver is released under the [MIT License](LICENSE). The vendored libraries keep their own
licences, both MIT: [volk](third_party/volk/LICENSE.md) (Arseny Kapoulkine) and the
[Vulkan Memory Allocator](third_party/vma/LICENSE.txt) (Advanced Micro Devices).
