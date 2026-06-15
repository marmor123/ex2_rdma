/*
 * client.c -- RDMA throughput benchmark client
 *
 * Hardcoded table-driven benchmark for 21 power-of-two sizes (1 .. 1048576).
 * Uses the same sliding-window RDMA WRITE engine as the convergence detector
 * but without convergence logic or debug output.
 *
 * Protocol (per message size):
 *   1. Warmup phase:  RDMA WRITE warmup_count times (batch signaled)
 *   2. Timed phase:   RDMA WRITE msg_count times with sliding window,
 *                     INLINE when size <= max_inline, SIGNALED on last
 *                     of each batch.  Then send IBV_WR_SEND "done" and
 *                     wait for server ACK.
 *   3. Throughput:    (size * msg_count * 8.0) / elapsed_sec
 *   4. Output:        one tab-separated line: size, throughput, unit
 */

#include "common.h"
#include <getopt.h>
#include <time.h>

#define NUM_SIZES 21

/* Hardcoded tables -- replace with convergence detector output */
static const int SIZES[NUM_SIZES] = {
	1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
	2048, 4096, 8192, 16384, 32768, 65536, 131072,
	262144, 524288, 1048576
};

/* PLACEHOLDER VALUES -- replace with convergence detector output */
static const int WARMUP_COUNTS[NUM_SIZES] = {
	16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 8, 8, 4, 4, 4, 4
};

static const int MSG_COUNTS[NUM_SIZES] = {
	1310720, 655360, 327680, 163840, 81920, 40960, 20480, 10240,
	5120, 2560, 1280, 640, 320, 160, 80, 40, 20, 10, 5, 3, 2
};

/* ---- helpers ----------------------------------------------------------- */

static inline int pp_min(int a, int b) { return a < b ? a : b; }

/* Query the effective max_send_wr from the created QP. */
static int get_tx_depth(struct pingpong_context *ctx)
{
	struct ibv_qp_attr qp_attr;
	struct ibv_qp_init_attr qp_init_attr;

	if (ibv_query_qp(ctx->qp, &qp_attr, 0, &qp_init_attr))
		return -1;
	return qp_init_attr.cap.max_send_wr;
}

/*
 * Run a single throughput measurement for a given (size, warmup, msgcnt).
 *
 * Returns throughput in bits/sec, or -1.0 on error.
 *
 * Warmup: post all WRITEs in tx_depth-sized batches; only the last WR
 * of each batch is signaled so we drain CQ between batches without
 * flooding the SQ.
 *
 * Timed phase (sliding window): post in tx_depth batches, signal the
 * last WR of each batch.  Between batches we wait for one completion.
 * When size <= max_inline every WR carries IBV_SEND_INLINE.
 */
static double run_measurement(struct pingpong_context *ctx,
			      struct pingpong_dest *rem_dest,
			      int size, int warmup_count, int msg_count,
			      int tx_depth, int use_inline)
{
	const int inline_flag = use_inline ? IBV_SEND_INLINE : 0;
	struct timespec start, stop;
	int remaining, batch, j;

	/* ---- warmup phase ---- */
	remaining = warmup_count;
	while (remaining > 0) {
		batch = pp_min(remaining, tx_depth);
		for (j = 0; j < batch; j++) {
			int flags = (j == batch - 1) ? IBV_SEND_SIGNALED : 0;
			if (unlikely(pp_post_send(ctx, size, IBV_WR_RDMA_WRITE,
						  flags,
						  rem_dest->remote_addr,
						  rem_dest->rkey)))
				return -1.0;
		}
		remaining -= batch;
		if (remaining > 0)
			if (unlikely(pp_wait_completions(ctx, 1)))
				return -1.0;
	}
	/* last batch completion */
	if (unlikely(pp_wait_completions(ctx, 1)))
		return -1.0;

	/* ---- timed phase ---- */
	if (unlikely(clock_gettime(CLOCK_MONOTONIC, &start)))
		return -1.0;

	remaining = msg_count;
	while (remaining > 0) {
		batch = pp_min(remaining, tx_depth);
		for (j = 0; j < batch; j++) {
			int flags = inline_flag;
			if (j == batch - 1)
				flags |= IBV_SEND_SIGNALED;
			if (unlikely(pp_post_send(ctx, size, IBV_WR_RDMA_WRITE,
						  flags,
						  rem_dest->remote_addr,
						  rem_dest->rkey)))
				return -1.0;
		}
		remaining -= batch;
		if (remaining > 0)
			if (unlikely(pp_wait_completions(ctx, 1)))
				return -1.0;
	}
	/* last batch completion */
	if (unlikely(pp_wait_completions(ctx, 1)))
		return -1.0;

	/* send "done" SEND to server (always INLINE + SIGNALED) */
	if (unlikely(pp_post_send(ctx, 8, IBV_WR_SEND,
				  IBV_SEND_SIGNALED | IBV_SEND_INLINE, 0, 0)))
		return -1.0;
	if (unlikely(pp_wait_completions(ctx, 1)))
		return -1.0;

	/* wait for server ACK (RECV completion) */
	if (unlikely(pp_wait_completions(ctx, 1)))
		return -1.0;

	/* re-post one recv buffer for next iteration */
	ctx->routs += pp_post_recv(ctx, 1);

	if (unlikely(clock_gettime(CLOCK_MONOTONIC, &stop)))
		return -1.0;

	double elapsed = (stop.tv_sec - start.tv_sec)
		       + (stop.tv_nsec - start.tv_nsec) / 1.0e9;

	if (unlikely(elapsed <= 0.0))
		return -1.0;

	return (size * (double)msg_count * 8.0) / elapsed;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device       *ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest;
	char                    *servername = NULL;
	char                    *ib_devname = NULL;
	int                      port     = 12345;
	int                      ib_port  = 1;
	enum ibv_mtu             mtu      = IBV_MTU_2048;
	int                      use_event = 0;
	int                      sl       = 0;
	int                      gidx     = -1;
	int                      i;
	int                      tx_depth;

	/* ---- option parsing ---- */
	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",    .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",  .has_arg = 1, .val = 'd' },
			{ .name = "ib-port", .has_arg = 1, .val = 'i' },
			{ .name = "mtu",     .has_arg = 1, .val = 'm' },
			{ .name = "sl",      .has_arg = 1, .val = 'l' },
			{ .name = "gid-idx", .has_arg = 1, .val = 'g' },
			{ .name = "events",  .has_arg = 0, .val = 'e' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:m:l:g:e", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = atoi(optarg);
			break;
		case 'd':
			ib_devname = optarg;
			break;
		case 'i':
			ib_port = atoi(optarg);
			break;
		case 'm':
			mtu = pp_mtu_to_enum(atoi(optarg));
			break;
		case 'l':
			sl = atoi(optarg);
			break;
		case 'g':
			gidx = atoi(optarg);
			break;
		case 'e':
			use_event = 1;
			break;
		default:
			return 1;
		}
	}

	if (optind >= argc)
		return 1;
	servername = argv[optind];

	/* ---- open IB device ---- */
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list)
		return 1;

	if (ib_devname) {
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
	} else {
		ib_dev = *dev_list;
	}
	if (!ib_dev) {
		ibv_free_device_list(dev_list);
		return 1;
	}

	/* ---- initialise context (1 MB MR, max-depth QP / CQ) ---- */
	ctx = pp_init_ctx(ib_dev, ib_port, use_event);
	if (!ctx) {
		ibv_free_device_list(dev_list);
		return 1;
	}

	/* ---- post initial receive buffers ---- */
	ctx->routs = pp_post_recv(ctx, ctx->rx_depth);
	if (unlikely(ctx->routs < ctx->rx_depth))
		return 1;

	/* ---- query port and build local destination ---- */
	if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo))
		return 1;

	my_dest.lid = ctx->portinfo.lid;

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid))
			return 1;
	} else {
		memset(&my_dest.gid, 0, sizeof my_dest.gid);
	}

	my_dest.qpn          = ctx->qp->qp_num;
	my_dest.psn          = lrand48() & 0xffffff;
	my_dest.remote_addr  = (uintptr_t)ctx->buf;
	my_dest.rkey         = ctx->mr->rkey;

	/* ---- TCP out-of-band exchange, connect QP ---- */
	rem_dest = pp_client_exch_dest(servername, port, &my_dest);
	if (!rem_dest)
		return 1;

	/* ---- discover device limits ---- */
	tx_depth = get_tx_depth(ctx);
	if (unlikely(tx_depth <= 0))
		return 1;

	/* ---- benchmark loop: 21 sizes (powers of two) ---- */
	for (i = 0; i < NUM_SIZES; i++) {
		int size       = SIZES[i];
		int warmup     = WARMUP_COUNTS[i];
		int msg_count  = MSG_COUNTS[i];
		int use_inline = (size <= (int)ctx->max_inline);

		double tput = run_measurement(ctx, rem_dest, size,
					      warmup, msg_count,
					      tx_depth, use_inline);
		if (unlikely(tput < 0.0))
			return 1;

		/* format throughput with appropriate SI unit */
		double scaled;
		const char *unit;

		if (tput < 1000.0) {
			scaled = tput;
			unit = "bps";
		} else if (tput < 1000000.0) {
			scaled = tput / 1000.0;
			unit = "Kbps";
		} else if (tput < 1000000000.0) {
			scaled = tput / 1000000.0;
			unit = "Mbps";
		} else {
			scaled = tput / 1000000000.0;
			unit = "Gbps";
		}

		printf("%d\t%.2f\t%s\n", size, scaled, unit);
	}

	/* ---- teardown ---- */
	ibv_free_device_list(dev_list);
	free(rem_dest);
	if (pp_close_ctx(ctx))
		return 1;

	return 0;
}
