# PRD: Lab #2 — Verbs API Throughput Benchmark

## Problem Statement

The student must transform the provided RDMA pingpong template (`bw_template.c`) into a **unidirectional throughput benchmark** using the Verbs API (libibverbs). The benchmark must measure point-to-point throughput across 21 message sizes (1B to 1MB, powers of 2) on Mellanox Connect-X3 (56Gb) hardware, producing output in the same tab-separated format as the student's Exercise 1 TCP benchmark. The student needs the highest possible throughput numbers to compete for the +5 bonus points.

## Solution

A two-phase approach:

1. **Convergence detector** — runs on the course hardware (mlxstud01–04) to empirically determine per-size warmup counts and message counts where throughput variance drops below 1%. Emits the discovered tables for hardcoding.

2. **Submission benchmark** — clean, optimized code with hardcoded tables. Produces exactly three tab-separated columns (`size`, `throughput_value`, `unit`) per line, no debug output. Built via `make`, producing `server` and `client` executables.

**Protocol:** Client sends data via RDMA WRITE (server passive), then signals completion via IBV_WR_SEND. Server acknowledges with IBV_WR_SEND. Timer on client wraps the entire timed batch plus the ACK round-trip.

## User Stories

1. As a student, I want to reuse the same output format as Exercise 1 (three tab-separated columns), so the auto-tester can validate my results without modification.
2. As a student, I want the benchmark to measure true unidirectional throughput, so the numbers reflect real RDMA hardware performance rather than software bottlenecks.
3. As a student, I want to empirically determine optimal warmup and message counts per size on the actual course hardware, so the submission uses data-driven parameters rather than guesses.
4. As a student, I want to hardcode the converged counts into a separate submission file, so the submitted code is deterministic and doesn't include measurement logic.
5. As a student, I want separate `server` and `client` binaries (not a symlink), so each binary contains only its own code path with no runtime dispatch overhead.
6. As a student, I want all shared code in a `static inline` header, so the compiler can inline aggressively without LTO dependencies.
7. As a student, I want to use IBV_SEND_INLINE for messages ≤ the device's max_inline_data limit, so small messages avoid an extra PCIe DMA read.
8. As a student, I want to signal only the last WR in each batch, so completion-queue polling overhead is minimized.
9. As a student, I want to query the device's maximum queue sizes and use them, so the SQ can hold the maximum number of outstanding WRs.
10. As a student, I want a sliding-window post/drain strategy, so the SQ never drains empty and throughput stays at line rate.
11. As a student, I want to use `clock_gettime(CLOCK_MONOTONIC)` for timing, so measurements have nanosecond resolution and are immune to NTP adjustments.
12. As a student, I want to compile with `-O3 -march=native -funroll-loops`, so the compiler applies the most aggressive optimizations for the course hardware.
13. As a student, I want the Makefile to build both convergence and submission targets, so I can run discovery and then submit without manual steps.
14. As a student, I want the server's memory region address and rkey exchanged over the existing TCP OOB channel, so the client can construct valid RDMA WRITE work requests.
15. As a student, I want a single 1MB registered memory region on each side, so MR registration overhead is paid once regardless of message size.

## Implementation Decisions

### Architecture

- Three shared `static inline` functions live in `common.h`: context initialization, QP connection (TCP OOB exchange), work request posting, and completion polling. Both `server.c` and `client.c` include this header, each compiling to a standalone binary with no runtime server/client dispatch.
- The convergence detector lives in separate files (`convergence/client.c`, `convergence/server.c`) and is not submitted — only its output (the hardcoded count tables) goes into the submission.

### Protocol

- **Data phase:** Client posts RDMA WRITEs to the server's registered buffer. Server is entirely passive — no recv completions generated for data.
- **Sync phase:** After the last RDMA WRITE completes, client posts one IBV_WR_SEND ("done"). Server has a pre-posted recv for this. When it completes, server posts one IBV_WR_SEND ("ACK") back to client.
- **Timer scope:** `clock_gettime` start before first RDMA WRITE in the timed batch; `clock_gettime` stop after the ACK recv completion arrives on client. This measures the full end-to-end: data transfer + server notification + ACK return.

### OOB Exchange Extension

The template's TCP OOB exchange format is extended to include two additional fields:
- Server's registered buffer address (`uint64_t`, hex-encoded)
- Server's MR remote key (`uint32_t`, hex-encoded)

The `pingpong_dest` struct gains `uint64_t remote_addr` and `uint32_t rkey` fields. Both sides populate and exchange these during the TCP handshake.

### RDMA WRITE Work Request Construction

- `opcode` = `IBV_WR_RDMA_WRITE` (no immediate data)
- `.wr.rdma.remote_addr` = server's buffer address from OOB exchange
- `.wr.rdma.rkey` = server's rkey from OOB exchange
- `.sg_list` = single SGE pointing to client's buffer at offset 0, length = current message size
- `send_flags` = `IBV_SEND_SIGNALED` only on the last WR in each batch; `IBV_SEND_INLINE` when `size <= max_inline_data`
- Control SENDs (both directions) always use `IBV_SEND_INLINE` and are always signaled

### Queue Sizing

- SQ depth: set to device maximum (queried via `ibv_query_device()` → `max_qp_wr`)
- RQ depth: modest (10–100) — only needed for the control SEND recvs
- CQ size: device maximum (`max_cqe`) to absorb all possible completions
- `WC_BATCH` bumped from 10 to 64 or 128 for fewer `ibv_poll_cq` calls

### Completion Strategy

- Sliding window: maintain `posted - completed < tx_depth` at all times. Reap completions with `ibv_poll_cq`, re-post WRs immediately as slots free. The SQ stays non-empty throughout the timed batch.
- Only the last WR per batch is signaled; serial ordering guarantees that when its CQE arrives, all prior unsignaled WRs on that SQ are also complete.
- Recv completions are re-posted immediately in the poll loop (template pattern retained).

### Memory

- Single 1MB buffer per side, allocated via `malloc`, registered once with `IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE`.
- No `memset` — buffer content is irrelevant for throughput measurement.
- For each message size, the client overwrites `[0..size-1]` of the server's buffer with each RDMA WRITE. Data is never read.

### max_inline Discovery

- After QP creation, call `ibv_query_qp()` and store `qp_attr.cap.max_inline_data` in the context struct.
- The inline decision is computed once per message size (not per WR), since all WRs in a batch have the same payload size.

### Compiler Flags

- `-O3 -march=native -funroll-loops` for both server and client
- `-DNDEBUG` to strip assertions
- No `fprintf(stderr, ...)` in the submission code — cold error paths return non-zero silently
- `__builtin_expect` (or `likely`/`unlikely` macros) on error branches in the poll loop
- No `#include <assert.h>` — unused

### Output Format

Exact match with Exercise 1:
```
<size>\t<throughput_value>\t<unit>\n
```
- `size`: integer (1, 2, 4, ..., 1048576)
- `throughput_value`: floating point, 2 decimal places
- `unit`: `bps`, `Kbps`, `Mbps`, or `Gbps` (SI base-1000)
- No header lines, no debug output, no progress messages
- Calculation: `throughput = (size * msg_count * 8) / elapsed_seconds`

### Hardcoded Tables

Per message size (21 entries each):
- `WARMUP_COUNTS[i]` — number of RDMA WRITEs to prime the pipeline before timing
- `MSG_COUNTS[i]` — number of RDMA WRITEs in the timed batch

These are discovered by the convergence detector on the course hardware, then copied into the submission's `client.c`.

## Testing Decisions

- Verification is manual: run on mlxstud01+mlxstud02 (development pair) during development, then on mlxstud03+mlxstud04 (testing pair) for final numbers.
- Correctness check: the server must receive all bytes (or at minimum, the control SEND must arrive after all RDMA WRITEs). Since RDMA WRITE over RC transport is reliable and ordered, and the control SEND is posted only after all WRITE completions are reaped, the ordering guarantee is provided by the hardware.
- Throughput sanity: results should approach 56 Gbps (minus protocol overhead) for large message sizes. If throughput is significantly lower, investigate pipeline stalls or insufficient tx_depth.
- Convergence: variance between consecutive doubled-count runs should fall below 1% before hardcoding.

## Out of Scope

- Bidirectional / duplex throughput measurement
- RDMA Read or Atomic operations
- Unreliable Datagram (UD) transport
- RoCE or iWARP — InfiniBand only (as used by the course setup)
- CM-based connection establishment (TCP OOB is retained for simplicity)
- Multi-stream or multi-QP parallelism (single QP per direction)
- Any message sizes beyond the 21 powers of 2 (1B to 1MB)
- Cross-subnet routing (single subnet, LID-based forwarding)

## Further Notes

- The convergence detector code is a tool for the developer, not part of the submission. It should be kept alongside for reproducibility.
- The +5 bonus points go to the best result in the class. Every microsecond counts — hence the aggressive optimization stance throughout.
- The course hardware has two pairs of nodes. Using pair 1 (mlxstud01+02) for development and pair 2 (mlxstud03+04) for final testing avoids concurrent-run interference.
- The original `bw_template.c` should be preserved unmodified for reference.
