//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "nng/nng.h"
#include <nuts.h>

#define SECOND 1000

#define BUS0_SELF 0x70
#define BUS0_PEER 0x70
#define BUS0_SELF_NAME "bus"
#define BUS0_PEER_NAME "bus"

void
test_bus_identity(void)
{
	nng_socket  s;
	uint16_t    p;
	const char *n;

	NUTS_PASS(nng_bus0_open(&s));
	NUTS_PASS(nng_socket_proto_id(s, &p));
	NUTS_TRUE(p == BUS0_SELF);
	NUTS_PASS(nng_socket_peer_id(s, &p));
	NUTS_TRUE(p == BUS0_PEER); // 49
	NUTS_PASS(nng_socket_proto_name(s, &n));
	NUTS_MATCH(n, BUS0_SELF_NAME);
	NUTS_PASS(nng_socket_peer_name(s, &n));
	NUTS_MATCH(n, BUS0_PEER_NAME);
	NUTS_CLOSE(s);
}

static void
test_bus_star(void)
{
	nng_socket s1, s2, s3;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_bus0_open(&s2));
	NUTS_PASS(nng_bus0_open(&s3));

	NUTS_PASS(nng_socket_set_ms(s1, NNG_OPT_RECVTIMEO, SECOND));
	NUTS_PASS(nng_socket_set_ms(s2, NNG_OPT_RECVTIMEO, SECOND));
	NUTS_PASS(nng_socket_set_ms(s3, NNG_OPT_RECVTIMEO, SECOND));

	NUTS_MARRY(s1, s2);
	NUTS_MARRY(s1, s3);

	NUTS_SEND(s1, "one");
	NUTS_RECV(s2, "one");
	NUTS_RECV(s3, "one");

	NUTS_SEND(s2, "two");
	NUTS_SEND(s1, "one");
	NUTS_RECV(s1, "two");
	NUTS_RECV(s2, "one");
	NUTS_RECV(s3, "one");

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
	NUTS_CLOSE(s3);
}

static void
test_bus_device(void)
{
	nng_socket s1, s2, s3;
	nng_socket none = NNG_SOCKET_INITIALIZER;
	nng_aio   *aio;

	NUTS_PASS(nng_bus0_open_raw(&s1));
	NUTS_PASS(nng_bus0_open(&s2));
	NUTS_PASS(nng_bus0_open(&s3));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));

	NUTS_PASS(nng_socket_set_ms(s1, NNG_OPT_RECVTIMEO, SECOND));
	NUTS_PASS(nng_socket_set_ms(s2, NNG_OPT_RECVTIMEO, SECOND));
	NUTS_PASS(nng_socket_set_ms(s3, NNG_OPT_RECVTIMEO, SECOND));

	NUTS_MARRY(s1, s2);
	NUTS_MARRY(s1, s3);

	nng_device_aio(aio, s1, none);

	NUTS_SEND(s2, "two");
	NUTS_SEND(s3, "three");
	NUTS_RECV(s2, "three");
	NUTS_RECV(s3, "two");

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
	NUTS_CLOSE(s3);

	nng_aio_free(aio);
}

static void
test_bus_validate_peer(void)
{
	nng_socket      s1, s2;
	nng_stat       *stats;
	const nng_stat *reject;
	char           *addr;

	NUTS_ADDR(addr, "inproc");
	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_pair0_open(&s2));

	NUTS_PASS(nng_listen(s1, addr, NULL, 0));
	NUTS_PASS(nng_dial(s2, addr, NULL, NNG_FLAG_NONBLOCK));

	NUTS_SLEEP(100);
	NUTS_PASS(nng_stats_get(&stats));

	NUTS_TRUE(stats != NULL);
	NUTS_TRUE((reject = nng_stat_find_socket(stats, s1)) != NULL);
	NUTS_TRUE((reject = nng_stat_find(reject, "reject")) != NULL);

	NUTS_TRUE(nng_stat_type(reject) == NNG_STAT_COUNTER);
	NUTS_TRUE(nng_stat_value(reject) > 0);

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
	nng_stats_free(stats);
}

static void
test_bus_no_context(void)
{
	nng_socket s;
	nng_ctx    ctx;

	NUTS_PASS(nng_bus0_open(&s));
	NUTS_FAIL(nng_ctx_open(&ctx, s), NNG_ENOTSUP);
	NUTS_CLOSE(s);
}

static void
test_bus_recv_cancel(void)
{
	nng_socket s1;
	nng_aio   *aio;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));

	nng_aio_set_timeout(aio, SECOND);
	nng_socket_recv(s1, aio);
	nng_aio_abort(aio, NNG_ECANCELED);

	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECANCELED);
	NUTS_CLOSE(s1);
	nng_aio_free(aio);
}

static void
test_bus_close_recv_abort(void)
{
	nng_socket s1;
	nng_aio   *aio;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));

	nng_aio_set_timeout(aio, SECOND);
	nng_socket_recv(s1, aio);
	NUTS_CLOSE(s1);

	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);
	nng_aio_free(aio);
}

static void
test_bus_aio_stopped(void)
{
	nng_socket s1;
	nng_aio   *aio1;
	nng_aio   *aio2;
	nng_msg   *msg;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_msg_alloc(&msg, 0));
	NUTS_PASS(nng_aio_alloc(&aio1, NULL, NULL));
	NUTS_PASS(nng_aio_alloc(&aio2, NULL, NULL));
	nng_aio_stop(aio1);
	nng_aio_stop(aio2);

	nng_socket_recv(s1, aio1);
	nng_aio_wait(aio1);
	NUTS_FAIL(nng_aio_result(aio1), NNG_ESTOPPED);

	nng_aio_set_msg(aio2, msg);
	nng_socket_send(s1, aio2);
	nng_aio_wait(aio2);
	NUTS_FAIL(nng_aio_result(aio2), NNG_ESTOPPED);

	nng_aio_free(aio1);
	nng_aio_free(aio2);
	nng_msg_free(msg);
	NUTS_CLOSE(s1);
}

static void
test_bus_aio_canceled(void)
{
	nng_socket s1;
	nng_aio   *aio;
	nng_msg   *msg;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_msg_alloc(&msg, 0));
	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));

	nng_socket_recv(s1, aio);
	nng_aio_cancel(aio);
	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECANCELED);

	nng_aio_free(aio);
	nng_msg_free(msg);
	NUTS_CLOSE(s1);
}

static void
test_bus_send_no_pipes(void)
{
	nng_socket s1;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_SEND(s1, "DROP1");
	NUTS_SEND(s1, "DROP2");
	NUTS_CLOSE(s1);
}

static void
test_bus_send_flood(void)
{
	nng_socket s1, s2;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_bus0_open(&s2));
	NUTS_PASS(nng_socket_set_int(s2, NNG_OPT_SENDBUF, 1));

	// Even after connect (no message yet)
	NUTS_MARRY(s1, s2);

	// Even if we send messages.
	for (int i = 0; i < 1000; i++) {
		NUTS_SEND(s2, "one thousand");
	}

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

static void
test_bus_poll_readable(void)
{
	int        fd;
	nng_socket s1, s2;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_bus0_open(&s2));
	NUTS_PASS(nng_socket_set_ms(s1, NNG_OPT_RECVTIMEO, 1000));
	NUTS_PASS(nng_socket_set_ms(s2, NNG_OPT_SENDTIMEO, 1000));
	NUTS_PASS(nng_socket_get_recv_poll_fd(s1, &fd));
	NUTS_TRUE(fd >= 0);

	// Not readable if not connected!
	NUTS_TRUE(nuts_poll_fd(fd) == false);

	// Even after connect (no message yet)
	NUTS_MARRY(s2, s1);
	NUTS_TRUE(nuts_poll_fd(fd) == false);

	// But once we send messages, it is.
	// We have to send a request, in order to send a reply.
	NUTS_SEND(s2, "abc");
	NUTS_SLEEP(100);
	NUTS_TRUE(nuts_poll_fd(fd));

	// and receiving makes it no longer ready
	NUTS_RECV(s1, "abc");
	NUTS_TRUE(nuts_poll_fd(fd) == false);

	NUTS_CLOSE(s2);
	NUTS_CLOSE(s1);
}

static void
test_bus_poll_writeable(void)
{
	int        fd;
	nng_socket s1, s2;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_bus0_open(&s2));
	NUTS_PASS(nng_socket_set_int(s2, NNG_OPT_SENDBUF, 1));
	NUTS_PASS(nng_socket_get_send_poll_fd(s2, &fd));
	NUTS_TRUE(fd >= 0);

	// Bus is *always* writeable
	NUTS_TRUE(nuts_poll_fd(fd));

	// Even after connect (no message yet)
	NUTS_MARRY(s1, s2);
	NUTS_TRUE(nuts_poll_fd(fd));

	// Even if we send messages.
	NUTS_SEND(s2, "abc");
	NUTS_TRUE(nuts_poll_fd(fd));

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

static void
test_bus_recv_buf_option(void)
{
	nng_socket  s;
	int         v;
	bool        b;
	const char *opt = NNG_OPT_RECVBUF;

	NUTS_PASS(nng_bus0_open(&s));

	NUTS_PASS(nng_socket_set_int(s, opt, 1));
	NUTS_FAIL(nng_socket_set_int(s, opt, 0), NNG_EINVAL);
	NUTS_FAIL(nng_socket_set_int(s, opt, -1), NNG_EINVAL);
	NUTS_FAIL(nng_socket_set_int(s, opt, 1000000), NNG_EINVAL);
	NUTS_PASS(nng_socket_set_int(s, opt, 3));
	NUTS_PASS(nng_socket_get_int(s, opt, &v));
	NUTS_TRUE(v == 3);
	NUTS_FAIL(nng_socket_set_bool(s, opt, true), NNG_EBADTYPE);
	NUTS_FAIL(nng_socket_get_bool(s, opt, &b), NNG_EBADTYPE);

	NUTS_CLOSE(s);
}

static void
test_bus_send_buf_option(void)
{
	nng_socket  s1;
	nng_socket  s2;
	int         v;
	bool        b;
	const char *opt = NNG_OPT_SENDBUF;

	NUTS_PASS(nng_bus0_open(&s1));
	NUTS_PASS(nng_bus0_open(&s2));
	NUTS_MARRY(s1, s2);

	NUTS_PASS(nng_socket_set_int(s1, opt, 1));
	NUTS_FAIL(nng_socket_set_int(s1, opt, 0), NNG_EINVAL);
	NUTS_FAIL(nng_socket_set_int(s1, opt, -1), NNG_EINVAL);
	NUTS_FAIL(nng_socket_set_int(s1, opt, 1000000), NNG_EINVAL);
	NUTS_PASS(nng_socket_set_int(s1, opt, 3));
	NUTS_PASS(nng_socket_get_int(s1, opt, &v));
	NUTS_TRUE(v == 3);
	NUTS_FAIL(nng_socket_set_bool(s1, opt, true), NNG_EBADTYPE);
	NUTS_FAIL(nng_socket_get_bool(s1, opt, &b), NNG_EBADTYPE);

	NUTS_CLOSE(s1);
	NUTS_CLOSE(s2);
}

static void
test_bus_cooked(void)
{
	nng_socket s;
	bool       b;

	NUTS_PASS(nng_bus0_open(&s));
	NUTS_PASS(nng_socket_raw(s, &b));
	NUTS_TRUE(!b);
	NUTS_CLOSE(s);

	// raw pub only differs in the option setting
	NUTS_PASS(nng_bus0_open_raw(&s));
	NUTS_PASS(nng_socket_raw(s, &b));
	NUTS_TRUE(b);
	NUTS_CLOSE(s);
}

static void
test_bug1247(void)
{
	nng_socket bus1, bus2;
	char      *addr;

	NUTS_ADDR(addr, "tcp");

	NUTS_PASS(nng_bus0_open(&bus1));
	NUTS_PASS(nng_bus0_open(&bus2));

	NUTS_PASS(nng_listen(bus1, addr, NULL, 0));
	NUTS_FAIL(nng_listen(bus2, addr, NULL, 0), NNG_EADDRINUSE);

	NUTS_CLOSE(bus2);
	NUTS_CLOSE(bus1);
}

TEST_LIST = {
	{ "bus identity", test_bus_identity },
	{ "bus star", test_bus_star },
	{ "bus device", test_bus_device },
	{ "bus validate peer", test_bus_validate_peer },
	{ "bus no context", test_bus_no_context },
	{ "bus poll read", test_bus_poll_readable },
	{ "bus poll write", test_bus_poll_writeable },
	{ "bus send no pipes", test_bus_send_no_pipes },
	{ "bus send flood", test_bus_send_flood },
	{ "bus recv cancel", test_bus_recv_cancel },
	{ "bus close recv abort", test_bus_close_recv_abort },
	{ "bus aio stopped", test_bus_aio_stopped },
	{ "bus aio canceled", test_bus_aio_canceled },
	{ "bus recv buf option", test_bus_recv_buf_option },
	{ "bus send buf option", test_bus_send_buf_option },
	{ "bus cooked", test_bus_cooked },
	{ "bug1247", test_bug1247 },
	{ NULL, NULL },
};
