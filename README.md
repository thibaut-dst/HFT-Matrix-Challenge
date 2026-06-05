# HFT Matrix Challenge

A small low-latency systems project built around a simple idea: a server streams matrix-multiplication challenges, and the client must parse, compute, and answer as fast as possible.

The challenge message format is:

```text
challenge_id
N
A (N*N integers)
B (N*N integers)
```

The client replies with:

```text
challenge_id checksum
```

where `checksum = sum(A * B) mod 997`.

## What is in the repo

- `hftserver2026/`: reference server for local testing
- `tools/blast_server.cpp`: stress server with configurable rate, matrix size, window, and burst modes
- `hftclient2026/main.cpp`: current optimized client
- `hftclient2026/main_v1.cpp`, `main_v2.cpp`, `main_v3.cpp`: earlier client iterations kept for comparison

## Current client

The current client focuses on low latency under load:

- streaming parser over a circular buffer
- flat matrix storage
- parallel compute workers
- dynamic tile scheduling
- cache-friendly tiled multiplication
- deferred modulo to remove `% 997` from the inner hot loop
- macOS compute-thread QoS
- socket tuning with `TCP_NODELAY` and larger receive buffers
- built-in benchmark mode

In local testing, the biggest performance win came from deferred modulo, followed by better worker scheduling and tile sizing.

## Version progression

- `v1`: first correct client, fixed TCP stream parsing and basic checksum computation
- `v2`: introduced a receive/parser/compute pipeline and worker threads
- `v3`: improved compute with tiling, cache-friendly loop order, and benchmark instrumentation
- `v4` (`main.cpp`): benchmark-driven tuning of worker count, tile size, ring buffer size, deferred modulo, prefetching, and macOS scheduling hints

## Build and run

Build with CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Start the reference server:

```bash
./build/bin/hftserver2026
```

Run the optimized client:

```bash
./build/bin/hftclient2026 127.0.0.1 12345 TeamA
```

Run with benchmark summary:

```bash
./build/bin/hftclient2026 127.0.0.1 12345 TeamA --benchmark
```

## Stress testing

Example blast-server run:

```bash
./build/bin/blast_server --rate 200 --window 10 --size 128 --mode 1
```

The current client was tested against:

- high message rates
- burst / overlap traffic
- `64x64`, `128x128`, `256x256`, and `512x512` matrices

and remained correct and stable in those local runs.
