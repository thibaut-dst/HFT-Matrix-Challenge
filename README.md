# HFT Matrix Challenge Notes

This repo contains a matrix-challenge server and several client versions. The client receives:

```text
challenge_id
N
A flattened as N*N integers
B flattened as N*N integers
```

It must answer:

```text
challenge_id checksum
```

where `checksum = sum(A * B) mod 997`.

## Build

For the normal CMake build:

```bash
cmake --build build -j8
```

For a clean release build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

For manual aggressive client testing:

```bash
c++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -funroll-loops hftclient2026/main.cpp -o build/bin/hftclient_v4_test
```

## Run

Start the basic server:

```bash
./build/bin/hftserver2026
```

Run the current optimized client:

```bash
./build/bin/hftclient_v4_test 127.0.0.1 12345 TeamA
```

Run with benchmark summary:

```bash
./build/bin/hftclient_v4_test 127.0.0.1 12345 TeamA --benchmark
```

## Benchmark Fields

`total_latency_ms` is the sum of measured first-byte-to-response latency across completed challenges.

`avg_latency_ms` is average latency per challenge, from first byte received to response fully sent.

`parse_wall_ms` is from first byte of a challenge to parser completion. It can include receive/scheduling effects.

`parse_cpu_ms` is only time spent inside the parser code.

`compute_ms` is from parse completion until all compute workers finish.

`send_ms` is time spent sending the response.

`ring_writer_waits` counts how often the receive thread found the ring buffer full.

`ring_reader_waits` counts how often the parser thread found the ring buffer empty.

## Client Evolution

### V1: Correctness First

File: `hftclient2026/main_v1.cpp`

V1 made the client actually solve the challenge:

- Parses the stream safely instead of assuming one `recv()` equals one challenge.
- Stores `A` and `B`.
- Multiplies matrices.
- Computes `sum(C) mod 997`.
- Sends `challenge_id checksum`.

Important fix: TCP is a byte stream, so partial and combined messages are normal. V1 introduced persistent buffering so the parser only solves complete challenges.

### V2: Pipeline and Worker Threads

File: `hftclient2026/main_v2.cpp`

V2 split the work into stages:

- One receive thread.
- One parser thread.
- A compute worker pool.
- A lock-free-ish SPSC ring buffer between receive and parse.
- Flat arrays instead of `vector<vector<int>>`.
- No `C` matrix; each worker accumulates a partial checksum.

This reduced memory traffic and made compute parallel. It also exposed a real tradeoff: more threads can help compute, but coordination and busy-waiting can add latency.

### V3: Tiled Parallel Compute

File: `hftclient2026/main_v3.cpp`

V3 focused on compute efficiency:

- Cache-friendly `i, k, j` loop order.
- Tiling/blocking for better cache locality.
- Per-worker partial sums.
- Benchmark mode with stage-level timing.
- Separate `parse_wall_ms` and `parse_cpu_ms` so we can tell actual parser cost from receive/scheduling delay.

This version showed compute was the main stage to optimize after parsing became cheap.

### V4: Measured Low-Latency Tuning

File: `hftclient2026/main.cpp`

V4 is the current optimized version. Each candidate was tested by compiling, starting the local server, running the client with `--benchmark`, and keeping only changes that improved or made sense with low risk.

Kept optimizations:

- macOS compute-worker QoS using `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`.
- Dynamic tile claiming with `next_tile.fetch_add(...)`, so faster cores can pick up more work.
- `8` compute workers.
- `64x64` compute tiles.
- Deferred modulo: accumulate products in `int64_t` and apply `% 997` once per worker partial, instead of inside the innermost loop.
- Prefetch the next `B` row inside the compute tile.
- Fixed stack response buffer instead of constructing a response `std::string` every challenge.
- `TCP_NODELAY` and larger socket receive buffer.

Rejected after testing:

- `8x8` tiles: too much tile-claim overhead.
- `128x128` tiles: not enough parallelism.
- `4` workers: slower end-to-end.
- `9` workers: slightly lower compute in one run, but worse total latency than `8`.
- Replacing `this_thread::yield()` with CPU spin hints: much worse.

Best measured local result during v4 tuning:

```text
avg_latency_ms ~= 0.250
compute_ms ~= 0.075
parse_cpu_ms ~= 0.160
send_ms ~= 0.013
```

Baseline before v4 aggressive tuning was approximately:

```text
avg_latency_ms ~= 0.710
compute_ms ~= 0.529
```

The biggest single win was deferred modulo. Removing `% 997` from the inner multiply loop dropped compute time dramatically.

## Notes on Skipped Optimizations

Some HFT-style optimizations are not practical or useful in this repo:

- Kernel bypass: outside project scope.
- Interrupt tuning: OS-level and not portable here.
- Page-size changes: not useful for this small client.
- Memory-mapped logging: we avoid hot-path logging instead.
- Linux affinity/priority: not applicable on macOS.
- Cable/fiber/physical network tuning: irrelevant for localhost tests.
- Disabling CPU power saving or hyperthreading: machine-level setting, not client code.

The relevant code-level ideas are already used or tested: circular buffers, fewer allocations, lock-free-ish handoff, cache-friendly layout, worker scheduling, prefetching, deferred runtime work, and benchmark-driven iteration.
