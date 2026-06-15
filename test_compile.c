#include "common.h"

int main(void)
{
	int ret;
	enum ibv_mtu mtu;
	uint16_t lid;
	struct ibv_port_attr attr;
	union ibv_gid gid;
	char wgid[33];
	struct pingpong_context ctx;
	struct pingpong_dest my_dest;
	struct pingpong_dest *rem_dest;
	struct ibv_device *dev;

	/* Zero-initialize everything to avoid -Wmaybe-uninitialized */
	memset(&ctx, 0, sizeof ctx);
	memset(&my_dest, 0, sizeof my_dest);

	/* pp_mtu_to_enum */
	mtu = pp_mtu_to_enum(1024);
	(void)mtu;

	/* pp_get_local_lid */
	lid = pp_get_local_lid(NULL, 1);
	(void)lid;

	/* pp_get_port_info */
	ret = pp_get_port_info(NULL, 1, &attr);
	(void)ret;

	/* wire_gid_to_gid / gid_to_wire_gid */
	wire_gid_to_gid("00000000000000000000000000000000", &gid);
	gid_to_wire_gid(&gid, wgid);

	/* pp_init_ctx */
	dev = NULL;
	ctx.size = 0;  /* just to use the variable to suppress warning */
	(void)ctx.size;
	ctx.buf = (void *)1;     /* prevent NULL-deref warning in inline */
	ctx.mr  = (void *)1;     /* prevent NULL-deref warning in inline */
	ctx.qp  = (void *)1;     /* prevent NULL-deref warning in inline */
	ret = pp_post_send(&ctx, 64, IBV_WR_SEND, IBV_SEND_SIGNALED, 0, 0);
	(void)ret;

	ret = pp_post_recv(&ctx, 1);
	(void)ret;

	ret = pp_wait_completions(&ctx, 1);
	(void)ret;

	ret = pp_connect_ctx(&ctx, 1, 0, IBV_MTU_1024, 0, &my_dest, -1);
	(void)ret;

	ret = pp_close_ctx(&ctx);
	(void)ret;

	/* pp_client_exch_dest — will fail at runtime, but must compile */
	rem_dest = pp_client_exch_dest("127.0.0.1", 12345, &my_dest);
	if (rem_dest)
		free(rem_dest);

	/* pp_server_exch_dest — will fail at runtime, but must compile */
	rem_dest = pp_server_exch_dest(&ctx, 1, IBV_MTU_1024, 12346, 0,
				       &my_dest, -1);
	if (rem_dest)
		free(rem_dest);

	return 0;
}
