/** 
 @file  win32.c
 @brief ENet Win32 system specific functions
*/
#ifdef _WIN32

#define DEVILS_BUILDING_LIB 1
#include "devils.h"
#include <windows.h>
#include <mmsystem.h>

static devils_uint32 timeBase = 0;

int devils_initialize(void)
{
    WORD versionRequested = MAKEWORD(1, 1);
    WSADATA wsaData;

    if (WSAStartup(versionRequested, &wsaData))
        return -1;

    if (LOBYTE(wsaData.wVersion) != 1 ||
        HIBYTE(wsaData.wVersion) != 1)
    {
        WSACleanup();

        return -1;
    }

    timeBeginPeriod(1);

    return 0;
}

void devils_deinitialize(void)
{
    timeEndPeriod(1);

    WSACleanup();
}

devils_uint32
devils_host_random_seed(void)
{
    return (devils_uint32)timeGetTime();
}

devils_uint32
devils_time_get(void)
{
    return (devils_uint32)timeGetTime() - timeBase;
}

void devils_time_set(devils_uint32 newTimeBase)
{
    timeBase = (devils_uint32)timeGetTime() - newTimeBase;
}

int devils_address_set_host_ip(devils_address *address, const char *name)
{
    devils_uint8 vals[4] = {0, 0, 0, 0};
    int i;

    for (i = 0; i < 4; ++i)
    {
        const char *next = name + 1;
        if (*name != '0')
        {
            long val = strtol(name, (char **)&next, 10);
            if (val < 0 || val > 255 || next == name || next - name > 3)
                return -1;
            vals[i] = (devils_uint8)val;
        }

        if (*next != (i < 3 ? '.' : '\0'))
            return -1;
        name = next + 1;
    }

    memcpy(&address->host, vals, sizeof(devils_uint32));
    return 0;
}

int devils_address_set_host(devils_address *address, const char *name)
{
    struct hostent *hostEntry;

    hostEntry = gethostbyname(name);
    if (hostEntry == NULL ||
        hostEntry->h_addrtype != AF_INET)
        return devils_address_set_host_ip(address, name);

    address->host = *(devils_uint32 *)hostEntry->h_addr_list[0];

    return 0;
}

int devils_address_get_host_ip(const devils_address *address, char *name, size_t nameLength)
{
    char *addr = inet_ntoa(*(struct in_addr *)&address->host);
    if (addr == NULL)
        return -1;
    else
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
            return -1;
        memcpy(name, addr, addrLen + 1);
    }
    return 0;
}

int devils_address_get_host(const devils_address *address, char *name, size_t nameLength)
{
    struct in_addr in;
    struct hostent *hostEntry;

    in.s_addr = address->host;

    hostEntry = gethostbyaddr((char *)&in, sizeof(struct in_addr), AF_INET);
    if (hostEntry == NULL)
        return devils_address_get_host_ip(address, name, nameLength);
    else
    {
        size_t hostLen = strlen(hostEntry->h_name);
        if (hostLen >= nameLength)
            return -1;
        memcpy(name, hostEntry->h_name, hostLen + 1);
    }

    return 0;
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
                sizeof(struct sockaddr_in)) == SOCKET_ERROR
               ? -1
               : 0;
}

int devils_socket_get_address(devils_socket socket, devils_address *address)
{
    struct sockaddr_in sin;
    int sinLength = sizeof(struct sockaddr_in);

    if (getsockname(socket, (struct sockaddr *)&sin, &sinLength) == -1)
        return -1;

    address->host = (devils_uint32)sin.sin_addr.s_addr;
    address->port = DEVILS_NET_TO_HOST_16(sin.sin_port);

    return 0;
}

int devils_socket_listen(devils_socket socket, int backlog)
{
    return listen(socket, backlog < 0 ? SOMAXCONN : backlog) == SOCKET_ERROR ? -1 : 0;
}

devils_socket
devils_socket_create(devils_socket_type type)
{
    return socket(PF_INET, type == DEVILS_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int devils_socket_set_option(devils_socket socket, devils_socket_option option, int value)
{
    int result = SOCKET_ERROR;
    switch (option)
    {
    case DEVILS_SOCKOPT_NONBLOCK:
    {
        u_long nonBlocking = (u_long)value;
        result = ioctlsocket(socket, FIONBIO, &nonBlocking);
        break;
    }

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
        result = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&value, sizeof(int));
        break;

    case DEVILS_SOCKOPT_SNDTIMEO:
        result = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&value, sizeof(int));
        break;

    case DEVILS_SOCKOPT_NODELAY:
        result = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&value, sizeof(int));
        break;

    default:
        break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int devils_socket_get_option(devils_socket socket, devils_socket_option option, int *value)
{
    int result = SOCKET_ERROR, len;
    switch (option)
    {
    case DEVILS_SOCKOPT_ERROR:
        len = sizeof(int);
        result = getsockopt(socket, SOL_SOCKET, SO_ERROR, (char *)value, &len);
        break;

    default:
        break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
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
    if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
        return -1;

    return 0;
}

devils_socket
devils_socket_accept(devils_socket socket, devils_address *address)
{
    SOCKET result;
    struct sockaddr_in sin;
    int sinLength = sizeof(struct sockaddr_in);

    result = accept(socket,
                    address != NULL ? (struct sockaddr *)&sin : NULL,
                    address != NULL ? &sinLength : NULL);

    if (result == INVALID_SOCKET)
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
    return shutdown(socket, (int)how) == SOCKET_ERROR ? -1 : 0;
}

void devils_socket_destroy(devils_socket socket)
{
    if (socket != INVALID_SOCKET)
        closesocket(socket);
}

int devils_socket_send(devils_socket socket,
                       const devils_address *address,
                       const devils_buffer *buffers,
                       size_t bufferCount)
{
    struct sockaddr_in sin;
    DWORD sentLength = 0;

    if (address != NULL)
    {
        memset(&sin, 0, sizeof(struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = DEVILS_HOST_TO_NET_16(address->port);
        sin.sin_addr.s_addr = address->host;
    }

    if (WSASendTo(socket,
                  (LPWSABUF)buffers,
                  (DWORD)bufferCount,
                  &sentLength,
                  0,
                  address != NULL ? (struct sockaddr *)&sin : NULL,
                  address != NULL ? sizeof(struct sockaddr_in) : 0,
                  NULL,
                  NULL) == SOCKET_ERROR)
    {
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            return 0;

        return -1;
    }

    return (int)sentLength;
}

int devils_socket_receive(devils_socket socket,
                          devils_address *address,
                          devils_buffer *buffers,
                          size_t bufferCount)
{
    INT sinLength = sizeof(struct sockaddr_in);
    DWORD flags = 0,
          recvLength = 0;
    struct sockaddr_in sin;

    if (WSARecvFrom(socket,
                    (LPWSABUF)buffers,
                    (DWORD)bufferCount,
                    &recvLength,
                    &flags,
                    address != NULL ? (struct sockaddr *)&sin : NULL,
                    address != NULL ? &sinLength : NULL,
                    NULL,
                    NULL) == SOCKET_ERROR)
    {
        switch (WSAGetLastError())
        {
        case WSAEWOULDBLOCK:
        case WSAECONNRESET:
            return 0;
        }

        return -1;
    }

    if (flags & MSG_PARTIAL)
        return -1;

    if (address != NULL)
    {
        address->host = (devils_uint32)sin.sin_addr.s_addr;
        address->port = DEVILS_NET_TO_HOST_16(sin.sin_port);
    }

    return (int)recvLength;
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
        return -1;

    *condition = DEVILS_SOCKET_WAIT_NONE;

    if (selectCount == 0)
        return 0;

    if (FD_ISSET(socket, &writeSet))
        *condition |= DEVILS_SOCKET_WAIT_SEND;

    if (FD_ISSET(socket, &readSet))
        *condition |= DEVILS_SOCKET_WAIT_RECEIVE;

    return 0;
}

#endif
