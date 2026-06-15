# ex2_rdma — Lab #2: Verbs API Throughput Benchmark

RDMA point-to-point unidirectional throughput benchmark using the Verbs API (libibverbs) on Mellanox Connect-X3 (56Gb) InfiniBand hardware.

## Structure

```
ex2_rdma/
├── PRD.md                  # Product requirements document
├── assignment.md           # Original lab assignment
├── bw_template.c           # Provided pingpong template (reference)
├── common.h                # Shared static inline functions (init, connect, OOB, post/poll)
├── convergence/            # Convergence detector (discovers per-size counts on hardware)
│   ├── client.c
│   ├── server.c
│   └── Makefile
├── submission/             # Final submission (hardcoded tables, clean output)
│   ├── client.c
│   ├── server.c
│   └── Makefile
└── Makefile                # Top-level: builds both convergence and submission
```

## Build

```bash
# Convergence detector (run on mlxstud01–04)
make convergence

# Submission build
make submission

# Or just
make
```

## Usage

```bash
# Server (on mlxstud01 or mlxstud03)
./server

# Client (on the paired node: mlxstud02 or mlxstud04)
./client <server-hostname>
```

Output: three tab-separated columns — `size`, `throughput_value`, `unit` — for 21 message sizes (1B to 1MB, powers of 2).

## Course Hardware

- Mellanox Connect-X3 (56Gb) NICs
- Pair 1: mlxstud01 + mlxstud02 (development)
- Pair 2: mlxstud03 + mlxstud04 (final testing)
