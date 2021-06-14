/** 
 @file  protocol.h
 @brief ENet protocol
*/
#ifndef __DEVILS_PROTOCOL_H__
#define __DEVILS_PROTOCOL_H__

#include "devils_types.h"

enum
{
   DEVILS_PROTOCOL_MINIMUM_MTU = 576,
   DEVILS_PROTOCOL_MAXIMUM_MTU = 4096,
   DEVILS_PROTOCOL_MAXIMUM_PACKET_COMMANDS = 32,
   DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE = 4096,
   DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE = 65536,
   DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT = 1,
   DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT = 255,
   DEVILS_PROTOCOL_MAXIMUM_PEER_ID = 0xFFF,
   DEVILS_PROTOCOL_MAXIMUM_FRAGMENT_COUNT = 1024 * 1024
};

typedef enum _devils_protocol_command
{
   DEVILS_PROTOCOL_COMMAND_NONE = 0,
   DEVILS_PROTOCOL_COMMAND_ACKNOWLEDGE = 1,
   DEVILS_PROTOCOL_COMMAND_CONNECT = 2,
   DEVILS_PROTOCOL_COMMAND_VERIFY_CONNECT = 3,
   DEVILS_PROTOCOL_COMMAND_DISCONNECT = 4,
   DEVILS_PROTOCOL_COMMAND_PING = 5,
   DEVILS_PROTOCOL_COMMAND_SEND_RELIABLE = 6,
   DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE = 7,
   DEVILS_PROTOCOL_COMMAND_SEND_FRAGMENT = 8,
   DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED = 9,
   DEVILS_PROTOCOL_COMMAND_BANDWIDTH_LIMIT = 10,
   DEVILS_PROTOCOL_COMMAND_THROTTLE_CONFIGURE = 11,
   DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT = 12,
   DEVILS_PROTOCOL_COMMAND_COUNT = 13,

   DEVILS_PROTOCOL_COMMAND_MASK = 0x0F
} devils_protocol_command;

typedef enum _devils_protocol_flag
{
   DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE = (1 << 7),
   DEVILS_PROTOCOL_COMMAND_FLAG_UNSEQUENCED = (1 << 6),

   DEVILS_PROTOCOL_HEADER_FLAG_COMPRESSED = (1 << 14),
   DEVILS_PROTOCOL_HEADER_FLAG_SENT_TIME = (1 << 15),
   DEVILS_PROTOCOL_HEADER_FLAG_MASK = DEVILS_PROTOCOL_HEADER_FLAG_COMPRESSED | DEVILS_PROTOCOL_HEADER_FLAG_SENT_TIME,

   DEVILS_PROTOCOL_HEADER_SESSION_MASK = (3 << 12),
   DEVILS_PROTOCOL_HEADER_SESSION_SHIFT = 12
} devils_protocol_flag;

#ifdef _MSC_VER
#pragma pack(push, 1)
#define DEVILS_PACKED
#elif defined(__GNUC__) || defined(__clang__)
#define DEVILS_PACKED __attribute__((packed))
#else
#define DEVILS_PACKED
#endif

typedef struct _devils_protocol_header
{
   devils_uint16 peerID;
   devils_uint16 sentTime;
} DEVILS_PACKED devils_protocol_header;

typedef struct _devils_protocol_command_header
{
   devils_uint8 command;
   devils_uint8 channelID;
   devils_uint16 reliableSequenceNumber;
} DEVILS_PACKED devils_protocol_command_header;

typedef struct _devils_protocol_acknowledge
{
   devils_protocol_command_header header;
   devils_uint16 receivedReliableSequenceNumber;
   devils_uint16 receivedSentTime;
} DEVILS_PACKED devils_protocol_acknowledge;

typedef struct _devils_protocol_connect
{
   devils_protocol_command_header header;
   devils_uint16 outgoingPeerID;
   devils_uint8 incomingSessionID;
   devils_uint8 outgoingSessionID;
   devils_uint32 mtu;
   devils_uint32 windowSize;
   devils_uint32 channelCount;
   devils_uint32 incomingBandwidth;
   devils_uint32 outgoingBandwidth;
   devils_uint32 packetThrottleInterval;
   devils_uint32 packetThrottleAcceleration;
   devils_uint32 packetThrottleDeceleration;
   devils_uint32 connectID;
   devils_uint32 data;
} DEVILS_PACKED devils_protocol_connect;

typedef struct _devils_protocol_verify_connect
{
   devils_protocol_command_header header;
   devils_uint16 outgoingPeerID;
   devils_uint8 incomingSessionID;
   devils_uint8 outgoingSessionID;
   devils_uint32 mtu;
   devils_uint32 windowSize;
   devils_uint32 channelCount;
   devils_uint32 incomingBandwidth;
   devils_uint32 outgoingBandwidth;
   devils_uint32 packetThrottleInterval;
   devils_uint32 packetThrottleAcceleration;
   devils_uint32 packetThrottleDeceleration;
   devils_uint32 connectID;
} DEVILS_PACKED devils_protocol_verify_connect;

typedef struct _devils_protocol_bandwidth_limit
{
   devils_protocol_command_header header;
   devils_uint32 incomingBandwidth;
   devils_uint32 outgoingBandwidth;
} DEVILS_PACKED devils_protocol_bandwidth_limit;

typedef struct _devils_protocol_throttle_configure
{
   devils_protocol_command_header header;
   devils_uint32 packetThrottleInterval;
   devils_uint32 packetThrottleAcceleration;
   devils_uint32 packetThrottleDeceleration;
} DEVILS_PACKED devils_protocol_throttle_configure;

typedef struct _devils_protocol_disconnect
{
   devils_protocol_command_header header;
   devils_uint32 data;
} DEVILS_PACKED devils_protocol_disconnect;

typedef struct _devils_protocol_ping
{
   devils_protocol_command_header header;
} DEVILS_PACKED devils_protocol_ping;

typedef struct _devils_protocol_send_reliable
{
   devils_protocol_command_header header;
   devils_uint16 dataLength;
} DEVILS_PACKED devils_protocol_send_reliable;

typedef struct _devils_protocol_send_unreliable
{
   devils_protocol_command_header header;
   devils_uint16 unreliableSequenceNumber;
   devils_uint16 dataLength;
} DEVILS_PACKED devils_protocol_send_unreliable;

typedef struct _devils_protocol_send_unsequenced
{
   devils_protocol_command_header header;
   devils_uint16 unsequencedGroup;
   devils_uint16 dataLength;
} DEVILS_PACKED devils_protocol_send_unsequenced;

typedef struct _devils_protocol_send_fragment
{
   devils_protocol_command_header header;
   devils_uint16 startSequenceNumber;
   devils_uint16 dataLength;
   devils_uint32 fragmentCount;
   devils_uint32 fragmentNumber;
   devils_uint32 totalLength;
   devils_uint32 fragmentOffset;
} DEVILS_PACKED devils_protocol_send_fragment;

typedef union _devils_protocol
{
   devils_protocol_command_header header;
   devils_protocol_acknowledge acknowledge;
   devils_protocol_connect connect;
   devils_protocol_verify_connect verifyConnect;
   devils_protocol_disconnect disconnect;
   devils_protocol_ping ping;
   devils_protocol_send_reliable sendReliable;
   devils_protocol_send_unreliable sendUnreliable;
   devils_protocol_send_unsequenced sendUnsequenced;
   devils_protocol_send_fragment sendFragment;
   devils_protocol_bandwidth_limit bandwidthLimit;
   devils_protocol_throttle_configure throttleConfigure;
} DEVILS_PACKED devils_protocol;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

#endif /* __DEVILS_PROTOCOL_H__ */
