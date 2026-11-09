# Contributing

## Building and testing

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/x52_tests              # unit tests, no device needed
./build/x52_tests mfd          # filter by test name
X52_TEST_HARDWARE=1 ./build/x52_it
```

The unit tests run on CI without a controller. Anything that changes protocol
encoding must come with a test that asserts the exact `(command, value)` pairs
against the recording transport.

## Code style

- C++20, 4 spaces, 100 columns (`.clang-format`, `.editorconfig`).
- SPDX header `// SPDX-License-Identifier: BSD-3-Clause` on every source file.
- Public headers in `include/x52`, implementation in `src/core`.
- No new runtime dependencies beyond libusb; GTK4/libadwaita only for the
  panel. JSON is vendored.

## Protocol changes

Read `docs/PROTOCOL.md` first. Two rules learned the hard way:

1. MFD appends need a gap between transfers on Logitech firmware; never remove
   the pacing in `Device::send`.
2. Verify on hardware before claiming success. A control transfer returning 0
   only means the packet was accepted by the USB stack.

Useful checks:

```bash
./build/x52probe info
./build/x52probe selftest
./build/x52ctl test
./build/x52ctl drift 5
```

## Pull requests

- One logical change per commit, present tense subject.
- Mention what you verified on hardware, and what you could not.
- Keep the panel usable with the daemon offline: it must degrade to a clear
  message, never to an empty window.
