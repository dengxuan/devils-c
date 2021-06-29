/** 
 @file  unix.h
 @brief ENet Unix header
*/
#ifndef __DEVILS_UNIX_H__
#define __DEVILS_UNIX_H__

#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#ifdef MSG_MAXIOVLEN
#define DEVILS_BUFFER_MAXIMUM MSG_MAXIOVLEN
#endif

typedef int devils_socket;

#define DEVILS_SOCKET_NULL -1

#define DEVILS_HOST_TO_NET_16(value) (htons(value)) /**< macro that converts host to net byte-order of a 16-bit value */
#define DEVILS_HOST_TO_NET_32(value) (htonl(value)) /**< macro that converts host to net byte-order of a 32-bit value */

#define DEVILS_NET_TO_HOST_16(value) (ntohs(value)) /**< macro that converts net to host byte-order of a 16-bit value */
#define DEVILS_NET_TO_HOST_32(value) (ntohl(value)) /**< macro that converts net to host byte-order of a 32-bit value */

typedef struct
{
    void *data;
    size_t dataLength;
} devils_buffer;

#define DEVILS_CALLBACK

#define DEVILS_API extern

typedef fd_set ENetSocketSet;

#define DEVILS_SOCKETSET_EMPTY(sockset) FD_ZERO(&(sockset))
#define DEVILS_SOCKETSET_ADD(sockset, socket) FD_SET(socket, &(sockset))
#define DEVILS_SOCKETSET_REMOVE(sockset, socket) FD_CLR(socket, &(sockset))
#define DEVILS_SOCKETSET_CHECK(sockset, socket) FD_ISSET(socket, &(sockset))

#endif /* __DEVILS_UNIX_H__ */
