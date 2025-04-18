//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
// Copyright 2019 Devolutions <info@devolutions.net>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

#include "tls_engine.h"

// NNG_TLS_MAX_SEND_SIZE limits the amount of data we will buffer for sending,
// exerting back-pressure if this size is exceeded.  The 16K is aligned to the
// maximum TLS record size.
#ifndef NNG_TLS_MAX_SEND_SIZE
#define NNG_TLS_MAX_SEND_SIZE 16384
#endif

// NNG_TLS_MAX_RECV_SIZE limits the amount of data we will receive in a single
// operation.  As we have to buffer data, this drives the size of our
// intermediary buffer.  The 16K is aligned to the maximum TLS record size.
#ifndef NNG_TLS_MAX_RECV_SIZE
#define NNG_TLS_MAX_RECV_SIZE 16384
#endif

// This file contains common code for TLS, and is only compiled if we
// have TLS configured in the system.  In particular, this provides the
// parts of TLS support that are invariant relative to different TLS
// libraries, such as dialer and listener support.

#ifdef NNG_SUPP_TLS

static nni_atomic_ptr tls_engine;

struct nng_tls_config {
	nng_tls_engine_config_ops ops;
	const nng_tls_engine     *engine; // store this so we can verify
	nni_mtx                   lock;
	int                       ref;
	bool                      busy;
	bool                      key_is_set;
	size_t                    size;

	// ... engine config data follows
};

typedef struct {
	nng_stream              stream;
	nng_tls_engine_conn_ops ops;
	nng_tls_config         *cfg;
	const nng_tls_engine   *engine;
	size_t                  size;
	nni_aio                *user_aio; // user's aio for connect/accept
	nni_aio                 conn_aio; // system aio for connect/accept
	nni_mtx                 lock;
	bool                    closed;
	nni_atomic_flag         did_close;
	bool                    hs_done;
	nni_list                send_queue;
	nni_list                recv_queue;
	nng_stream             *tcp;      // lower level stream
	nni_aio                 tcp_send; // lower level send pending
	nni_aio                 tcp_recv; // lower level recv pending
	uint8_t                *tcp_send_buf;
	uint8_t                *tcp_recv_buf;
	size_t                  tcp_recv_len;
	size_t                  tcp_recv_off;
	bool                    tcp_recv_pend;
	bool                    tcp_send_active;
	size_t                  tcp_send_len;
	size_t                  tcp_send_head;
	size_t                  tcp_send_tail;
	nni_reap_node           reap;

	// ... engine connection data follows
} tls_conn;

static void tls_tcp_send_cb(void *arg);
static void tls_tcp_recv_cb(void *arg);
static void tls_do_send(tls_conn *);
static void tls_do_recv(tls_conn *);
static void tls_tcp_send_start(tls_conn *);
static void tls_free(void *);
static void tls_reap(void *);
static int  tls_alloc(tls_conn **, nng_tls_config *, nng_aio *);
static int  tls_start(tls_conn *, nng_stream *);
static void tls_tcp_error(tls_conn *, int);

static nni_reap_list tls_conn_reap_list = {
	.rl_offset = offsetof(tls_conn, reap),
	.rl_func   = tls_reap,
};

typedef struct {
	nng_stream_dialer  ops;
	nng_stream_dialer *d; // underlying TCP dialer
	nng_tls_config    *cfg;
	bool               started;
	nni_mtx            lk; // protects the config
} tls_dialer;

static void
tls_dialer_close(void *arg)
{
	tls_dialer *d = arg;
	nng_stream_dialer_close(d->d);
}

static void
tls_dialer_free(void *arg)
{
	tls_dialer *d;
	if ((d = arg) != NULL) {
		nng_stream_dialer_free(d->d);
		nng_tls_config_free(d->cfg);
		nni_mtx_fini(&d->lk);
		NNI_FREE_STRUCT(d);
	}
}

static void
tls_dialer_stop(void *arg)
{
	tls_dialer *d = arg;

	nng_stream_dialer_stop(d->d);
}

// For dialing, we need to have our own completion callback, instead of
// the user's completion callback.

static void
tls_conn_cb(void *arg)
{
	tls_conn   *conn = arg;
	nng_stream *tcp;
	int         rv;

	if ((rv = nni_aio_result(&conn->conn_aio)) != 0) {
		nni_aio_finish_error(conn->user_aio, rv);
		nng_stream_free(&conn->stream);
		return;
	}

	tcp = nni_aio_get_output(&conn->conn_aio, 0);

	if ((rv = tls_start(conn, tcp)) != 0) {
		nni_aio_finish_error(conn->user_aio, rv);
		nng_stream_free(&conn->stream);
		return;
	}

	nni_aio_set_output(conn->user_aio, 0, &conn->stream);
	nni_aio_finish(conn->user_aio, 0, 0);
}

// Dialer cancel is called when the user has indicated that they no longer
// want to wait for the connection to establish.
static void
tls_conn_cancel(nni_aio *aio, void *arg, int rv)
{
	tls_conn *conn = arg;

	NNI_ARG_UNUSED(aio);

	// Just pass this down.  If the connection is already done, this
	// will have no effect.
	nni_aio_abort(&conn->conn_aio, rv);
}

static void
tls_dialer_dial(void *arg, nng_aio *aio)
{
	tls_dialer *d = arg;
	tls_conn   *conn;
	int         rv;

	nni_aio_reset(aio);
	if ((rv = tls_alloc(&conn, d->cfg, aio)) != 0) {
		nni_aio_finish_error(aio, rv);
		return;
	}

	if (!nni_aio_start(aio, tls_conn_cancel, conn)) {
		tls_free(conn);
		return;
	}
	nni_mtx_lock(&d->lk);
	d->started = true;
	nni_mtx_unlock(&d->lk);

	nng_stream_dialer_dial(d->d, &conn->conn_aio);
}

static int
tls_dialer_set_tls(void *arg, nng_tls_config *cfg)
{
	tls_dialer     *d = arg;
	nng_tls_config *old;
	if (cfg == NULL) {
		return (NNG_EINVAL);
	}

	nng_tls_config_hold(cfg);

	nni_mtx_lock(&d->lk);
	if (d->started) {
		nni_mtx_unlock(&d->lk);
		nng_tls_config_free(cfg);
		return (NNG_EBUSY);
	}
	old    = d->cfg;
	d->cfg = cfg;
	nni_mtx_unlock(&d->lk);

	nng_tls_config_free(old);
	return (0);
}

static int
tls_dialer_get_tls(void *arg, nng_tls_config **cfg)
{
	tls_dialer *d = arg;
	nni_mtx_lock(&d->lk);
	*cfg = d->cfg;
	nni_mtx_unlock(&d->lk);
	return (0);
}

static int
tls_dialer_get(void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	tls_dialer *d = arg;

	return (nni_stream_dialer_get(d->d, name, buf, szp, t));
}

static int
tls_dialer_set(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	tls_dialer *d = arg;

	return (nni_stream_dialer_set(d->d, name, buf, sz, t));
}

int
nni_tls_dialer_alloc(nng_stream_dialer **dp, const nng_url *url)
{
	tls_dialer *d;
	int         rv;
	nng_url     my_url;

	memcpy(&my_url, url, sizeof(my_url));
	if (strncmp(my_url.u_scheme, "tls+", 4) == 0) {
		my_url.u_scheme += 4;
	}

	if ((d = NNI_ALLOC_STRUCT(d)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&d->lk);

	if ((rv = nng_stream_dialer_alloc_url(&d->d, &my_url)) != 0) {
		nni_mtx_fini(&d->lk);
		NNI_FREE_STRUCT(d);
		return (rv);
	}
	if ((rv = nng_tls_config_alloc(&d->cfg, NNG_TLS_MODE_CLIENT)) != 0) {
		nng_stream_dialer_free(d->d);
		nni_mtx_fini(&d->lk);
		NNI_FREE_STRUCT(d);
		return (rv);
	}

	// Set the expected outbound hostname
	nng_tls_config_server_name(d->cfg, url->u_hostname);

	d->ops.sd_close   = tls_dialer_close;
	d->ops.sd_free    = tls_dialer_free;
	d->ops.sd_stop    = tls_dialer_stop;
	d->ops.sd_dial    = tls_dialer_dial;
	d->ops.sd_get     = tls_dialer_get;
	d->ops.sd_set     = tls_dialer_set;
	d->ops.sd_get_tls = tls_dialer_get_tls;
	d->ops.sd_set_tls = tls_dialer_set_tls;

	*dp = (void *) d;
	return (rv);
}

typedef struct {
	nng_stream_listener  ops;
	nng_stream_listener *l;
	nng_tls_config      *cfg;
	bool                 started;
	nni_mtx              lk;
} tls_listener;

static void
tls_listener_close(void *arg)
{
	tls_listener *l = arg;
	nng_stream_listener_close(l->l);
}

static void
tls_listener_stop(void *arg)
{
	tls_listener *l = arg;
	nng_stream_listener_close(l->l);
}

static void
tls_listener_free(void *arg)
{
	tls_listener *l;
	if ((l = arg) != NULL) {
		tls_listener_close(l);
		nng_tls_config_free(l->cfg);
		nng_stream_listener_free(l->l);
		nni_mtx_fini(&l->lk);
		NNI_FREE_STRUCT(l);
	}
}

static int
tls_listener_listen(void *arg)
{
	tls_listener *l = arg;
	nni_mtx_lock(&l->lk);
	l->started = true;
	nni_mtx_unlock(&l->lk);
	return (nng_stream_listener_listen(l->l));
}

static void
tls_listener_accept(void *arg, nng_aio *aio)
{
	tls_listener *l = arg;
	int           rv;
	tls_conn     *conn;

	nni_aio_reset(aio);
	if ((rv = tls_alloc(&conn, l->cfg, aio)) != 0) {
		nni_aio_finish_error(aio, rv);
		return;
	}

	if (!nni_aio_start(aio, tls_conn_cancel, conn)) {
		tls_free(conn);
		return;
	}

	nng_stream_listener_accept(l->l, &conn->conn_aio);
}

static int
tls_listener_set_tls(void *arg, nng_tls_config *cfg)
{
	tls_listener   *l = arg;
	nng_tls_config *old;
	if (cfg == NULL) {
		return (NNG_EINVAL);
	}
	nng_tls_config_hold(cfg);

	nni_mtx_lock(&l->lk);
	if (l->started) {
		nni_mtx_unlock(&l->lk);
		nng_tls_config_free(cfg);
		return (NNG_EBUSY);
	}
	old    = l->cfg;
	l->cfg = cfg;
	nni_mtx_unlock(&l->lk);

	nng_tls_config_free(old);
	return (0);
}

static int
tls_listener_get_tls(void *arg, nng_tls_config **cfg)
{
	tls_listener *l = arg;
	nni_mtx_lock(&l->lk);
	*cfg = l->cfg;
	nni_mtx_unlock(&l->lk);
	return (0);
}

static int
tls_listener_get(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	tls_listener *l = arg;

	return (nni_stream_listener_get(l->l, name, buf, szp, t));
}

static int
tls_listener_set(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	tls_listener *l = arg;

	return (nni_stream_listener_set(l->l, name, buf, sz, t));
}

int
nni_tls_listener_alloc(nng_stream_listener **lp, const nng_url *url)
{
	tls_listener *l;
	int           rv;
	nng_url       my_url;

	memcpy(&my_url, url, sizeof(my_url));

	if (strncmp(my_url.u_scheme, "tls+", 4) == 0) {
		my_url.u_scheme += 4;
	}

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&l->lk);

	if ((rv = nng_stream_listener_alloc_url(&l->l, &my_url)) != 0) {
		nni_mtx_fini(&l->lk);
		NNI_FREE_STRUCT(l);
		return (rv);
	}
	if ((rv = nng_tls_config_alloc(&l->cfg, NNG_TLS_MODE_SERVER)) != 0) {
		nng_stream_listener_free(l->l);
		nni_mtx_fini(&l->lk);
		NNI_FREE_STRUCT(l);
		return (rv);
	}
	l->ops.sl_free    = tls_listener_free;
	l->ops.sl_close   = tls_listener_close;
	l->ops.sl_stop    = tls_listener_stop;
	l->ops.sl_accept  = tls_listener_accept;
	l->ops.sl_listen  = tls_listener_listen;
	l->ops.sl_get     = tls_listener_get;
	l->ops.sl_set     = tls_listener_set;
	l->ops.sl_get_tls = tls_listener_get_tls;
	l->ops.sl_set_tls = tls_listener_set_tls;
	*lp               = (void *) l;
	return (0);
}

static void
tls_cancel(nni_aio *aio, void *arg, int rv)
{
	tls_conn *conn = arg;
	nni_mtx_lock(&conn->lock);
	if (aio == nni_list_first(&conn->recv_queue)) {
		nni_aio_abort(&conn->tcp_recv, rv);
	} else if (aio == nni_list_first(&conn->send_queue)) {
		nni_aio_abort(&conn->tcp_send, rv);
	} else if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&conn->lock);
}

// tls_send implements the upper layer stream send operation.
static void
tls_send(void *arg, nni_aio *aio)
{
	tls_conn *conn = arg;

	nni_aio_reset(aio);
	nni_mtx_lock(&conn->lock);
	if (!nni_aio_start(aio, tls_cancel, conn)) {
		nni_mtx_unlock(&conn->lock);
		return;
	}
	if (conn->closed) {
		nni_mtx_unlock(&conn->lock);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_list_append(&conn->send_queue, aio);
	tls_do_send(conn);
	nni_mtx_unlock(&conn->lock);
}

static void
tls_recv(void *arg, nni_aio *aio)
{
	tls_conn *conn = arg;

	nni_aio_reset(aio);
	nni_mtx_lock(&conn->lock);
	if (!nni_aio_start(aio, tls_cancel, conn)) {
		nni_mtx_unlock(&conn->lock);
		return;
	}
	if (conn->closed) {
		nni_mtx_unlock(&conn->lock);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	nni_list_append(&conn->recv_queue, aio);
	tls_do_recv(conn);
	nni_mtx_unlock(&conn->lock);
}

static void
tls_close(void *arg)
{
	tls_conn *conn = arg;

	if (!nni_atomic_flag_test_and_set(&conn->did_close)) {
		nni_mtx_lock(&conn->lock);
		conn->ops.close((void *) (conn + 1));
		tls_tcp_error(conn, NNG_ECLOSED);
		nni_mtx_unlock(&conn->lock);
		nng_stream_close(conn->tcp);
	}
}

static void
tls_stop(void *arg)
{
	tls_conn *conn = arg;

	tls_close(conn);
	nng_stream_stop(conn->tcp);
	nni_aio_stop(&conn->conn_aio);
	nni_aio_stop(&conn->tcp_send);
	nni_aio_stop(&conn->tcp_recv);
}

static int
tls_get_verified(void *arg, void *buf, size_t *szp, nni_type t)
{
	tls_conn *conn = arg;
	bool      v;

	nni_mtx_lock(&conn->lock);
	v = conn->ops.verified((void *) (conn + 1));
	nni_mtx_unlock(&conn->lock);
	return (nni_copyout_bool(v, buf, szp, t));
}

static int
tls_get_peer_cn(void *arg, void *buf, size_t *szp, nni_type t)
{
	NNI_ARG_UNUSED(szp);

	if (t != NNI_TYPE_STRING) {
		return (NNG_EBADTYPE);
	}

	tls_conn *conn = arg;
	nni_mtx_lock(&conn->lock);
	*(char **) buf = conn->ops.peer_cn((void *) (conn + 1));
	nni_mtx_unlock(&conn->lock);
	return (0);
}

static const nni_option tls_options[] = {
	{
	    .o_name = NNG_OPT_TLS_VERIFIED,
	    .o_get  = tls_get_verified,
	},
	{
	    .o_name = NNG_OPT_TLS_PEER_CN,
	    .o_get  = tls_get_peer_cn,
	},
	{
	    .o_name = NULL,
	},
};

static int
tls_get(void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	tls_conn *conn = arg;
	int       rv;

	if ((rv = nni_stream_get(conn->tcp, name, buf, szp, t)) !=
	    NNG_ENOTSUP) {
		return (rv);
	}
	return (nni_getopt(tls_options, name, conn, buf, szp, t));
}

static int
tls_alloc(tls_conn **conn_p, nng_tls_config *cfg, nng_aio *user_aio)
{
	tls_conn             *conn;
	const nng_tls_engine *eng;
	size_t                size;

	eng = cfg->engine;

	nni_mtx_lock(&cfg->lock);
	cfg->busy = true;
	nni_mtx_unlock(&cfg->lock);

	size = NNI_ALIGN_UP(sizeof(*conn)) + eng->conn_ops->size;

	if ((conn = nni_zalloc(size)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (((conn->tcp_send_buf = nni_alloc(NNG_TLS_MAX_SEND_SIZE)) ==
	        NULL) ||
	    ((conn->tcp_recv_buf = nni_alloc(NNG_TLS_MAX_RECV_SIZE)) ==
	        NULL)) {
		tls_free(conn);
		return (NNG_ENOMEM);
	}
	conn->size     = size;
	conn->ops      = *eng->conn_ops;
	conn->engine   = eng;
	conn->user_aio = user_aio;
	conn->cfg      = cfg;

	nni_aio_init(&conn->conn_aio, tls_conn_cb, conn);
	nni_aio_init(&conn->tcp_recv, tls_tcp_recv_cb, conn);
	nni_aio_init(&conn->tcp_send, tls_tcp_send_cb, conn);
	nni_aio_list_init(&conn->send_queue);
	nni_aio_list_init(&conn->recv_queue);
	nni_mtx_init(&conn->lock);
	nni_aio_set_timeout(&conn->conn_aio, NNG_DURATION_INFINITE);
	nni_aio_set_timeout(&conn->tcp_send, NNG_DURATION_INFINITE);
	nni_aio_set_timeout(&conn->tcp_recv, NNG_DURATION_INFINITE);
	nni_atomic_flag_reset(&conn->did_close);

	conn->stream.s_close = tls_close;
	conn->stream.s_free  = tls_free;
	conn->stream.s_stop  = tls_stop;
	conn->stream.s_send  = tls_send;
	conn->stream.s_recv  = tls_recv;
	conn->stream.s_get   = tls_get;

	nng_tls_config_hold(cfg);
	*conn_p = conn;
	return (0);
}

static void
tls_reap(void *arg)
{
	tls_conn *conn = arg;

	tls_stop(conn);
	conn->ops.fini((void *) (conn + 1));
	nni_aio_fini(&conn->conn_aio);
	nni_aio_fini(&conn->tcp_send);
	nni_aio_fini(&conn->tcp_recv);
	nng_stream_free(conn->tcp);
	if (conn->cfg != NULL) {
		nng_tls_config_free(conn->cfg); // this drops our hold on it
	}
	if (conn->tcp_send_buf != NULL) {
		nni_free(conn->tcp_send_buf, NNG_TLS_MAX_SEND_SIZE);
	}
	if (conn->tcp_recv_buf != NULL) {
		nni_free(conn->tcp_recv_buf, NNG_TLS_MAX_RECV_SIZE);
	}
	nni_mtx_fini(&conn->lock);
	NNI_FREE_STRUCT(conn);
}

static void
tls_free(void *arg)
{
	tls_conn *conn = arg;

	nni_reap(&tls_conn_reap_list, conn);
}

static int
tls_start(tls_conn *conn, nng_stream *tcp)
{
	int rv;

	conn->tcp = tcp;
	rv        = conn->ops.init(
            (void *) (conn + 1), conn, (void *) (conn->cfg + 1));
	return (rv);
}

static void
tls_tcp_error(tls_conn *conn, int rv)
{
	// An error here is fatal.  Shut it all down.
	nni_aio *aio;
	nng_stream_close(conn->tcp);
	nni_aio_close(&conn->tcp_send);
	nni_aio_close(&conn->tcp_recv);
	while (((aio = nni_list_first(&conn->send_queue)) != NULL) ||
	    ((aio = nni_list_first(&conn->recv_queue)) != NULL)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
}

static bool
tls_do_handshake(tls_conn *conn)
{
	int rv;
	if (conn->hs_done) {
		return (true);
	}
	rv = conn->ops.handshake((void *) (conn + 1));
	if (rv == NNG_EAGAIN) {
		// We need more data.
		return (false);
	}
	if (rv == 0) {
		conn->hs_done = true;
		return (true);
	}
	tls_tcp_error(conn, rv);
	return (true);
}

static void
tls_do_recv(tls_conn *conn)
{
	nni_aio *aio;

	while ((aio = nni_list_first(&conn->recv_queue)) != NULL) {
		uint8_t *buf = NULL;
		size_t   len = 0;
		nni_iov *iov;
		unsigned nio;
		int      rv;

		nni_aio_get_iov(aio, &nio, &iov);

		for (unsigned i = 0; i < nio; i++) {
			if (iov[i].iov_len != 0) {
				buf = iov[i].iov_buf;
				len = iov[i].iov_len;
				break;
			}
		}
		if (len == 0 || buf == NULL) {
			// Caller has asked to receive "nothing".
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_EINVAL);
			continue;
		}

		rv = conn->ops.recv((void *) (conn + 1), buf, &len);
		if (rv == NNG_EAGAIN) {
			// Nothing more we can do, the engine doesn't
			// have anything else for us (yet).
			return;
		}

		// Unlike the send side, we want to return back to the
		// caller as *soon* as we have some data.
		nni_aio_list_remove(aio);

		if (rv != 0) {
			nni_aio_finish_error(aio, rv);
		} else {
			nni_aio_finish(aio, 0, len);
		}
	}
}

// tls_do_send attempts to send user data.
static void
tls_do_send(tls_conn *conn)
{
	nni_aio *aio;

	while ((aio = nni_list_first(&conn->send_queue)) != NULL) {
		uint8_t *buf = NULL;
		size_t   len = 0;
		nni_iov *iov;
		unsigned nio;
		int      rv;

		nni_aio_get_iov(aio, &nio, &iov);

		for (unsigned i = 0; i < nio; i++) {
			if (iov[i].iov_len != 0) {
				buf = iov[i].iov_buf;
				len = iov[i].iov_len;
				break;
			}
		}
		if (len == 0 || buf == NULL) {
			nni_aio_list_remove(aio);
			// Presumably this means we've completed this
			// one, lets preserve the count, and move to the
			// next.
			nni_aio_finish(aio, 0, nni_aio_count(aio));
			continue;
		}

		// Ask the engine to send.
		rv = conn->ops.send((void *) (conn + 1), buf, &len);
		if (rv == NNG_EAGAIN) {
			// Can't send any more, wait for callback.
			return;
		}

		if (rv != 0) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, rv);
		} else {
			nni_aio_list_remove(aio);
			nni_aio_finish(aio, 0, len);
		}
	}
}

static void
tls_tcp_send_cb(void *arg)
{
	tls_conn *conn = arg;
	nng_aio  *aio  = &conn->tcp_send;
	int       rv;
	size_t    count;

	nni_mtx_lock(&conn->lock);
	conn->tcp_send_active = false;

	if ((rv = nni_aio_result(aio)) != 0) {
		tls_tcp_error(conn, rv);
		nni_mtx_unlock(&conn->lock);
		return;
	}

	count = nni_aio_count(aio);
	NNI_ASSERT(count <= conn->tcp_send_len);
	conn->tcp_send_len -= count;
	conn->tcp_send_tail += count;
	conn->tcp_send_tail %= NNG_TLS_MAX_SEND_SIZE;
	tls_tcp_send_start(conn);

	if (tls_do_handshake(conn)) {
		tls_do_send(conn);
		tls_do_recv(conn);
	}

	nni_mtx_unlock(&conn->lock);
}

static void
tls_tcp_recv_cb(void *arg)
{
	tls_conn *conn = arg;
	nni_aio  *aio  = &conn->tcp_recv;
	int       rv;

	nni_mtx_lock(&conn->lock);

	conn->tcp_recv_pend = false;
	if ((rv = nni_aio_result(aio)) != 0) {
		tls_tcp_error(conn, rv);
		nni_mtx_unlock(&conn->lock);
		return;
	}

	NNI_ASSERT(conn->tcp_recv_len == 0);
	NNI_ASSERT(conn->tcp_recv_off == 0);
	conn->tcp_recv_len = nni_aio_count(aio);

	if (tls_do_handshake(conn)) {
		tls_do_recv(conn);
		tls_do_send(conn);
	}

	nni_mtx_unlock(&conn->lock);
}

static void
tls_tcp_recv_start(tls_conn *conn)
{
	nng_iov iov;

	if (conn->tcp_recv_len != 0) {
		// We already have data in the buffer.
		return;
	}
	if (conn->tcp_recv_pend) {
		// Already have a receive in flight.
		return;
	}
	conn->tcp_recv_off = 0;
	iov.iov_len        = NNG_TLS_MAX_RECV_SIZE;
	iov.iov_buf        = conn->tcp_recv_buf;

	conn->tcp_recv_pend = true;
	nng_aio_set_iov(&conn->tcp_recv, 1, &iov);

	nng_stream_recv(conn->tcp, &conn->tcp_recv);
}

static void
tls_tcp_send_start(tls_conn *conn)
{
	nni_iov  iov[2];
	unsigned nio = 0;
	size_t   len;
	size_t   tail;
	size_t   head;

	if (conn->tcp_send_active) {
		return;
	}
	if (conn->tcp_send_len == 0) {
		return;
	}
	len  = conn->tcp_send_len;
	head = conn->tcp_send_head;
	tail = conn->tcp_send_tail;

	while (len > 0) {
		size_t cnt;
		NNI_ASSERT(nio < 2);
		if (tail < head) {
			cnt = head - tail;
		} else {
			cnt = NNG_TLS_MAX_SEND_SIZE - tail;
		}
		if (cnt > len) {
			cnt = len;
		}
		iov[nio].iov_buf = conn->tcp_send_buf + tail;
		iov[nio].iov_len = cnt;
		len -= cnt;
		tail += cnt;
		tail %= NNG_TLS_MAX_SEND_SIZE;
		nio++;
	}
	conn->tcp_send_active = true;
	nni_aio_set_iov(&conn->tcp_send, nio, iov);
	nng_stream_send(conn->tcp, &conn->tcp_send);
}

int
nng_tls_engine_send(void *arg, const uint8_t *buf, size_t *szp)
{
	tls_conn *conn = arg;
	size_t    len  = *szp;
	size_t    head = conn->tcp_send_head;
	size_t    tail = conn->tcp_send_tail;
	size_t    space;
	size_t    cnt;

	space = NNG_TLS_MAX_SEND_SIZE - conn->tcp_send_len;

	if (space == 0) {
		return (NNG_EAGAIN);
	}

	if (conn->closed) {
		return (NNG_ECLOSED);
	}

	if (len > space) {
		len = space;
	}

	// We are committed at this point to sending out len bytes.
	// Update this now, so that we can use len to update.
	*szp = len;
	conn->tcp_send_len += len;
	NNI_ASSERT(conn->tcp_send_len <= NNG_TLS_MAX_SEND_SIZE);

	while (len > 0) {
		if (head >= tail) {
			cnt = NNG_TLS_MAX_SEND_SIZE - head;
		} else {
			cnt = tail - head;
		}
		if (cnt > len) {
			cnt = len;
		}

		memcpy(conn->tcp_send_buf + head, buf, cnt);
		buf += cnt;
		head += cnt;
		head %= NNG_TLS_MAX_SEND_SIZE;
		len -= cnt;
	}

	conn->tcp_send_head = head;

	tls_tcp_send_start(conn);
	return (0);
}

int
nng_tls_engine_recv(void *arg, uint8_t *buf, size_t *szp)
{
	tls_conn *conn = arg;
	size_t    len  = *szp;

	if (conn->closed) {
		return (NNG_ECLOSED);
	}
	if (conn->tcp_recv_len == 0) {
		tls_tcp_recv_start(conn);
		return (NNG_EAGAIN);
	}
	if (len > conn->tcp_recv_len) {
		len = conn->tcp_recv_len;
	}
	memcpy(buf, conn->tcp_recv_buf + conn->tcp_recv_off, len);
	conn->tcp_recv_off += len;
	conn->tcp_recv_len -= len;

	// If we still have data left in the buffer, then the following
	// call is a no-op.
	tls_tcp_recv_start(conn);

	*szp = len;
	return (0);
}

int
nng_tls_config_cert_key_file(
    nng_tls_config *cfg, const char *path, const char *pass)
{
	int    rv;
	void  *data;
	size_t size;
	char  *pem;

	if ((rv = nni_file_get(path, &data, &size)) != 0) {
		return (rv);
	}
	if ((pem = nni_zalloc(size + 1)) == NULL) {
		nni_free(data, size);
		return (NNG_ENOMEM);
	}
	memcpy(pem, data, size);
	nni_free(data, size);
	rv = nng_tls_config_own_cert(cfg, pem, pem, pass);
	nni_free(pem, size + 1);
	return (rv);
}

int
nng_tls_config_ca_file(nng_tls_config *cfg, const char *path)
{
	int    rv;
	void  *data;
	size_t size;
	char  *pem;

	if ((rv = nni_file_get(path, &data, &size)) != 0) {
		return (rv);
	}
	if ((pem = nni_zalloc(size + 1)) == NULL) {
		nni_free(data, size);
		return (NNG_ENOMEM);
	}
	memcpy(pem, data, size);
	nni_free(data, size);
	if (strstr(pem, "-----BEGIN X509 CRL-----") != NULL) {
		rv = nng_tls_config_ca_chain(cfg, pem, pem);
	} else {
		rv = nng_tls_config_ca_chain(cfg, pem, NULL);
	}
	nni_free(pem, size + 1);
	return (rv);
}

int
nng_tls_config_version(
    nng_tls_config *cfg, nng_tls_version min_ver, nng_tls_version max_ver)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.version((void *) (cfg + 1), min_ver, max_ver);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_server_name(nng_tls_config *cfg, const char *name)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.server((void *) (cfg + 1), name);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_ca_chain(
    nng_tls_config *cfg, const char *certs, const char *crl)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.ca_chain((void *) (cfg + 1), certs, crl);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_own_cert(
    nng_tls_config *cfg, const char *cert, const char *key, const char *pass)
{
	int rv;
	nni_mtx_lock(&cfg->lock);
	// NB: we cannot set the key if we already have done so.
	// This is because some lower layers create a "stack" of keys
	// and certificates, and this will almost certainly lead to confusion.
	if (cfg->busy || cfg->key_is_set) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.own_cert((void *) (cfg + 1), cert, key, pass);
		if (rv == 0) {
			cfg->key_is_set = true;
		}
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_psk(nng_tls_config *cfg, const char *identity,
    const uint8_t *key, size_t key_len)
{
	int rv;
	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.psk((void *) (cfg + 1), identity, key, key_len);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_auth_mode(nng_tls_config *cfg, nng_tls_auth_mode mode)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.auth((void *) (cfg + 1), mode);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_alloc(nng_tls_config **cfg_p, nng_tls_mode mode)
{
	nng_tls_config       *cfg;
	const nng_tls_engine *eng;
	size_t                size;
	int                   rv;

	eng = nni_atomic_get_ptr(&tls_engine);

	if (eng == NULL) {
		return (NNG_ENOTSUP);
	}

	size = NNI_ALIGN_UP(sizeof(*cfg)) + eng->config_ops->size;

	if ((cfg = nni_zalloc(size)) == NULL) {
		return (NNG_ENOMEM);
	}

	cfg->ops    = *eng->config_ops;
	cfg->size   = size;
	cfg->engine = eng;
	cfg->ref    = 1;
	cfg->busy   = false;
	nni_mtx_init(&cfg->lock);

	if ((rv = cfg->ops.init((void *) (cfg + 1), mode)) != 0) {
		nni_free(cfg, cfg->size);
		return (rv);
	}
	*cfg_p = cfg;
	return (0);
}

void
nng_tls_config_free(nng_tls_config *cfg)
{
	nni_mtx_lock(&cfg->lock);
	cfg->ref--;
	if (cfg->ref != 0) {
		nni_mtx_unlock(&cfg->lock);
		return;
	}
	nni_mtx_unlock(&cfg->lock);
	nni_mtx_fini(&cfg->lock);
	cfg->ops.fini((void *) (cfg + 1));
	nni_free(cfg, cfg->size);
}

void
nng_tls_config_hold(nng_tls_config *cfg)
{
	nni_mtx_lock(&cfg->lock);
	cfg->ref++;
	nni_mtx_unlock(&cfg->lock);
}

const char *
nng_tls_engine_name(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? "none" : eng->name);
}

const char *
nng_tls_engine_description(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? "" : eng->description);
}

bool
nng_tls_engine_fips_mode(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? false : eng->fips_mode);
}

int
nng_tls_engine_register(const nng_tls_engine *engine)
{
	if (engine->version != NNG_TLS_ENGINE_VERSION) {
		nng_log_err("NNG-TLS-ENGINE-VER",
		    "TLS Engine version mismatch: %d != %d", engine->version,
		    NNG_TLS_ENGINE_VERSION);
		return (NNG_ENOTSUP);
	}
	nng_log_info("NNG-TLS-INFO", "TLS Engine: %s", engine->description);
	nni_atomic_set_ptr(&tls_engine, (void *) engine);
	return (0);
}

#ifdef NNG_TLS_ENGINE_INIT
extern int NNG_TLS_ENGINE_INIT(void);
#else
static int
NNG_TLS_ENGINE_INIT(void)
{
	return (0);
}
#endif

#ifdef NNG_TLS_ENGINE_FINI
extern void NNG_TLS_ENGINE_FINI(void);
#else
static void
NNG_TLS_ENGINE_FINI(void)
{
}
#endif

int
nni_tls_sys_init(void)
{
	int rv;

	rv = NNG_TLS_ENGINE_INIT();
	if (rv != 0) {
		return (rv);
	}
	return (0);
}

void
nni_tls_sys_fini(void)
{
	NNG_TLS_ENGINE_FINI();
}

#else // NNG_SUPP_TLS

// Provide stubs for the case where TLS is not enabled.
void
nni_tls_config_fini(nng_tls_config *cfg)
{
	NNI_ARG_UNUSED(cfg);
}

int
nni_tls_config_init(nng_tls_config **cpp, enum nng_tls_mode mode)
{
	NNI_ARG_UNUSED(cpp);
	NNI_ARG_UNUSED(mode);
	return (NNG_ENOTSUP);
}

void
nni_tls_config_hold(nng_tls_config *cfg)
{
	NNI_ARG_UNUSED(cfg);
}

int
nng_tls_config_server_name(nng_tls_config *cfg, const char *name)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(name);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_auth_mode(nng_tls_config *cfg, nng_tls_auth_mode mode)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(mode);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_ca_chain(
    nng_tls_config *cfg, const char *certs, const char *crl)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(certs);
	NNI_ARG_UNUSED(crl);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_own_cert(
    nng_tls_config *cfg, const char *cert, const char *key, const char *pass)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(cert);
	NNI_ARG_UNUSED(key);
	NNI_ARG_UNUSED(pass);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_ca_file(nng_tls_config *cfg, const char *path)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(path);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_cert_key_file(
    nng_tls_config *cfg, const char *path, const char *pass)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(path);
	NNI_ARG_UNUSED(pass);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_key(nng_tls_config *cfg, const uint8_t *key, size_t size)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(key);
	NNI_ARG_UNUSED(size);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_pass(nng_tls_config *cfg, const char *pass)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(pass);
	return (NNG_ENOTSUP);
}

int
nng_tls_config_alloc(nng_tls_config **cfgp, nng_tls_mode mode)
{

	NNI_ARG_UNUSED(cfgp);
	NNI_ARG_UNUSED(mode);
	return (NNG_ENOTSUP);
}

void
nng_tls_config_hold(nng_tls_config *cfg)
{
	NNI_ARG_UNUSED(cfg);
}

void
nng_tls_config_free(nng_tls_config *cfg)
{
	NNI_ARG_UNUSED(cfg);
}

int
nng_tls_config_version(
    nng_tls_config *cfg, nng_tls_version min_ver, nng_tls_version max_ver)
{
	NNI_ARG_UNUSED(cfg);
	NNI_ARG_UNUSED(min_ver);
	NNI_ARG_UNUSED(max_ver);
	return (NNG_ENOTSUP);
}

int
nni_tls_dialer_alloc(nng_stream_dialer **dp, const nng_url *url)
{
	NNI_ARG_UNUSED(dp);
	NNI_ARG_UNUSED(url);
	return (NNG_ENOTSUP);
}

int
nni_tls_listener_alloc(nng_stream_listener **lp, const nng_url *url)
{
	NNI_ARG_UNUSED(lp);
	NNI_ARG_UNUSED(url);
	return (NNG_ENOTSUP);
}

int
nni_tls_checkopt(const char *nm, const void *buf, size_t sz, nni_type t)
{
	NNI_ARG_UNUSED(nm);
	NNI_ARG_UNUSED(buf);
	NNI_ARG_UNUSED(sz);
	NNI_ARG_UNUSED(t);
	return (NNG_ENOTSUP);
}

const char *
nng_tls_engine_name(void)
{
	return ("none");
}

const char *
nng_tls_engine_description(void)
{
	return ("");
}

bool
nng_tls_engine_fips_mode(void)
{
	return (false);
}

int
nng_tls_engine_register(const nng_tls_engine *engine)
{
	NNI_ARG_UNUSED(engine);
	return (NNG_ENOTSUP);
}

int
nni_tls_sys_init(void)
{
	return (0);
}

void
nni_tls_sys_fini(void)
{
}

#endif // !NNG_SUPP_TLS
