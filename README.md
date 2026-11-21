# Elgato Game Capture HD60 S on macOS

A userspace driver and OBS Studio plugin for the Elgato Game Capture HD60 S,
which Elgato does not support on Apple silicon Macs.

It is a port of Doug Brown's reverse-engineered
[Linux driver](https://github.com/dougg3/hd60s-linux-driver). The hardware
logic is his code, compiled unmodified; this project replaces the Linux kernel
underneath it with libusb and the V4L2/ALSA front end with a small capture
library. No kernel extension, driver extension or root access is involved.

## Status

Working, lightly tested.

Tested on one setup: an M1 MacBook Pro, macOS 27.3, OBS Studio 33.0.2,
libusb 1.0.27, and a first-generation HD60 S (USB ID `0fd9:005e`) fed 1080p60
over HDMI. On that setup:

- 1080p60 video at a steady 60 fps over isochronous USB, with no lost packets
- 48 kHz stereo audio
- start, stop and transport changes leave the device healthy
- the OBS source shows picture and sound

Not yet tried:

- the other hardware revisions (`0fd9:004f`, `0074` and the second-generation
  `0076`), although the Linux driver supports them
- resolutions other than 1080p60, and interlaced modes
- the source changing resolution, or the device being unplugged, mid-capture
- HDCP-protected sources
- sleep and wake while capturing
- Intel Macs

## Requirements

- macOS on Apple silicon, with the Xcode command line tools
- [Homebrew](https://brew.sh) libusb: `brew install libusb`
- OBS Studio in `/Applications`, for the plugin
- the HD60 S on a USB 3 port

## Build and test

The Linux driver is a git submodule, so clone with `--recursive`, or run
`git submodule update --init` in an existing clone.

```
make            # the capture library and the hd60s-grab tool
make check      # offline self-test; needs no device
```

`hd60s-grab` brings the card up, reports the HDMI signal, captures for a
moment and saves one frame:

```
./hd60s-grab -w 60 -t 5 -o frame.png
```

`-w` is how long to wait for a signal, `-t` how long to capture, `-b` selects
bulk transfers instead of isochronous, `-i` initialises without capturing and
`-v` turns on debug logging. `HD60S_TRACE=1` logs every USB control transfer.

The line `ready: /dev/video0` in the output comes from the unmodified Linux
code and means nothing here.

## OBS plugin

```
make install-obs
```

This builds the plugin and copies it to
`~/Library/Application Support/obs-studio/plugins/`. The first build downloads
the libobs headers matching the installed OBS, and the SIMDe headers they
need, from GitHub into `build/`. Restart OBS, then add the **Elgato HD60 S**
source. `make uninstall-obs` removes it.

The source has two settings: the audio input (HDMI or the 3.5 mm jack) and a
switch to use bulk USB transfers, which is a fallback if the picture stutters.

Only one source can own the card, and `hd60s-grab` cannot run while OBS is
using it. To use the card in other applications, start OBS's virtual camera.

The plugin is built against one OBS version and ad-hoc signed, so rebuild it
after a major OBS update.

`make check-obs` runs the plugin's source against the real card without OBS.
It needs a live HDMI source.

## How it is put together

| Path | What it is |
|---|---|
| `upstream/` | The Linux driver, as a git submodule pinned to the commit this was tested with. |
| `shim/` | Stub `<linux/...>` and `<media/...>` headers, all leading to `kshim.h`. |
| `mac/kshim.c` | The kernel services those headers promise: logging, work queues, control transfers over libusb. |
| `mac/video.c` | The USB transfer ring and the capture API; replaces `hd60s-video.c` and `hd60s-audio.c`. |
| `mac/hd60s-mac.h` | The capture library's public interface. |
| `mac/grab.c` | The `hd60s-grab` tool. |
| `obs/` | The OBS source plugin. |
| `tests/` | The offline self-test and the plugin harness. |

`hd60s-core.c`, `hd60s-mstar.c`, `hd60s-rev4.c`, `hd60s-modes.c` and
`hd60s-parse.c` are compiled straight from `upstream/`. To pick up upstream
fixes, check out the newer commit in `upstream/`, rebuild, run the tests, and
commit the new submodule pointer. Changes to `hd60s-video.c` have to be
carried into `mac/video.c` by hand.

If the device ever stops answering after a failed capture, unplug it and plug
it back in. The Linux driver documents that a badly ordered stream stop can
hang the device's USB controller until it is power-cycled.

## Credits and licence

All of the reverse engineering is Doug Brown's; see
[his repository](https://github.com/dougg3/hd60s-linux-driver) for his notes
and credits.

GPL-2.0, the same as the Linux driver. See [LICENSE](LICENSE).
