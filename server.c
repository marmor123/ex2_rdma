/*
 * server.c -- Minimal RDMA throughput server
 *
 * Unidirectional throughput benchmark: for each of 21 sizes
 * (powers of two, 1 .. 1048576) the server waits for a RECV
 * completion from the client, sends a tiny inline ACK, and
 * waits for that send to complete.  Silent, no output.
 */

#include "common.h"
#include <getopt.h>
#include <time.h>

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device       *ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest;
	char                    *ib_devname = NULL;
	int                      port     = 12345;
	int                      ib_port  = 1;
	enum ibv_mtu             mtu      = IBV_MTU_2048;
	int                      use_event = 0;
	int                      sl       = 0;
	int                      gidx     = -1;
	int                      i;

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
	if (ctx->routs < ctx->rx_depth)
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
	rem_dest = pp_server_exch_dest(ctx, ib_port, mtu,
				       port, sl, &my_dest, gidx);
	if (!rem_dest)
		return 1;

	/* ---- throughput loop: 21 sizes (powers of two) ---- */
	for (i = 0; i < 21; i++) {
		/* 2. wait for RECV completion (client data) */
		if (pp_wait_completions(ctx, 1))
			return 1;

		/* 3. send 8-byte inline ACK */
		if (pp_post_send(ctx, 8, IBV_WR_SEND,
				 IBV_SEND_SIGNALED | IBV_SEND_INLINE,
				 0, 0))
			return 1;

		/* 4. wait for SEND completion */
		if (pp_wait_completions(ctx, 1))
			return 1;

		/* 5. re-post one recv buffer for the next iteration */
		if (pp_post_recv(ctx, 1) != 1)
			return 1;
		ctx->routs++;
	}

	/* ---- teardown ---- */
	ibv_free_device_list(dev_list);
	free(rem_dest);
	if (pp_close_ctx(ctx))
		return 1;

	return 0;
}
