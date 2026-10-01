# Wayland + Vulkan GPU stress

A small native Wayland client in C, using Vulkan 1.0 and xdg-shell directly.
No window toolkit, textures, vertex buffers, or runtime shader files.

Each frame draws a fullscreen triangle with a long, dependent fragment-shader
math loop. Increase the iteration count or the number of blended draws to make
**GPU rendering** take longer than a refresh interval. Every draw contributes
to the final image. A soft teal circle moves across a muted background, making
slow client updates visible. The expensive loop adds a small, smooth color tint.

## Build

Dependencies: a C compiler, Make, pkg-config, Vulkan headers/loader,
wayland-client, wayland-protocols, wayland-scanner, and glslangValidator.

For example, on Fedora:

```sh
sudo dnf install gcc make pkgconf-pkg-config vulkan-headers vulkan-loader-devel \
    wayland-devel wayland-protocols-devel glslang
make
```

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libvulkan-dev libwayland-dev \
    wayland-protocols glslang-tools
make
```

The build generates xdg-shell bindings and embeds the compiled SPIR-V shaders
in the executable. Run it from any directory inside a Wayland session.

## Run and tune

```sh
./gpu-stress
./gpu-stress --iterations 4096 --hz 60
./gpu-stress --iterations 4096 --passes 2 --hz 144
```

The terminal reports GPU timestamp durations approximately once a second:

```text
23.5 frames/s | GPU avg 42.10 ms, max 44.20 ms | 24/24 above 16.67 ms (60.0 Hz)
```

This line is illustrative; actual times depend on your GPU, driver, and window
size. Double `--iterations` until the reported GPU time exceeds your refresh
interval, then adjust to taste. `--passes` adds more draws; resizing the window
also changes the load. A useful starting target is 30–50 ms on a 60 Hz display.
`--hz` only sets the comparison threshold; it does not change the monitor mode
or throttle rendering. At 144 Hz the interval is 6.94 ms.

Use `--device N` to select a listed Vulkan device, ideally the GPU driving your
compositor. A software renderer does not exercise physical GPU contention.
`--frames N` permits a bounded run. `--fifo` forces FIFO presentation; the
default prefers MAILBOX and falls back to FIFO. `--help` lists all options.
Close the window or press Ctrl-C in its terminal to stop.

## What this tests

The motivation is [Ensuring steady frame rates with GPU-intensive clients](https://blogs.gnome.org/shell-dev/2023/03/30/ensuring-steady-frame-rates-with-gpu-intensive-clients/).
Mutter's transaction handling, introduced in GNOME 44, lets it retain the
previous ready buffer while a client's new buffer is still being rendered.

The important ordering here is:

```text
acquire image → submit expensive graphics → queue presentation → wait for rendering
```

There is one graphics submission in flight, keeping queued GPU work bounded.
The program does not wait for rendering before calling `vkQueuePresentKHR`.
Vulkan WSI handles Wayland buffer commits and synchronization; precisely when
it commits the buffer depends on the driver. Calling presentation early does
not guarantee that every WSI implementation exposes an unfinished buffer to
the compositor. MAILBOX avoids a FIFO presentation queue, but Wayland does not
guarantee tearing or a completely unthrottled presentation path.

Keep the window visible and move another window, open the overview, or watch a
separate smooth animation. The stress client should visibly update slowly;
observe whether the compositor keeps updating smoothly. A cursor alone is
insufficient evidence because it may use a hardware plane. Keeping another
window overlapping the client also helps exercise composition rather than
direct scanout. For a consistent refresh interval, disable VRR if enabled.

This is a workload generator, not an automatic pass/fail compositor test. GPU
timestamps measure elapsed GPU time between rendering markers, including
scheduling delays, not compositor frame delivery. Compositor responsiveness
also depends on GPU scheduling and preemption, as the article discusses.
