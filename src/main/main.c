/**
 * @file main.c  Main polling routine
 *
 * Copyright (C) 2010 Creytiv.com
 * Copyright (C) 2020-2022 Sebastian Reimers
 */
#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/types.h>
#undef _STRICT_ANSI
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/resource.h>
#endif
#ifdef HAVE_SIGNAL
#include <signal.h>
#endif
#ifdef HAVE_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_POLL
#include <poll.h>
#endif
#ifdef HAVE_EPOLL
#include <sys/epoll.h>
#endif
#ifdef HAVE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#undef LIST_INIT
#undef LIST_FOREACH
#endif
#include <re_types.h>
#include <re_fmt.h>
#include <re_net.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_tmr.h>
#include <re_main.h>
#include <re_thread.h>
#include <re_btrace.h>
#include <re_atomic.h>
#include "main.h"


#define DEBUG_MODULE "main"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Main loop values */
enum {
	MAX_BLOCKING = 500,    /**< Maximum time spent in handler in [ms] */
#if defined (FD_SETSIZE)
	DEFAULT_MAXFDS = FD_SETSIZE
#else
	DEFAULT_MAXFDS = 128
#endif
};

/** File descriptor handler struct */
struct fhs {
	struct le he;        /**< Hash entry                        */
	struct le le_delete; /**< Delete entry                      */
	int index;           /**< Index used for arrays             */
	re_sock_t fd;        /**< File Descriptor                   */
	int flags;           /**< Polling flags (Read, Write, etc.) */
	fd_h* fh;            /**< Event handler                     */
	void* arg;           /**< Handler argument                  */
};

/** Polling loop data */
struct re {
	struct hash *fhl;            /**< File descriptor hash list         */
	struct list fhs_delete;      /**< File descriptor delete list       */
	bool fhs_reuse;              /**< Reuse file descriptors            */
	int maxfds;                  /**< Maximum number of polling fds     */
	int max_fd;                  /**< Maximum fd number                 */
	int nfds;                    /**< Number of active file descriptors */
	enum poll_method method;     /**< The current polling method        */
	bool update;                 /**< File descriptor set need updating */
	RE_ATOMIC bool polling;      /**< Is polling flag                   */
	int sig;                     /**< Last caught signal                */
	struct list tmrl;            /**< List of timers                    */

#ifdef HAVE_POLL
	struct pollfd *fds;          /**< Event set for poll()              */
#endif

#ifdef HAVE_EPOLL
	struct epoll_event *events;  /**< Event set for epoll()             */
	int epfd;                    /**< epoll control file descriptor     */
#endif

#ifdef HAVE_KQUEUE
	struct kevent *evlist;
	int kqfd;
#endif
	mtx_t *mutex;                /**< Mutex for thread synchronization  */
	mtx_t *mutexp;               /**< Pointer to active mutex           */
	thrd_t tid;                  /**< Thread id                         */
	RE_ATOMIC bool thread_enter; /**< Thread enter is called            */
};

static struct re *re_global = NULL;
static tss_t key;
static once_flag flag = ONCE_FLAG_INIT;

static void poll_close(struct re *re);


static void re_destructor(void *arg)
{
	struct re *re = arg;

	poll_close(re);
	mem_deref(re->mutex);
	hash_flush(re->fhl);
	mem_deref(re->fhl);
}


/** fallback destructor if thread gets destroyed before re_thread_close() */
static void thread_destructor(void *arg)
{
	struct re *re = arg;

	if (!re)
		return;

	mem_deref(re);
}


int re_alloc(struct re **rep)
{
	struct re *re;
	int err;

	if (!rep)
		return EINVAL;

	re = mem_zalloc(sizeof(struct re), re_destructor);
	if (!re)
		return ENOMEM;

	err = mutex_alloc(&re->mutex);
	if (err) {
		DEBUG_WARNING("thread_init: mtx_init error\n");
		goto out;
	}
	re->mutexp = re->mutex;

	list_init(&re->tmrl);
	list_init(&re->fhs_delete);

#ifndef WIN32
	re->fhs_reuse = true;
#endif

	re->tid = thrd_current();

#ifdef HAVE_EPOLL
	re->epfd = -1;
#endif

#ifdef HAVE_KQUEUE
	re->kqfd = -1;
#endif


out:
	if (err)
		mem_deref(re);
	else
		*rep = re;

	return err;
}


static void re_once(void)
{
	int err;

	err = tss_create(&key, thread_destructor);
	if (err != thrd_success) {
		DEBUG_WARNING("tss_create failed\n");
		exit(ENOMEM);
	}
}


/**
 * Get thread specific re pointer (fallback to re_global if called by non re
 * thread)
 *
 * @return re pointer on success, otherwise NULL if libre_init() or
 * re_thread_init() is missing
 */
static struct re *re_get(void)
{
	struct re *re;

	call_once(&flag, re_once);
	re = tss_get(key);
	if (!re)
		re = re_global;

	return re;
}


static bool fhs_lookup(struct le *he, void *arg)
{
	re_sock_t fd = *(re_sock_t *)arg;
	struct fhs *fhs = he->data;

	if (fd == fhs->fd)
		return true;

	return false;
}


static inline void re_lock(struct re *re)
{
	int err;

	err = mtx_lock(re->mutexp);
	if (err != thrd_success)
		DEBUG_WARNING("re_lock err\n");
}


static inline void re_unlock(struct re *re)
{
	int err;

	err = mtx_unlock(re->mutexp);
	if (err != thrd_success)
		DEBUG_WARNING("re_unlock err\n");
}

#if MAIN_DEBUG
/**
 * Call the application event handler
 *
 * @param re     Poll state
 * @param i	 File descriptor handler index
 * @param flags  Event flags
 */
static void fd_handler(struct fhs *fhs, int flags)
{
	const uint64_t tick = tmr_jiffies();
	uint32_t diff;

	DEBUG_INFO("event on fd=%d index=%d (flags=0x%02x)...\n",
		   fhs->fd, i, flags);

	fhs->fh(flags, fhs->arg);

	diff = (uint32_t)(tmr_jiffies() - tick);

	if (diff > MAX_BLOCKING) {
		DEBUG_WARNING("long async blocking: %u>%u ms (h=%p arg=%p)\n",
			      diff, MAX_BLOCKING,
			      fhs->fh, fhs->arg);
	}
}
#endif


#ifdef HAVE_POLL
static int set_poll_fds(struct re *re, struct fhs *fhs)
{
	if (!re->fds)
		return 0;

	if (!fhs)
		return EINVAL;

	re_sock_t fd = fhs->fd;
	int flags = fhs->flags;
	int index = fhs->index;

	if (index >= re->maxfds)
		return EMFILE;

	if (flags)
		re->fds[index].fd = fd;
	else
		re->fds[index].fd = -1;

	re->fds[index].events = 0;
	if (flags & FD_READ)
		re->fds[index].events |= POLLIN;
	if (flags & FD_WRITE)
		re->fds[index].events |= POLLOUT;
	if (flags & FD_EXCEPT)
		re->fds[index].events |= POLLERR;

	return 0;
}
#endif


#ifdef HAVE_EPOLL
static int set_epoll_fds(struct re *re, struct fhs *fhs)
{
	struct epoll_event event;
	int err = 0;

	if (!fhs)
		return EINVAL;

	re_sock_t fd = fhs->fd;
	int flags    = fhs->flags;

	if (re->epfd < 0)
		return EBADFD;

	memset(&event, 0, sizeof(event));

	DEBUG_INFO("set_epoll_fds: fd=%d flags=0x%02x\n", fd, flags);

	if (flags) {
		event.data.ptr = fhs;

		if (flags & FD_READ)
			event.events |= EPOLLIN;
		if (flags & FD_WRITE)
			event.events |= EPOLLOUT;
		if (flags & FD_EXCEPT)
			event.events |= EPOLLERR;

		/* Try to add it first */
		if (-1 == epoll_ctl(re->epfd, EPOLL_CTL_ADD, fd, &event)) {

			/* If already exist then modify it */
			if (EEXIST == errno) {

				if (-1 == epoll_ctl(re->epfd, EPOLL_CTL_MOD,
						    fd, &event)) {
					err = errno;
					DEBUG_WARNING("epoll_ctl:"
						      " EPOLL_CTL_MOD:"
						      " fd=%d (%m)\n",
						      fd, err);
				}
			}
			else {
				err = errno;
				DEBUG_WARNING("epoll_ctl: EPOLL_CTL_ADD:"
					      " fd=%d (%m)\n",
					      fd, err);
			}
		}
	}
	else {
		if (-1 == epoll_ctl(re->epfd, EPOLL_CTL_DEL, fd, &event)) {
			err = errno;
			DEBUG_INFO("epoll_ctl: EPOLL_CTL_DEL: fd=%d (%m)\n",
				   fd, err);
		}
	}

	return err;
}
#endif


#ifdef HAVE_KQUEUE
static int set_kqueue_fds(struct re *re, struct fhs *fhs)
{
	struct kevent kev[2];
	int r, n = 0;

	if (!fhs)
		return EINVAL;

	re_sock_t fd = fhs->fd;
	int flags    = fhs->flags;

	memset(kev, 0, sizeof(kev));

	/* always delete the events */
	EV_SET(&kev[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, 0);
	EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
	kevent(re->kqfd, kev, 2, NULL, 0, NULL);

	memset(kev, 0, sizeof(kev));

	if (flags & FD_WRITE) {
		EV_SET(&kev[n], fd, EVFILT_WRITE, EV_ADD, 0, 0, fhs);
		++n;
	}
	if (flags & FD_READ) {
		EV_SET(&kev[n], fd, EVFILT_READ, EV_ADD, 0, 0, fhs);
		++n;
	}

	if (n) {
		r = kevent(re->kqfd, kev, n, NULL, 0, NULL);
		if (r < 0) {
			int err = errno;

			DEBUG_WARNING("set: [fd=%d, flags=%x] kevent: %m\n",
				      fd, flags, err);
			return err;
		}
	}

	return 0;
}
#endif


/**
 * Rebuild the file descriptor mapping. This must be done whenever
 * the polling method is changed.
 */
static bool rebuild_fd(struct le *he, void *arg)
{
	int err = 0;
	struct re *re = arg;
	struct fhs *fhs = he->data;


	/* Update fd sets */
	if (!fhs->fh)
		return false;

	switch (re->method) {

#ifdef HAVE_POLL
	case METHOD_POLL:
		err = set_poll_fds(re, fhs);
		break;
#endif
#ifdef HAVE_EPOLL
	case METHOD_EPOLL:
		err = set_epoll_fds(re, fhs);
		break;
#endif

#ifdef HAVE_KQUEUE
	case METHOD_KQUEUE:
		err = set_kqueue_fds(re, fhs);
		break;
#endif

	default:
		break;
	}

	if (err) {
		DEBUG_WARNING("rebuild_fd: set fd error: %m\n", err);
		return true;
	}

	return false;
}


static int poll_init(struct re *re)
{
	int err;

	DEBUG_INFO("poll init (maxfds=%d)\n", re->maxfds);

	if (!re->maxfds) {
		DEBUG_WARNING("poll init: maxfds is 0\n");
		return EINVAL;
	}

	if (!re->fhl) {
		err = hash_alloc(&re->fhl, hash_valid_size(re->maxfds));
		if (err)
			return err;
	}

	switch (re->method) {

#ifdef HAVE_POLL
	case METHOD_POLL:
		if (!re->fds) {
			re->fds = mem_zalloc(re->maxfds * sizeof(*re->fds),
					    NULL);
			if (!re->fds)
				return ENOMEM;
		}
		break;
#endif
#ifdef HAVE_EPOLL
	case METHOD_EPOLL:
		if (!re->events) {
			DEBUG_INFO("allocate %u bytes for epoll set\n",
				   re->maxfds * sizeof(*re->events));
			re->events = mem_zalloc(re->maxfds*sizeof(*re->events),
					      NULL);
			if (!re->events)
				return ENOMEM;
		}

		if (re->epfd < 0
		    && -1 == (re->epfd = epoll_create(re->maxfds))) {
			err = errno;

			DEBUG_WARNING("epoll_create: %m (maxfds=%d)\n",
				      err, re->maxfds);
			return err;
		}
		DEBUG_INFO("init: epoll_create() epfd=%d\n", re->epfd);
		break;
#endif

#ifdef HAVE_KQUEUE
	case METHOD_KQUEUE:

		if (!re->evlist) {
			size_t sz = re->maxfds * sizeof(*re->evlist);
			re->evlist = mem_zalloc(sz, NULL);
			if (!re->evlist)
				return ENOMEM;
		}

		if (re->kqfd < 0) {
			re->kqfd = kqueue();
			if (re->kqfd < 0)
				return errno;
			DEBUG_INFO("kqueue: fd=%d\n", re->kqfd);
		}

		break;
#endif

	default:
		break;
	}
	return 0;
}


/** Free all resources */
static void poll_close(struct re *re)
{
	if (!re)
		return;

	DEBUG_INFO("poll close\n");

	re->maxfds = 0;

#ifdef HAVE_POLL
	re->fds = mem_deref(re->fds);
#endif
#ifdef HAVE_EPOLL
	DEBUG_INFO("poll_close: epfd=%d\n", re->epfd);

	if (re->epfd >= 0) {
		(void)close(re->epfd);
		re->epfd = -1;
	}

	re->events = mem_deref(re->events);
#endif

#ifdef HAVE_KQUEUE
	if (re->kqfd >= 0) {
		close(re->kqfd);
		re->kqfd = -1;
	}

	re->evlist = mem_deref(re->evlist);
#endif
}


static int poll_setup(struct re *re)
{
	int err;

	err = fd_setsize(DEFAULT_MAXFDS);
	if (err)
		goto out;

	if (METHOD_NULL == re->method) {
		err = poll_method_set(poll_method_best());
		if (err)
			goto out;

		DEBUG_INFO("poll setup: poll method not set - set to `%s'\n",
			   poll_method_name(re->method));
	}

	err = poll_init(re);

 out:
	if (err)
		poll_close(re);

	return err;
}


static void fhs_destroy(void *data)
{
	struct fhs *fhs = data;

	hash_unlink(&fhs->he);
}


static int fhs_update(struct re *re, struct fhs **fhsp, re_sock_t fd,
		      int flags, fd_h *fh, void *arg)
{
	struct fhs *fhs = NULL;
	struct le *he = hash_lookup(re->fhl, (uint32_t)fd, fhs_lookup, &fd);

	if (he) {
		fhs = he->data;
	}
	else {
		fhs = mem_zalloc(sizeof(struct fhs), fhs_destroy);
		if (!fhs)
			return ENOMEM;

		fhs->index = -1;
	}

	if (fhs->index == -1)
		fhs->index = ++re->nfds - 1;

	fhs->fd	   = fd;
	fhs->flags = flags;
	fhs->fh	   = fh;
	fhs->arg   = arg;

	if (!he)
		hash_append(re->fhl, (uint32_t)fd, &fhs->he, fhs);

	*fhsp = fhs;

	return 0;
}


/**
 * Listen for events on a file descriptor
 *
 * @param fd     File descriptor
 * @param flags  Wanted event flags
 * @param fh     Event handler
 * @param arg    Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int fd_listen(re_sock_t fd, int flags, fd_h *fh, void *arg)
{
	struct re *re = re_get();
	struct fhs *fhs = NULL;
	int err = 0;

	if (!re) {
		DEBUG_WARNING("fd_listen: re not ready\n");
		return EINVAL;
	}

	DEBUG_INFO("fd_listen: fd=%d flags=0x%02x\n", fd, flags);

#ifndef RELEASE
	err = re_thread_check();
	if (err)
		return err;
#endif

	if (fd == BAD_SOCK) {
		DEBUG_WARNING("fd_listen: corrupt fd %d\n", fd);
		return EBADF;
	}

	if (flags || fh) {
		err = poll_setup(re);
		if (err)
			return err;
	}

	err = fhs_update(re, &fhs, fd, flags, fh, arg);
	if (err)
		return err;

	switch (re->method) {
#ifdef HAVE_SELECT
	case METHOD_SELECT:
#ifdef WIN32
		if (re->nfds >= DEFAULT_MAXFDS)
			err = EMFILE;
#else
		if (fd + 1 >= DEFAULT_MAXFDS)
			err = EMFILE;
#endif
		break;
#endif

#ifdef HAVE_POLL
	case METHOD_POLL:
		err = set_poll_fds(re, fhs);
		break;
#endif

#ifdef HAVE_EPOLL
	case METHOD_EPOLL:
		if (re->epfd < 0)
			return EBADFD;
		err = set_epoll_fds(re, fhs);
		break;
#endif

#ifdef HAVE_KQUEUE
	case METHOD_KQUEUE:
		err = set_kqueue_fds(re, fhs);
		break;
#endif

	default:
		break;
	}

#ifndef WIN32
	if (!err)
		re->max_fd = max(re->max_fd, fd + 1);
#endif

	if (!flags) {
		if (!re->fhs_reuse) {
			if (re->polling)
				list_append(&re->fhs_delete, &fhs->le_delete,
					    fhs);
			else
				mem_deref(fhs);
		}
		fhs->index = -1;
		--re->nfds;
	}

	if (err && flags) {
		fd_close(fd);
		DEBUG_WARNING("fd_listen: fd=%d flags=0x%02x (%m)\n", fd,
			      flags, err);
	}

	return err;
}


/**
 * Stop listening for events on a file descriptor
 *
 * @param fd     File descriptor
 */
void fd_close(re_sock_t fd)
{
	(void)fd_listen(fd, 0, NULL, NULL);
}


/**
 * Polling loop
 *
 * @param re Poll state.
 *
 * @return 0 if success, otherwise errorcode
 */
static int fd_poll(struct re *re)
{
	const uint64_t to = tmr_next_timeout(&re->tmrl);
	int n;
	struct le *he;
	int nfds = re->nfds;
#ifdef HAVE_SELECT
	fd_set rfds, wfds, efds;
#ifdef WIN32
	re_sock_t sfds[DEFAULT_MAXFDS];
#endif
#endif

	DEBUG_INFO("next timer: %llu ms\n", to);

	/* Wait for I/O */
	switch (re->method) {

#ifdef HAVE_POLL
	case METHOD_POLL:
		re_unlock(re);
		n = poll(re->fds, re->nfds, to ? (int)to : -1);
		re_lock(re);
		break;
#endif
#ifdef HAVE_SELECT
	case METHOD_SELECT: {
		struct timeval tv;

		/* Clear and update fd sets */
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&efds);

		uint32_t bsize = hash_bsize(re->fhl);
		for (uint32_t i = 0; i < bsize; i++) {
			LIST_FOREACH(hash_list_idx(re->fhl, i), he)
			{
				struct fhs *fhs = he->data;

				if (!fhs->flags)
					continue;
#ifdef WIN32
				if (fhs->index < DEFAULT_MAXFDS)
					sfds[fhs->index] = fhs->fd;
				else
					return EMFILE;
#endif

				if (fhs->flags & FD_READ)
					FD_SET(fhs->fd, &rfds);
				if (fhs->flags & FD_WRITE)
					FD_SET(fhs->fd, &wfds);
				if (fhs->flags & FD_EXCEPT)
					FD_SET(fhs->fd, &efds);
			}
		}

		if (re->max_fd)
			nfds = re->max_fd;
#ifdef WIN32
		tv.tv_sec  = (long) to / 1000;
#else
		tv.tv_sec  = (time_t) to / 1000;
#endif
		tv.tv_usec = (uint32_t) (to % 1000) * 1000;
		re_unlock(re);
		n = select(nfds, &rfds, &wfds, &efds, to ? &tv : NULL);
		re_lock(re);
	}
		break;
#endif
#ifdef HAVE_EPOLL
	case METHOD_EPOLL:
		re_unlock(re);
		n = epoll_wait(re->epfd, re->events, re->maxfds,
			       to ? (int)to : -1);
		re_lock(re);
		break;
#endif

#ifdef HAVE_KQUEUE
	case METHOD_KQUEUE: {
		struct timespec timeout;

		timeout.tv_sec = (time_t) (to / 1000);
		timeout.tv_nsec = (to % 1000) * 1000000;

		re_unlock(re);
		n = kevent(re->kqfd, NULL, 0, re->evlist, re->maxfds,
			   to ? &timeout : NULL);
		re_lock(re);
		}
		break;
#endif

	default:
		(void)to;
		DEBUG_WARNING("no polling method set\n");
		return EINVAL;
	}

	if (n < 0)
		return ERRNO_SOCK;

	for (int i = 0; (n > 0) && (i < nfds); i++) {
		re_sock_t fd;
		struct fhs *fhs = NULL;
		int flags = 0;

		switch (re->method) {
#ifdef HAVE_SELECT
		case METHOD_SELECT:
#ifdef WIN32
			fd = sfds[i];
#else
			fd = i;
#endif
			if (FD_ISSET(fd, &rfds))
				flags |= FD_READ;
			if (FD_ISSET(fd, &wfds))
				flags |= FD_WRITE;
			if (FD_ISSET(fd, &efds))
				flags |= FD_EXCEPT;
			break;
#endif
#ifdef HAVE_POLL
		case METHOD_POLL:
			fd = re->fds[i].fd;

			if (re->fds[i].revents & POLLIN)
				flags |= FD_READ;
			if (re->fds[i].revents & POLLOUT)
				flags |= FD_WRITE;
			if (re->fds[i].revents & (POLLERR|POLLHUP|POLLNVAL))
				flags |= FD_EXCEPT;
			if (re->fds[i].revents & POLLNVAL) {
				DEBUG_WARNING("event: fd=%d POLLNVAL"
					      " (fds.fd=%d,"
					      " fds.events=0x%02x)\n",
					      fd, re->fds[i].fd,
					      re->fds[i].events);
			}
			/* Clear events */
			re->fds[i].revents = 0;
			break;
#endif
#ifdef HAVE_EPOLL
		case METHOD_EPOLL:
			fhs = re->events[i].data.ptr;
			fd = fhs->fd;

			if (re->events[i].events & EPOLLIN)
				flags |= FD_READ;
			if (re->events[i].events & EPOLLOUT)
				flags |= FD_WRITE;
			if (re->events[i].events & (EPOLLERR|EPOLLHUP))
				flags |= FD_EXCEPT;

			if (!flags) {
				DEBUG_WARNING("epoll: no flags fd=%d\n", fd);
			}

			break;
#endif

#ifdef HAVE_KQUEUE
		case METHOD_KQUEUE: {

			struct kevent *kev = &re->evlist[i];

			fd = (int)kev->ident;
			fhs = kev->udata;

			if (fd >= re->maxfds) {
				DEBUG_WARNING("large fd=%d\n", fd);
				break;
			}

			if (kev->filter == EVFILT_READ)
				flags |= FD_READ;
			else if (kev->filter == EVFILT_WRITE)
				flags |= FD_WRITE;
			else {
				DEBUG_WARNING("kqueue: unhandled "
					      "filter %x\n",
					      kev->filter);
			}

			if (kev->flags & EV_EOF) {
				flags |= FD_EXCEPT;
			}
			if (kev->flags & EV_ERROR) {
				DEBUG_WARNING("kqueue: EV_ERROR on fd %d\n",
					      fd);
			}

			if (!flags) {
				DEBUG_WARNING("kqueue: no flags fd=%d\n", fd);
			}
		}
			break;
#endif

		default:
			return EINVAL;
		}

		if (!flags)
			continue;

		if (!fhs) {
			he = hash_lookup(re->fhl, (uint32_t)fd, fhs_lookup,
					 &fd);
			if (!he) {
				DEBUG_WARNING("hash_lookup err fd=%d\n", fd);
				continue;
			}
			fhs = he->data;
		}

		if (fhs->fh && fhs->index >= 0) {
#if MAIN_DEBUG
			fd_handler(fhs, flags);
#else
			fhs->fh(flags, fhs->arg);
#endif
		}

		/* Check if polling method was changed */
		if (re->update) {
			re->update = false;
			return 0;
		}

		/* Handle only active events */
		--n;
	}

	list_flush(&re->fhs_delete);

	return 0;
}


/**
 * Set the maximum number of file descriptors
 *
 * @note Only first call inits maxfds, so call after libre_init() and
 * before re_main() in custom applications.
 *
 * @param maxfds Max FDs. 0 to free and -1 for RLIMIT_NOFILE (Linux/Unix only)
 *
 *
 * @return 0 if success, otherwise errorcode
 */
int fd_setsize(int maxfds)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("fd_setsize: re not ready\n");
		return EINVAL;
	}

	if (!maxfds) {
		fd_debug();
		poll_close(re);
		return 0;
	}

#ifdef WIN32
	if (maxfds < 0)
		return ENOSYS;
#else
	if (maxfds < 0) {
		struct rlimit limits;
		int err;

		err = getrlimit(RLIMIT_NOFILE, &limits);
		if (err) {
			DEBUG_WARNING("fd_setsize: error rlimit: %m\n", err);
			return err;
		}

		maxfds = (int)limits.rlim_cur;
	}
#endif

	if (!re->maxfds)
		re->maxfds = maxfds;

	return 0;
}


/**
 * Print all file descriptors in-use
 */
void fd_debug(void)
{
	const struct re *re = re_get();
	struct le *he;

	if (!re) {
		DEBUG_WARNING("fd_debug: re not ready\n");
		return;
	}

	if (!re->fhl)
		return;

	uint32_t bsize = hash_bsize(re->fhl);
	for (uint32_t i = 0; i < bsize; i++) {
		LIST_FOREACH(hash_list_idx(re->fhl, i), he)
		{
			struct fhs *fhs = he->data;

			if (!fhs->flags)
				continue;

			(void)re_fprintf(
				stderr,
				"fd %d in use: flags=%x fh=%p arg=%p\n",
				fhs->fd, fhs->flags, fhs->fh, fhs->arg);
		}
	}
}


#ifdef HAVE_SIGNAL
/* Thread-safe signal handling */
static void signal_handler(int sig)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("signal_handler: re not ready\n");
		return;
	}

	(void)signal(sig, signal_handler);
	re->sig = sig;
}
#endif


/**
 * Main polling loop for async I/O events. This function will only return when
 * re_cancel() is called or an error occured.
 *
 * @param signalh Optional Signal handler
 *
 * @return 0 if success, otherwise errorcode
 */
int re_main(re_signal_h *signalh)
{
	struct re *re = re_get();
	int err;

	if (!re) {
		DEBUG_WARNING("re_main: re not ready\n");
		return EINVAL;
	}

#ifdef HAVE_SIGNAL
	if (signalh) {
		(void)signal(SIGINT, signal_handler);
		(void)signal(SIGALRM, signal_handler);
		(void)signal(SIGTERM, signal_handler);
	}
#endif

	if (re->polling) {
		DEBUG_WARNING("main loop already polling\n");
		return EALREADY;
	}

	err = poll_setup(re);
	if (err)
		goto out;

	DEBUG_INFO("Using async I/O polling method: `%s'\n",
		   poll_method_name(re->method));

	re->polling = true;

	re_lock(re);
	for (;;) {

		if (re->sig) {
			if (signalh)
				signalh(re->sig);

			re->sig = 0;
		}

		if (!re->polling) {
			err = 0;
			break;
		}


		err = fd_poll(re);
		if (err) {
			if (EINTR == err)
				continue;

#ifdef DARWIN
			/* NOTE: workaround for Darwin */
			if (EBADF == err)
				continue;

#endif
#ifdef WIN32
			if (WSAEINVAL == err) {
				tmr_poll(&re->tmrl);
				continue;
			}
#endif
			break;
		}

		tmr_poll(&re->tmrl);
	}
	re_unlock(re);

 out:
	re->polling = false;

	return err;
}


/**
 * Cancel the main polling loop
 */
void re_cancel(void)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("re_cancel: re not ready\n");
		return;
	}

	re->polling = false;
}


/**
 * Debug the main polling loop
 *
 * @param pf     Print handler where debug output is printed to
 * @param unused Unused parameter
 *
 * @return 0 if success, otherwise errorcode
 */
int re_debug(struct re_printf *pf, void *unused)
{
	struct re *re = re_get();
	int err = 0;

	(void)unused;

	if (!re) {
		DEBUG_WARNING("re_debug: re not ready\n");
		return EINVAL;
	}

	err |= re_hprintf(pf, "re main loop:\n");
	err |= re_hprintf(pf, "  maxfds:  %d\n", re->maxfds);
	err |= re_hprintf(pf, "  nfds:    %d\n", re->nfds);
	err |= re_hprintf(pf, "  method:  %d (%s)\n", re->method,
			  poll_method_name(re->method));

	return err;
}


/**
 * Get number of active file descriptors
 *
 * @return nfds
 */
int re_nfds(void)
{
	struct re *re = re_get();

	return re ? re->nfds : 0;
}


/**
 * Get current async I/O polling method.
 *
 * @return enum poll_method
 */
enum poll_method poll_method_get(void)
{
	struct re *re = re_get();

	return re ? re->method : METHOD_NULL;
}


/**
 * Set async I/O polling method. This function can also be called while the
 * program is running.
 *
 * @param method New polling method
 *
 * @return 0 if success, otherwise errorcode
 */
int poll_method_set(enum poll_method method)
{
	struct re *re = re_get();
	struct le *le;
	int err;

	if (!re) {
		DEBUG_WARNING("poll_method_set: re not ready\n");
		return EINVAL;
	}

	err = fd_setsize(DEFAULT_MAXFDS);
	if (err)
		return err;

	switch (method) {

#ifdef HAVE_POLL
	case METHOD_POLL:
		break;
#endif
#ifdef HAVE_SELECT
	case METHOD_SELECT:
#ifdef WIN32
		if (re->nfds > (int)DEFAULT_MAXFDS) {
			DEBUG_WARNING("poll_method_set: can not use SELECT "
				      "max. FDs are reached\n");
			return EMFILE;
		}
#else
		if (re->max_fd > (int)DEFAULT_MAXFDS) {
			DEBUG_WARNING("poll_method_set: can not use SELECT "
				      "max. FDs are reached\n");
			return EMFILE;
		}
#endif
		break;
#endif
#ifdef HAVE_EPOLL
	case METHOD_EPOLL:
		break;
#endif
#ifdef HAVE_KQUEUE
	case METHOD_KQUEUE:
		break;
#endif
	default:
		DEBUG_WARNING("poll method not supported: '%s'\n",
			      poll_method_name(method));
		return EINVAL;
	}

	re->method = method;
	re->update = true;

	DEBUG_INFO("Setting async I/O polling method to `%s'\n",
		   poll_method_name(re->method));

	err = poll_init(re);
	if (err)
		return err;

	DEBUG_INFO("rebuilding fds (nfds=%d)\n", re->nfds);
	le = hash_apply(re->fhl, rebuild_fd, re);
	if (le)
		return EBADF;

	return 0;
}


/**
 * Add a worker thread for this thread
 *
 * @note: for main thread this is called by libre_init()
 *
 * @return 0 if success, otherwise errorcode
 */
int re_thread_init(void)
{
	struct re *re;
	int err;

	call_once(&flag, re_once);

	re = tss_get(key);
	if (re) {
		DEBUG_WARNING("thread_init: already added for thread\n");
		return EALREADY;
	}

	err = re_alloc(&re);
	if (err)
		return err;

	if (!re_global)
		re_global = re;

	err = tss_set(key, re);
	if (err != thrd_success) {
		err = ENOMEM;
		DEBUG_WARNING("thread_init: tss_set error\n");
	}

	return err;
}


/**
 * Remove the worker thread for this thread
 */
void re_thread_close(void)
{
	struct re *re;

	call_once(&flag, re_once);

	re = tss_get(key);
	if (re) {
		if (re == re_global)
			re_global = NULL;
		mem_deref(re);
		tss_set(key, NULL);
	}
}


/**
 * Enter an 're' thread
 */
void re_thread_enter(void)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("re_thread_enter: re not ready\n");
		return;
	}

	re_lock(re);

	/* Disable fhs_reuse (no multithreading support) */
	re->fhs_reuse = false;

	/* set only for non-re threads */
	if (!thrd_equal(re->tid, thrd_current()))
		re->thread_enter = true;
}


/**
 * Leave an 're' thread
 */
void re_thread_leave(void)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("re_thread_leave: re not ready\n");
		return;
	}

	re->thread_enter = false;
	re_unlock(re);
}


/**
 * Set if the fhs allocation should be reused
 *
 * NOTE: POSIX defines that the lowest-numbered file descriptor is returned
 * for the current process. So re reuses the fhs hash entry by default
 * (Linux/Unix only). For lower memory usage this can be disabled.
 *
 * @param re     re context (NULL for re thread/global fallback)
 * @param reuse  If true reuse fhs hash entry, otherwise delete on fd_close
 */
void re_fhs_reuse_set(struct re *re, bool reuse)
{
	if (!re)
		re = re_get();

	if (!re) {
		DEBUG_WARNING("re_fhs_reuse_set: re not ready\n");
		return;
	}

	re->fhs_reuse = reuse;
}


/**
 * Attach the current thread to re context
 */
int re_thread_attach(struct re *context)
{
	struct re *re;

	if (!context)
		return EINVAL;

	call_once(&flag, re_once);

	re = tss_get(key);
	if (re) {
		if (re != context)
			return EALREADY;
		return 0;
	}

	tss_set(key, context);

	return 0;
}


/**
 * Detach the current thread from re context
 */
void re_thread_detach(void)
{
	call_once(&flag, re_once);

	tss_set(key, NULL);
}


/**
 * Set an external mutex for this thread
 *
 * @param mutexp Pointer to external mutex, NULL to use internal
 */
void re_set_mutex(void *mutexp)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("re_set_mutex: re not ready\n");
		return;
	}

	re->mutexp = mutexp ? mutexp : re->mutex;
}


/**
 * Check for NON-RE thread calls
 *
 * @return 0 if success, otherwise EPERM
 */
int re_thread_check(void)
{
	struct re *re = re_get();

	if (!re)
		return EINVAL;

	if (re->thread_enter)
		return 0;

	if (thrd_equal(re->tid, thrd_current()))
		return 0;

	DEBUG_WARNING("thread check: called from a NON-RE thread without "
		      "thread_enter()!\n");

#if DEBUG_LEVEL > 5
	struct btrace trace;
	btrace(&trace);
	DEBUG_INFO("%H", btrace_println, &trace);
#endif

	return EPERM;
}


/**
 * Get the timer-list for this thread
 *
 * @return Timer list
 *
 * @note only used by tmr module
 */
struct list *tmrl_get(void);
struct list *tmrl_get(void)
{
	struct re *re = re_get();

	if (!re) {
		DEBUG_WARNING("tmrl_get: re not ready\n");
		return NULL;
	}

	return &re->tmrl;
}
