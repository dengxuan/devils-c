/** 
 @file host.c
 @brief ENet host management functions
*/
#define DEVILS_BUILDING_LIB 1
#include <string.h>
#include "include/devils.h"

/** @defgroup host ENet host functions
    @{
*/

/** Creates a host for communicating to peers.  

    @param address   the address at which other peers may connect to this host.  If NULL, then no peers may connect to the host.
    @param peerCount the maximum number of peers that should be allocated for the host.
    @param channelLimit the maximum number of channels allowed; if 0, then this is equivalent to DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT
    @param incomingBandwidth downstream bandwidth of the host in bytes/second; if 0, ENet will assume unlimited bandwidth.
    @param outgoingBandwidth upstream bandwidth of the host in bytes/second; if 0, ENet will assume unlimited bandwidth.

    @returns the host on success and NULL on failure

    @remarks ENet will strategically drop packets on specific sides of a connection between hosts
    to ensure the host's bandwidth is not overwhelmed.  The bandwidth parameters also determine
    the window size of a connection which limits the amount of reliable packets that may be in transit
    at any given time.
*/
devils_host *
devils_host_create(const devils_address *address, size_t peerCount, size_t channelLimit, devils_uint32 incomingBandwidth, devils_uint32 outgoingBandwidth)
{
  devils_host *host;
  devils_peer *currentPeer;

  if (peerCount > DEVILS_PROTOCOL_MAXIMUM_PEER_ID)
    return NULL;

  host = (devils_host *)devils_malloc(sizeof(devils_host));
  if (host == NULL)
    return NULL;
  memset(host, 0, sizeof(devils_host));

  host->peers = (devils_peer *)devils_malloc(peerCount * sizeof(devils_peer));
  if (host->peers == NULL)
  {
    devils_free(host);

    return NULL;
  }
  memset(host->peers, 0, peerCount * sizeof(devils_peer));

  host->socket = devils_socket_create(DEVILS_SOCKET_TYPE_DATAGRAM);
  if (host->socket == DEVILS_SOCKET_NULL || (address != NULL && devils_socket_bind(host->socket, address) < 0))
  {
    if (host->socket != DEVILS_SOCKET_NULL)
      devils_socket_destroy(host->socket);

    devils_free(host->peers);
    devils_free(host);

    return NULL;
  }

  devils_socket_set_option(host->socket, DEVILS_SOCKOPT_NONBLOCK, 1);
  devils_socket_set_option(host->socket, DEVILS_SOCKOPT_BROADCAST, 1);
  devils_socket_set_option(host->socket, DEVILS_SOCKOPT_RCVBUF, DEVILS_HOST_RECEIVE_BUFFER_SIZE);
  devils_socket_set_option(host->socket, DEVILS_SOCKOPT_SNDBUF, DEVILS_HOST_SEND_BUFFER_SIZE);

  if (address != NULL && devils_socket_get_address(host->socket, &host->address) < 0)
    host->address = *address;

  if (!channelLimit || channelLimit > DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
    channelLimit = DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
  else if (channelLimit < DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT)
    channelLimit = DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT;

  host->randomSeed = (devils_uint32)(size_t)host;
  host->randomSeed += devils_host_random_seed();
  host->randomSeed = (host->randomSeed << 16) | (host->randomSeed >> 16);
  host->channelLimit = channelLimit;
  host->incomingBandwidth = incomingBandwidth;
  host->outgoingBandwidth = outgoingBandwidth;
  host->bandwidthThrottleEpoch = 0;
  host->recalculateBandwidthLimits = 0;
  host->mtu = DEVILS_HOST_DEFAULT_MTU;
  host->peerCount = peerCount;
  host->commandCount = 0;
  host->bufferCount = 0;
  host->checksum = NULL;
  host->receivedAddress.host = DEVILS_HOST_ANY;
  host->receivedAddress.port = 0;
  host->receivedData = NULL;
  host->receivedDataLength = 0;

  host->totalSentData = 0;
  host->totalSentPackets = 0;
  host->totalReceivedData = 0;
  host->totalReceivedPackets = 0;

  host->connectedPeers = 0;
  host->bandwidthLimitedPeers = 0;
  host->duplicatePeers = DEVILS_PROTOCOL_MAXIMUM_PEER_ID;
  host->maximumPacketSize = DEVILS_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
  host->maximumWaitingData = DEVILS_HOST_DEFAULT_MAXIMUM_WAITING_DATA;

  host->compressor.context = NULL;
  host->compressor.compress = NULL;
  host->compressor.decompress = NULL;
  host->compressor.destroy = NULL;

  host->intercept = NULL;

  devils_list_clear(&host->dispatchQueue);

  for (currentPeer = host->peers;
       currentPeer < &host->peers[host->peerCount];
       ++currentPeer)
  {
    currentPeer->host = host;
    currentPeer->incomingPeerID = currentPeer - host->peers;
    currentPeer->outgoingSessionID = currentPeer->incomingSessionID = 0xFF;
    currentPeer->data = NULL;

    devils_list_clear(&currentPeer->acknowledgements);
    devils_list_clear(&currentPeer->sentReliableCommands);
    devils_list_clear(&currentPeer->sentUnreliableCommands);
    devils_list_clear(&currentPeer->outgoingCommands);
    devils_list_clear(&currentPeer->dispatchedCommands);

    devils_peer_reset(currentPeer);
  }

  return host;
}

/** Destroys the host and all resources associated with it.
    @param host pointer to the host to destroy
*/
void devils_host_destroy(devils_host *host)
{
  devils_peer *currentPeer;

  if (host == NULL)
    return;

  devils_socket_destroy(host->socket);

  for (currentPeer = host->peers;
       currentPeer < &host->peers[host->peerCount];
       ++currentPeer)
  {
    devils_peer_reset(currentPeer);
  }

  if (host->compressor.context != NULL && host->compressor.destroy)
    (*host->compressor.destroy)(host->compressor.context);

  devils_free(host->peers);
  devils_free(host);
}

devils_uint32
devils_host_random(devils_host *host)
{
  /* Mulberry32 by Tommy Ettinger */
  devils_uint32 n = (host->randomSeed += 0x6D2B79F5U);
  n = (n ^ (n >> 15)) * (n | 1U);
  n ^= n + (n ^ (n >> 7)) * (n | 61U);
  return n ^ (n >> 14);
}

/** Initiates a connection to a foreign host.
    @param host host seeking the connection
    @param address destination for the connection
    @param channelCount number of channels to allocate
    @param data user data supplied to the receiving host 
    @returns a peer representing the foreign host on success, NULL on failure
    @remarks The peer returned will have not completed the connection until devils_host_service()
    notifies of an DEVILS_EVENT_TYPE_CONNECT event for the peer.
*/
devils_peer *
devils_host_connect(devils_host *host, const devils_address *address, size_t channelCount, devils_uint32 data)
{
  devils_peer *currentPeer;
  devils_channel *channel;
  devils_protocol command;

  if (channelCount < DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT)
    channelCount = DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT;
  else if (channelCount > DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
    channelCount = DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT;

  for (currentPeer = host->peers;
       currentPeer < &host->peers[host->peerCount];
       ++currentPeer)
  {
    if (currentPeer->state == DEVILS_PEER_STATE_DISCONNECTED)
      break;
  }

  if (currentPeer >= &host->peers[host->peerCount])
    return NULL;

  currentPeer->channels = (devils_channel *)devils_malloc(channelCount * sizeof(devils_channel));
  if (currentPeer->channels == NULL)
    return NULL;
  currentPeer->channelCount = channelCount;
  currentPeer->state = DEVILS_PEER_STATE_CONNECTING;
  currentPeer->address = *address;
  currentPeer->connectID = devils_host_random(host);

  if (host->outgoingBandwidth == 0)
    currentPeer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;
  else
    currentPeer->windowSize = (host->outgoingBandwidth /
                               DEVILS_PEER_WINDOW_SIZE_SCALE) *
                              DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;

  if (currentPeer->windowSize < DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE)
    currentPeer->windowSize = DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;
  else if (currentPeer->windowSize > DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE)
    currentPeer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;

  for (channel = currentPeer->channels;
       channel < &currentPeer->channels[channelCount];
       ++channel)
  {
    channel->outgoingReliableSequenceNumber = 0;
    channel->outgoingUnreliableSequenceNumber = 0;
    channel->incomingReliableSequenceNumber = 0;
    channel->incomingUnreliableSequenceNumber = 0;

    devils_list_clear(&channel->incomingReliableCommands);
    devils_list_clear(&channel->incomingUnreliableCommands);

    channel->usedReliableWindows = 0;
    memset(channel->reliableWindows, 0, sizeof(channel->reliableWindows));
  }

  command.header.command = DEVILS_PROTOCOL_COMMAND_CONNECT | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
  command.header.channelID = 0xFF;
  command.connect.outgoingPeerID = DEVILS_HOST_TO_NET_16(currentPeer->incomingPeerID);
  command.connect.incomingSessionID = currentPeer->incomingSessionID;
  command.connect.outgoingSessionID = currentPeer->outgoingSessionID;
  command.connect.mtu = DEVILS_HOST_TO_NET_32(currentPeer->mtu);
  command.connect.windowSize = DEVILS_HOST_TO_NET_32(currentPeer->windowSize);
  command.connect.channelCount = DEVILS_HOST_TO_NET_32(channelCount);
  command.connect.incomingBandwidth = DEVILS_HOST_TO_NET_32(host->incomingBandwidth);
  command.connect.outgoingBandwidth = DEVILS_HOST_TO_NET_32(host->outgoingBandwidth);
  command.connect.packetThrottleInterval = DEVILS_HOST_TO_NET_32(currentPeer->packetThrottleInterval);
  command.connect.packetThrottleAcceleration = DEVILS_HOST_TO_NET_32(currentPeer->packetThrottleAcceleration);
  command.connect.packetThrottleDeceleration = DEVILS_HOST_TO_NET_32(currentPeer->packetThrottleDeceleration);
  command.connect.connectID = currentPeer->connectID;
  command.connect.data = DEVILS_HOST_TO_NET_32(data);

  devils_peer_queue_outgoing_command(currentPeer, &command, NULL, 0, 0);

  return currentPeer;
}

/** Queues a packet to be sent to all peers associated with the host.
    @param host host on which to broadcast the packet
    @param channelID channel on which to broadcast
    @param packet packet to broadcast
*/
void devils_host_broadcast(devils_host *host, devils_uint8 channelID, devils_packet *packet)
{
  devils_peer *currentPeer;

  for (currentPeer = host->peers;
       currentPeer < &host->peers[host->peerCount];
       ++currentPeer)
  {
    if (currentPeer->state != DEVILS_PEER_STATE_CONNECTED)
      continue;

    devils_peer_send(currentPeer, channelID, packet);
  }

  if (packet->referenceCount == 0)
    devils_packet_destroy(packet);
}

/** Sets the packet compressor the host should use to compress and decompress packets.
    @param host host to enable or disable compression for
    @param compressor callbacks for for the packet compressor; if NULL, then compression is disabled
*/
void devils_host_compress(devils_host *host, const devils_compressor *compressor)
{
  if (host->compressor.context != NULL && host->compressor.destroy)
    (*host->compressor.destroy)(host->compressor.context);

  if (compressor)
    host->compressor = *compressor;
  else
    host->compressor.context = NULL;
}

/** Limits the maximum allowed channels of future incoming connections.
    @param host host to limit
    @param channelLimit the maximum number of channels allowed; if 0, then this is equivalent to DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT
*/
void devils_host_channel_limit(devils_host *host, size_t channelLimit)
{
  if (!channelLimit || channelLimit > DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
    channelLimit = DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
  else if (channelLimit < DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT)
    channelLimit = DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT;

  host->channelLimit = channelLimit;
}

/** Adjusts the bandwidth limits of a host.
    @param host host to adjust
    @param incomingBandwidth new incoming bandwidth
    @param outgoingBandwidth new outgoing bandwidth
    @remarks the incoming and outgoing bandwidth parameters are identical in function to those
    specified in devils_host_create().
*/
void devils_host_bandwidth_limit(devils_host *host, devils_uint32 incomingBandwidth, devils_uint32 outgoingBandwidth)
{
  host->incomingBandwidth = incomingBandwidth;
  host->outgoingBandwidth = outgoingBandwidth;
  host->recalculateBandwidthLimits = 1;
}

void devils_host_bandwidth_throttle(devils_host *host)
{
  devils_uint32 timeCurrent = devils_time_get(),
                elapsedTime = timeCurrent - host->bandwidthThrottleEpoch,
                peersRemaining = (devils_uint32)host->connectedPeers,
                dataTotal = ~0,
                bandwidth = ~0,
                throttle = 0,
                bandwidthLimit = 0;
  int needsAdjustment = host->bandwidthLimitedPeers > 0 ? 1 : 0;
  devils_peer *peer;
  devils_protocol command;

  if (elapsedTime < DEVILS_HOST_BANDWIDTH_THROTTLE_INTERVAL)
    return;

  host->bandwidthThrottleEpoch = timeCurrent;

  if (peersRemaining == 0)
    return;

  if (host->outgoingBandwidth != 0)
  {
    dataTotal = 0;
    bandwidth = (host->outgoingBandwidth * elapsedTime) / 1000;

    for (peer = host->peers;
         peer < &host->peers[host->peerCount];
         ++peer)
    {
      if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
        continue;

      dataTotal += peer->outgoingDataTotal;
    }
  }

  while (peersRemaining > 0 && needsAdjustment != 0)
  {
    needsAdjustment = 0;

    if (dataTotal <= bandwidth)
      throttle = DEVILS_PEER_PACKET_THROTTLE_SCALE;
    else
      throttle = (bandwidth * DEVILS_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

    for (peer = host->peers;
         peer < &host->peers[host->peerCount];
         ++peer)
    {
      devils_uint32 peerBandwidth;

      if ((peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER) ||
          peer->incomingBandwidth == 0 ||
          peer->outgoingBandwidthThrottleEpoch == timeCurrent)
        continue;

      peerBandwidth = (peer->incomingBandwidth * elapsedTime) / 1000;
      if ((throttle * peer->outgoingDataTotal) / DEVILS_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
        continue;

      peer->packetThrottleLimit = (peerBandwidth *
                                   DEVILS_PEER_PACKET_THROTTLE_SCALE) /
                                  peer->outgoingDataTotal;

      if (peer->packetThrottleLimit == 0)
        peer->packetThrottleLimit = 1;

      if (peer->packetThrottle > peer->packetThrottleLimit)
        peer->packetThrottle = peer->packetThrottleLimit;

      peer->outgoingBandwidthThrottleEpoch = timeCurrent;

      peer->incomingDataTotal = 0;
      peer->outgoingDataTotal = 0;

      needsAdjustment = 1;
      --peersRemaining;
      bandwidth -= peerBandwidth;
      dataTotal -= peerBandwidth;
    }
  }

  if (peersRemaining > 0)
  {
    if (dataTotal <= bandwidth)
      throttle = DEVILS_PEER_PACKET_THROTTLE_SCALE;
    else
      throttle = (bandwidth * DEVILS_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

    for (peer = host->peers;
         peer < &host->peers[host->peerCount];
         ++peer)
    {
      if ((peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER) ||
          peer->outgoingBandwidthThrottleEpoch == timeCurrent)
        continue;

      peer->packetThrottleLimit = throttle;

      if (peer->packetThrottle > peer->packetThrottleLimit)
        peer->packetThrottle = peer->packetThrottleLimit;

      peer->incomingDataTotal = 0;
      peer->outgoingDataTotal = 0;
    }
  }

  if (host->recalculateBandwidthLimits)
  {
    host->recalculateBandwidthLimits = 0;

    peersRemaining = (devils_uint32)host->connectedPeers;
    bandwidth = host->incomingBandwidth;
    needsAdjustment = 1;

    if (bandwidth == 0)
      bandwidthLimit = 0;
    else
      while (peersRemaining > 0 && needsAdjustment != 0)
      {
        needsAdjustment = 0;
        bandwidthLimit = bandwidth / peersRemaining;

        for (peer = host->peers;
             peer < &host->peers[host->peerCount];
             ++peer)
        {
          if ((peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER) ||
              peer->incomingBandwidthThrottleEpoch == timeCurrent)
            continue;

          if (peer->outgoingBandwidth > 0 &&
              peer->outgoingBandwidth >= bandwidthLimit)
            continue;

          peer->incomingBandwidthThrottleEpoch = timeCurrent;

          needsAdjustment = 1;
          --peersRemaining;
          bandwidth -= peer->outgoingBandwidth;
        }
      }

    for (peer = host->peers;
         peer < &host->peers[host->peerCount];
         ++peer)
    {
      if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
        continue;

      command.header.command = DEVILS_PROTOCOL_COMMAND_BANDWIDTH_LIMIT | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
      command.header.channelID = 0xFF;
      command.bandwidthLimit.outgoingBandwidth = DEVILS_HOST_TO_NET_32(host->outgoingBandwidth);

      if (peer->incomingBandwidthThrottleEpoch == timeCurrent)
        command.bandwidthLimit.incomingBandwidth = DEVILS_HOST_TO_NET_32(peer->outgoingBandwidth);
      else
        command.bandwidthLimit.incomingBandwidth = DEVILS_HOST_TO_NET_32(bandwidthLimit);

      devils_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
    }
  }
}

/** @} */
