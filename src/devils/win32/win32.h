/** 
 @file  win32.h
 @brief ENet Win32 header
*/
#ifndef __DEVILS_WIN32_H__
#define __DEVILS_WIN32_H__

#ifdef _MSC_VER
#ifdef DEVILS_BUILDING_LIB
#pragma warning(disable : 4267) // size_t to int conversion
#pragma warning(disable : 4244) // 64bit to 32bit int
#pragma warning(disable : 4018) // signed/unsigned mismatch
#pragma warning(disable : 4146) // unary minus operator applied to unsigned type
#define _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include <stdlib.h>
#include <winsock2.h>

typedef SOCKET devils_socket;

#define DEVILS_SOCKET_NULL INVALID_SOCKET

#define DEVILS_HOST_TO_NET_16(value) (htons(value))
#define DEVILS_HOST_TO_NET_32(value) (htonl(value))

#define DEVILS_NET_TO_HOST_16(value) (ntohs(value))
#define DEVILS_NET_TO_HOST_32(value) (ntohl(value))

typedef struct
{
    size_t dataLength;
    void *data;
} devils_buffer;

#define DEVILS_CALLBACK __cdecl

#ifdef DEVILS_DLL
#ifdef DEVILS_BUILDING_LIB
#define DEVILS_API __declspec(dllexport)
#else
#define DEVILS_API __declspec(dllimport)
#endif /* DEVILS_BUILDING_LIB */
#else  /* !DEVILS_DLL */
#define DEVILS_API extern
#endif /* DEVILS_DLL */

typedef fd_set ENetSocketSet;

#define DEVILS_SOCKETSET_EMPTY(sockset) FD_ZERO(&(sockset))
#define DEVILS_SOCKETSET_ADD(sockset, socket) FD_SET(socket, &(sockset))
#define DEVILS_SOCKETSET_REMOVE(sockset, socket) FD_CLR(socket, &(sockset))
#define DEVILS_SOCKETSET_CHECK(sockset, socket) FD_ISSET(socket, &(sockset))

#endif /* __DEVILS_WIN32_H__ */
