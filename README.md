# ex2_rdma — Lab #2: Verbs API Throughput Benchmark

RDMA point-to-point unidirectional throughput benchmark using the Verbs API (libibverbs) on Mellanox Connect-X3 (56Gb) InfiniBand hardware.

## Structure

```
ex2_rdma/
├── PRD.md                  # Product requirements document
├── README.md
├── assignment.md           # Original lab assignment
├── bw_template.c           # Provided pingpong template (reference, unmodified)
├── common.h                # Shared static inline functions (init, connect, OOB, post/poll)
├── server.c                # Submission server (21-iteration loop)
├── test_compile.c          # Compile-time verification for common.h API
├── test_server_compile.c   # Compile-time verification for server.c symbols
├── convergence/            # Convergence detector (discovers per-size counts on hardware)
│   ├── client.c
│   ├── server.c
│   └── Makefile
├── submission/             # Final submission (hardcoded tables, clean output)
│   ├── client.c
│   └── Makefile            # Builds server from ../server.c, produces archive
└── Makefile                # Top-level: builds both convergence and submission
```

## Dependencies

```bash
sudo apt install libibverbs-dev gcc make
```

- **Build**: works on any Linux with `libibverbs-dev` (including WSL2)
- **Run**: requires RDMA hardware (Mellanox Connect-X3 or newer) or SoftRoCE (`modprobe rdma_rxe`)

## Two-Phase Workflow

### Phase 1 — Convergence Detection (on course hardware)

Run on the development pair to discover optimal per-size parameters:

```bash
make convergence

# On mlxstud01 (server):
./convergence/server

# On mlxstud02 (client):
./convergence/client mlxstud01
```

The client converges both `WARMUP_COUNTS[]` and `MSG_COUNTS[]` for each of 21 sizes
(1B to 1MB, powers of 2) by doubling counts until throughput variance drops below 1%.
Output is copy-pasteable C array initializers.

### Phase 2 — Submission Benchmark

1. Copy the converged arrays into `submission/client.c`
2. Build and run on the testing pair:

```bash
make submission

# On mlxstud03 (server):
./submission/server

# On mlxstud04 (client):
./submission/client mlxstud03
```

Output: 21 tab-separated lines — `size`, `throughput_value`, `unit` (bps/Kbps/Mbps/Gbps, SI base-1000).

### Archive for Submission

```bash
cd submission && make archive
```

Produces `<id1>_<id2>.tgz` containing `Makefile`, `server.c`, `client.c`, `common.h`.
Extract and `make` builds cleanly.

## Protocol

- **Data**: Client → Server via RDMA WRITE (server is passive, no CPU involvement)
- **Sync**: Client sends `IBV_WR_SEND` "done" → Server replies `IBV_WR_SEND` "ACK"
- **Timer**: `clock_gettime(CLOCK_MONOTONIC)` wraps the timed batch + sync round-trip
- **Inline**: `IBV_SEND_INLINE` when `size ≤ max_inline_data` (device cap, queried at init)
- **Signaling**: Only the last WR per batch is signaled (serial ordering guarantees prior WRs)
- **Posting**: WRs are chained via `.next` and posted with a single doorbell per batch
- **OOB**: TCP socket exchanges QPN, LID, PSN, GID, buffer address, and rkey

## Build

```bash
make              # builds submission binaries
make convergence # builds convergence binaries
make clean       # removes all binaries and archives
```

Flags: `-O3 -march=native -funroll-loops -DNDEBUG`. Link: `-libverbs -lm`.

## Course Hardware

- Mellanox Connect-X3 (56Gb) NICs
- Pair 1: mlxstud01 + mlxstud02 (development — convergence detection)
- Pair 2: mlxstud03 + mlxstud04 (testing — final benchmark)
- Port: 12345 (hardcoded)
