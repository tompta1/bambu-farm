# Bambu Farm 🧑🏽‍🌾

Run your own LAN-first replacement for Bambu's proprietary networking plugin and keep Bambu Studio / Orca-style printer workflows usable without the vendor cloud.

Bambu Studio loads a plugin called `libbambu_networking.so` at runtime. The upstream binary is proprietary. This repository provides an open replacement plugin plus a local Rust sidecar server for the parts that are awkward or unsafe to implement directly inside the shared library.

## How does it work?

At startup, Bambu Studio looks for a plugin called `libbambu_networking.so`, `dlopen`s it and calls into it for networking functionality. By default, it will use the proprietary version installed from Bambu Labs' servers. I don't like that, so I wrote my own. It's a drop-in replacement, and there's a Makefile in this project that will symlink the build artifacts into `$HOME/.config/BambuStudio/plugins/*` to install the FOSS plugin. Bambu Studio will then use this version instead of its own.

Unfortunately, the C++ ecosystem is full of footguns, and dynamic linking is a sham, so you can't use OpenSSL in `libbambu_networking.so` or it'll segfault. This presents a lot of issues considering Bambu Labs' use of TLS for MQTT and FTP, so I extracted all of the command and control logic into a server process and use gRPC to communicate between `libbambu_networking.so` and the server. That gRPC link will eventually be able to use TLS with `rustls` and `ring`, dodging the OpenSSL difficulties. A side-effect of this separation between client and server is that this makes the architecture scale to arbitrarily large print farms and should allow communication between clients and printers on different networks, as well as allowing the implementation of fine-grained access controls.

## What's the current status?

Current `main` is focused on Linux `aarch64` / ARM64 usage, especially Flatpak Bambu Studio and Orca-style plugin ABI compatibility.

What currently works in LAN mode:

- printer discovery and connect/disconnect
- printer host autodiscovery when `host` is omitted or stale in config
- printer status monitoring and live state updates
- local print submission, including larger streamed `.3mf` uploads
- AMS mapping forwarding for print jobs
- LAN login / logout UI compatibility
- reconnecting the last selected printer on Studio startup
- live camera feed in the device tab
- storage browsing through the LAN tunnel
- timelapse listing, thumbnails, and downloads
- model listing from the printer SD card
- Linux ARM release packaging in GitHub Actions

What is still limited or incomplete:

- cloud and MakerWorld features are intentionally unsupported; the plugin now routes the main browser-entry paths to explicit local notice pages instead of pretending they work
- storage support is aimed at the Studio paths exercised so far, not full parity with the vendor plugin
- timelapse and large file downloads work, but the tunnel/file APIs have only been hardened for the tested flows
- the codebase still has a lot of ABI-compatibility glue and global state that should be refactored
- the project is functional for LAN usage, but still not polished as a general consumer-ready replacement

If you need vendor-cloud workflows, account sync, or full MakerWorld support, this project does not provide that today.

## What do I need to build/run it?

You need, at minimum:

- Rust and Cargo
- GNU Make
- a C/C++ toolchain
- `protoc`
- `curl`
- OpenSSL development headers
- Boost development headers
- MessagePack C++ headers

On Debian/Ubuntu-like systems that usually means something close to:

```sh
sudo apt-get install -y \
  build-essential \
  curl \
  libboost-all-dev \
  libmsgpack-cxx-dev \
  libprotobuf-dev \
  libssl-dev \
  protobuf-compiler
```

## Local configuration

The server reads `bambu-farm-server/bambufarm.toml` when you run it from the repo, and that file is intentionally ignored by git. Start from `bambu-farm-server/bambufarm_example.toml` and fill in your own printer details locally.

`host`, `name`, and `model` are optional. The only fields that still need to be configured are `dev_id` and `password`.

If `host` is omitted, the server will try to discover the printer on local private IPv4 ranges using the configured `dev_id` and `password`. If `host` is set but stops working, the server will also try to rediscover it on reconnect.

If `name` or `model` are omitted, the server will try to infer them from the printer's MQTT `get_version` response and use that metadata in the device list.

## Building the plugin

Build the shared libraries:

```sh
make -C bambu-farm-client shared
```

That produces:

- `bambu-farm-client/target/debug/shared/libbambu_networking.so`
- `bambu-farm-client/target/debug/shared/libBambuSource.so`

Install them into the host and Flatpak plugin directories:

```sh
cd bambu-farm-client
./install-flatpak-plugin.sh install
```

The install helper resolves paths relative to the repo or a release bundle, and copies `bambufarm.toml` if present, otherwise `bambufarm_example.toml`.

## Running the server

Start the local server from the repo root with:

```sh
cargo run --manifest-path bambu-farm-server/Cargo.toml
```

In normal local development the client plugin talks to the server over localhost gRPC.

## Testing

Helper and server tests:

```sh
make -C bambu-farm-client test-helpers
cargo test --manifest-path bambu-farm-server/Cargo.toml
```

There is also an opt-in real-printer integration test for upload coverage. See the section below for the required environment variables.

## GitHub Actions and release artifacts

The repository includes an ARM workflow at `.github/workflows/arm-plugin.yml` that builds on GitHub-hosted `ubuntu-24.04-arm` runners, uploads a packaged artifact for each run, and publishes the `*.tar.gz` bundle on tags matching `v*`.

## Real printer integration test

There is an opt-in real-printer upload test in `bambu-farm-server/src/main.rs`. It is ignored by default and only runs when you explicitly provide printer credentials through environment variables.

Required environment variables:

- `BAMBU_FARM_RUN_PRINTER_TESTS=1`
- `BAMBU_FARM_TEST_PRINTER_DEV_ID`
- `BAMBU_FARM_TEST_PRINTER_HOST`
- `BAMBU_FARM_TEST_PRINTER_PASSWORD`

Optional environment variables:

- `BAMBU_FARM_TEST_PRINTER_NAME`
- `BAMBU_FARM_TEST_PRINTER_MODEL`

Run it with:

```sh
cargo test --manifest-path bambu-farm-server/Cargo.toml upload_file_stream_rpc_reaches_real_printer -- --ignored --nocapture
```

The test uploads a small unique text file through the real gRPC -> server -> FTPS path and then attempts to delete it from the printer.

## Known gaps / TODO

Highest-value follow-up work:

- reduce and simplify the ABI-compatibility layer in the client plugin
- replace remaining global/shared state with cleaner session-oriented code
- expand automated integration coverage beyond upload and storage paths
- document supported and unsupported Studio/Orca workflows more precisely
- continue converting remaining non-LAN partial stubs into explicit unsupported responses

Likely unsupported or intentionally out of scope for now:

- vendor cloud account features
- MakerWorld browsing, sync, and publishing
- full parity with the proprietary plugin across every UI surface
- non-Linux packaging and release support

## Get involved!

If a feature you need is missing open an issue to run it by me, but it's very likely I want that feature and will accept a PR for it. A lot of the cruft in this project exists for a reason, so open an issue before you try any major refactors, or try to switch around dependencies and stuff. There's a good chance I've already tried what you have in mind and lost a few hours of my life to it.

## Any caveats?

Be aware that once you install *any* networking plugin, Bambu Studio will assume that you've installed *theirs* and will "upgrade" it whenever you install a new version of Bambu Studio by replacing it with their proprietary version without your consent. I plan on opening a bug against them for that.

## Licensing information

This project is licensed under the AGPLv3 because it contains code from Bambu Studio.
