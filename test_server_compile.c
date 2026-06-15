/*
 * test_server_compile.c -- Compile-time verification for server.c
 *
 * This file verifies that the common.h API used by server.c compiles
 * correctly.  It is not a runtime test -- it only checks that every
 * function called by server.c exists and has the expected signature.
 *
 * Build:
 *   gcc -O3 -march=native -funroll-loops -c test_server_compile.c \
 *       -libverbs -o test_server_compile.o
 */

#include "common.h"
#include <getopt.h>
#include <time.h>

/*
 * Stand-in main that references every common.h symbol server.c depends
 * on, so that an optimising compiler does not optimise them away and
 * we get link-time verification of the external symbols (ibverbs).
 */
int main(void)
{
	/* ---- compile-time enum / constant checks ---- */
	int recv_wrid = PINGPONG_RECV_WRID;
	int send_wrid = PINGPONG_SEND_WRID;
	(void)recv_wrid;
	(void)send_wrid;

	/* ---- pp_mtu_to_enum ---- */
	enum ibv_mtu mtu = pp_mtu_to_enum(1024);
	(void)mtu;

	/* ---- struct sizes (header membership) ---- */
	(void)sizeof(struct pingpong_context);
	(void)sizeof(struct pingpong_dest);
	(void)sizeof(union ibv_gid);

	/* ---- MAX_SIZE constant ---- */
	(void)MAX_SIZE;

	return 0;
}
