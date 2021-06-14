/** 
 @file  unix.c
 @brief ENet Unix system specific functions
*/
#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define DEVILS_BUILDING_LIB 1
#include "devils/devils.h"

#ifdef __APPLE__
#ifdef HAS_POLL
#undef HAS_POLL
#endif
#ifndef HAS_FCNTL
#define HAS_FCNTL 1
#endif
#ifndef HAS_INET_PTON
#define HAS_INET_PTON 1
#endif
#ifndef HAS_INET_NTOP
#define HAS_INET_NTOP 1
#endif
#ifndef HAS_MSGHDR_FLAGS
#define HAS_MSGHDR_FLAGS 1
#endif
#ifndef HAS_SOCKLEN_T
#define HAS_SOCKLEN_T 1
#endif
#ifndef HAS_GETADDRINFO
#define HAS_GETADDRINFO 1
#endif
#ifndef HAS_GETNAMEINFO
#define HAS_GETNAMEINFO 1
#endif
#endif

#ifdef HAS_FCNTL
#include <fcntl.h>
#endif

#ifdef HAS_POLL
#include <poll.h>
#endif

#if !defined(HAS_SOCKLEN_T) && !defined(__socklen_t_defined)
typedef int socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static devils_uint32 timeBase = 0;

int devils_initialize(void)
{
    return 0;
}

void devils_deinitialize(void)
{
}

devils_uint32
devils_host_random_seed(void)
{
    return (devils_uint32)time(NULL);
}

devils_uint32
devils_time_get(void)
{
    struct timeval timeVal;

    gettimeofday(&timeVal, NULL);

    return timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - timeBase;
}

void devils_time_set(devils_uint32 newTimeBase)
{
    struct timeval timeVal;

    gettimeofday(&timeVal, NULL);

    timeBase = timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - newTimeBase;
}

int devils_address_set_host_ip(devils_address *address, const char *name)
{
#ifdef HAS_INET_PTON
    if (!inet_pton(AF_INET, name, &address->host))
#else
    if (!inet_aton(name, (struct in_addr *)&address->host))
#endif
        return -1;

    return 0;
}

int devils_address_set_host(devils_address *address, const char *name)
{
#ifdef HAS_GETADDRINFO
    struct addrinfo hints, *resultList = NULL, *result = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    if (getaddrinfo(name, NULL, NULL, &resultList) != 0)
        return -1;

    for (result = resultList; result != NULL; result = result->ai_next)
    {
        if (result->ai_family == AF_INET && result->ai_addr != NULL && result->ai_addrlen >= sizeof(struct sockaddr_in))
        {
            struct sockaddr_in *sin = (struct sockaddr_in *)result->ai_addr;

            address->host = sin->sin_addr.s_addr;

            freeaddrinfo(resultList);

            return 0;
        }
    }

    if (resultList != NULL)
        freeaddrinfo(resultList);
#else
    struct hostent *hostEntry = NULL;
#ifdef HAS_GETHOSTBYNAME_R
    struct hostent hostData;
    char buffer[2048];
    int errnum;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
    gethostbyname_r(name, &hostData, buffer, sizeof(buffer), &hostEntry, &errnum);
#else
    hostEntry = gethostbyname_r(name, &hostData, buffer, sizeof(buffer), &errnum);
#endif
#else
    hostEntry = gethostbyname(name);
#endif

    if (hostEntry != NULL && hostEntry->h_addrtype == AF_INET)
    {
        address->host = *(devils_uint32 *)hostEntry->h_addr_list[0];

        return 0;
    }
#endif

    return devils_address_set_host_ip(address, name);
}

int devils_address_get_host_ip(const devils_address *address, char *name, size_t nameLength)
{
#ifdef HAS_INET_NTOP
    if (inet_ntop(AF_INET, &address->host, name, nameLength) == NULL)
#else
    char *addr = inet_ntoa(*(struct in_addr *)&address->host);
    if (addr != NULL)
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
            return -1;
        memcpy(name, addr, addrLen + 1);
    }
    else
#endif
        return -1;
    return 0;
}

int devils_address_get_host(const devils_address *address, char *name, size_t nameLength)
{
#ifdef HAS_GETNAMEINFO
    struct sockaddr_in sin;
    int err;

    memset(&sin, 0, sizeof(struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = DEVILS_HOST_TO_NET_16(address->port);
    sin.sin_addr.s_addr = address->host;

    err = getnameinfo((struct sockaddr *)&sin, sizeof(sin), name, nameLength, NULL, 0, NI_NAMEREQD);
    if (!err)
    {
        if (name != NULL && nameLength > 0 && !memchr(name, '\0', nameLength))
            return -1;
        return 0;
    }
    if (err != EAI_NONAME)
        return -1;
#else
    struct in_addr in;
    struct hostent *hostEntry = NULL;
#ifdef HAS_GETHOSTBYADDR_R
    struct hostent hostData;
    char buffer[2048];
    int errnum;

    in.s_addr = address->host;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
    gethostbyaddr_r((char *)&in, sizeof(struct in_addr), AF_INET, &hostData, buffer, sizeof(buffer), &hostEntry, &errnum);
#else
    hostEntry = gethostbyaddr_r((char *)&in, sizeof(struct in_addr), AF_INET, &hostData, buffer, sizeof(buffer), &errnum);
#endif
#else
    in.s_addr = address->host;

    hostEntry = gethostbyaddr((char *)&in, sizeof(struct in_addr), AF_INET);
#endif

    if (hostEntry != NULL)
    {
        size_t hostLen = strlen(hostEntry->h_name);
        if (hostLen >= nameLength)
            return -1;
        memcpy(name, hostEntry->h_name, hostLen + 1);
        return 0;
    }
#endif

    return devils_address_get_host_ip(address, name, nameLength);
}

int devils_socket_bind(devils_socket socket, const devils_address *address)
{
    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
        sin.sin_port = DEVILS_HOST_TO_NET_16(address->port);
        sin.sin_addr.s_addr = address->host;
    }
    else
    {
        sin.sin_port = 0;
        sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind(socket,
                (struct sockaddr *)&sin,
                sizeof(struct sockaddr_in));
}

int devils_socket_get_address(devils_socket socket, devils_address *address)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof(struct sockaddr_in);

    if (getsockname(socket, (struct sockaddr *)&sin, &sinLength) == -1)
        return -1;

    address->host = (devils_uint32)sin.sin_addr.s_addr;
    address->port = DEVILS_NET_TO_HOST_16(sin.sin_port);

    return 0;
}

int devils_socket_listen(devils_socket socket, int backlog)
{
    return listen(socket, backlog < 0 ? SOMAXCONN : backlog);
}

devils_socket
devils_socket_create(devils_socket_type type)
{
    return socket(PF_INET, type == DEVILS_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int devils_socket_set_option(devils_socket socket, devils_socket_option option, int value)
{
    int result = -1;
    switch (option)
    {
    case DEVILS_SOCKOPT_NONBLOCK:
#ifdef HAS_FCNTL
        result = fcntl(socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl(socket, F_GETFL) & ~O_NONBLOCK));
#else
        result = ioctl(socket, FIONBIO, &value);
#endif
        break;

    case DEVILS_SOCKOPT_BROADCAST:
        result = setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (char *)&value, sizeof(int));
        break;

    case DEVILS_SOCKOPT_REUSEADDR:
        result = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)&value, sizeof(int));
        break;

    case DEVILS_SOCKOPT_RCVBUF:
        result = setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (char *)&value, sizeof(int));
        break;

    case DEVILS_SOCKOPT_SNDBUF:
        result = setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (char *)&value, sizeof(int));
        break;

    case DEVILS_SOCKOPT_RCVTIMEO:
    {
        struct timeval timeVal;
        timeVal.tv_sec = value / 1000;
        timeVal.tv_usec = (value % 1000) * 1000;
        result = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeVal, sizeof(struct timeval));
        break;
    }

    case DEVILS_SOCKOPT_SNDTIMEO:
    {
        struct timeval timeVal;
        timeVal.tv_sec = value / 1000;
        timeVal.tv_usec = (value % 1000) * 1000;
        result = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeVal, sizeof(struct timeval));
        break;
    }

    case DEVILS_SOCKOPT_NODELAY:
        result = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&value, sizeof(int));
        break;

    default:
        break;
    }
    return result == -1 ? -1 : 0;
}

int devils_socket_get_option(devils_socket socket, devils_socket_option option, int *value)
{
    int result = -1;
    socklen_t len;
    switch (option)
    {
    case DEVILS_SOCKOPT_ERROR:
        len = sizeof(int);
        result = getsockopt(socket, SOL_SOCKET, SO_ERROR, value, &len);
        break;

    default:
        break;
    }
    return result == -1 ? -1 : 0;
}

int devils_socket_connect(devils_socket socket, const devils_address *address)
{
    struct sockaddr_in sin;
    int result;

    memset(&sin, 0, sizeof(struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = DEVILS_HOST_TO_NET_16(address->port);
    sin.sin_addr.s_addr = address->host;

    result = connect(socket, (struct sockaddr *)&sin, sizeof(struct sockaddr_in));
    if (result == -1 && errno == EINPROGRESS)
        return 0;

    return result;
}

devils_socket
devils_socket_accept(devils_socket socket, devils_address *address)
{
    int result;
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof(struct sockaddr_in);

    result = accept(socket,
                    address != NULL ? (struct sockaddr *)&sin : NULL,
                    address != NULL ? &sinLength : NULL);

    if (result == -1)
        return DEVILS_SOCKET_NULL;

    if (address != NULL)
    {
        address->host = (devils_uint32)sin.sin_addr.s_addr;
        address->port = DEVILS_NET_TO_HOST_16(sin.sin_port);
    }

    return result;
}

int devils_socket_shutdown(devils_socket socket, devils_socket_shutdown_type how)
{
    return shutdown(socket, (int)how);
}

void devils_socket_destroy(devils_socket socket)
{
    if (socket != -1)
        close(socket);
}

int devils_socket_send(devils_socket socket,
                       const devils_address *address,
                       const devils_buffer *buffers,
                       size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int sentLength;

    memset(&msgHdr, 0, sizeof(struct msghdr));

    if (address != NULL)
    {
        memset(&sin, 0, sizeof(struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = DEVILS_HOST_TO_NET_16(address->port);
        sin.sin_addr.s_addr = address->host;

        msgHdr.msg_name = &sin;
        msgHdr.msg_namelen = sizeof(struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *)buffers;
    msgHdr.msg_iovlen = bufferCount;

    sentLength = sendmsg(socket, &msgHdr, MSG_NOSIGNAL);

    if (sentLength == -1)
    {
        if (errno == EWOULDBLOCK)
            return 0;

        return -1;
    }

    return sentLength;
}

int devils_socket_receive(devils_socket socket,
                          devils_address *address,
                          devils_buffer *buffers,
                          size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int recvLength;

    memset(&msgHdr, 0, sizeof(struct msghdr));

    if (address != NULL)
    {
        msgHdr.msg_name = &sin;
        msgHdr.msg_namelen = sizeof(struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *)buffers;
    msgHdr.msg_iovlen = bufferCount;

    recvLength = recvmsg(socket, &msgHdr, MSG_NOSIGNAL);

    if (recvLength == -1)
    {
        if (errno == EWOULDBLOCK)
            return 0;

        return -1;
    }

#ifdef HAS_MSGHDR_FLAGS
    if (msgHdr.msg_flags & MSG_TRUNC)
        return -1;
#endif

    if (address != NULL)
    {
        address->host = (devils_uint32)sin.sin_addr.s_addr;
        address->port = DEVILS_NET_TO_HOST_16(sin.sin_port);
    }

    return recvLength;
}

int devils_socketset_select(devils_socket maxSocket, ENetSocketSet *readSet, ENetSocketSet *writeSet, devils_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    return select(maxSocket + 1, readSet, writeSet, NULL, &timeVal);
}

int devils_socket_wait(devils_socket socket, devils_uint32 *condition, devils_uint32 timeout)
{
#ifdef HAS_POLL
    struct pollfd pollSocket;
    int pollCount;

    pollSocket.fd = socket;
    pollSocket.events = 0;

    if (*condition & DEVILS_SOCKET_WAIT_SEND)
        pollSocket.events |= POLLOUT;

    if (*condition & DEVILS_SOCKET_WAIT_RECEIVE)
        pollSocket.events |= POLLIN;

    pollCount = poll(&pollSocket, 1, timeout);

    if (pollCount < 0)
    {
        if (errno == EINTR && *condition & DEVILS_SOCKET_WAIT_INTERRUPT)
        {
            *condition = DEVILS_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    *condition = DEVILS_SOCKET_WAIT_NONE;

    if (pollCount == 0)
        return 0;

    if (pollSocket.revents & POLLOUT)
        *condition |= DEVILS_SOCKET_WAIT_SEND;

    if (pollSocket.revents & POLLIN)
        *condition |= DEVILS_SOCKET_WAIT_RECEIVE;

    return 0;
#else
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);

    if (*condition & DEVILS_SOCKET_WAIT_SEND)
        FD_SET(socket, &writeSet);

    if (*condition & DEVILS_SOCKET_WAIT_RECEIVE)
        FD_SET(socket, &readSet);

    selectCount = select(socket + 1, &readSet, &writeSet, NULL, &timeVal);

    if (selectCount < 0)
    {
        if (errno == EINTR && *condition & DEVILS_SOCKET_WAIT_INTERRUPT)
        {
            *condition = DEVILS_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    *condition = DEVILS_SOCKET_WAIT_NONE;

    if (selectCount == 0)
        return 0;

    if (FD_ISSET(socket, &writeSet))
        *condition |= DEVILS_SOCKET_WAIT_SEND;

    if (FD_ISSET(socket, &readSet))
        *condition |= DEVILS_SOCKET_WAIT_RECEIVE;

    return 0;
#endif
}

#endif
