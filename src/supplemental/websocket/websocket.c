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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "nng/http.h"
#include "supplemental/http/http_api.h"

#include "base64.h"
#include "sha1.h"
#include "websocket.h"

// This should be removed or handled differently in the future.
typedef int (*nni_ws_listen_hook)(void *, nng_http *);

// We have chosen to be a bit more stringent in the size of the frames that
// we send, while we more generously allow larger incoming frames.  These
// may be tuned by options.
#define WS_DEF_RECVMAX (1U << 20)    // 1MB Message limit (message mode only)
#define WS_DEF_MAXRXFRAME (1U << 20) // 1MB Frame size (recv)
#define WS_DEF_MAXTXFRAME (1U << 16) // 64KB Frame size (send)

// Alias for checking the prefix of a string.
#define startswith(s, t) (strncmp(s, t, strlen(t)) == 0)

// Pre-defined types for some prototypes.  These are from other subsystems.
typedef struct ws_frame ws_frame;

typedef struct ws_header {
	nni_list_node node;
	char         *name;
	char         *value;
} ws_header;

struct nni_ws {
	nng_stream       ops;
	nni_list_node    node;
	nni_reap_node    reap;
	bool             server;
	bool             closed;      // received a close, or initiated a close
	bool             peer_closed; // we received a close frame
	bool             ready;
	bool             wclose;
	bool             isstream;
	bool             inmsg;
	bool             send_text;
	bool             recv_text;
	bool             recv_res;
	nni_mtx          mtx;
	nni_list         sendq;
	nni_list         recvq;
	nni_list         txq;
	nni_list         rxq;
	ws_frame        *txframe;
	ws_frame        *rxframe;
	nni_aio          txaio; // physical aios
	nni_aio          rxaio;
	nni_aio          closeaio; // used for lingering/draining close
	nni_aio          httpaio;
	nni_aio          connaio; // connect aio
	nni_aio         *useraio; // user aio, during HTTP negotiation
	nng_http        *http;
	size_t           maxframe;
	size_t           fragsize;
	size_t           recvmax; // largest message size
	nni_ws_listener *listener;
	nni_ws_dialer   *dialer;
	char             keybuf[29]; // key on client, accept on server
	struct {
		nni_http_header connection;
		nni_http_header upgrade;
		nni_http_header wsaccept;
		nni_http_header wskey;
		nni_http_header wsproto;
		nni_http_header wsversion;
	} hdrs;
};

struct nni_ws_listener {
	nng_stream_listener ops;
	nni_http_server    *server;
	char               *proto;
	nni_mtx             mtx;
	nni_cv              cv;
	nni_list            pend;
	nni_list            reply;
	nni_list            aios;
	nng_url            *url;
	bool                started;
	bool                closed;
	bool                isstream;
	bool                send_text;
	bool                recv_text;
	nni_http_handler   *handler;
	nni_ws_listen_hook  hookfn;
	void               *hookarg;
	nni_list            headers; // response headers
	size_t              maxframe;
	size_t              fragsize;
	size_t              recvmax; // largest message size
};

// The dialer tracks user aios in two lists. The first list is for aios
// waiting for the http connection to be established, while the second
// are waiting for the HTTP negotiation to complete.  We keep two lists
// so we know whether to initiate another outgoing connection after the
// completion of an earlier connection.  (We don't want to establish
// requests when we already have connects negotiating.)
struct nni_ws_dialer {
	nng_stream_dialer ops;
	nni_http_req     *req;
	nni_http_res     *res;
	nni_http_client  *client;
	nni_mtx           mtx;
	nni_cv            cv;
	char             *proto;
	nng_url          *url;
	nni_list          wspend; // ws structures still negotiating
	bool              closed;
	bool              isstream;
	bool              send_text;
	bool              recv_text;
	nni_list          headers; // request headers
	size_t            maxframe;
	size_t            fragsize;
	size_t            recvmax;
};

typedef enum ws_type {
	WS_CONT   = 0x0,
	WS_TEXT   = 0x1,
	WS_BINARY = 0x2,
	WS_CLOSE  = 0x8,
	WS_PING   = 0x9,
	WS_PONG   = 0xA,
} ws_type;

typedef enum ws_reason {
	WS_CLOSE_NORMAL_CLOSE  = 1000,
	WS_CLOSE_GOING_AWAY    = 1001,
	WS_CLOSE_PROTOCOL_ERR  = 1002,
	WS_CLOSE_UNSUPP_FORMAT = 1003,
	WS_CLOSE_INVALID_DATA  = 1007,
	WS_CLOSE_POLICY        = 1008,
	WS_CLOSE_TOO_BIG       = 1009,
	WS_CLOSE_NO_EXTENSION  = 1010,
	WS_CLOSE_INTERNAL      = 1011,
} ws_reason;

struct ws_frame {
	nni_list_node node;
	uint8_t       head[14];   // maximum header size
	uint8_t       mask[4];    // read by server, sent by client
	uint8_t       sdata[125]; // short data (for short frames only)
	size_t        hlen;       // header length
	size_t        len;        // payload length
	enum ws_type  op;
	bool          final;
	bool          masked;
	size_t        asize; // allocated size
	uint8_t      *adata;
	uint8_t      *buf;
	nng_aio      *aio;
};

static void ws_send_close(nni_ws *ws, uint16_t code);
static void ws_conn_cb(void *);
static void ws_close_cb(void *);
static void ws_read_cb(void *);
static void ws_write_cb(void *);
static void ws_close_error(nni_ws *ws, uint16_t code);

static void    ws_str_free(void *);
static void    ws_str_close(void *);
static void    ws_str_send(void *, nng_aio *);
static void    ws_str_recv(void *, nng_aio *);
static nng_err ws_str_get(void *, const char *, void *, size_t *, nni_type);

static void ws_listener_close(void *);
static void ws_listener_free(void *);

static int
ws_check_string(const void *v, size_t sz, nni_opt_type t)
{
	if (t != NNI_TYPE_STRING) {
		return (NNG_EBADTYPE);
	}
	if (nni_strnlen(v, sz) >= sz) {
		return (NNG_EINVAL);
	}
	return (0);
}

static int
ws_set_header_ext(nni_list *l, const char *n, const char *v, bool strip_dups)
{
	ws_header *hdr;
	char      *nv;

	if ((nv = nni_strdup(v)) == NULL) {
		return (NNG_ENOMEM);
	}

	if (strip_dups) {
		NNI_LIST_FOREACH (l, hdr) {
			if (nni_strcasecmp(hdr->name, n) == 0) {
				nni_strfree(hdr->value);
				hdr->value = nv;
				return (0);
			}
		}
	}

	if ((hdr = NNI_ALLOC_STRUCT(hdr)) == NULL) {
		nni_strfree(nv);
		return (NNG_ENOMEM);
	}
	if ((hdr->name = nni_strdup(n)) == NULL) {
		nni_strfree(nv);
		NNI_FREE_STRUCT(hdr);
		return (NNG_ENOMEM);
	}
	hdr->value = nv;
	nni_list_append(l, hdr);
	return (0);
}

static int
ws_set_header(nni_list *l, const char *n, const char *v)
{
	return (ws_set_header_ext(l, n, v, true));
}

// This looks, case independently for a word in a list, which is either
// space or comma separated.
static bool
ws_contains_word(const char *phrase, const char *word)
{
	size_t len = strlen(word);

	while ((phrase != NULL) && (*phrase != '\0')) {
		if ((nni_strncasecmp(phrase, word, len) == 0) &&
		    ((phrase[len] == 0) || (phrase[len] == ' ') ||
		        (phrase[len] == ','))) {
			return (true);
		}
		// Skip to next word.
		if ((phrase = strchr(phrase, ' ')) != NULL) {
			while ((*phrase == ' ') || (*phrase == ',')) {
				phrase++;
			}
		}
	}
	return (false);
}

// input is base64 challenge, output is the accepted.  input should be
// 24 character base 64, output is 28 character base64 reply.  (output
// must be large enough to hold 29 bytes to allow for termination.)
// Returns 0 on success, NNG_EINVAL if the input is malformed somehow.
static int
ws_make_accept(const char *key, char *accept)
{
	uint8_t      digest[20];
	nni_sha1_ctx ctx;

#define WS_KEY_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_KEY_GUIDLEN 36

	if (strlen(key) != 24) {
		return (NNG_EINVAL);
	}

	nni_sha1_init(&ctx);
	nni_sha1_update(&ctx, key, 24);
	nni_sha1_update(&ctx, WS_KEY_GUID, WS_KEY_GUIDLEN);
	nni_sha1_final(&ctx, digest);

	nni_base64_encode(digest, 20, accept, 28);
	accept[28] = '\0';
	return (0);
}

static void
ws_frame_fini(ws_frame *frame)
{
	if (frame->asize != 0) {
		nni_free(frame->adata, frame->asize);
	}
	NNI_FREE_STRUCT(frame);
}

static void
ws_mask_frame(ws_frame *frame)
{
	uint32_t r;
	// frames sent by client need mask.
	if (frame->masked) {
		return;
	}
	r = nni_random();
	NNI_PUT32(frame->mask, r);
	for (size_t i = 0; i < frame->len; i++) {
		frame->buf[i] ^= frame->mask[i % 4];
	}
	memcpy(frame->head + frame->hlen, frame->mask, 4);
	frame->hlen += 4;
	frame->head[1] |= 0x80; // set masked bit
	frame->masked = true;
}

static void
ws_unmask_frame(ws_frame *frame)
{
	// frames sent by client need mask.
	if (!frame->masked) {
		return;
	}
	for (size_t i = 0; i < frame->len; i++) {
		frame->buf[i] ^= frame->mask[i % 4];
	}
	frame->hlen -= 4;
	frame->head[1] &= 0x7f; // clear masked bit
	frame->masked = false;
}

static int
ws_msg_init_control(
    ws_frame **framep, nni_ws *ws, uint8_t op, const uint8_t *buf, size_t len)
{
	ws_frame *frame;

	if (len > 125) {
		return (NNG_EINVAL);
	}

	if ((frame = NNI_ALLOC_STRUCT(frame)) == NULL) {
		return (NNG_ENOMEM);
	}

	memcpy(frame->sdata, buf, len);
	frame->len     = len;
	frame->final   = true;
	frame->op      = op;
	frame->head[0] = op | 0x80; // final frame (control)
	frame->head[1] = len & 0x7F;
	frame->hlen    = 2;
	frame->buf     = frame->sdata;
	frame->asize   = 0;

	if (ws->server) {
		frame->masked = false;
	} else {
		ws_mask_frame(frame);
	}

	*framep = frame;
	return (0);
}

static int
ws_frame_prep_tx(nni_ws *ws, ws_frame *frame)
{
	nng_aio *aio = frame->aio;
	nni_iov *iov;
	unsigned niov;
	size_t   len;
	uint8_t *buf;

	// Figure out how much we need for the entire aio.
	frame->len = 0;
	nni_aio_get_iov(aio, &niov, &iov);
	for (unsigned i = 0; i < niov; i++) {
		frame->len += iov[i].iov_len;
	}

	if ((frame->len > ws->fragsize) && (ws->fragsize > 0)) {
		// Limit it to a single frame per policy (fragsize), as needed.
		frame->len = ws->fragsize;
		// For stream mode, we constrain ourselves to one frame
		// per message. Submitter may see a partial transmit, and
		// should resubmit as needed.  For message mode, we will
		// continue to resubmit.
		frame->final = ws->isstream ? true : false;
	} else {
		// It all fits in this frame (which might not be the first),
		// so we're done.
		frame->final = true;
	}
	// Potentially allocate space for the data if we need to.
	// Note that an empty message is legal.
	if ((frame->asize < frame->len) && (frame->len > 0)) {
		nni_free(frame->adata, frame->asize);
		frame->adata = nni_alloc(frame->len);
		if (frame->adata == NULL) {
			frame->asize = 0;
			return (NNG_ENOMEM);
		}
		frame->asize = frame->len;
		frame->buf   = frame->adata;
	}
	buf = frame->buf;

	// Now copy the data into the frame.
	len = frame->len;
	while (len != 0) {
		size_t n = len;
		if (n > iov->iov_len) {
			n = iov->iov_len;
		}
		memcpy(buf, iov->iov_buf, n);
		iov++;
		len -= n;
		buf += n;
	}

	if (nni_aio_count(aio) == 0) {
		// This is the first frame.
		if (ws->send_text) {
			frame->op = WS_TEXT;
		} else {
			frame->op = WS_BINARY;
		}
	} else {
		frame->op = WS_CONT;
	}

	// Populate the frame header.
	frame->head[0] = frame->op;
	frame->hlen    = 2;
	if (frame->final) {
		frame->head[0] |= 0x80; // final frame bit
	}
	if (frame->len < 126) {
		frame->head[1] = frame->len & 0x7f;
	} else if (frame->len < 65536) {
		frame->head[1] = 126;
		NNI_PUT16(frame->head + 2, (frame->len & 0xffff));
		frame->hlen += 2;
	} else {
		frame->head[1] = 127;
		NNI_PUT64(frame->head + 2, (uint64_t) frame->len);
		frame->hlen += 8;
	}

	// If we are on the client, then we need to mask the frame.
	frame->masked = false;
	if (!ws->server) {
		ws_mask_frame(frame);
	}
	return (0);
}

static void
ws_close_cb(void *arg)
{
	nni_ws   *ws = arg;
	ws_frame *frame;

	nni_aio_close(&ws->txaio);
	nni_aio_close(&ws->rxaio);
	nni_aio_close(&ws->httpaio);

	// Either we sent a close frame, or we didn't.  Either way,
	// we are done, and its time to abort everything else.
	nni_mtx_lock(&ws->mtx);

	nni_http_conn_close(ws->http);

	while ((frame = nni_list_first(&ws->txq)) != NULL) {
		nni_list_remove(&ws->txq, frame);
		if (frame->aio != NULL) {
			nni_aio_list_remove(frame->aio);
			nni_aio_finish_error(frame->aio, NNG_ECLOSED);
		}
		ws_frame_fini(frame);
	}

	// Any txframe should have been killed with its wmsg.
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_close(nni_ws *ws, uint16_t code)
{
	nng_aio *aio;

	// Receive stuff gets aborted always.  No further receives
	// once we get a close.
	while ((aio = nni_list_first(&ws->recvq)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	// If were closing "gracefully", then don't abort in-flight
	// stuff yet.  Note that reads should have stopped already.
	// However, we *do* abort any inflight HTTP negotiation, or
	// pending connect request.
	if (!ws->closed) {
		// ABORT connection negotiation.
		nni_aio_close(&ws->connaio);
		nni_aio_close(&ws->httpaio);
		ws_send_close(ws, code);
	}
}

static void
ws_start_write(nni_ws *ws)
{
	ws_frame *frame;
	nni_iov   iov[2];
	int       niov;

	if ((ws->txframe != NULL) || (!ws->ready)) {
		return; // busy
	}

	if ((frame = nni_list_first(&ws->txq)) == NULL) {
		return; // nothing to send
	}
	NNI_ASSERT(frame != NULL);
	nni_list_remove(&ws->txq, frame);

	// Push it out.
	ws->txframe    = frame;
	niov           = 1;
	iov[0].iov_len = frame->hlen;
	iov[0].iov_buf = frame->head;
	if (frame->len > 0) {
		niov++;
		iov[1].iov_len = frame->len;
		iov[1].iov_buf = frame->buf;
	}
	nni_aio_set_iov(&ws->txaio, niov, iov);
	nni_http_write_full(ws->http, &ws->txaio);
}

static void
ws_cancel_close(nni_aio *aio, void *arg, nng_err rv)
{
	nni_ws *ws = arg;
	nni_mtx_lock(&ws->mtx);
	if (ws->wclose) {
		ws->wclose = false;
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_write_cb(void *arg)
{
	nni_ws   *ws = arg;
	ws_frame *frame;
	nni_aio  *aio;
	int       rv;

	nni_mtx_lock(&ws->mtx);

	if ((frame = ws->txframe) == NULL) {
		nni_mtx_unlock(&ws->mtx);
		return;
	}
	ws->txframe = NULL;

	if (frame->op == WS_CLOSE) {
		// If this was a close frame, we are done.
		// No other messages may succeed..
		ws->txframe = NULL;
		ws_frame_fini(frame);
		while ((frame = nni_list_first(&ws->txq)) != NULL) {
			nni_list_remove(&ws->txq, frame);
			if ((aio = frame->aio) != NULL) {
				frame->aio = NULL;
				nni_aio_list_remove(aio);
				nni_aio_finish_error(aio, NNG_ECLOSED);
				ws_frame_fini(frame);
			}
		}
		if (ws->peer_closed) {
			if (ws->wclose) { // could assert this?
				ws->wclose = false;
				nni_aio_finish(&ws->closeaio, 0, 0);
			}
		}
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	aio = frame->aio;
	if ((rv = nni_aio_result(&ws->txaio)) != 0) {
		// if tx fails, we can't send a close frame either
		// we expect the caller to just close this connection
		frame->aio = NULL;
		if (aio != NULL) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, rv);
		}
		ws_frame_fini(frame);
		ws->closed = true;
		nni_http_conn_close(ws->http);
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	if (aio != NULL) {
		nni_aio_iov_advance(aio, frame->len);
		nni_aio_bump_count(aio, frame->len);
		if (frame->final) {
			frame->aio = NULL;
			nni_aio_list_remove(aio);
		} else {
			// Clear the aio so that we won't attempt to finish
			// it outside the lock
			aio = NULL;
		}
	}

	if (frame->final) {
		ws_frame_fini(frame);
	} else {
		// This one cannot fail here, since we only do allocation
		// at initial scheduling.
		ws_frame_prep_tx(ws, frame);
		// Schedule at end.  This permits other frames to interleave.
		nni_list_append(&ws->txq, frame);
	}

	ws_start_write(ws);
	nni_mtx_unlock(&ws->mtx);

	// We attempt to finish the operation synchronously, outside the lock.
	if (aio != NULL) {
		nni_msg *msg;
		// Successful send, don't leak the message!
		if ((msg = nni_aio_get_msg(aio)) != NULL) {
			nni_aio_set_msg(aio, NULL);
			nni_msg_free(msg);
		}
		nni_aio_finish_sync(aio, 0, nni_aio_count(aio));
	}
}

static void
ws_write_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	nni_ws   *ws = arg;
	ws_frame *frame;

	// Is this aio active?  We can tell by looking at the active tx frame.

	nni_mtx_lock(&ws->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&ws->mtx);
		return;
	}
	frame = nni_aio_get_prov_data(aio);
	if (frame == ws->txframe) {
		nni_aio_abort(&ws->txaio, rv);
		// We will wait for callback on the txaio to finish aio.
	} else {
		// If scheduled, just need to remove node and complete it.
		nni_list_remove(&ws->txq, frame);
		frame->aio = NULL;
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
		ws_frame_fini(frame);
	}
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_send_close(nni_ws *ws, uint16_t code)
{
	ws_frame *frame;
	uint8_t   buf[sizeof(uint16_t)];
	int       rv;
	nni_aio  *aio = &ws->closeaio;

	NNI_PUT16(buf, code);

	if (ws->closed || !ws->ready) {
		return;
	}
	ws->closed = true;

	nni_aio_reset(aio);
	ws->wclose = true;
	rv = ws_msg_init_control(&frame, ws, WS_CLOSE, buf, sizeof(buf));
	if (rv != 0) {
		ws->wclose = false;
		nni_aio_finish_error(aio, rv);
		return;
	}
	if (!nni_aio_start(aio, ws_cancel_close, ws)) {
		ws->wclose = false;
		ws_frame_fini(frame);
		return;
	}
	// This gets inserted at the head.
	nni_list_prepend(&ws->txq, frame);
	ws_start_write(ws);
}

static void
ws_send_control(nni_ws *ws, uint8_t op, uint8_t *buf, size_t len)
{
	ws_frame *frame;

	// Note that we do not care if this works or not.  So no AIO needed.

	if ((ws->closed) ||
	    (ws_msg_init_control(&frame, ws, op, buf, len) != 0)) {
		return;
	}

	// Control frames at head of list.  (Note that this may preempt
	// the close frame or other ping/pong requests.  Oh well.)
	nni_list_prepend(&ws->txq, frame);
	ws_start_write(ws);
}

static void
ws_start_read(nni_ws *ws)
{
	ws_frame *frame;
	nni_aio  *aio;
	nni_iov   iov;

	if ((ws->rxframe != NULL) || ws->closed) {
		return; // already reading or closed
	}

	// If nobody is waiting for recv, and we already have a data
	// frame, stop reading.  This keeps us from buffering infinitely.
	if (nni_list_empty(&ws->recvq) && !nni_list_empty(&ws->rxq)) {
		return;
	}

	if ((frame = NNI_ALLOC_STRUCT(frame)) == NULL) {
		if ((aio = nni_list_first(&ws->recvq)) != NULL) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_ENOMEM);
		}
		ws_close(ws, WS_CLOSE_INTERNAL);
		return;
	}

	// Note that the frame is *not* associated with the message
	// as yet, because we don't know if that's right until we receive it.
	frame->hlen = 0;
	frame->len  = 0;
	ws->rxframe = frame;

	aio         = &ws->rxaio;
	iov.iov_len = 2; // We want the first two bytes.
	iov.iov_buf = frame->head;
	nni_aio_set_iov(aio, 1, &iov);
	nni_http_read_full(ws->http, aio);
}

static void
ws_read_finish_str(nni_ws *ws)
{
	for (;;) {
		nni_aio  *aio;
		nni_iov  *iov;
		unsigned  niov;
		ws_frame *frame;

		if ((aio = nni_list_first(&ws->recvq)) == NULL) {
			return;
		}

		if ((frame = nni_list_first(&ws->rxq)) == NULL) {
			return;
		}

		// Discard 0 length frames -- in stream mode they are not used.
		if (frame->len == 0) {
			nni_list_remove(&ws->rxq, frame);
			ws_frame_fini(frame);
			continue;
		}

		// We are completing this aio one way or the other.
		nni_aio_list_remove(aio);
		nni_aio_get_iov(aio, &niov, &iov);

		while ((frame != NULL) && (niov != 0)) {
			size_t n;

			if ((n = frame->len) > iov->iov_len) {
				// This eats the entire iov.
				n = iov->iov_len;
			}
			if (n != 0) {
				memcpy(iov->iov_buf, frame->buf, n);
				iov->iov_buf = ((uint8_t *) iov->iov_buf) + n;
				iov->iov_len -= n;
				if (iov->iov_len == 0) {
					iov++;
					niov--;
				}
			}

			if (frame->len == n) {
				nni_list_remove(&ws->rxq, frame);
				ws_frame_fini(frame);
				frame = nni_list_first(&ws->rxq);
			} else {
				frame->len -= n;
				frame->buf += n;
			}

			nni_aio_bump_count(aio, n);
		}

		nni_aio_finish(aio, 0, nni_aio_count(aio));
	}
}

static void
ws_read_finish_msg(nni_ws *ws)
{
	nni_aio  *aio;
	size_t    len;
	ws_frame *frame;
	nni_msg  *msg;
	int       rv;
	uint8_t  *body;

	// If we have no data, no waiter, or have not received the complete
	// message yet, then there is nothing to do.
	if (ws->inmsg || nni_list_empty(&ws->rxq) ||
	    ((aio = nni_list_first(&ws->recvq)) == NULL)) {
		return;
	}

	// At this point, we have both a complete message in the queue (and
	// there should not be any frames other than the for the message),
	// and a waiting reader.
	len = 0;
	NNI_LIST_FOREACH (&ws->rxq, frame) {
		len += frame->len;
	}

	nni_aio_list_remove(aio);

	if ((rv = nni_msg_alloc(&msg, len)) != 0) {
		nni_aio_finish_error(aio, rv);
		ws_close_error(ws, WS_CLOSE_INTERNAL);
		return;
	}
	body = nni_msg_body(msg);
	while ((frame = nni_list_first(&ws->rxq)) != NULL) {
		nni_list_remove(&ws->rxq, frame);
		memcpy(body, frame->buf, frame->len);
		body += frame->len;
		ws_frame_fini(frame);
	}

	nni_aio_set_msg(aio, msg);
	nni_aio_bump_count(aio, nni_msg_len(msg));
	nni_aio_finish(aio, 0, nni_msg_len(msg));
}

static void
ws_read_finish(nni_ws *ws)
{
	if (ws->isstream) {
		ws_read_finish_str(ws);
	} else {
		ws_read_finish_msg(ws);
	}
}

static void
ws_read_frame_cb(nni_ws *ws, ws_frame *frame)
{
	switch (frame->op) {
	case WS_CONT:
		if (!ws->inmsg) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		if (frame->final) {
			ws->inmsg = false;
		}
		ws->rxframe = NULL;
		nni_list_append(&ws->rxq, frame);
		break;
	case WS_TEXT:
		if (!ws->recv_text) {
			// No support for text mode at present.
			ws_close(ws, WS_CLOSE_UNSUPP_FORMAT);
		}
		// FALLTHROUGH
	case WS_BINARY:
		if (ws->inmsg) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		if (!frame->final) {
			ws->inmsg = true;
		}
		ws->rxframe = NULL;
		nni_list_append(&ws->rxq, frame);
		break;

	case WS_PING:
		if (frame->len > 125) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		ws_send_control(ws, WS_PONG, frame->buf, frame->len);
		ws->rxframe = NULL;
		ws_frame_fini(frame);
		break;
	case WS_PONG:
		if (frame->len > 125) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			return;
		}
		ws->rxframe = NULL;
		ws_frame_fini(frame);
		break;
	case WS_CLOSE:
		// if we did not send a close frame yet, do so.
		// (this might be a response to our close)
		ws->peer_closed = true;
		if (!ws->closed) {
			ws_close(ws, WS_CLOSE_NORMAL_CLOSE);
		} else {
			ws->wclose = false;
			nni_aio_finish(&ws->closeaio, 0, 0);
		}
		return;
	default:
		ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
		return;
	}

	ws_read_finish(ws);
}

static void
ws_read_cb(void *arg)
{
	nni_ws   *ws  = arg;
	nni_aio  *aio = &ws->rxaio;
	ws_frame *frame;

	nni_mtx_lock(&ws->mtx);
	if ((frame = ws->rxframe) == NULL) {
		nni_mtx_unlock(&ws->mtx); // canceled during close
		return;
	}

	if (nni_aio_result(aio) != 0) {
		// on a read error, we assume the connection was
		// abruptly closed, and we don't try to shut down nicely
		ws->closed = true;
		ws_close(ws, 0);
		nni_mtx_unlock(&ws->mtx);
		return;
	}

	if (frame->hlen == 0) {
		frame->hlen   = 2;
		frame->op     = frame->head[0] & 0x7fu;
		frame->final  = (frame->head[0] & 0x80u) ? 1 : 0;
		frame->masked = (frame->head[1] & 0x80u) ? 1 : 0;
		if (frame->masked) {
			frame->hlen += 4;
		}
		if ((frame->head[1] & 0x7Fu) == 127) {
			frame->hlen += 8;
		} else if ((frame->head[1] & 0x7Fu) == 126) {
			frame->hlen += 2;
		}

		// If we didn't read the full header yet, then read
		// the rest of it.
		if (frame->hlen != 2) {
			nni_iov iov;
			iov.iov_buf = frame->head + 2;
			iov.iov_len = frame->hlen - 2;
			nni_aio_set_iov(aio, 1, &iov);
			nni_http_read_full(ws->http, aio);
			nni_mtx_unlock(&ws->mtx);
			return;
		}
	}

	// If we are returning from a read of additional data, then
	// the buf will be set.  Otherwise, we need to determine
	// how much data to read.  As our headers are complete, we take
	// this time to do some protocol checks -- no point in waiting
	// to read data.  (Frame size check needs to be done first
	// anyway to prevent DoS.)

	if (frame->buf == NULL) {

		// Determine expected frame size.
		switch ((frame->len = (frame->head[1] & 0x7Fu))) {
		case 127:
			NNI_GET64(frame->head + 2, frame->len);
			if (frame->len < 65536) {
				ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
				nni_mtx_unlock(&ws->mtx);
				return;
			}
			break;
		case 126:
			NNI_GET16(frame->head + 2, frame->len);
			if (frame->len < 126) {
				ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
				nni_mtx_unlock(&ws->mtx);
				return;
			}

			break;
		}

		if ((frame->len > ws->maxframe) && (ws->maxframe > 0)) {
			ws_close(ws, WS_CLOSE_TOO_BIG);
			nni_mtx_unlock(&ws->mtx);
			return;
		}
		// For message mode, also check to make sure that the overall
		// length of the message has not exceeded our recvmax.
		// (Protect against an infinite stream of small messages!)
		if ((!ws->isstream) && (ws->recvmax > 0)) {
			size_t    totlen = frame->len;
			ws_frame *fr2;
			NNI_LIST_FOREACH (&ws->rxq, fr2) {
				totlen += fr2->len;
			}
			if (totlen > ws->recvmax) {
				ws_close(ws, WS_CLOSE_TOO_BIG);
				nni_mtx_unlock(&ws->mtx);
				return;
			}
		}

		// Check for masking.  (We don't actually unmask
		// here, because we don't have data yet.)
		if (frame->masked) {
			memcpy(frame->mask, frame->head + frame->hlen - 4, 4);
			if (!ws->server) {
				ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
				nni_mtx_unlock(&ws->mtx);
				return;
			}
		} else if (ws->server) {
			ws_close(ws, WS_CLOSE_PROTOCOL_ERR);
			nni_mtx_unlock(&ws->mtx);
			return;
		}

		// If we expected data, then ask for it.
		if (frame->len != 0) {

			nni_iov iov;

			// Short frames can avoid an alloc
			if (frame->len < 126) {
				frame->buf   = frame->sdata;
				frame->asize = 0;
			} else {
				frame->adata = nni_alloc(frame->len);
				if (frame->adata == NULL) {
					ws_close(ws, WS_CLOSE_INTERNAL);
					nni_mtx_unlock(&ws->mtx);
					return;
				}
				frame->asize = frame->len;
				frame->buf   = frame->adata;
			}

			iov.iov_buf = frame->buf;
			iov.iov_len = frame->len;
			nni_aio_set_iov(aio, 1, &iov);
			nni_http_read_full(ws->http, aio);
			nni_mtx_unlock(&ws->mtx);
			return;
		}
	}

	// At this point, we have a complete frame.
	ws_unmask_frame(frame); // idempotent

	ws_read_frame_cb(ws, frame);
	ws_start_read(ws);
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_read_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	nni_ws *ws = arg;

	nni_mtx_lock(&ws->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_close_error(nni_ws *ws, uint16_t code)
{
	nni_mtx_lock(&ws->mtx);
	ws_close(ws, code);
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_stop(void *arg)
{
	nni_ws *ws = arg;
	ws_close_error(ws, WS_CLOSE_NORMAL_CLOSE);

	// Give a chance for the close frame to drain.
	nni_aio_wait(&ws->closeaio);

	nni_aio_stop(&ws->rxaio);
	nni_aio_stop(&ws->txaio);
	nni_aio_stop(&ws->closeaio);
	nni_aio_stop(&ws->httpaio);

	if (nni_list_node_active(&ws->node)) {
		nni_ws_dialer *d;

		if ((d = ws->dialer) != NULL) {
			nni_mtx_lock(&d->mtx);
			nni_list_node_remove(&ws->node);
			ws->dialer = NULL;
			nni_mtx_unlock(&d->mtx);
		}
	}
}

static void
ws_fini(void *arg)
{
	nni_ws   *ws = arg;
	ws_frame *frame;
	nng_aio  *aio;

	ws_stop(ws);

	nni_mtx_lock(&ws->mtx);
	while ((frame = nni_list_first(&ws->rxq)) != NULL) {
		nni_list_remove(&ws->rxq, frame);
		ws_frame_fini(frame);
	}

	while ((frame = nni_list_first(&ws->txq)) != NULL) {
		nni_list_remove(&ws->txq, frame);
		ws_frame_fini(frame);
	}

	if (ws->rxframe != NULL) {
		ws_frame_fini(ws->rxframe);
	}
	if (ws->txframe != NULL) {
		ws_frame_fini(ws->txframe);
	}

	while (((aio = nni_list_first(&ws->recvq)) != NULL) ||
	    ((aio = nni_list_first(&ws->sendq)) != NULL)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	nni_mtx_unlock(&ws->mtx);

	if (ws->http) {
		nni_http_conn_fini(ws->http);
	}

	nni_aio_fini(&ws->rxaio);
	nni_aio_fini(&ws->txaio);
	nni_aio_fini(&ws->closeaio);
	nni_aio_fini(&ws->httpaio);
	nni_aio_fini(&ws->connaio);
	nni_mtx_fini(&ws->mtx);
	NNI_FREE_STRUCT(ws);
}

static nni_reap_list ws_reap_list = {
	.rl_offset = offsetof(nni_ws, reap),
	.rl_func   = ws_fini,
};

static void
ws_reap(nni_ws *ws)
{
	nni_reap(&ws_reap_list, ws);
}

static void
ws_http_cb_listener(nni_ws *ws, nni_aio *aio)
{
	nni_ws_listener *l = ws->listener;

	nni_mtx_lock(&l->mtx);
	nni_list_remove(&l->reply, ws);
	if (nni_aio_result(aio) != 0) {
		nni_mtx_unlock(&l->mtx);
		ws_reap(ws);
		return;
	}
	ws->ready = true;
	if ((aio = nni_list_first(&l->aios)) != NULL) {
		nni_list_remove(&l->aios, aio);
		nni_aio_set_output(aio, 0, ws);
		nni_aio_finish(aio, 0, 0);
	} else {
		nni_list_append(&l->pend, ws);
	}
	if (nni_list_empty(&l->reply)) {
		nni_cv_wake(&l->cv);
	}
	nni_mtx_unlock(&l->mtx);
}

static void
ws_http_cb_dialer(nni_ws *ws, nni_aio *aio)
{
	nni_ws_dialer *d;
	nni_aio       *uaio;
	int            rv;
	uint16_t       status;
	char           wskey[29];
	const char    *ptr;

	d = ws->dialer;
	nni_mtx_lock(&d->mtx);
	uaio = ws->useraio;

	// We have two steps.  In step 1, we just sent the request,
	// and need to retrieve the reply.  In step two we have
	// received the reply, and need to validate it.
	// Note that its possible that the user canceled the request,
	// in which case we no longer care, and just go to the error
	// case to discard the ws.
	if (((rv = nni_aio_result(aio)) != 0) || (uaio == NULL)) {
		goto err;
	}

	// There is a race between the dialer closing and any connections
	// that were in progress completing.
	if (d->closed) {
		rv = NNG_ECLOSED;
		goto err;
	}

	// If we have no response structure, then this was completion
	// of sending the request.  Prepare an empty response, and read it.
	if (!ws->recv_res) {
		ws->recv_res = true;
		nng_http_read_response(ws->http, &ws->httpaio);
		nni_mtx_unlock(&d->mtx);
		return;
	}

	status = nni_http_get_status(ws->http);
	switch (status) {
	case NNG_HTTP_STATUS_SWITCHING:
		break;
	case NNG_HTTP_STATUS_FORBIDDEN:
	case NNG_HTTP_STATUS_UNAUTHORIZED:
		rv = NNG_EPERM;
		goto err;
	case NNG_HTTP_STATUS_NOT_FOUND:
	case NNG_HTTP_STATUS_METHOD_NOT_ALLOWED:
	case NNG_HTTP_STATUS_NOT_IMPLEMENTED:
		rv = NNG_ECONNREFUSED; // Treat these as refusals.
		goto err;
	case NNG_HTTP_STATUS_BAD_REQUEST:
	default:
		// Perhaps we should use NNG_ETRANERR...
		rv = NNG_EPROTO;
		goto err;
	}

	rv = ws_make_accept(ws->keybuf, wskey);
	if (rv != 0) {
		goto err;
	}

	if (((ptr = nng_http_get_header(ws->http, "Sec-WebSocket-Accept")) ==
	        NULL) ||
	    (strcmp(ptr, wskey) != 0) ||
	    ((ptr = nng_http_get_header(ws->http, "Connection")) == NULL) ||
	    (!ws_contains_word(ptr, "upgrade")) ||
	    ((ptr = nng_http_get_header(ws->http, "Upgrade")) == NULL) ||
	    (strcmp(ptr, "websocket") != 0)) {
		ws_close_error(ws, WS_CLOSE_PROTOCOL_ERR);
		rv = NNG_EPROTO;
		goto err;
	}
	if (d->proto != NULL) {
		if (((ptr = nng_http_get_header(
		          ws->http, "Sec-WebSocket-Protocol")) == NULL) ||
		    (!ws_contains_word(d->proto, ptr))) {
			ws_close_error(ws, WS_CLOSE_PROTOCOL_ERR);
			rv = NNG_EPROTO;
			goto err;
		}
	}

	// At this point, we are in business!
	nni_list_remove(&d->wspend, ws);
	ws->ready   = true;
	ws->useraio = NULL;
	ws->dialer  = NULL;
	nni_aio_set_output(uaio, 0, ws);
	nni_aio_finish(uaio, 0, 0);
	if (nni_list_empty(&d->wspend)) {
		nni_cv_wake(&d->cv);
	}
	nni_mtx_unlock(&d->mtx);
	return;
err:
	nni_list_remove(&d->wspend, ws);
	ws->useraio = NULL;
	ws->dialer  = NULL;
	if (nni_list_empty(&d->wspend)) {
		nni_cv_wake(&d->cv);
	}
	if (uaio != NULL) {
		nni_aio_finish_error(uaio, rv);
	}
	nni_mtx_unlock(&d->mtx);

	ws_reap(ws);
}

static void
ws_http_cb(void *arg)
{
	nni_ws  *ws  = arg;
	nni_aio *aio = &ws->httpaio;

	if (ws->server) {
		ws_http_cb_listener(ws, aio);
	} else {
		ws_http_cb_dialer(ws, aio);
	}
}

static int
ws_init(nni_ws **wsp)
{
	nni_ws *ws;

	if ((ws = NNI_ALLOC_STRUCT(ws)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&ws->mtx);
	NNI_LIST_INIT(&ws->rxq, ws_frame, node);
	NNI_LIST_INIT(&ws->txq, ws_frame, node);
	nni_aio_list_init(&ws->sendq);
	nni_aio_list_init(&ws->recvq);

	nni_aio_init(&ws->closeaio, ws_close_cb, ws);
	nni_aio_init(&ws->txaio, ws_write_cb, ws);
	nni_aio_init(&ws->rxaio, ws_read_cb, ws);
	nni_aio_init(&ws->httpaio, ws_http_cb, ws);
	nni_aio_init(&ws->connaio, ws_conn_cb, ws);

	nni_aio_set_timeout(&ws->closeaio, 100);
	nni_aio_set_timeout(&ws->httpaio, 2000);

	ws->ops.s_close = ws_str_close;
	ws->ops.s_free  = ws_str_free;
	ws->ops.s_stop  = ws_stop;
	ws->ops.s_send  = ws_str_send;
	ws->ops.s_recv  = ws_str_recv;
	ws->ops.s_get   = ws_str_get;

	ws->fragsize = 1 << 20; // we won't send a frame larger than this
	*wsp         = ws;
	return (0);
}

static void
ws_listener_stop(void *arg)
{
	nni_ws_listener  *l = arg;
	nni_http_handler *h;
	nni_http_server  *s;

	ws_listener_close(l);

	nni_mtx_lock(&l->mtx);
	while (!nni_list_empty(&l->reply)) {
		nni_cv_wait(&l->cv);
	}
	h          = l->handler;
	s          = l->server;
	l->handler = NULL;
	l->server  = NULL;
	nni_mtx_unlock(&l->mtx);

	if (h != NULL) {
		nni_http_handler_fini(h);
	}
	if (s != NULL) {
		nni_http_server_fini(s);
	}
}

static void
ws_listener_free(void *arg)
{
	nni_ws_listener *l = arg;
	ws_header       *hdr;

	ws_listener_stop(l);

	nni_cv_fini(&l->cv);
	nni_mtx_fini(&l->mtx);
	nni_strfree(l->proto);
	while ((hdr = nni_list_first(&l->headers)) != NULL) {
		nni_list_remove(&l->headers, hdr);
		nni_strfree(hdr->name);
		nni_strfree(hdr->value);
		NNI_FREE_STRUCT(hdr);
	}
	if (l->url) {
		nng_url_free(l->url);
	}
	NNI_FREE_STRUCT(l);
}

static void
ws_handler(nng_http *conn, void *arg, nng_aio *aio)
{
	nni_ws_listener *l = arg;
	nni_ws          *ws;
	const char      *ptr;
	const char      *proto;
	uint16_t         status;
	int              rv;
	char             key[29];
	ws_header       *hdr;

	nni_mtx_lock(&l->mtx);
	if (l->closed) {
		status = NNG_HTTP_STATUS_SERVICE_UNAVAILABLE;
		goto err;
	}

	// Now check the headers, etc.
	if (strcmp(nng_http_get_version(conn), "HTTP/1.1") != 0) {
		status = NNG_HTTP_STATUS_HTTP_VERSION_NOT_SUPP;
		goto err;
	}

	if (strcmp(nng_http_get_method(conn), "GET") != 0) {
		// HEAD request.  We can't really deal with it.
		status = NNG_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	if ((((ptr = nng_http_get_header(conn, "Content-Length")) != NULL) &&
	        (atoi(ptr) > 0)) ||
	    (((ptr = nng_http_get_header(conn, "Transfer-Encoding")) !=
	         NULL) &&
	        (nni_strcasestr(ptr, "chunked") != NULL))) {
		status = NNG_HTTP_STATUS_CONTENT_TOO_LARGE;
		goto err;
	}

	// These headers have to be present.
	if (((ptr = nng_http_get_header(conn, "Upgrade")) == NULL) ||
	    (!ws_contains_word(ptr, "websocket")) ||
	    ((ptr = nng_http_get_header(conn, "Connection")) == NULL) ||
	    (!ws_contains_word(ptr, "upgrade")) ||
	    ((ptr = nng_http_get_header(conn, "Sec-WebSocket-Version")) ==
	        NULL) ||
	    (strcmp(ptr, "13") != 0)) {
		status = NNG_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	if (((ptr = nng_http_get_header(conn, "Sec-WebSocket-Key")) == NULL) ||
	    (ws_make_accept(ptr, key) != 0)) {
		status = NNG_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	// If the client has requested a specific subprotocol, then
	// we need to try to match it to what the handler says we
	// support. (If no suitable option is found in the handler, we
	// fail the request.)
	proto = nng_http_get_header(conn, "Sec-WebSocket-Protocol");
	if (proto == NULL) {
		if (l->proto != NULL) {
			status = NNG_HTTP_STATUS_BAD_REQUEST;
			goto err;
		}
	} else if ((l->proto == NULL) ||
	    (!ws_contains_word(l->proto, proto))) {
		status = NNG_HTTP_STATUS_BAD_REQUEST;
		goto err;
	}

	nng_http_set_status(conn, NNG_HTTP_STATUS_SWITCHING, NULL);

	// Set any user supplied headers.  This is better than using a hook
	// for most things, because it is loads easier.  Note that websocket
	// headers we care about will be overridden below!
	NNI_LIST_FOREACH (&l->headers, hdr) {
		if (nng_http_set_header(conn, hdr->name, hdr->value) != 0) {
			status = NNG_HTTP_STATUS_INTERNAL_SERVER_ERROR;
			goto err;
		}
	}

	// The hook function gives us the ability to intercept the HTTP
	// response altogether.  Its best not to do this unless you really
	// need to, because it's much more complex.  But if you want to set
	// up an HTTP Authorization handler this might be the only choice.
	if (l->hookfn != NULL) {
		rv = l->hookfn(l->hookarg, conn);
		if (rv != 0) {
			nni_aio_finish_error(aio, rv);
			nni_mtx_unlock(&l->mtx);
			return;
		}

		if (nng_http_get_status(conn) != NNG_HTTP_STATUS_SWITCHING) {
			// The hook has decided to give back a
			// different reply and we are not upgrading
			// anymore.  For example the Origin might not
			// be permitted, or another level of
			// authentication may be required.
			nni_aio_finish(aio, 0, 0);
			nni_mtx_unlock(&l->mtx);
			return;
		}
	}

	// We are good to go, provided we can get the websocket struct,
	// and send the reply.
	if ((rv = ws_init(&ws)) != 0) {
		status = NNG_HTTP_STATUS_INTERNAL_SERVER_ERROR;
		goto err;
	}
	ws->http      = conn;
	ws->server    = true;
	ws->maxframe  = l->maxframe;
	ws->fragsize  = l->fragsize;
	ws->recvmax   = l->recvmax;
	ws->isstream  = l->isstream;
	ws->recv_text = l->recv_text;
	ws->send_text = l->send_text;
	ws->listener  = l;
	memcpy(ws->keybuf, key, sizeof(ws->keybuf));

	nni_http_set_static_header(
	    conn, &ws->hdrs.connection, "Connection", "Upgrade");
	nni_http_set_static_header(
	    conn, &ws->hdrs.upgrade, "Upgrade", "websocket");
	nni_http_set_static_header(
	    conn, &ws->hdrs.wsaccept, "Sec-WebSocket-Accept", ws->keybuf);
	if (proto != NULL) {
		// NB: we still have the request protocol in the header, so
		// that should be fine.
		nni_http_set_static_header(
		    conn, &ws->hdrs.wsproto, "Sec-WebSocket-Protocol", proto);
	}

	nni_list_append(&l->reply, ws);
	nng_http_write_response(conn, &ws->httpaio);
	(void) nni_http_hijack(conn);
	nni_aio_set_output(aio, 0, NULL);
	nni_aio_finish(aio, 0, 0);
	nni_mtx_unlock(&l->mtx);
	return;

err:
	if ((rv = nni_http_set_error(conn, status, NULL, NULL)) != 0) {
		nni_aio_finish_error(aio, rv);
	} else {
		nni_aio_finish(aio, 0, 0);
	}
	nni_mtx_unlock(&l->mtx);
}

static void
ws_accept_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	nni_ws_listener *l = arg;

	nni_mtx_lock(&l->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&l->mtx);
}

static void
ws_listener_accept(void *arg, nni_aio *aio)
{
	nni_ws_listener *l = arg;
	nni_ws          *ws;

	nni_aio_reset(aio);
	nni_mtx_lock(&l->mtx);
	if (l->closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		nni_mtx_unlock(&l->mtx);
		return;
	}
	if (!l->started) {
		nni_aio_finish_error(aio, NNG_ESTATE);
		nni_mtx_unlock(&l->mtx);
		return;
	}
	if ((ws = nni_list_first(&l->pend)) != NULL) {
		nni_list_remove(&l->pend, ws);
		nni_mtx_unlock(&l->mtx);
		nni_aio_set_output(aio, 0, ws);
		nni_aio_finish(aio, 0, 0);
		return;
	}
	if (!nni_aio_start(aio, ws_accept_cancel, l)) {
		nni_mtx_unlock(&l->mtx);
		return;
	}
	nni_list_append(&l->aios, aio);
	nni_mtx_unlock(&l->mtx);
}

static void
ws_listener_close(void *arg)
{
	nni_ws_listener *l = arg;
	nni_ws          *ws;
	nni_mtx_lock(&l->mtx);
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return;
	}
	l->closed = true;
	if (l->started) {
		nni_http_server_del_handler(l->server, l->handler);
		nni_http_server_close(l->server);
		l->started = false;
	}
	NNI_LIST_FOREACH (&l->pend, ws) {
		ws_close_error(ws, WS_CLOSE_GOING_AWAY);
	}
	NNI_LIST_FOREACH (&l->reply, ws) {
		ws_close_error(ws, WS_CLOSE_GOING_AWAY);
	}
	nni_mtx_unlock(&l->mtx);
}

// XXX: Consider replacing this with an option.
void
nni_ws_listener_hook(
    nni_ws_listener *l, nni_ws_listen_hook hookfn, void *hookarg)
{
	nni_mtx_lock(&l->mtx);
	l->hookfn  = hookfn;
	l->hookarg = hookarg;
	nni_mtx_unlock(&l->mtx);
}

static nng_err
ws_listener_listen(void *arg)
{
	nni_ws_listener *l = arg;
	nng_err          rv;

	nni_mtx_lock(&l->mtx);
	if (l->closed) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ECLOSED);
	}
	if (l->started) {
		nni_mtx_unlock(&l->mtx);
		return (NNG_ESTATE);
	}

	if ((rv = nni_http_server_add_handler(l->server, l->handler)) !=
	    NNG_OK) {
		nni_http_server_fini(l->server);
		l->server = NULL;
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}

	if ((rv = nni_http_server_start(l->server)) != NNG_OK) {
		nni_http_server_del_handler(l->server, l->handler);
		nni_http_server_fini(l->server);
		l->server = NULL;
		nni_mtx_unlock(&l->mtx);
		return (rv);
	}

	l->started = true;

	nni_mtx_unlock(&l->mtx);
	return (NNG_OK);
}

static nng_err
ws_listener_set_size(
    nni_ws_listener *l, size_t *valp, const void *buf, size_t sz, nni_type t)
{
	size_t  val;
	nng_err rv;

	// Max size is limited to 4 GB, but you really never want to have
	// to have a larger value.  If you think you need that, you're doing it
	// wrong.  You *can* set the size to 0 for unlimited.
	if ((rv = nni_copyin_size(&val, buf, sz, 0, NNI_MAXSZ, t)) == NNG_OK) {
		nni_mtx_lock(&l->mtx);
		*valp = val;
		nni_mtx_unlock(&l->mtx);
	}
	return (rv);
}

static nng_err
ws_listener_get_size(
    nni_ws_listener *l, size_t *valp, void *buf, size_t *szp, nni_type t)
{
	size_t val;
	nni_mtx_lock(&l->mtx);
	val = *valp;
	nni_mtx_unlock(&l->mtx);
	return (nni_copyout_size(val, buf, szp, t));
}

static nng_err
ws_listener_set_maxframe(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	return (ws_listener_set_size(l, &l->maxframe, buf, sz, t));
}

static nng_err
ws_listener_get_maxframe(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	return (ws_listener_get_size(l, &l->maxframe, buf, szp, t));
}

static nng_err
ws_listener_set_fragsize(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	return (ws_listener_set_size(l, &l->fragsize, buf, sz, t));
}

static nng_err
ws_listener_get_fragsize(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	return (ws_listener_get_size(l, &l->fragsize, buf, szp, t));
}

static nng_err
ws_listener_set_recvmax(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	return (ws_listener_set_size(l, &l->recvmax, buf, sz, t));
}

static nng_err
ws_listener_get_recvmax(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	return (ws_listener_get_size(l, &l->recvmax, buf, szp, t));
}

static nng_err
ws_listener_set_proto(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;

	if ((rv = ws_check_string(buf, sz, t)) == NNG_OK) {
		char *ns;
		if ((ns = nni_strdup(buf)) == NULL) {
			rv = NNG_ENOMEM;
		} else {
			nni_mtx_lock(&l->mtx);
			if (l->proto != NULL) {
				nni_strfree(l->proto);
			}
			l->proto = ns;
			nni_mtx_unlock(&l->mtx);
		}
	}
	return (rv);
}

static nng_err
ws_listener_get_proto(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;
	nni_mtx_lock(&l->mtx);
	rv = nni_copyout_str(l->proto != NULL ? l->proto : "", buf, szp, t);
	nni_mtx_unlock(&l->mtx);
	return (rv);
}

static nng_err
ws_listener_set_msgmode(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;
	bool             b;

	if ((rv = nni_copyin_bool(&b, buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&l->mtx);
		l->isstream = !b;
		nni_mtx_unlock(&l->mtx);
	}
	return (rv);
}

static nng_err
ws_listener_set_recv_text(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;
	bool             b;

	if ((rv = nni_copyin_bool(&b, buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&l->mtx);
		l->recv_text = b;
		nni_mtx_unlock(&l->mtx);
	}
	return (rv);
}

static nng_err
ws_listener_set_send_text(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;
	bool             b;

	if ((rv = nni_copyin_bool(&b, buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&l->mtx);
		l->send_text = b;
		nni_mtx_unlock(&l->mtx);
	}
	return (rv);
}

static nng_err
ws_listener_get_recv_text(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;
	nni_mtx_lock(&l->mtx);
	rv = nni_copyout_bool(l->recv_text, buf, szp, t);
	nni_mtx_unlock(&l->mtx);
	return (rv);
}

static nng_err
ws_listener_get_send_text(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;
	nni_mtx_lock(&l->mtx);
	rv = nni_copyout_bool(l->send_text, buf, szp, t);
	nni_mtx_unlock(&l->mtx);
	return (rv);
}

static const nni_option ws_listener_options[] = {
	{
	    .o_name = NNI_OPT_WS_MSGMODE,
	    .o_set  = ws_listener_set_msgmode,
	},
	{
	    .o_name = NNG_OPT_WS_RECVMAXFRAME,
	    .o_set  = ws_listener_set_maxframe,
	    .o_get  = ws_listener_get_maxframe,
	},
	{
	    .o_name = NNG_OPT_WS_SENDMAXFRAME,
	    .o_set  = ws_listener_set_fragsize,
	    .o_get  = ws_listener_get_fragsize,
	},
	{
	    .o_name = NNG_OPT_RECVMAXSZ,
	    .o_set  = ws_listener_set_recvmax,
	    .o_get  = ws_listener_get_recvmax,
	},
	{
	    .o_name = NNG_OPT_WS_PROTOCOL,
	    .o_set  = ws_listener_set_proto,
	    .o_get  = ws_listener_get_proto,
	},
	{
	    .o_name = NNG_OPT_WS_RECV_TEXT,
	    .o_set  = ws_listener_set_recv_text,
	    .o_get  = ws_listener_get_recv_text,
	},
	{
	    .o_name = NNG_OPT_WS_SEND_TEXT,
	    .o_set  = ws_listener_set_send_text,
	    .o_get  = ws_listener_get_send_text,
	},
	{
	    .o_name = NULL,
	},
};

static nng_err
ws_listener_set_header(nni_ws_listener *l, const char *name, const void *buf,
    size_t sz, nni_type t)
{
	nng_err rv;
	name += strlen(NNG_OPT_WS_HEADER);
	if ((rv = ws_check_string(buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&l->mtx);
		rv = ws_set_header(&l->headers, name, buf);
		nni_mtx_unlock(&l->mtx);
	}
	return (rv);
}

static nng_err
ws_listener_set(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;

	rv = nni_setopt(ws_listener_options, name, l, buf, sz, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_http_server_set(l->server, name, buf, sz, t);
	}

	if (rv == NNG_ENOTSUP) {
		if (startswith(name, NNG_OPT_WS_HEADER)) {
			rv = ws_listener_set_header(l, name, buf, sz, t);
		}
	}
	return (rv);
}

static nng_err
ws_listener_get(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	nni_ws_listener *l = arg;
	nng_err          rv;

	rv = nni_getopt(ws_listener_options, name, l, buf, szp, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_http_server_get(l->server, name, buf, szp, t);
	}
	return (rv);
}

static nng_err
ws_listener_get_tls(void *arg, nng_tls_config **cfgp)
{
	nni_ws_listener *l = arg;
	return (nni_http_server_get_tls(l->server, cfgp));
}

static nng_err
ws_listener_set_tls(void *arg, nng_tls_config *cfg)
{
	nni_ws_listener *l = arg;
	return (nni_http_server_set_tls(l->server, cfg));
}

nng_err
nni_ws_listener_alloc(nng_stream_listener **wslp, const nng_url *url)
{
	nni_ws_listener *l;
	nng_err          rv;
	char            *host;

	if ((l = NNI_ALLOC_STRUCT(l)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&l->mtx);
	nni_cv_init(&l->cv, &l->mtx);
	nni_aio_list_init(&l->aios);

	NNI_LIST_INIT(&l->pend, nni_ws, node);
	NNI_LIST_INIT(&l->reply, nni_ws, node);
	NNI_LIST_INIT(&l->headers, ws_header, node);

	// make a private copy of the url structure.
	if ((rv = nng_url_clone(&l->url, url)) != 0) {
		ws_listener_free(l);
		return (rv);
	}

	host = l->url->u_hostname;
	if (strlen(host) == 0) {
		host = NULL;
	}
	rv = nni_http_handler_init(&l->handler, url->u_path, ws_handler);
	if (rv != 0) {
		ws_listener_free(l);
		return (rv);
	}

	nni_http_handler_set_host(l->handler, host);
	nni_http_handler_set_data(l->handler, l, 0);

	if ((rv = nni_http_server_init(&l->server, url)) != 0) {
		ws_listener_free(l);
		return (rv);
	}

	l->fragsize       = WS_DEF_MAXTXFRAME;
	l->maxframe       = WS_DEF_MAXRXFRAME;
	l->recvmax        = WS_DEF_RECVMAX;
	l->isstream       = true;
	l->ops.sl_free    = ws_listener_free;
	l->ops.sl_close   = ws_listener_close;
	l->ops.sl_stop    = ws_listener_stop;
	l->ops.sl_accept  = ws_listener_accept;
	l->ops.sl_listen  = ws_listener_listen;
	l->ops.sl_set     = ws_listener_set;
	l->ops.sl_get     = ws_listener_get;
	l->ops.sl_get_tls = ws_listener_get_tls;
	l->ops.sl_set_tls = ws_listener_set_tls;
	*wslp             = (void *) l;
	return (NNG_OK);
}

void
ws_conn_cb(void *arg)
{
	nni_ws_dialer *d;
	nni_ws        *ws;
	nni_aio       *uaio;
	int            rv;
	uint8_t        raw[16];
	ws_header     *hdr;

	ws = arg;

	d = ws->dialer;
	if ((rv = nni_aio_result(&ws->connaio)) != 0) {
		nni_mtx_lock(&ws->mtx);
		if ((uaio = ws->useraio) != NULL) {
			ws->useraio = NULL;
			nni_aio_finish_error(uaio, rv);
		}
		nni_mtx_unlock(&ws->mtx);
		nni_mtx_lock(&d->mtx);
		if (nni_list_node_active(&ws->node)) {
			nni_list_remove(&d->wspend, ws);
			ws->dialer = NULL;
			if (nni_list_empty(&d->wspend)) {
				nni_cv_wake(&d->cv);
			}
			nni_mtx_unlock(&d->mtx);
			ws_reap(ws);
		} else {
			nni_mtx_unlock(&d->mtx);
		}
		return;
	}

	ws->http = nni_aio_get_output(&ws->connaio, 0);
	nni_mtx_lock(&ws->mtx);
	uaio = ws->useraio;
	nni_aio_set_output(&ws->connaio, 0, NULL);
	if (uaio == NULL) {
		// This request was canceled for some reason.
		nni_mtx_unlock(&ws->mtx);
		ws_reap(ws);
		return;
	}

	for (int i = 0; i < 16; i++) {
		raw[i] = (uint8_t) nni_random();
	}
	nni_base64_encode(raw, 16, ws->keybuf, 24);
	ws->keybuf[24] = '\0';

	if ((rv = nni_http_set_uri(
	         ws->http, d->url->u_path, d->url->u_query)) != 0) {
		goto err;
	}

	nni_http_set_static_header(
	    ws->http, &ws->hdrs.connection, "Connection", "Upgrade");
	nni_http_set_static_header(
	    ws->http, &ws->hdrs.upgrade, "Upgrade", "websocket");
	nni_http_set_static_header(
	    ws->http, &ws->hdrs.wskey, "Sec-WebSocket-Key", ws->keybuf);
	nni_http_set_static_header(
	    ws->http, &ws->hdrs.wsversion, "Sec-WebSocket-Version", "13");

	if (d->proto != NULL) {
		nni_http_set_static_header(ws->http, &ws->hdrs.wsproto,
		    "Sec-WebSocket-Protocol", d->proto);
	}

	NNI_LIST_FOREACH (&d->headers, hdr) {
		if ((rv = nni_http_set_header(
		         ws->http, hdr->name, hdr->value)) != 0) {
			goto err;
		}
	}

	nni_http_write_req(ws->http, &ws->httpaio);
	nni_mtx_unlock(&ws->mtx);
	return;

err:
	nni_aio_finish_error(uaio, rv);
	nni_mtx_unlock(&ws->mtx);
	ws_reap(ws);
}

static void
ws_dialer_close(void *arg)
{
	nni_ws_dialer *d = arg;
	nni_ws        *ws;
	nni_mtx_lock(&d->mtx);
	if (d->closed) {
		nni_mtx_unlock(&d->mtx);
		return;
	}
	d->closed = true;
	NNI_LIST_FOREACH (&d->wspend, ws) {
		nni_aio_close(&ws->connaio);
		nni_aio_close(&ws->httpaio);
	}
	nni_mtx_unlock(&d->mtx);
}

static void
ws_dialer_stop(void *arg)
{
	nni_ws_dialer *d = arg;

	ws_dialer_close(d);
	nni_mtx_lock(&d->mtx);
	while (!nni_list_empty(&d->wspend)) {
		nni_cv_wait(&d->cv);
	}
	nni_mtx_unlock(&d->mtx);
}

static void
ws_dialer_free(void *arg)
{
	nni_ws_dialer *d = arg;
	ws_header     *hdr;

	ws_dialer_stop(d);

	nni_strfree(d->proto);
	while ((hdr = nni_list_first(&d->headers)) != NULL) {
		nni_list_remove(&d->headers, hdr);
		nni_strfree(hdr->name);
		nni_strfree(hdr->value);
		NNI_FREE_STRUCT(hdr);
	}
	if (d->client) {
		nni_http_client_fini(d->client);
	}
	if (d->url) {
		nng_url_free(d->url);
	}
	nni_cv_fini(&d->cv);
	nni_mtx_fini(&d->mtx);
	NNI_FREE_STRUCT(d);
}

static void
ws_dial_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	nni_ws *ws = arg;

	nni_mtx_lock(&ws->mtx);
	if (aio == ws->useraio) {
		nni_aio_abort(&ws->connaio, rv);
		nni_aio_abort(&ws->httpaio, rv);
		ws->useraio = NULL;
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_dialer_dial(void *arg, nni_aio *aio)
{
	nni_ws_dialer *d = arg;
	nni_ws        *ws;
	int            rv;

	nni_aio_reset(aio);
	if ((rv = ws_init(&ws)) != 0) {
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_mtx_lock(&d->mtx);
	if (d->closed) {
		nni_mtx_unlock(&d->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		ws_reap(ws);
		return;
	}
	if (!nni_aio_start(aio, ws_dial_cancel, ws)) {
		nni_mtx_unlock(&d->mtx);
		ws_reap(ws);
		return;
	}
	ws->dialer    = d;
	ws->useraio   = aio;
	ws->server    = false;
	ws->maxframe  = d->maxframe;
	ws->isstream  = d->isstream;
	ws->recv_text = d->recv_text;
	ws->send_text = d->send_text;
	nni_list_append(&d->wspend, ws);
	nni_http_client_connect(d->client, &ws->connaio);
	nni_mtx_unlock(&d->mtx);
}

static nng_err
ws_dialer_set_msgmode(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;
	bool           b;

	if ((rv = nni_copyin_bool(&b, buf, sz, t)) == 0) {
		nni_mtx_lock(&d->mtx);
		d->isstream = !b;
		nni_mtx_unlock(&d->mtx);
	}
	return (rv);
}

static nng_err
ws_dialer_set_recv_text(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;
	bool           b;

	if ((rv = nni_copyin_bool(&b, buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&d->mtx);
		d->recv_text = b;
		nni_mtx_unlock(&d->mtx);
	}
	return (rv);
}

static nng_err
ws_dialer_set_send_text(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;
	bool           b;

	if ((rv = nni_copyin_bool(&b, buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&d->mtx);
		d->send_text = b;
		nni_mtx_unlock(&d->mtx);
	}
	return (rv);
}

static nng_err
ws_dialer_set_size(
    nni_ws_dialer *d, size_t *valp, const void *buf, size_t sz, nni_type t)
{
	size_t  val;
	nng_err rv;

	// Max size is limited to 4 GB, but you really never want to have
	// to have a larger value.  If you think you need that, you're doing it
	// wrong.  You *can* set the size to 0 for unlimited.
	if ((rv = nni_copyin_size(&val, buf, sz, 0, NNI_MAXSZ, t)) == NNG_OK) {
		nni_mtx_lock(&d->mtx);
		*valp = val;
		nni_mtx_unlock(&d->mtx);
	}
	return (rv);
}

static nng_err
ws_dialer_get_size(
    nni_ws_dialer *d, size_t *valp, void *buf, size_t *szp, nni_type t)
{
	size_t val;
	nni_mtx_lock(&d->mtx);
	val = *valp;
	nni_mtx_unlock(&d->mtx);
	return (nni_copyout_size(val, buf, szp, t));
}

static nng_err
ws_dialer_set_maxframe(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	return (ws_dialer_set_size(d, &d->maxframe, buf, sz, t));
}

static nng_err
ws_dialer_get_maxframe(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	return (ws_dialer_get_size(d, &d->maxframe, buf, szp, t));
}

static nng_err
ws_dialer_set_fragsize(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	return (ws_dialer_set_size(d, &d->fragsize, buf, sz, t));
}

static nng_err
ws_dialer_get_fragsize(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	return (ws_dialer_get_size(d, &d->fragsize, buf, szp, t));
}

static nng_err
ws_dialer_set_recvmax(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	return (ws_dialer_set_size(d, &d->recvmax, buf, sz, t));
}

static nng_err
ws_dialer_get_recvmax(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	return (ws_dialer_get_size(d, &d->recvmax, buf, szp, t));
}

static nng_err
ws_dialer_set_proto(void *arg, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;

	if ((rv = ws_check_string(buf, sz, t)) == NNG_OK) {
		char *ns;
		if ((ns = nni_strdup(buf)) == NULL) {
			rv = NNG_ENOMEM;
		} else {
			nni_mtx_lock(&d->mtx);
			if (d->proto != NULL) {
				nni_strfree(d->proto);
			}
			d->proto = ns;
			nni_mtx_unlock(&d->mtx);
		}
	}
	return (rv);
}

static nng_err
ws_dialer_get_proto(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;
	nni_mtx_lock(&d->mtx);
	rv = nni_copyout_str(d->proto != NULL ? d->proto : "", buf, szp, t);
	nni_mtx_unlock(&d->mtx);
	return (rv);
}

static nng_err
ws_dialer_get_recv_text(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;
	nni_mtx_lock(&d->mtx);
	rv = nni_copyout_bool(d->recv_text, buf, szp, t);
	nni_mtx_unlock(&d->mtx);
	return (rv);
}

static nng_err
ws_dialer_get_send_text(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;
	nni_mtx_lock(&d->mtx);
	rv = nni_copyout_bool(d->send_text, buf, szp, t);
	nni_mtx_unlock(&d->mtx);
	return (rv);
}

static const nni_option ws_dialer_options[] = {
	{
	    .o_name = NNI_OPT_WS_MSGMODE,
	    .o_set  = ws_dialer_set_msgmode,
	},
	{
	    .o_name = NNG_OPT_WS_RECVMAXFRAME,
	    .o_set  = ws_dialer_set_maxframe,
	    .o_get  = ws_dialer_get_maxframe,
	},
	{
	    .o_name = NNG_OPT_WS_SENDMAXFRAME,
	    .o_set  = ws_dialer_set_fragsize,
	    .o_get  = ws_dialer_get_fragsize,
	},
	{
	    .o_name = NNG_OPT_RECVMAXSZ,
	    .o_set  = ws_dialer_set_recvmax,
	    .o_get  = ws_dialer_get_recvmax,
	},
	{
	    .o_name = NNG_OPT_WS_PROTOCOL,
	    .o_set  = ws_dialer_set_proto,
	    .o_get  = ws_dialer_get_proto,
	},
	{
	    .o_name = NNG_OPT_WS_RECV_TEXT,
	    .o_set  = ws_dialer_set_recv_text,
	    .o_get  = ws_dialer_get_recv_text,
	},
	{
	    .o_name = NNG_OPT_WS_SEND_TEXT,
	    .o_set  = ws_dialer_set_send_text,
	    .o_get  = ws_dialer_get_send_text,
	},

	{
	    .o_name = NULL,
	},
};

static nng_err
ws_dialer_set_header(
    nni_ws_dialer *d, const char *name, const void *buf, size_t sz, nni_type t)
{
	nng_err rv;
	name += strlen(NNG_OPT_WS_HEADER);
	if ((rv = ws_check_string(buf, sz, t)) == NNG_OK) {
		nni_mtx_lock(&d->mtx);
		rv = ws_set_header(&d->headers, name, buf);
		nni_mtx_unlock(&d->mtx);
	}
	return (rv);
}

static nng_err
ws_dialer_set(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;

	rv = nni_setopt(ws_dialer_options, name, d, buf, sz, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_http_client_set(d->client, name, buf, sz, t);
	}

	if (rv == NNG_ENOTSUP) {
		if (startswith(name, NNG_OPT_WS_HEADER)) {
			rv = ws_dialer_set_header(d, name, buf, sz, t);
		}
	}
	return (rv);
}

static nng_err
ws_dialer_get(void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	nni_ws_dialer *d = arg;
	nng_err        rv;

	rv = nni_getopt(ws_dialer_options, name, d, buf, szp, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_http_client_get(d->client, name, buf, szp, t);
	}
	return (rv);
}

static nng_err
ws_dialer_get_tls(void *arg, nng_tls_config **cfgp)
{
	nni_ws_dialer *d = arg;
	return (nni_http_client_get_tls(d->client, cfgp));
}

static nng_err
ws_dialer_set_tls(void *arg, nng_tls_config *cfg)
{
	nni_ws_dialer *d = arg;
	return (nni_http_client_set_tls(d->client, cfg));
}

nng_err
nni_ws_dialer_alloc(nng_stream_dialer **dp, const nng_url *url)
{
	nni_ws_dialer *d;
	nng_err        rv;

	if ((d = NNI_ALLOC_STRUCT(d)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&d->headers, ws_header, node);
	NNI_LIST_INIT(&d->wspend, nni_ws, node);
	NNI_LIST_INIT(&d->headers, ws_header, node);
	nni_mtx_init(&d->mtx);
	nni_cv_init(&d->cv, &d->mtx);

	if ((rv = nng_url_clone(&d->url, url)) != 0) {
		ws_dialer_free(d);
		return (rv);
	}

	if ((rv = nni_http_client_init(&d->client, url)) != 0) {
		ws_dialer_free(d);
		return (rv);
	}
	d->isstream = true;
	d->recvmax  = WS_DEF_RECVMAX;
	d->maxframe = WS_DEF_MAXRXFRAME;
	d->fragsize = WS_DEF_MAXTXFRAME;

	d->ops.sd_free    = ws_dialer_free;
	d->ops.sd_close   = ws_dialer_close;
	d->ops.sd_stop    = ws_dialer_stop;
	d->ops.sd_dial    = ws_dialer_dial;
	d->ops.sd_set     = ws_dialer_set;
	d->ops.sd_get     = ws_dialer_get;
	d->ops.sd_set_tls = ws_dialer_set_tls;
	d->ops.sd_get_tls = ws_dialer_get_tls;
	*dp               = (void *) d;
	return (NNG_OK);
}

// Dialer does not get a hook chance, as it can examine the request
// and reply after dial is done; this is not a 3-way handshake, so
// the dialer does not confirm the server's response at the HTTP
// level. (It can still issue a websocket close).

static void
ws_str_free(void *arg)
{
	nni_ws *ws = arg;
	ws_reap(ws);
}

static void
ws_str_close(void *arg)
{
	nni_ws *ws = arg;
	ws_close_error(ws, WS_CLOSE_NORMAL_CLOSE);
}

static void
ws_str_send(void *arg, nni_aio *aio)
{
	nni_ws   *ws = arg;
	int       rv;
	ws_frame *frame;

	nni_aio_reset(aio);

	if (!ws->isstream) {
		nni_msg *msg;
		unsigned niov;
		nni_iov  iov[2];
		if ((msg = nni_aio_get_msg(aio)) == NULL) {
			nni_aio_finish_error(aio, NNG_EINVAL);
			return;
		}
		niov = 0;
		if (nng_msg_header_len(msg) > 0) {
			iov[niov].iov_len = nni_msg_header_len(msg);
			iov[niov].iov_buf = nni_msg_header(msg);
			niov++;
		}
		iov[niov].iov_len = nni_msg_len(msg);
		iov[niov].iov_buf = nni_msg_body(msg);
		niov++;

		// Scribble into the iov for now.
		nni_aio_set_iov(aio, niov, iov);
	}

	if ((frame = NNI_ALLOC_STRUCT(frame)) == NULL) {
		nni_aio_finish_error(aio, NNG_ENOMEM);
		return;
	}
	frame->aio = aio;
	if ((rv = ws_frame_prep_tx(ws, frame)) != 0) {
		nni_aio_finish_error(aio, rv);
		ws_frame_fini(frame);
		return;
	}

	nni_mtx_lock(&ws->mtx);

	if (ws->closed) {
		nni_mtx_unlock(&ws->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		ws_frame_fini(frame);
		return;
	}
	if (!nni_aio_start(aio, ws_write_cancel, ws)) {
		nni_mtx_unlock(&ws->mtx);
		ws_frame_fini(frame);
		return;
	}
	nni_aio_set_prov_data(aio, frame);
	nni_list_append(&ws->sendq, aio);
	nni_list_append(&ws->txq, frame);
	ws_start_write(ws);
	nni_mtx_unlock(&ws->mtx);
}

static void
ws_str_recv(void *arg, nng_aio *aio)
{
	nni_ws *ws = arg;

	nni_aio_reset(aio);
	nni_mtx_lock(&ws->mtx);
	if (!nni_aio_start(aio, ws_read_cancel, ws)) {
		nni_mtx_unlock(&ws->mtx);
		return;
	}
	nni_list_append(&ws->recvq, aio);
	if (nni_list_first(&ws->recvq) == aio) {
		ws_read_finish(ws);
	}
	ws_start_read(ws);

	nni_mtx_unlock(&ws->mtx);
}

static nng_err
ws_get_request_uri(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws *ws = arg;
	return (nni_copyout_str(nni_http_get_uri(ws->http), buf, szp, t));
}

static nng_err
ws_get_recv_text(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws *ws = arg;
	bool    b;
	nni_mtx_lock(&ws->mtx);
	b = ws->recv_text;
	nni_mtx_unlock(&ws->mtx);

	return (nni_copyout_bool(b, buf, szp, t));
}

static nng_err
ws_get_send_text(void *arg, void *buf, size_t *szp, nni_type t)
{
	nni_ws *ws = arg;
	bool    b;
	nni_mtx_lock(&ws->mtx);
	b = ws->send_text;
	nni_mtx_unlock(&ws->mtx);

	return (nni_copyout_bool(b, buf, szp, t));
}

static const nni_option ws_options[] = {
	{
	    .o_name = NNG_OPT_WS_REQUEST_URI,
	    .o_get  = ws_get_request_uri,
	},
	{
	    .o_name = NNG_OPT_WS_RECV_TEXT,
	    .o_get  = ws_get_recv_text,
	},
	{
	    .o_name = NNG_OPT_WS_SEND_TEXT,
	    .o_get  = ws_get_send_text,
	},
	{
	    .o_name = NULL,
	},
};

static nng_err
ws_get_header(nni_ws *ws, const char *nm, void *buf, size_t *szp, nni_type t)
{
	const char *s;
	nm += strlen(NNG_OPT_WS_HEADER);
	s = nni_http_get_header(ws->http, nm);
	if (s == NULL) {
		return (NNG_ENOENT);
	}
	return (nni_copyout_str(s, buf, szp, t));
}

static nng_err
ws_str_get(void *arg, const char *nm, void *buf, size_t *szp, nni_type t)
{
	nni_ws *ws = arg;
	nng_err rv;

	nni_mtx_lock(&ws->mtx);
	if (ws->closed) {
		nni_mtx_unlock(&ws->mtx);
		return (NNG_ECLOSED);
	}
	nni_mtx_unlock(&ws->mtx);
	rv = nni_http_conn_getopt(ws->http, nm, buf, szp, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_getopt(ws_options, nm, ws, buf, szp, t);
	}
	// Check for generic headers...
	if (rv == NNG_ENOTSUP) {
		if (startswith(nm, NNG_OPT_WS_HEADER)) {
			rv = ws_get_header(ws, nm, buf, szp, t);
		}
	}
	return (rv);
}
