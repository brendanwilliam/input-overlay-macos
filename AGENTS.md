# Repository Guidelines

## Project Structure & Module Organization

This repository builds the `input-overlay` OBS plugin and a separate headless input client. Plugin code is in `src/`: `gui/` contains Qt settings UI, `hook/` captures input, `network/` handles WebSocket traffic, and `util/` implements configuration and overlay elements. The client lives in `client/src/`. Shared and third-party dependencies are under `deps/`; avoid editing vendored code unless updating the dependency. Runtime web assets and translations are in `data/`, while `presets/` contains user-facing overlay layouts. CI and packaging scripts are in `.github/`.

## Build, Test, and Development Commands

Use CMake 3.25+ and an OBS development environment with `libobs`, the frontend API, and Qt 6 available.

- `cmake --preset macos && cmake --build --preset macos` configures and builds the macOS plugin.
- `cmake --preset ubuntu-x86_64 && cmake --build --preset ubuntu-x86_64` builds on Linux; use the matching platform preset from `CMakePresets.json`.
- `cmake -S client -B build_client && cmake --build build_client` builds the standalone client.
- `./format.sh` applies the repository formatter to C/C++, headers, and maintained web assets.

There is no dedicated unit-test suite. Treat a successful platform build and manual verification in OBS (or of the headless client) as the baseline; CI builds the supported platforms for pull requests.

## Coding Style & Naming Conventions

Follow `.clang-format`: four spaces, no tabs, 120-column limit, and right-aligned pointers. Run `./format.sh` before submitting C/C++ changes. Use lowercase snake_case for files, functions, and variables (for example, `websocket_server.cpp`); keep paired declarations and implementations as `.hpp`/`.cpp`. Preserve existing platform suffixes such as `_macos.mm`, `_win.cpp`, and `_linux.cpp`. Do not reorder includes automatically: the formatter deliberately preserves include order.

## Testing Guidelines

Exercise changed input paths, configuration persistence, and affected overlay presets manually. For changes to `data/` or `presets/`, load the preset in OBS and verify keyboard, mouse, and gamepad rendering. Validate network/client work on a trusted local network only: the client protocol is unencrypted.

## Commit & Pull Request Guidelines

Match the concise history format: `Area: Imperative summary` (for example, `Presets: Fix Switch Pro Controller` or `CI: Update buildspec`). Keep commits focused. Pull requests should explain the user-visible effect, link relevant issues, describe validation and platforms tested, and include screenshots or recordings for UI, overlay, or preset changes. Do not commit generated build directories, release artifacts, signing material, or local credentials.
