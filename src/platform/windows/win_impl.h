//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef PLATFORM_WIN_IMPL_H
#define PLATFORM_WIN_IMPL_H

#ifdef PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
#include <process.h>
#include <ws2tcpip.h>


// These types are provided for here, to permit them to be directly inlined
// elsewhere.

typedef struct nni_win_event nni_win_event;

// nni_win_event is used with io completion ports.  This allows us to get
// to a specific completion callback without requiring the poller (in the
// completion port) to know anything about the event itself.  We also use
// this to pass back status and counts to the routine, which may not be
// conveyed in the OVERLAPPED directly.
struct nni_win_event {
	OVERLAPPED	olpd;
	void *		ptr;
	nni_cb		cb;
	int		status;
	int		nbytes;
};

struct nni_plat_ipcsock {
	HANDLE			p;

	char			path[256];
	WSAOVERLAPPED		recv_olpd;
	WSAOVERLAPPED		send_olpd;
	WSAOVERLAPPED		conn_olpd; // Use for both connect and accept
	CRITICAL_SECTION	cs;

	int			server;
};

struct nni_plat_thr {
	void	(*func)(void *);
	void *	arg;
	HANDLE	handle;
};

struct nni_plat_mtx {
	CRITICAL_SECTION	cs;
	DWORD			owner;
	int			init;
};

struct nni_plat_cv {
	CONDITION_VARIABLE	cv;
	CRITICAL_SECTION *	cs;
};

extern int nni_win_error(int);
extern int nni_winsock_error(int);

extern int nni_win_iocp_sysinit(void);
extern void nni_win_iocp_sysfini(void);

extern int nni_win_resolv_sysinit(void);
extern void nni_win_resolv_sysfini(void);

#endif  // PLATFORM_WINDOWS

#endif  // PLATFORM_WIN_IMPL_H
