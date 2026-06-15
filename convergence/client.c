/*
 * client.c -- RDMA throughput convergence detector
 *
 * Empirically discovers optimal warmup and message counts per size
 * for stable throughput measurements.  For each of 21 power-of-two
 * sizes (1 .. 1048576) it first converges the warmup count, then
 * converges the message count, doubling each until consecutive
 * throughput measurements differ by less than 1 %.
 *
 * Protocol (client side, per message size):
 *   1. Warmup phase:  RDMA WRITE warmup_count times (batch signaled)
 *   2. Timed phase:   RDMA WRITE msg_count times with sliding window,
 *                     INLINE when size <= max_inline, SIGNALED on last
 *                     of each batch.  Then send IBV_WR_SEND "done" and
 *                     wait for server ACK.
 *   3. Throughput:    (size * msg_count * 8.0) / elapsed_sec
 *   4. Convergence:   double count, recompute, stop when variance < 1 %
 */

#include "common.h"
#include <getopt.h>
#include <time.h>
#include <math.h>
#include <float.h>

#define NUM_SIZES      21
/* Conservative MSG_COUNT used while converging the warmup parameter */
#define CONSERVATIVE_MSG_COUNT 100000
/* Safety cap to avoid infinite loops */
#define MAX_CONVERGE_ITERS 30

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
			if (pp_post_send(ctx, size, IBV_WR_RDMA_WRITE, flags,
					 rem_dest->remote_addr,
					 rem_dest->rkey))
				return -1.0;
		}
		remaining -= batch;
		if (remaining > 0)
			if (pp_wait_completions(ctx, 1))
				return -1.0;
	}
	/* last batch completion */
	if (pp_wait_completions(ctx, 1))
		return -1.0;

	/* ---- timed phase ---- */
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return -1.0;

	remaining = msg_count;
	while (remaining > 0) {
		batch = pp_min(remaining, tx_depth);
		for (j = 0; j < batch; j++) {
			int flags = inline_flag;
			if (j == batch - 1)
				flags |= IBV_SEND_SIGNALED;
			if (pp_post_send(ctx, size, IBV_WR_RDMA_WRITE, flags,
					 rem_dest->remote_addr,
					 rem_dest->rkey))
				return -1.0;
		}
		remaining -= batch;
		if (remaining > 0)
			if (pp_wait_completions(ctx, 1))
				return -1.0;
	}
	/* last batch completion */
	if (pp_wait_completions(ctx, 1))
		return -1.0;

	/* send "done" SEND to server (always INLINE + SIGNALED) */
	if (pp_post_send(ctx, 8, IBV_WR_SEND,
			 IBV_SEND_SIGNALED | IBV_SEND_INLINE, 0, 0))
		return -1.0;
	if (pp_wait_completions(ctx, 1))
		return -1.0;

	/* wait for server ACK (RECV completion) */
	if (pp_wait_completions(ctx, 1))
		return -1.0;

	/* re-post one recv buffer for next iteration */
	ctx->routs += pp_post_recv(ctx, 1);

	if (clock_gettime(CLOCK_MONOTONIC, &stop))
		return -1.0;

	double elapsed = (stop.tv_sec - start.tv_sec)
		       + (stop.tv_nsec - start.tv_nsec) / 1.0e9;

	if (elapsed <= 0.0)
		return -1.0;

	return (size * (double)msg_count * 8.0) / elapsed;
}

/*
 * Converge a single parameter (warmup or msg_count) by doubling until
 * the throughput variance between consecutive runs falls below 1 %.
 *
 * Parameters:
 *   ctx, rem_dest -- RDMA context and remote destination
 *   size          -- current message size (bytes)
 *   tx_depth      -- max outstanding WRs
 *   use_inline    -- whether to use IBV_SEND_INLINE for this size
 *   fixed_param   -- the OTHER parameter held constant
 *   is_warmup     -- 1 = converging warmup count, 0 = converging msg_count
 *
 * Returns the converged value, or 0 on error.
 */
static int converge_param(struct pingpong_context *ctx,
			   struct pingpong_dest *rem_dest,
			   int size, int tx_depth, int use_inline,
			   int fixed_param, int is_warmup)
{
	int count = 10;		/* starting base */
	double prev_tput = 0.0;
	int iter;

	for (iter = 0; iter < MAX_CONVERGE_ITERS; iter++) {
		double tput;

		if (is_warmup)
			tput = run_measurement(ctx, rem_dest, size,
					       count, fixed_param,
					       tx_depth, use_inline);
		else
			tput = run_measurement(ctx, rem_dest, size,
					       fixed_param, count,
					       tx_depth, use_inline);

		if (tput < 0.0) {
			fprintf(stderr,
				"ERROR: measurement failed size=%d %s=%d\n",
				size, is_warmup ? "warmup" : "msgcnt", count);
			return 0;
		}

		printf("%d\t%s\t%d\t%.2f\n",
		       size, is_warmup ? "warmup" : "msgcnt", count, tput);

		if (iter > 0) {
			double variance = fabs(tput - prev_tput) / prev_tput;
			if (variance < 0.01) {
				printf("  -> converged %s=%d (v=%.4f)\n",
				       is_warmup ? "warmup" : "msgcnt",
				       count, variance * 100.0);
				return count;
			}
		}

		prev_tput = tput;
		count *= 2;
	}

	fprintf(stderr,
		"WARNING: %s did not converge for size=%d (max=%ld)\n",
		is_warmup ? "warmup" : "msgcnt", size,
		10L * (1L << MAX_CONVERGE_ITERS));
	return count >> 1;  /* return previous (best-effort) */
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
	int                      use_inline;
	int					   sizes[NUM_SIZES];
	int                      warmup_counts[NUM_SIZES];
	int                      msg_counts[NUM_SIZES];

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
			if (port < 0 || port > 65535)
				return 1;
			break;
		case 'd':
			ib_devname = optarg;
			break;
		case 'i':
			ib_port = atoi(optarg);
			if (ib_port < 0)
				return 1;
			break;
		case 'm':
			mtu = pp_mtu_to_enum(atoi(optarg));
			if ((int)mtu < 0)
				return 1;
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

	if (optind >= argc) {
		fprintf(stderr, "Usage: %s <servername> [options]\n", argv[0]);
		return 1;
	}
	servername = argv[optind];

	/* ---- open IB device ---- */
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		fprintf(stderr, "ibv_get_device_list failed\n");
		return 1;
	}

	if (ib_devname) {
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
	} else {
		ib_dev = *dev_list;
	}
	if (!ib_dev) {
		fprintf(stderr, "IB device not found\n");
		ibv_free_device_list(dev_list);
		return 1;
	}

	/* ---- initialise context (1 MB MR, max-depth QP / CQ) ---- */
	ctx = pp_init_ctx(ib_dev, ib_port, use_event);
	if (!ctx) {
		fprintf(stderr, "pp_init_ctx failed\n");
		ibv_free_device_list(dev_list);
		return 1;
	}

	/* ---- post initial receive buffers ---- */
	ctx->routs = pp_post_recv(ctx, ctx->rx_depth);
	if (ctx->routs < ctx->rx_depth) {
		fprintf(stderr, "pp_post_recv initial failed\n");
		return 1;
	}

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
	if (!rem_dest) {
		fprintf(stderr, "pp_client_exch_dest failed\n");
		return 1;
	}

	/* ---- discover device limits ---- */
	tx_depth = get_tx_depth(ctx);
	if (tx_depth <= 0) {
		fprintf(stderr, "failed to query tx_depth\n");
		return 1;
	}
	printf("# tx_depth = %d, max_inline = %u\n", tx_depth, ctx->max_inline);
	printf("# col: size\tparam\tcount\tthroughput(bps)\n");

	/* ---- convergence loop: 21 sizes (powers of two) ---- */
	for (i = 0; i < NUM_SIZES; i++) {
		sizes[i] = 1 << i;		      /* 1, 2, 4, ..., 1048576 */
		use_inline = (sizes[i] <= (int)ctx->max_inline);

		printf("# ---- size=%d (inline=%d) ----\n",
		       sizes[i], use_inline);

		/* Step 1: converge warmup with conservative msg_count */
		int w = converge_param(ctx, rem_dest, sizes[i],
				       tx_depth, use_inline,
				       CONSERVATIVE_MSG_COUNT, 1);
		if (w <= 0)
			return 1;
		warmup_counts[i] = w;

		/* Step 2: converge msg_count with discovered warmup */
		int m = converge_param(ctx, rem_dest, sizes[i],
				       tx_depth, use_inline,
				       warmup_counts[i], 0);
		if (m <= 0)
			return 1;
		msg_counts[i] = m;
	}

	/* ---- print converged tables as C array initializers ---- */
	printf("\n--- Converged tables ---\n\n");

	printf("static const int WARMUP_COUNTS[21] = {");
	for (i = 0; i < NUM_SIZES; i++) {
		if (i > 0) printf(", ");
		printf("%d", warmup_counts[i]);
	}
	printf("};\n");

	printf("static const int MSG_COUNTS[21] = {");
	for (i = 0; i < NUM_SIZES; i++) {
		if (i > 0) printf(", ");
		printf("%d", msg_counts[i]);
	}
	printf("};\n");

	/* ---- teardown ---- */
	ibv_free_device_list(dev_list);
	free(rem_dest);
	if (pp_close_ctx(ctx))
		return 1;

	return 0;
}
