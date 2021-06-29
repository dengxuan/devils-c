/** 
 @file  devils.h
 @brief Devils public header file
*/
#ifndef __DEVILS_DEVILS_H__
#define __DEVILS_DEVILS_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdlib.h>

#ifdef _WIN32
#include "../win32/win32.h"
#else
#include "../unix/unix.h"
#endif

#include "devils_types.h"
#include "devils_protocol.h"
#include "devils_list.h"
#include "devils_callbacks.h"

#define DEVILS_VERSION_MAJOR 1
#define DEVILS_VERSION_MINOR 3
#define DEVILS_VERSION_PATCH 17
#define DEVILS_VERSION_CREATE(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define DEVILS_VERSION_GET_MAJOR(version) (((version) >> 16) & 0xFF)
#define DEVILS_VERSION_GET_MINOR(version) (((version) >> 8) & 0xFF)
#define DEVILS_VERSION_GET_PATCH(version) ((version)&0xFF)
#define DEVILS_VERSION DEVILS_VERSION_CREATE(DEVILS_VERSION_MAJOR, DEVILS_VERSION_MINOR, DEVILS_VERSION_PATCH)

   typedef devils_uint32 devils_version;

   struct _devils_host;
   struct _devils_event;
   struct _devils_packet;

   typedef enum _devils_socket_type
   {
      DEVILS_SOCKET_TYPE_STREAM = 1,
      DEVILS_SOCKET_TYPE_DATAGRAM = 2
   } devils_socket_type;

   typedef enum _devils_socket_wait_type
   {
      DEVILS_SOCKET_WAIT_NONE = 0,
      DEVILS_SOCKET_WAIT_SEND = (1 << 0),
      DEVILS_SOCKET_WAIT_RECEIVE = (1 << 1),
      DEVILS_SOCKET_WAIT_INTERRUPT = (1 << 2)
   } devils_socket_wait_type;

   typedef enum _devils_socket_option
   {
      DEVILS_SOCKOPT_NONBLOCK = 1,
      DEVILS_SOCKOPT_BROADCAST = 2,
      DEVILS_SOCKOPT_RCVBUF = 3,
      DEVILS_SOCKOPT_SNDBUF = 4,
      DEVILS_SOCKOPT_REUSEADDR = 5,
      DEVILS_SOCKOPT_RCVTIMEO = 6,
      DEVILS_SOCKOPT_SNDTIMEO = 7,
      DEVILS_SOCKOPT_ERROR = 8,
      DEVILS_SOCKOPT_NODELAY = 9
   } devils_socket_option;

   typedef enum _devils_socket_shutdown_type
   {
      DEVILS_SOCKET_SHUTDOWN_READ = 0,
      DEVILS_SOCKET_SHUTDOWN_WRITE = 1,
      DEVILS_SOCKET_SHUTDOWN_READ_WRITE = 2
   } devils_socket_shutdown_type;

#define DEVILS_HOST_ANY 0
#define DEVILS_HOST_BROADCAST 0xFFFFFFFFU
#define DEVILS_PORT_ANY 0

   /**
 * Portable internet address structure. 
 *
 * The host must be specified in network byte-order, and the port must be in host 
 * byte-order. The constant DEVILS_HOST_ANY may be used to specify the default 
 * server host. The constant DEVILS_HOST_BROADCAST may be used to specify the
 * broadcast address (255.255.255.255).  This makes sense for devils_host_connect,
 * but not for devils_host_create.  Once a server responds to a broadcast, the
 * address is updated from DEVILS_HOST_BROADCAST to the server's actual IP address.
 */
   typedef struct _devils_address
   {
      devils_uint32 host;
      devils_uint16 port;
   } devils_address;

   /**
 * Packet flag bit constants.
 *
 * The host must be specified in network byte-order, and the port must be in
 * host byte-order. The constant DEVILS_HOST_ANY may be used to specify the
 * default server host.
 
   @sa devils_packet
*/
   typedef enum _devils_packet_flag
   {
      /** packet must be received by the target peer and resend attempts should be
     * made until the packet is delivered */
      DEVILS_PACKET_FLAG_RELIABLE = (1 << 0),
      /** packet will not be sequenced with other packets
     * not supported for reliable packets
     */
      DEVILS_PACKET_FLAG_UNSEQUENCED = (1 << 1),
      /** packet will not allocate data, and user must supply it instead */
      DEVILS_PACKET_FLAG_NO_ALLOCATE = (1 << 2),
      /** packet will be fragmented using unreliable (instead of reliable) sends
     * if it exceeds the MTU */
      DEVILS_PACKET_FLAG_UNRELIABLE_FRAGMENT = (1 << 3),

      /** whether the packet has been sent from all queues it has been entered into */
      DEVILS_PACKET_FLAG_SENT = (1 << 8)
   } devils_packet_flag;

   typedef void(DEVILS_CALLBACK *devils_packet_free_callback)(struct _devils_packet *);

   /**
 * ENet packet structure.
 *
 * An ENet data packet that may be sent to or received from a peer. The shown 
 * fields should only be read and never modified. The data field contains the 
 * allocated data for the packet. The dataLength fields specifies the length 
 * of the allocated data.  The flags field is either 0 (specifying no flags), 
 * or a bitwise-or of any combination of the following flags:
 *
 *    DEVILS_PACKET_FLAG_RELIABLE - packet must be received by the target peer
 *    and resend attempts should be made until the packet is delivered
 *
 *    DEVILS_PACKET_FLAG_UNSEQUENCED - packet will not be sequenced with other packets 
 *    (not supported for reliable packets)
 *
 *    DEVILS_PACKET_FLAG_NO_ALLOCATE - packet will not allocate data, and user must supply it instead
 *
 *    DEVILS_PACKET_FLAG_UNRELIABLE_FRAGMENT - packet will be fragmented using unreliable
 *    (instead of reliable) sends if it exceeds the MTU
 *
 *    DEVILS_PACKET_FLAG_SENT - whether the packet has been sent from all queues it has been entered into
   @sa devils_packet_flag
 */
   typedef struct _devils_packet
   {
      size_t referenceCount;                    /**< internal use only */
      devils_uint32 flags;                      /**< bitwise-or of devils_packet_flag constants */
      devils_uint8 *data;                       /**< allocated data for packet */
      size_t dataLength;                        /**< length of data */
      devils_packet_free_callback freeCallback; /**< function to be called when the packet is no longer in use */
      void *userData;                           /**< application private data, may be freely modified */
   } devils_packet;

   typedef struct _devils_acknowledgement
   {
      devils_list_node acknowledgementList;
      devils_uint32 sentTime;
      devils_protocol command;
   } devils_acknowledgement;

   typedef struct _devils_outgoing_command
   {
      devils_list_node outgoingCommandList;
      devils_uint16 reliableSequenceNumber;
      devils_uint16 unreliableSequenceNumber;
      devils_uint32 sentTime;
      devils_uint32 roundTripTimeout;
      devils_uint32 roundTripTimeoutLimit;
      devils_uint32 fragmentOffset;
      devils_uint16 fragmentLength;
      devils_uint16 sendAttempts;
      devils_protocol command;
      devils_packet *packet;
   } devils_outgoing_command;

   typedef struct _devils_incoming_command
   {
      devils_list_node incomingCommandList;
      devils_uint16 reliableSequenceNumber;
      devils_uint16 unreliableSequenceNumber;
      devils_protocol command;
      devils_uint32 fragmentCount;
      devils_uint32 fragmentsRemaining;
      devils_uint32 *fragments;
      devils_packet *packet;
   } devils_incoming_command;

   typedef enum _devils_peer_state
   {
      DEVILS_PEER_STATE_DISCONNECTED = 0,
      DEVILS_PEER_STATE_CONNECTING = 1,
      DEVILS_PEER_STATE_ACKNOWLEDGING_CONNECT = 2,
      DEVILS_PEER_STATE_CONNECTION_PENDING = 3,
      DEVILS_PEER_STATE_CONNECTION_SUCCEEDED = 4,
      DEVILS_PEER_STATE_CONNECTED = 5,
      DEVILS_PEER_STATE_DISCONNECT_LATER = 6,
      DEVILS_PEER_STATE_DISCONNECTING = 7,
      DEVILS_PEER_STATE_ACKNOWLEDGING_DISCONNECT = 8,
      DEVILS_PEER_STATE_ZOMBIE = 9
   } devils_peer_state;

#ifndef DEVILS_BUFFER_MAXIMUM
#define DEVILS_BUFFER_MAXIMUM (1 + 2 * DEVILS_PROTOCOL_MAXIMUM_PACKET_COMMANDS)
#endif

   enum
   {
      DEVILS_HOST_RECEIVE_BUFFER_SIZE = 256 * 1024,
      DEVILS_HOST_SEND_BUFFER_SIZE = 256 * 1024,
      DEVILS_HOST_BANDWIDTH_THROTTLE_INTERVAL = 1000,
      DEVILS_HOST_DEFAULT_MTU = 1400,
      DEVILS_HOST_DEFAULT_MAXIMUM_PACKET_SIZE = 32 * 1024 * 1024,
      DEVILS_HOST_DEFAULT_MAXIMUM_WAITING_DATA = 32 * 1024 * 1024,

      DEVILS_PEER_DEFAULT_ROUND_TRIP_TIME = 500,
      DEVILS_PEER_DEFAULT_PACKET_THROTTLE = 32,
      DEVILS_PEER_PACKET_THROTTLE_SCALE = 32,
      DEVILS_PEER_PACKET_THROTTLE_COUNTER = 7,
      DEVILS_PEER_PACKET_THROTTLE_ACCELERATION = 2,
      DEVILS_PEER_PACKET_THROTTLE_DECELERATION = 2,
      DEVILS_PEER_PACKET_THROTTLE_INTERVAL = 5000,
      DEVILS_PEER_PACKET_LOSS_SCALE = (1 << 16),
      DEVILS_PEER_PACKET_LOSS_INTERVAL = 10000,
      DEVILS_PEER_WINDOW_SIZE_SCALE = 64 * 1024,
      DEVILS_PEER_TIMEOUT_LIMIT = 32,
      DEVILS_PEER_TIMEOUT_MINIMUM = 5000,
      DEVILS_PEER_TIMEOUT_MAXIMUM = 30000,
      DEVILS_PEER_PING_INTERVAL = 500,
      DEVILS_PEER_UNSEQUENCED_WINDOWS = 64,
      DEVILS_PEER_UNSEQUENCED_WINDOW_SIZE = 1024,
      DEVILS_PEER_FREE_UNSEQUENCED_WINDOWS = 32,
      DEVILS_PEER_RELIABLE_WINDOWS = 16,
      DEVILS_PEER_RELIABLE_WINDOW_SIZE = 0x1000,
      DEVILS_PEER_FREE_RELIABLE_WINDOWS = 8
   };

   typedef struct _devils_channel
   {
      devils_uint16 outgoingReliableSequenceNumber;
      devils_uint16 outgoingUnreliableSequenceNumber;
      devils_uint16 usedReliableWindows;
      devils_uint16 reliableWindows[DEVILS_PEER_RELIABLE_WINDOWS];
      devils_uint16 incomingReliableSequenceNumber;
      devils_uint16 incomingUnreliableSequenceNumber;
      devils_list incomingReliableCommands;
      devils_list incomingUnreliableCommands;
   } devils_channel;

   typedef enum _devils_peer_flag
   {
      DEVILS_PEER_FLAG_NEEDS_DISPATCH = (1 << 0)
   } devils_peer_flag;

   /**
 * An ENet peer which data packets may be sent or received from. 
 *
 * No fields should be modified unless otherwise specified. 
 */
   typedef struct _devils_peer
   {
      devils_list_node dispatchList;
      struct _devils_host *host;
      devils_uint16 outgoingPeerID;
      devils_uint16 incomingPeerID;
      devils_uint32 connectID;
      devils_uint8 outgoingSessionID;
      devils_uint8 incomingSessionID;
      devils_address address; /**< Internet address of the peer */
      void *data;             /**< Application private data, may be freely modified */
      devils_peer_state state;
      devils_channel *channels;
      size_t channelCount;             /**< Number of channels allocated for communication with peer */
      devils_uint32 incomingBandwidth; /**< Downstream bandwidth of the client in bytes/second */
      devils_uint32 outgoingBandwidth; /**< Upstream bandwidth of the client in bytes/second */
      devils_uint32 incomingBandwidthThrottleEpoch;
      devils_uint32 outgoingBandwidthThrottleEpoch;
      devils_uint32 incomingDataTotal;
      devils_uint32 outgoingDataTotal;
      devils_uint32 lastSendTime;
      devils_uint32 lastReceiveTime;
      devils_uint32 nextTimeout;
      devils_uint32 earliestTimeout;
      devils_uint32 packetLossEpoch;
      devils_uint32 packetsSent;
      devils_uint32 packetsLost;
      devils_uint32 packetLoss; /**< mean packet loss of reliable packets as a ratio with respect to the constant DEVILS_PEER_PACKET_LOSS_SCALE */
      devils_uint32 packetLossVariance;
      devils_uint32 packetThrottle;
      devils_uint32 packetThrottleLimit;
      devils_uint32 packetThrottleCounter;
      devils_uint32 packetThrottleEpoch;
      devils_uint32 packetThrottleAcceleration;
      devils_uint32 packetThrottleDeceleration;
      devils_uint32 packetThrottleInterval;
      devils_uint32 pingInterval;
      devils_uint32 timeoutLimit;
      devils_uint32 timeoutMinimum;
      devils_uint32 timeoutMaximum;
      devils_uint32 lastRoundTripTime;
      devils_uint32 lowestRoundTripTime;
      devils_uint32 lastRoundTripTimeVariance;
      devils_uint32 highestRoundTripTimeVariance;
      devils_uint32 roundTripTime; /**< mean round trip time (RTT), in milliseconds, between sending a reliable packet and receiving its acknowledgement */
      devils_uint32 roundTripTimeVariance;
      devils_uint32 mtu;
      devils_uint32 windowSize;
      devils_uint32 reliableDataInTransit;
      devils_uint16 outgoingReliableSequenceNumber;
      devils_list acknowledgements;
      devils_list sentReliableCommands;
      devils_list sentUnreliableCommands;
      devils_list outgoingCommands;
      devils_list dispatchedCommands;
      devils_uint16 flags;
      devils_uint16 reserved;
      devils_uint16 incomingUnsequencedGroup;
      devils_uint16 outgoingUnsequencedGroup;
      devils_uint32 unsequencedWindow[DEVILS_PEER_UNSEQUENCED_WINDOW_SIZE / 32];
      devils_uint32 eventData;
      size_t totalWaitingData;
   } devils_peer;

   /** An ENet packet compressor for compressing UDP packets before socket sends or receives.
 */
   typedef struct _devils_compressor
   {
      /** Context data for the compressor. Must be non-NULL. */
      void *context;
      /** Compresses from inBuffers[0:inBufferCount-1], containing inLimit bytes, to outData, outputting at most outLimit bytes. Should return 0 on failure. */
      size_t(DEVILS_CALLBACK *compress)(void *context, const devils_buffer *inBuffers, size_t inBufferCount, size_t inLimit, devils_uint8 *outData, size_t outLimit);
      /** Decompresses from inData, containing inLimit bytes, to outData, outputting at most outLimit bytes. Should return 0 on failure. */
      size_t(DEVILS_CALLBACK *decompress)(void *context, const devils_uint8 *inData, size_t inLimit, devils_uint8 *outData, size_t outLimit);
      /** Destroys the context when compression is disabled or the host is destroyed. May be NULL. */
      void(DEVILS_CALLBACK *destroy)(void *context);
   } devils_compressor;

   /** Callback that computes the checksum of the data held in buffers[0:bufferCount-1] */
   typedef devils_uint32(DEVILS_CALLBACK *devils_checksum_callback)(const devils_buffer *buffers, size_t bufferCount);

   /** Callback for intercepting received raw UDP packets. Should return 1 to intercept, 0 to ignore, or -1 to propagate an error. */
   typedef int(DEVILS_CALLBACK *devils_intercept_callback)(struct _devils_host *host, struct _devils_event *event);

   /** An ENet host for communicating with peers.
  *
  * No fields should be modified unless otherwise stated.

    @sa devils_host_create()
    @sa devils_host_destroy()
    @sa devils_host_connect()
    @sa devils_host_service()
    @sa devils_host_flush()
    @sa devils_host_broadcast()
    @sa devils_host_compress()
    @sa devils_host_compress_with_range_coder()
    @sa devils_host_channel_limit()
    @sa devils_host_bandwidth_limit()
    @sa devils_host_bandwidth_throttle()
  */
   typedef struct _devils_host
   {
      devils_socket socket;
      devils_address address;          /**< Internet address of the host */
      devils_uint32 incomingBandwidth; /**< downstream bandwidth of the host */
      devils_uint32 outgoingBandwidth; /**< upstream bandwidth of the host */
      devils_uint32 bandwidthThrottleEpoch;
      devils_uint32 mtu;
      devils_uint32 randomSeed;
      int recalculateBandwidthLimits;
      devils_peer *peers;  /**< array of peers allocated for this host */
      size_t peerCount;    /**< number of peers allocated for this host */
      size_t channelLimit; /**< maximum number of channels allowed for connected peers */
      devils_uint32 serviceTime;
      devils_list dispatchQueue;
      int continueSending;
      size_t packetSize;
      devils_uint16 headerFlags;
      devils_protocol commands[DEVILS_PROTOCOL_MAXIMUM_PACKET_COMMANDS];
      size_t commandCount;
      devils_buffer buffers[DEVILS_BUFFER_MAXIMUM];
      size_t bufferCount;
      devils_checksum_callback checksum; /**< callback the user can set to enable packet checksums for this host */
      devils_compressor compressor;
      devils_uint8 packetData[2][DEVILS_PROTOCOL_MAXIMUM_MTU];
      devils_address receivedAddress;
      devils_uint8 *receivedData;
      size_t receivedDataLength;
      devils_uint32 totalSentData;         /**< total data sent, user should reset to 0 as needed to prevent overflow */
      devils_uint32 totalSentPackets;      /**< total UDP packets sent, user should reset to 0 as needed to prevent overflow */
      devils_uint32 totalReceivedData;     /**< total data received, user should reset to 0 as needed to prevent overflow */
      devils_uint32 totalReceivedPackets;  /**< total UDP packets received, user should reset to 0 as needed to prevent overflow */
      devils_intercept_callback intercept; /**< callback the user can set to intercept received raw UDP packets */
      size_t connectedPeers;
      size_t bandwidthLimitedPeers;
      size_t duplicatePeers;     /**< optional number of allowed peers from duplicate IPs, defaults to DEVILS_PROTOCOL_MAXIMUM_PEER_ID */
      size_t maximumPacketSize;  /**< the maximum allowable packet size that may be sent or received on a peer */
      size_t maximumWaitingData; /**< the maximum aggregate amount of buffer space a peer may use waiting for packets to be delivered */
   } devils_host;

   /**
 * An ENet event type, as specified in @ref devils_event.
 */
   typedef enum _devils_event_type
   {
      /** no event occurred within the specified time limit */
      DEVILS_EVENT_TYPE_NONE = 0,

      /** a connection request initiated by devils_host_connect has completed.  
     * The peer field contains the peer which successfully connected. 
     */
      DEVILS_EVENT_TYPE_CONNECT = 1,

      /** a peer has disconnected.  This event is generated on a successful 
     * completion of a disconnect initiated by devils_peer_disconnect, if 
     * a peer has timed out, or if a connection request intialized by 
     * devils_host_connect has timed out.  The peer field contains the peer 
     * which disconnected. The data field contains user supplied data 
     * describing the disconnection, or 0, if none is available.
     */
      DEVILS_EVENT_TYPE_DISCONNECT = 2,

      /** a packet has been received from a peer.  The peer field specifies the
     * peer which sent the packet.  The channelID field specifies the channel
     * number upon which the packet was received.  The packet field contains
     * the packet that was received; this packet must be destroyed with
     * devils_packet_destroy after use.
     */
      DEVILS_EVENT_TYPE_RECEIVE = 3
   } devils_event_type;

   /**
 * An ENet event as returned by devils_host_service().
   
   @sa devils_host_service
 */
   typedef struct _devils_event
   {
      devils_event_type type; /**< type of the event */
      devils_peer *peer;      /**< peer that generated a connect, disconnect or receive event */
      devils_uint8 channelID; /**< channel on the peer that generated the event, if appropriate */
      devils_uint32 data;     /**< data associated with the event, if appropriate */
      devils_packet *packet;  /**< packet associated with the event, if appropriate */
   } devils_event;

   /** @defgroup global ENet global functions
    @{ 
*/

   /** 
  Initializes ENet globally.  Must be called prior to using any functions in
  ENet.
  @returns 0 on success, < 0 on failure
*/
   DEVILS_API int devils_initialize(void);

   /** 
  Initializes ENet globally and supplies user-overridden callbacks. Must be called prior to using any functions in ENet. Do not use devils_initialize() if you use this variant. Make sure the devils_callbacks structure is zeroed out so that any additional callbacks added in future versions will be properly ignored.

  @param version the constant DEVILS_VERSION should be supplied so ENet knows which version of devils_callbacks struct to use
  @param inits user-overridden callbacks where any NULL callbacks will use ENet's defaults
  @returns 0 on success, < 0 on failure
*/
   DEVILS_API int devils_initialize_with_callbacks(devils_version version, const devils_callbacks *inits);

   /** 
  Shuts down ENet globally.  Should be called when a program that has
  initialized ENet exits.
*/
   DEVILS_API void devils_deinitialize(void);

   /**
  Gives the linked version of the ENet library.
  @returns the version number 
*/
   DEVILS_API devils_version devils_linked_version(void);

   /** @} */

   /** @defgroup private ENet private implementation functions */

   /**
  Returns the wall-time in milliseconds.  Its initial value is unspecified
  unless otherwise set.
  */
   DEVILS_API devils_uint32 devils_time_get(void);
   /**
  Sets the current wall-time in milliseconds.
  */
   DEVILS_API void devils_time_set(devils_uint32);

   /** @defgroup socket ENet socket functions
    @{
*/
   DEVILS_API devils_socket devils_socket_create(devils_socket_type);
   DEVILS_API int devils_socket_bind(devils_socket, const devils_address *);
   DEVILS_API int devils_socket_get_address(devils_socket, devils_address *);
   DEVILS_API int devils_socket_listen(devils_socket, int);
   DEVILS_API devils_socket devils_socket_accept(devils_socket, devils_address *);
   DEVILS_API int devils_socket_connect(devils_socket, const devils_address *);
   DEVILS_API int devils_socket_send(devils_socket, const devils_address *, const devils_buffer *, size_t);
   DEVILS_API int devils_socket_receive(devils_socket, devils_address *, devils_buffer *, size_t);
   DEVILS_API int devils_socket_wait(devils_socket, devils_uint32 *, devils_uint32);
   DEVILS_API int devils_socket_set_option(devils_socket, devils_socket_option, int);
   DEVILS_API int devils_socket_get_option(devils_socket, devils_socket_option, int *);
   DEVILS_API int devils_socket_shutdown(devils_socket, devils_socket_shutdown_type);
   DEVILS_API void devils_socket_destroy(devils_socket);
   DEVILS_API int devils_socketset_select(devils_socket, ENetSocketSet *, ENetSocketSet *, devils_uint32);

   /** @} */

   /** @defgroup Address ENet address functions
    @{
*/

   /** Attempts to parse the printable form of the IP address in the parameter hostName
    and sets the host field in the address parameter if successful.
    @param address destination to store the parsed IP address
    @param hostName IP address to parse
    @retval 0 on success
    @retval < 0 on failure
    @returns the address of the given hostName in address on success
*/
   DEVILS_API int devils_address_set_host_ip(devils_address *address, const char *hostName);

   /** Attempts to resolve the host named by the parameter hostName and sets
    the host field in the address parameter if successful.
    @param address destination to store resolved address
    @param hostName host name to lookup
    @retval 0 on success
    @retval < 0 on failure
    @returns the address of the given hostName in address on success
*/
   DEVILS_API int devils_address_set_host(devils_address *address, const char *hostName);

   /** Gives the printable form of the IP address specified in the address parameter.
    @param address    address printed
    @param hostName   destination for name, must not be NULL
    @param nameLength maximum length of hostName.
    @returns the null-terminated name of the host in hostName on success
    @retval 0 on success
    @retval < 0 on failure
*/
   DEVILS_API int devils_address_get_host_ip(const devils_address *address, char *hostName, size_t nameLength);

   /** Attempts to do a reverse lookup of the host field in the address parameter.
    @param address    address used for reverse lookup
    @param hostName   destination for name, must not be NULL
    @param nameLength maximum length of hostName.
    @returns the null-terminated name of the host in hostName on success
    @retval 0 on success
    @retval < 0 on failure
*/
   DEVILS_API int devils_address_get_host(const devils_address *address, char *hostName, size_t nameLength);

   /** @} */

   DEVILS_API devils_packet *devils_packet_create(const void *, size_t, devils_uint32);
   DEVILS_API void devils_packet_destroy(devils_packet *);
   DEVILS_API int devils_packet_resize(devils_packet *, size_t);
   DEVILS_API devils_uint32 devils_crc32(const devils_buffer *, size_t);

   DEVILS_API devils_host *devils_host_create(const devils_address *, size_t, size_t, devils_uint32, devils_uint32);
   DEVILS_API void devils_host_destroy(devils_host *);
   DEVILS_API devils_peer *devils_host_connect(devils_host *, const devils_address *, size_t, devils_uint32);
   DEVILS_API int devils_host_check_events(devils_host *, devils_event *);
   DEVILS_API int devils_host_service(devils_host *, devils_event *, devils_uint32);
   DEVILS_API void devils_host_flush(devils_host *);
   DEVILS_API void devils_host_broadcast(devils_host *, devils_uint8, devils_packet *);
   DEVILS_API void devils_host_compress(devils_host *, const devils_compressor *);
   DEVILS_API int devils_host_compress_with_range_coder(devils_host *host);
   DEVILS_API void devils_host_channel_limit(devils_host *, size_t);
   DEVILS_API void devils_host_bandwidth_limit(devils_host *, devils_uint32, devils_uint32);
   extern void devils_host_bandwidth_throttle(devils_host *);
   extern devils_uint32 devils_host_random_seed(void);
   extern devils_uint32 devils_host_random(devils_host *);

   DEVILS_API int devils_peer_send(devils_peer *, devils_uint8, devils_packet *);
   DEVILS_API devils_packet *devils_peer_receive(devils_peer *, devils_uint8 *channelID);
   DEVILS_API void devils_peer_ping(devils_peer *);
   DEVILS_API void devils_peer_ping_interval(devils_peer *, devils_uint32);
   DEVILS_API void devils_peer_timeout(devils_peer *, devils_uint32, devils_uint32, devils_uint32);
   DEVILS_API void devils_peer_reset(devils_peer *);
   DEVILS_API void devils_peer_disconnect(devils_peer *, devils_uint32);
   DEVILS_API void devils_peer_disconnect_now(devils_peer *, devils_uint32);
   DEVILS_API void devils_peer_disconnect_later(devils_peer *, devils_uint32);
   DEVILS_API void devils_peer_throttle_configure(devils_peer *, devils_uint32, devils_uint32, devils_uint32);
   extern int devils_peer_throttle(devils_peer *, devils_uint32);
   extern void devils_peer_reset_queues(devils_peer *);
   extern void devils_peer_setup_outgoing_command(devils_peer *, devils_outgoing_command *);
   extern devils_outgoing_command *devils_peer_queue_outgoing_command(devils_peer *, const devils_protocol *, devils_packet *, devils_uint32, devils_uint16);
   extern devils_incoming_command *devils_peer_queue_incoming_command(devils_peer *, const devils_protocol *, const void *, size_t, devils_uint32, devils_uint32);
   extern devils_acknowledgement *devils_peer_queue_acknowledgement(devils_peer *, const devils_protocol *, devils_uint16);
   extern void devils_peer_dispatch_incoming_unreliable_commands(devils_peer *, devils_channel *, devils_incoming_command *);
   extern void devils_peer_dispatch_incoming_reliable_commands(devils_peer *, devils_channel *, devils_incoming_command *);
   extern void devils_peer_on_connect(devils_peer *);
   extern void devils_peer_on_disconnect(devils_peer *);

   DEVILS_API void *devils_range_coder_create(void);
   DEVILS_API void devils_range_coder_destroy(void *);
   DEVILS_API size_t devils_range_coder_compress(void *, const devils_buffer *, size_t, size_t, devils_uint8 *, size_t);
   DEVILS_API size_t devils_range_coder_decompress(void *, const devils_uint8 *, size_t, devils_uint8 *, size_t);

   extern size_t devils_protocol_command_size(devils_uint8);

#ifdef __cplusplus
}
#endif

#endif /* __DEVILS_DEVILS_H__ */
