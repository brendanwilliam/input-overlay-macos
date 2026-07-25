![logo](./docs/io.png)

[![Push to master](https://github.com/univrsal/input-overlay/actions/workflows/push.yaml/badge.svg)](https://github.com/univrsal/input-overlay/actions/workflows/push.yaml)

Show keyboard, mouse and gamepad input on stream.\
Available for OBS Studio on Windows, Linux, and macOS (64bit).
Head over to [releases](https://github.com/univrsal/input-overlay/releases) for binaries.

## macOS

macOS builds target macOS 11.0 or newer and are universal (`arm64` and `x86_64`). Install the signed `.pkg` from a
release, then restart OBS. To capture local keyboard and mouse input, enable **OBS** under **System Settings → Privacy
& Security → Accessibility**, then restart OBS. The standalone `io_client` is distributed as a separate universal
archive for trusted local-network forwarding.

See the [macOS installation guide](docs/macos.md) for release installation, local builds, permissions, and
troubleshooting.

## [Wiki](https://github.com/univrsal/input-overlay/wiki)
## [Installation](https://github.com/univrsal/input-overlay/wiki/Installation)
## Credits
input-overlay depends on [libuiohook](https://github.com/kwhat/libuiohook) by [kwhat](https://github.com/kwhat) licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.txt), [mongoose](https://github.com/cesanta/mongoose) licensed under the [GNU General Public License v2.0](https://www.gnu.org/licenses/gpl-2.0.txt), [SDL2](https://libsdl.org) licensed under the [zlib license](https://www.zlib.net/zlib_license.html).

## More Information:
- [OBS resource page](https://obsproject.com/forum/resources/input-overlay.552/)
- [Config creation tool](https://univrsal.github.io/input-overlay/cct/)
- [Convert old *.ini presets to JSON (WIP)](https://univrsal.github.io/input-overlay/converter/)
