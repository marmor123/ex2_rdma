#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <infiniband/verbs.h>

#define unlikely(x) __builtin_expect(!!(x), 0)

#define MAX_SIZE  (1048576)
#define WC_BATCH  (128)

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

struct pingpong_context {
	struct ibv_context      *context;
	struct ibv_comp_channel *channel;
	struct ibv_pd           *pd;
	struct ibv_mr           *mr;
	struct ibv_cq           *cq;
	struct ibv_qp           *qp;
	void                    *buf;
	int                      size;
	int                      rx_depth;
	int                      routs;
	struct ibv_port_attr     portinfo;
	uint32_t                 max_inline;
};

struct pingpong_dest {
	int          lid;
	int          qpn;
	int          psn;
	union ibv_gid gid;
	uint64_t     remote_addr;
	uint32_t     rkey;
};

static inline enum ibv_mtu pp_mtu_to_enum(int mtu)
{
	switch (mtu) {
	case 256:  return IBV_MTU_256;
	case 512:  return IBV_MTU_512;
	case 1024: return IBV_MTU_1024;
	case 2048: return IBV_MTU_2048;
	case 4096: return IBV_MTU_4096;
	default:   return -1;
	}
}

static inline uint16_t pp_get_local_lid(struct ibv_context *context, int port)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(context, port, &attr))
		return 0;

	return attr.lid;
}

static inline int pp_get_port_info(struct ibv_context *context, int port,
				   struct ibv_port_attr *attr)
{
	return ibv_query_port(context, port, attr);
}

static inline void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	uint32_t v32;
	int i;

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		*(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
	}
}

static inline void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	int i;

	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x",
			htonl(*(uint32_t *)(gid->raw + i * 4)));
}

static inline struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev,
						   int port, int use_event)
{
	struct pingpong_context *ctx;
	struct ibv_device_attr device_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_qp_init_attr qp_init_attr;
	int page_size;
	size_t mr_len;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = MAX_SIZE;
	ctx->rx_depth = 100;
	ctx->routs    = ctx->rx_depth;

	ctx->buf = malloc(MAX_SIZE);
	if (!ctx->buf)
		goto err_cleanup;

	memset(ctx->buf, 0, MAX_SIZE);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context)
		goto err_cleanup;

	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel)
			goto err_cleanup;
	}

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd)
		goto err_cleanup;

	memset(&device_attr, 0, sizeof device_attr);
	if (ibv_query_device(ctx->context, &device_attr))
		goto err_cleanup;

	ctx->cq = ibv_create_cq(ctx->context, device_attr.max_cqe,
				 NULL, ctx->channel, 0);
	if (!ctx->cq)
		goto err_cleanup;

	{
		struct ibv_qp_init_attr attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = device_attr.max_qp_wr,
				.max_recv_wr  = device_attr.max_qp_wr,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};

		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)
			goto err_cleanup;
	}

	if (ibv_query_qp(ctx->qp, &qp_attr, 0, &qp_init_attr))
		goto err_cleanup;
	ctx->max_inline = qp_init_attr.cap.max_inline_data;

	page_size = sysconf(_SC_PAGESIZE);
	mr_len  = roundup(MAX_SIZE, page_size);
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, mr_len,
			     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->mr)
		goto err_cleanup;

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qp_access_flags = IBV_ACCESS_REMOTE_READ |
					   IBV_ACCESS_REMOTE_WRITE
		};

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE |
				  IBV_QP_PKEY_INDEX |
				  IBV_QP_PORT |
				  IBV_QP_ACCESS_FLAGS))
			goto err_cleanup;
	}

	return ctx;

err_cleanup:
	if (ctx->mr)
		ibv_dereg_mr(ctx->mr);
	if (ctx->qp)
		ibv_destroy_qp(ctx->qp);
	if (ctx->cq)
		ibv_destroy_cq(ctx->cq);
	if (ctx->pd)
		ibv_dealloc_pd(ctx->pd);
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);
	if (ctx->context)
		ibv_close_device(ctx->context);
	free(ctx->buf);
	free(ctx);
	return NULL;
}

static inline int pp_close_ctx(struct pingpong_context *ctx)
{
	if (ibv_destroy_qp(ctx->qp))
		return -1;

	if (ibv_destroy_cq(ctx->cq))
		return -1;

	if (ibv_dereg_mr(ctx->mr))
		return -1;

	if (ibv_dealloc_pd(ctx->pd))
		return -1;

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel))
			return -1;
	}

	if (ibv_close_device(ctx->context))
		return -1;

	free(ctx->buf);
	free(ctx);

	return 0;
}

static inline int pp_connect_ctx(struct pingpong_context *ctx, int port,
				 int my_psn, enum ibv_mtu mtu, int sl,
				 struct pingpong_dest *dest, int sgid_idx)
{
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num		= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE |
			  IBV_QP_AV |
			  IBV_QP_PATH_MTU |
			  IBV_QP_DEST_QPN |
			  IBV_QP_RQ_PSN |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER))
		return -1;

	attr.qp_state	  = IBV_QPS_RTS;
	attr.timeout	  = 14;
	attr.retry_cnt	  = 7;
	attr.rnr_retry	  = 7;
	attr.sq_psn	  = my_psn;
	attr.max_rd_atomic = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE |
			  IBV_QP_TIMEOUT |
			  IBV_QP_RETRY_CNT |
			  IBV_QP_RNR_RETRY |
			  IBV_QP_SQ_PSN |
			  IBV_QP_MAX_QP_RD_ATOMIC))
		return -1;

	return 0;
}

static inline int pp_post_send(struct pingpong_context *ctx, int size,
			       enum ibv_wr_opcode opcode, int send_flags,
			       uint64_t remote_addr, uint32_t rkey)
{
	struct ibv_sge list = {
		.addr   = (uintptr_t)ctx->buf,
		.length = size,
		.lkey   = ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = opcode,
		.send_flags = send_flags,
		.next       = NULL
	};
	struct ibv_send_wr *bad_wr;

	wr.wr.rdma.remote_addr = remote_addr;
	wr.wr.rdma.rkey = rkey;

	return ibv_post_send(ctx->qp, &wr, &bad_wr) ? -1 : 0;
}

/*
 * Post a batch of identical WRs as a linked list — single doorbell.
 * Only the last WR in the chain is signaled; earlier WRs inherit
 * @base_flags (typically 0 or IBV_SEND_INLINE).
 */
static inline int pp_post_send_batch(struct pingpong_context *ctx,
				      int size, enum ibv_wr_opcode opcode,
				      uint64_t remote_addr, uint32_t rkey,
				      int count, int base_flags)
{
	struct ibv_send_wr *bad_wr;
	int j;

	if (unlikely(count <= 0))
		return 0;

	/* VLAs are fine on Ubuntu/GCC — tx_depth is bounded by HCA caps */
	struct ibv_send_wr wrs[count];
	struct ibv_sge      sges[count];

	for (j = 0; j < count; j++) {
		sges[j] = (struct ibv_sge){
			.addr   = (uintptr_t)ctx->buf,
			.length = size,
			.lkey   = ctx->mr->lkey
		};

		int flags = base_flags;
		if (j == count - 1)
			flags |= IBV_SEND_SIGNALED;

		wrs[j] = (struct ibv_send_wr){
			.wr_id      = PINGPONG_SEND_WRID,
			.sg_list    = &sges[j],
			.num_sge    = 1,
			.opcode     = opcode,
			.send_flags = flags,
			.next       = (j + 1 < count) ? &wrs[j + 1] : NULL,
			.wr.rdma    = {
				.remote_addr = remote_addr,
				.rkey        = rkey
			}
		};
	}

	return ibv_post_send(ctx->qp, &wrs[0], &bad_wr) ? -1 : 0;
}

static inline int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr   = (uintptr_t)ctx->buf,
		.length = ctx->size,
		.lkey   = ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.next       = NULL
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static inline int pp_wait_completions(struct pingpong_context *ctx, int iters)
{
	int rcnt = 0, scnt = 0;

	while (rcnt + scnt < iters) {
		struct ibv_wc wc[WC_BATCH];
		int ne, i;

		do {
			ne = ibv_poll_cq(ctx->cq, WC_BATCH, wc);
			if (unlikely(ne < 0))
				return -1;
		} while (ne < 1);

		for (i = 0; i < ne; ++i) {
			if (unlikely(wc[i].status != IBV_WC_SUCCESS))
				return -1;

			switch ((int)wc[i].wr_id) {
			case PINGPONG_SEND_WRID:
				++scnt;
				break;

			case PINGPONG_RECV_WRID:
				if (--ctx->routs <= 10) {
					ctx->routs += pp_post_recv(ctx,
						ctx->rx_depth - ctx->routs);
					if (unlikely(ctx->routs < ctx->rx_depth))
						return -1;
				}
				++rcnt;
				break;

			default:
				return -1;
			}
		}
	}

	return 0;
}

static inline struct pingpong_dest *pp_client_exch_dest(
					const char *servername, int port,
					const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[128];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];
	unsigned long long ra_temp;

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0)
		return NULL;

	gid_to_wire_gid(&my_dest->gid, gid);
	snprintf(msg, sizeof msg,
		 "%04x:%06x:%06x:%016llx:%08x:%s",
		 my_dest->lid, my_dest->qpn, my_dest->psn,
		 (unsigned long long)my_dest->remote_addr,
		 (unsigned int)my_dest->rkey,
		 gid);
	if (write(sockfd, msg, sizeof msg) != (ssize_t)sizeof msg)
		goto out;

	if (read(sockfd, msg, sizeof msg) != (ssize_t)sizeof msg)
		goto out;

	{ int _ign = write(sockfd, "done", sizeof "done"); (void)_ign; }

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%llx:%x:%s",
	       &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn,
	       &ra_temp, &rem_dest->rkey, gid);
	rem_dest->remote_addr = (uint64_t)ra_temp;
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}

static inline struct pingpong_dest *pp_server_exch_dest(
					struct pingpong_context *ctx,
					int ib_port, enum ibv_mtu mtu,
					int port, int sl,
					const struct pingpong_dest *my_dest,
					int sgid_idx)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[128];
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];
	unsigned long long ra_temp;

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0)
		return NULL;

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	close(sockfd);
	if (connfd < 0)
		return NULL;

	n = read(connfd, msg, sizeof msg);
	if (n != (ssize_t)sizeof msg)
		goto out;

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%llx:%x:%s",
	       &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn,
	       &ra_temp, &rem_dest->rkey, gid);
	rem_dest->remote_addr = (uint64_t)ra_temp;
	wire_gid_to_gid(gid, &rem_dest->gid);

	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl,
			   rem_dest, sgid_idx)) {
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	snprintf(msg, sizeof msg,
		 "%04x:%06x:%06x:%016llx:%08x:%s",
		 my_dest->lid, my_dest->qpn, my_dest->psn,
		 (unsigned long long)my_dest->remote_addr,
		 (unsigned int)my_dest->rkey,
		 gid);
	if (write(connfd, msg, sizeof msg) != (ssize_t)sizeof msg) {
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	{ int _ign = read(connfd, msg, sizeof msg); (void)_ign; }

out:
	close(connfd);
	return rem_dest;
}

#endif /* COMMON_H */
