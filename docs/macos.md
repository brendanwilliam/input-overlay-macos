# Installing Input Overlay on macOS

Input Overlay supports macOS 11 (Big Sur) and newer. Builds are universal, so the same plugin works on Apple Silicon
and Intel Macs. The current baseline is OBS Studio 31.1.1.

## Install a release

1. Download the macOS universal `.pkg` from this repository's [Releases](https://github.com/brendanwilliam/input-overlay-macos/releases) page.
2. Quit OBS completely.
3. Open the downloaded package and complete the installer.
4. Start OBS and open **Tools → input-overlay settings**.

If macOS blocks the installer, open **System Settings → Privacy & Security** and choose **Open Anyway** for the
downloaded package. Only install release files downloaded from this repository.

## Allow keyboard and mouse capture

macOS protects global keyboard and mouse capture. To enable local input display:

1. Open **System Settings → Privacy & Security → Accessibility**.
2. Enable the OBS application you use, normally `/Applications/OBS.app`.
3. Quit and reopen OBS.
4. In **Tools → input-overlay settings**, enable the mouse and keyboard hook.

Input Overlay runs inside OBS, so grant permission to **OBS**, not the `.plugin` bundle. Gamepad capture does not use
this permission. If key or mouse input is not shown, confirm that the correct OBS application is enabled and restart
OBS after changing the permission.

## Build from source

Install the current Xcode command-line tools and [Homebrew](https://brew.sh), then clone this repository with its
submodules:

```zsh
git clone --recurse-submodules https://github.com/brendanwilliam/input-overlay-macos.git
cd input-overlay-macos
brew bundle --file .github/scripts/.Brewfile
cmake --preset macos
cmake --build --preset macos
cmake --install build_macos --config Release
```

The last command installs `input-overlay.plugin` into:

```text
~/Library/Application Support/obs-studio/plugins
```

The install step automatically applies an ad-hoc signature so OBS can load a local build. Published releases are
signed for distribution.

Restart OBS and follow the Accessibility steps above. The first configure can take a while because it downloads and
builds the OBS development dependencies.

## Add an overlay in OBS

Add an **Input Overlay** source to a scene, then open its properties. Use the **Built-in preset** dropdown to select a
keyboard, mouse, or controller layout; it automatically applies the matching texture and layout files. The file fields
remain available for custom layouts.

### Procedural keyboard style

Enable **Use procedural keyboard style** in the same source properties to render a live WASD and arrow-key overlay
without image assets. Adjust the key shape, size, gap, corner radius, font size, and idle, pressed, border, and text
colors directly in OBS. This mode uses the local Input Overlay hook and requires the Accessibility permission above.

To remove a source or release installation, quit OBS and delete:

```text
~/Library/Application Support/obs-studio/plugins/input-overlay.plugin
```

## Standalone input client

The `io_client` executable forwards keyboard, mouse, and gamepad events to an Input Overlay WebSocket server. It is
for trusted local networks only; the protocol is unencrypted.

Build it from the repository root with:

```zsh
cmake -S client -B build_client_macos -DCMAKE_BUILD_TYPE=Release
cmake --build build_client_macos
./build_client_macos/io_client --help
```

Grant Accessibility permission to the terminal application that launches `io_client`, then consult the
[client README](../client/README.md) for connection options and usage.

## Troubleshooting

- **The plugin is missing from OBS:** confirm that `input-overlay.plugin` is directly inside the plugins directory
  above, then restart OBS.
- **Keyboard or mouse input does not appear:** enable Accessibility for the OBS app and restart it. Check the OBS log
  for `Accessibility permission` messages.
- **The installer or local build will not open:** make sure macOS is 11 or newer and OBS meets the documented
  baseline. A local build is ad-hoc signed; only published releases are intended for normal distribution.
- **Wrong architecture:** the release/plugin should contain both `arm64` and `x86_64`. Verify a local artifact with
  `lipo path/to/input-overlay -archs`.
