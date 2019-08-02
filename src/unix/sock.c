// Copyright 2015-2018 The NATS Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "../natsp.h"
#include "../mem.h"
#include "../comsock.h"

void
natsSys_Init(void)
{
    // Would do anything that needs to be initialized when
    // the library loads, specific to unix.
}

natsStatus
natsSock_Init(natsSockCtx *ctx)
{
    memset(ctx, 0, sizeof(natsSockCtx));

    ctx->fd = NATS_SOCK_INVALID;

#if NATS_ASYNC_IO == NATS_ASYNC_IO_SELECT
    return natsSock_CreateFDSet(&ctx->fdSet);
#elif NATS_ASYNC_IO == NATS_ASYNC_IO_EPOLL
    ctx->epFD = epoll_create1(0);
    if (ctx->epFD < 0)
        return nats_setError(NATS_IO_ERROR, "epoll_create1 error: %d", ctx->epFD);
#else
#error invalid NATS_ASYNC_IO
#endif
    return NATS_OK;
}

void
natsSock_Clear(natsSockCtx *ctx)
{
#if NATS_ASYNC_IO == NATS_ASYNC_IO_SELECT
    natsSock_DestroyFDSet(ctx->fdSet);
#elif NATS_ASYNC_IO == NATS_ASYNC_IO_EPOLL
    close(ctx->epFD);
#endif
}

#if NATS_ASYNC_IO == NATS_ASYNC_IO_SELECT
natsStatus
natsSock_WaitReady(int waitMode, natsSockCtx *ctx)
{
    struct timeval  *timeout = NULL;
    int             res;
    fd_set          *fdSet = ctx->fdSet;
    natsSock        sock = ctx->fd;
    natsDeadline    *deadline = &(ctx->deadline);

    FD_ZERO(fdSet);
    FD_SET(sock, fdSet);

     if (deadline != NULL)
        timeout = natsDeadline_GetTimeout(deadline);

    switch (waitMode)
    {
        case WAIT_FOR_READ:     res = select((int) (sock + 1), fdSet, NULL, NULL, timeout); break;
        case WAIT_FOR_WRITE:    res = select((int) (sock + 1), NULL, fdSet, NULL, timeout); break;
        case WAIT_FOR_CONNECT:  res = select((int) (sock + 1), NULL, fdSet, NULL, timeout); break;
        default: abort();
    }

    if (res == NATS_SOCK_ERROR)
        return nats_setError(NATS_IO_ERROR, "select error: %d", res);

    if ((res == 0) || !FD_ISSET(sock, fdSet))
        return nats_setDefaultError(NATS_TIMEOUT);

    return NATS_OK;
}
#elif NATS_ASYNC_IO == NATS_ASYNC_IO_EPOLL
natsStatus
natsSock_WaitReady(int waitMode, natsSockCtx *ctx)
{
    struct timeval     *timeout = NULL;
    int                timeoutMS = -1;
    int                nfds, i;
    struct epoll_event ev, ev_ret[1];
    natsSock           sock = ctx->fd;
    natsDeadline       *deadline = &(ctx->deadline);

    memset(&ev, 0, sizeof(ev));
    switch (waitMode)
    {
        case WAIT_FOR_READ:     ev.events = EPOLLIN; break;
        case WAIT_FOR_WRITE:    ev.events = EPOLLOUT; break;
        case WAIT_FOR_CONNECT:  ev.events = EPOLLOUT; break;
        default: abort();
    }
    ev.data.fd = sock;
    if (epoll_ctl(ctx->epFD, EPOLL_CTL_ADD, sock, &ev) != 0)
        return nats_setError(NATS_IO_ERROR, "epoll_ctl(ADD) error: %d", errno);

    if (deadline != NULL)
        timeout = natsDeadline_GetTimeout(deadline);
    if (timeout != NULL)
        timeoutMS = timeout->tv_sec * 1000 + timeout->tv_usec * 0.001;

    nfds = epoll_wait(ctx->epFD, ev_ret, 1, timeoutMS);

    if (epoll_ctl(ctx->epFD, EPOLL_CTL_DEL, sock, &ev) != 0)
        return nats_setError(NATS_IO_ERROR, "epoll_ctl(DEL) error: %d", errno);

    if (nfds == -1)
        return nats_setError(NATS_IO_ERROR, "epoll_wait error: %d", nfds);

    if (nfds != 1 || ev_ret[0].data.fd != sock)
        return nats_setDefaultError(NATS_TIMEOUT);

    return NATS_OK;
}
#endif

natsStatus
natsSock_SetBlocking(natsSock fd, bool blocking)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL)) == -1)
        return nats_setError(NATS_SYS_ERROR, "fcntl error: %d", errno);

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1)
        return nats_setError(NATS_SYS_ERROR, "fcntl error: %d", errno);

    return NATS_OK;
}

bool
natsSock_IsConnected(natsSock fd)
{
    int         res;
    int         error = 0;
    socklen_t   errorLen = (socklen_t) sizeof(int);

    res = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLen);
    if ((res == NATS_SOCK_ERROR) || (error != 0))
        return false;

    return true;
}

natsStatus
natsSock_Flush(natsSock fd)
{
    if (fsync(fd) != 0)
        return nats_setError(NATS_IO_ERROR,
                             "Error flushing socket. Error: %d",
                             NATS_SOCK_GET_ERROR);

    return NATS_OK;
}
