/** 
 @file  peer.c
 @brief ENet peer management functions
*/
#include <string.h>
#define DEVILS_BUILDING_LIB 1
#include "include/devils.h"

/** @defgroup peer ENet peer functions 
    @{
*/

/** Configures throttle parameter for a peer.

    Unreliable packets are dropped by ENet in response to the varying conditions
    of the Internet connection to the peer.  The throttle represents a probability
    that an unreliable packet should not be dropped and thus sent by ENet to the peer.
    The lowest mean round trip time from the sending of a reliable packet to the
    receipt of its acknowledgement is measured over an amount of time specified by
    the interval parameter in milliseconds.  If a measured round trip time happens to
    be significantly less than the mean round trip time measured over the interval, 
    then the throttle probability is increased to allow more traffic by an amount
    specified in the acceleration parameter, which is a ratio to the DEVILS_PEER_PACKET_THROTTLE_SCALE
    constant.  If a measured round trip time happens to be significantly greater than
    the mean round trip time measured over the interval, then the throttle probability
    is decreased to limit traffic by an amount specified in the deceleration parameter, which
    is a ratio to the DEVILS_PEER_PACKET_THROTTLE_SCALE constant.  When the throttle has
    a value of DEVILS_PEER_PACKET_THROTTLE_SCALE, no unreliable packets are dropped by 
    ENet, and so 100% of all unreliable packets will be sent.  When the throttle has a
    value of 0, all unreliable packets are dropped by ENet, and so 0% of all unreliable
    packets will be sent.  Intermediate values for the throttle represent intermediate
    probabilities between 0% and 100% of unreliable packets being sent.  The bandwidth
    limits of the local and foreign hosts are taken into account to determine a 
    sensible limit for the throttle probability above which it should not raise even in
    the best of conditions.

    @param peer peer to configure 
    @param interval interval, in milliseconds, over which to measure lowest mean RTT; the default value is DEVILS_PEER_PACKET_THROTTLE_INTERVAL.
    @param acceleration rate at which to increase the throttle probability as mean RTT declines
    @param deceleration rate at which to decrease the throttle probability as mean RTT increases
*/
void devils_peer_throttle_configure(devils_peer *peer, devils_uint32 interval, devils_uint32 acceleration, devils_uint32 deceleration)
{
  devils_protocol command;

  peer->packetThrottleInterval = interval;
  peer->packetThrottleAcceleration = acceleration;
  peer->packetThrottleDeceleration = deceleration;

  command.header.command = DEVILS_PROTOCOL_COMMAND_THROTTLE_CONFIGURE | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
  command.header.channelID = 0xFF;

  command.throttleConfigure.packetThrottleInterval = DEVILS_HOST_TO_NET_32(interval);
  command.throttleConfigure.packetThrottleAcceleration = DEVILS_HOST_TO_NET_32(acceleration);
  command.throttleConfigure.packetThrottleDeceleration = DEVILS_HOST_TO_NET_32(deceleration);

  devils_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}

int devils_peer_throttle(devils_peer *peer, devils_uint32 rtt)
{
  if (peer->lastRoundTripTime <= peer->lastRoundTripTimeVariance)
  {
    peer->packetThrottle = peer->packetThrottleLimit;
  }
  else if (rtt <= peer->lastRoundTripTime)
  {
    peer->packetThrottle += peer->packetThrottleAcceleration;

    if (peer->packetThrottle > peer->packetThrottleLimit)
      peer->packetThrottle = peer->packetThrottleLimit;

    return 1;
  }
  else if (rtt > peer->lastRoundTripTime + 2 * peer->lastRoundTripTimeVariance)
  {
    if (peer->packetThrottle > peer->packetThrottleDeceleration)
      peer->packetThrottle -= peer->packetThrottleDeceleration;
    else
      peer->packetThrottle = 0;

    return -1;
  }

  return 0;
}

/** Queues a packet to be sent.
    @param peer destination for the packet
    @param channelID channel on which to send
    @param packet packet to send
    @retval 0 on success
    @retval < 0 on failure
*/
int devils_peer_send(devils_peer *peer, devils_uint8 channelID, devils_packet *packet)
{
  devils_channel *channel;
  devils_protocol command;
  size_t fragmentLength;

  if (peer->state != DEVILS_PEER_STATE_CONNECTED ||
      channelID >= peer->channelCount ||
      packet->dataLength > peer->host->maximumPacketSize)
    return -1;

  channel = &peer->channels[channelID];
  fragmentLength = peer->mtu - sizeof(devils_protocol_header) - sizeof(devils_protocol_send_fragment);
  if (peer->host->checksum != NULL)
    fragmentLength -= sizeof(devils_uint32);

  if (packet->dataLength > fragmentLength)
  {
    devils_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength,
                  fragmentNumber,
                  fragmentOffset;
    devils_uint8 commandNumber;
    devils_uint16 startSequenceNumber;
    devils_list fragments;
    devils_outgoing_command *fragment;

    if (fragmentCount > DEVILS_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
      return -1;

    if ((packet->flags & (DEVILS_PACKET_FLAG_RELIABLE | DEVILS_PACKET_FLAG_UNRELIABLE_FRAGMENT)) == DEVILS_PACKET_FLAG_UNRELIABLE_FRAGMENT &&
        channel->outgoingUnreliableSequenceNumber < 0xFFFF)
    {
      commandNumber = DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
      startSequenceNumber = DEVILS_HOST_TO_NET_16(channel->outgoingUnreliableSequenceNumber + 1);
    }
    else
    {
      commandNumber = DEVILS_PROTOCOL_COMMAND_SEND_FRAGMENT | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
      startSequenceNumber = DEVILS_HOST_TO_NET_16(channel->outgoingReliableSequenceNumber + 1);
    }

    devils_list_clear(&fragments);

    for (fragmentNumber = 0,
        fragmentOffset = 0;
         fragmentOffset < packet->dataLength;
         ++fragmentNumber,
        fragmentOffset += fragmentLength)
    {
      if (packet->dataLength - fragmentOffset < fragmentLength)
        fragmentLength = packet->dataLength - fragmentOffset;

      fragment = (devils_outgoing_command *)devils_malloc(sizeof(devils_outgoing_command));
      if (fragment == NULL)
      {
        while (!devils_list_empty(&fragments))
        {
          fragment = (devils_outgoing_command *)devils_list_remove(devils_list_begin(&fragments));

          devils_free(fragment);
        }

        return -1;
      }

      fragment->fragmentOffset = fragmentOffset;
      fragment->fragmentLength = fragmentLength;
      fragment->packet = packet;
      fragment->command.header.command = commandNumber;
      fragment->command.header.channelID = channelID;
      fragment->command.sendFragment.startSequenceNumber = startSequenceNumber;
      fragment->command.sendFragment.dataLength = DEVILS_HOST_TO_NET_16(fragmentLength);
      fragment->command.sendFragment.fragmentCount = DEVILS_HOST_TO_NET_32(fragmentCount);
      fragment->command.sendFragment.fragmentNumber = DEVILS_HOST_TO_NET_32(fragmentNumber);
      fragment->command.sendFragment.totalLength = DEVILS_HOST_TO_NET_32(packet->dataLength);
      fragment->command.sendFragment.fragmentOffset = DEVILS_NET_TO_HOST_32(fragmentOffset);

      devils_list_insert(devils_list_end(&fragments), fragment);
    }

    packet->referenceCount += fragmentNumber;

    while (!devils_list_empty(&fragments))
    {
      fragment = (devils_outgoing_command *)devils_list_remove(devils_list_begin(&fragments));

      devils_peer_setup_outgoing_command(peer, fragment);
    }

    return 0;
  }

  command.header.channelID = channelID;

  if ((packet->flags & (DEVILS_PACKET_FLAG_RELIABLE | DEVILS_PACKET_FLAG_UNSEQUENCED)) == DEVILS_PACKET_FLAG_UNSEQUENCED)
  {
    command.header.command = DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED | DEVILS_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
    command.sendUnsequenced.dataLength = DEVILS_HOST_TO_NET_16(packet->dataLength);
  }
  else if (packet->flags & DEVILS_PACKET_FLAG_RELIABLE || channel->outgoingUnreliableSequenceNumber >= 0xFFFF)
  {
    command.header.command = DEVILS_PROTOCOL_COMMAND_SEND_RELIABLE | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.sendReliable.dataLength = DEVILS_HOST_TO_NET_16(packet->dataLength);
  }
  else
  {
    command.header.command = DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE;
    command.sendUnreliable.dataLength = DEVILS_HOST_TO_NET_16(packet->dataLength);
  }

  if (devils_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
    return -1;

  return 0;
}

/** Attempts to dequeue any incoming queued packet.
    @param peer peer to dequeue packets from
    @param channelID holds the channel ID of the channel the packet was received on success
    @returns a pointer to the packet, or NULL if there are no available incoming queued packets
*/
devils_packet *
devils_peer_receive(devils_peer *peer, devils_uint8 *channelID)
{
  devils_incoming_command *incomingCommand;
  devils_packet *packet;

  if (devils_list_empty(&peer->dispatchedCommands))
    return NULL;

  incomingCommand = (devils_incoming_command *)devils_list_remove(devils_list_begin(&peer->dispatchedCommands));

  if (channelID != NULL)
    *channelID = incomingCommand->command.header.channelID;

  packet = incomingCommand->packet;

  --packet->referenceCount;

  if (incomingCommand->fragments != NULL)
    devils_free(incomingCommand->fragments);

  devils_free(incomingCommand);

  peer->totalWaitingData -= packet->dataLength;

  return packet;
}

static void
devils_peer_reset_outgoing_commands(devils_list *queue)
{
  devils_outgoing_command *outgoingCommand;

  while (!devils_list_empty(queue))
  {
    outgoingCommand = (devils_outgoing_command *)devils_list_remove(devils_list_begin(queue));

    if (outgoingCommand->packet != NULL)
    {
      --outgoingCommand->packet->referenceCount;

      if (outgoingCommand->packet->referenceCount == 0)
        devils_packet_destroy(outgoingCommand->packet);
    }

    devils_free(outgoingCommand);
  }
}

static void
devils_peer_remove_incoming_commands(devils_list *queue, devils_list_iterator startCommand, devils_list_iterator endCommand, devils_incoming_command *excludeCommand)
{
  devils_list_iterator currentCommand;

  for (currentCommand = startCommand; currentCommand != endCommand;)
  {
    devils_incoming_command *incomingCommand = (devils_incoming_command *)currentCommand;

    currentCommand = devils_list_next(currentCommand);

    if (incomingCommand == excludeCommand)
      continue;

    devils_list_remove(&incomingCommand->incomingCommandList);

    if (incomingCommand->packet != NULL)
    {
      --incomingCommand->packet->referenceCount;

      if (incomingCommand->packet->referenceCount == 0)
        devils_packet_destroy(incomingCommand->packet);
    }

    if (incomingCommand->fragments != NULL)
      devils_free(incomingCommand->fragments);

    devils_free(incomingCommand);
  }
}

static void
devils_peer_reset_incoming_commands(devils_list *queue)
{
  devils_peer_remove_incoming_commands(queue, devils_list_begin(queue), devils_list_end(queue), NULL);
}

void devils_peer_reset_queues(devils_peer *peer)
{
  devils_channel *channel;

  if (peer->flags & DEVILS_PEER_FLAG_NEEDS_DISPATCH)
  {
    devils_list_remove(&peer->dispatchList);

    peer->flags &= ~DEVILS_PEER_FLAG_NEEDS_DISPATCH;
  }

  while (!devils_list_empty(&peer->acknowledgements))
    devils_free(devils_list_remove(devils_list_begin(&peer->acknowledgements)));

  devils_peer_reset_outgoing_commands(&peer->sentReliableCommands);
  devils_peer_reset_outgoing_commands(&peer->sentUnreliableCommands);
  devils_peer_reset_outgoing_commands(&peer->outgoingCommands);
  devils_peer_reset_incoming_commands(&peer->dispatchedCommands);

  if (peer->channels != NULL && peer->channelCount > 0)
  {
    for (channel = peer->channels;
         channel < &peer->channels[peer->channelCount];
         ++channel)
    {
      devils_peer_reset_incoming_commands(&channel->incomingReliableCommands);
      devils_peer_reset_incoming_commands(&channel->incomingUnreliableCommands);
    }

    devils_free(peer->channels);
  }

  peer->channels = NULL;
  peer->channelCount = 0;
}

void devils_peer_on_connect(devils_peer *peer)
{
  if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
  {
    if (peer->incomingBandwidth != 0)
      ++peer->host->bandwidthLimitedPeers;

    ++peer->host->connectedPeers;
  }
}

void devils_peer_on_disconnect(devils_peer *peer)
{
  if (peer->state == DEVILS_PEER_STATE_CONNECTED || peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER)
  {
    if (peer->incomingBandwidth != 0)
      --peer->host->bandwidthLimitedPeers;

    --peer->host->connectedPeers;
  }
}

/** Forcefully disconnects a peer.
    @param peer peer to forcefully disconnect
    @remarks The foreign host represented by the peer is not notified of the disconnection and will timeout
    on its connection to the local host.
*/
void devils_peer_reset(devils_peer *peer)
{
  devils_peer_on_disconnect(peer);

  peer->outgoingPeerID = DEVILS_PROTOCOL_MAXIMUM_PEER_ID;
  peer->connectID = 0;

  peer->state = DEVILS_PEER_STATE_DISCONNECTED;

  peer->incomingBandwidth = 0;
  peer->outgoingBandwidth = 0;
  peer->incomingBandwidthThrottleEpoch = 0;
  peer->outgoingBandwidthThrottleEpoch = 0;
  peer->incomingDataTotal = 0;
  peer->outgoingDataTotal = 0;
  peer->lastSendTime = 0;
  peer->lastReceiveTime = 0;
  peer->nextTimeout = 0;
  peer->earliestTimeout = 0;
  peer->packetLossEpoch = 0;
  peer->packetsSent = 0;
  peer->packetsLost = 0;
  peer->packetLoss = 0;
  peer->packetLossVariance = 0;
  peer->packetThrottle = DEVILS_PEER_DEFAULT_PACKET_THROTTLE;
  peer->packetThrottleLimit = DEVILS_PEER_PACKET_THROTTLE_SCALE;
  peer->packetThrottleCounter = 0;
  peer->packetThrottleEpoch = 0;
  peer->packetThrottleAcceleration = DEVILS_PEER_PACKET_THROTTLE_ACCELERATION;
  peer->packetThrottleDeceleration = DEVILS_PEER_PACKET_THROTTLE_DECELERATION;
  peer->packetThrottleInterval = DEVILS_PEER_PACKET_THROTTLE_INTERVAL;
  peer->pingInterval = DEVILS_PEER_PING_INTERVAL;
  peer->timeoutLimit = DEVILS_PEER_TIMEOUT_LIMIT;
  peer->timeoutMinimum = DEVILS_PEER_TIMEOUT_MINIMUM;
  peer->timeoutMaximum = DEVILS_PEER_TIMEOUT_MAXIMUM;
  peer->lastRoundTripTime = DEVILS_PEER_DEFAULT_ROUND_TRIP_TIME;
  peer->lowestRoundTripTime = DEVILS_PEER_DEFAULT_ROUND_TRIP_TIME;
  peer->lastRoundTripTimeVariance = 0;
  peer->highestRoundTripTimeVariance = 0;
  peer->roundTripTime = DEVILS_PEER_DEFAULT_ROUND_TRIP_TIME;
  peer->roundTripTimeVariance = 0;
  peer->mtu = peer->host->mtu;
  peer->reliableDataInTransit = 0;
  peer->outgoingReliableSequenceNumber = 0;
  peer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;
  peer->incomingUnsequencedGroup = 0;
  peer->outgoingUnsequencedGroup = 0;
  peer->eventData = 0;
  peer->totalWaitingData = 0;
  peer->flags = 0;

  memset(peer->unsequencedWindow, 0, sizeof(peer->unsequencedWindow));

  devils_peer_reset_queues(peer);
}

/** Sends a ping request to a peer.
    @param peer destination for the ping request
    @remarks ping requests factor into the mean round trip time as designated by the 
    roundTripTime field in the devils_peer structure.  ENet automatically pings all connected
    peers at regular intervals, however, this function may be called to ensure more
    frequent ping requests.
*/
void devils_peer_ping(devils_peer *peer)
{
  devils_protocol command;

  if (peer->state != DEVILS_PEER_STATE_CONNECTED)
    return;

  command.header.command = DEVILS_PROTOCOL_COMMAND_PING | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
  command.header.channelID = 0xFF;

  devils_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}

/** Sets the interval at which pings will be sent to a peer. 
    
    Pings are used both to monitor the liveness of the connection and also to dynamically
    adjust the throttle during periods of low traffic so that the throttle has reasonable
    responsiveness during traffic spikes.

    @param peer the peer to adjust
    @param pingInterval the interval at which to send pings; defaults to DEVILS_PEER_PING_INTERVAL if 0
*/
void devils_peer_ping_interval(devils_peer *peer, devils_uint32 pingInterval)
{
  peer->pingInterval = pingInterval ? pingInterval : DEVILS_PEER_PING_INTERVAL;
}

/** Sets the timeout parameters for a peer.

    The timeout parameter control how and when a peer will timeout from a failure to acknowledge
    reliable traffic. Timeout values use an exponential backoff mechanism, where if a reliable
    packet is not acknowledge within some multiple of the average RTT plus a variance tolerance, 
    the timeout will be doubled until it reaches a set limit. If the timeout is thus at this
    limit and reliable packets have been sent but not acknowledged within a certain minimum time 
    period, the peer will be disconnected. Alternatively, if reliable packets have been sent
    but not acknowledged for a certain maximum time period, the peer will be disconnected regardless
    of the current timeout limit value.
    
    @param peer the peer to adjust
    @param timeoutLimit the timeout limit; defaults to DEVILS_PEER_TIMEOUT_LIMIT if 0
    @param timeoutMinimum the timeout minimum; defaults to DEVILS_PEER_TIMEOUT_MINIMUM if 0
    @param timeoutMaximum the timeout maximum; defaults to DEVILS_PEER_TIMEOUT_MAXIMUM if 0
*/

void devils_peer_timeout(devils_peer *peer, devils_uint32 timeoutLimit, devils_uint32 timeoutMinimum, devils_uint32 timeoutMaximum)
{
  peer->timeoutLimit = timeoutLimit ? timeoutLimit : DEVILS_PEER_TIMEOUT_LIMIT;
  peer->timeoutMinimum = timeoutMinimum ? timeoutMinimum : DEVILS_PEER_TIMEOUT_MINIMUM;
  peer->timeoutMaximum = timeoutMaximum ? timeoutMaximum : DEVILS_PEER_TIMEOUT_MAXIMUM;
}

/** Force an immediate disconnection from a peer.
    @param peer peer to disconnect
    @param data data describing the disconnection
    @remarks No DEVILS_EVENT_DISCONNECT event will be generated. The foreign peer is not
    guaranteed to receive the disconnect notification, and is reset immediately upon
    return from this function.
*/
void devils_peer_disconnect_now(devils_peer *peer, devils_uint32 data)
{
  devils_protocol command;

  if (peer->state == DEVILS_PEER_STATE_DISCONNECTED)
    return;

  if (peer->state != DEVILS_PEER_STATE_ZOMBIE &&
      peer->state != DEVILS_PEER_STATE_DISCONNECTING)
  {
    devils_peer_reset_queues(peer);

    command.header.command = DEVILS_PROTOCOL_COMMAND_DISCONNECT | DEVILS_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
    command.header.channelID = 0xFF;
    command.disconnect.data = DEVILS_HOST_TO_NET_32(data);

    devils_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

    devils_host_flush(peer->host);
  }

  devils_peer_reset(peer);
}

/** Request a disconnection from a peer.
    @param peer peer to request a disconnection
    @param data data describing the disconnection
    @remarks An DEVILS_EVENT_DISCONNECT event will be generated by devils_host_service()
    once the disconnection is complete.
*/
void devils_peer_disconnect(devils_peer *peer, devils_uint32 data)
{
  devils_protocol command;

  if (peer->state == DEVILS_PEER_STATE_DISCONNECTING ||
      peer->state == DEVILS_PEER_STATE_DISCONNECTED ||
      peer->state == DEVILS_PEER_STATE_ACKNOWLEDGING_DISCONNECT ||
      peer->state == DEVILS_PEER_STATE_ZOMBIE)
    return;

  devils_peer_reset_queues(peer);

  command.header.command = DEVILS_PROTOCOL_COMMAND_DISCONNECT;
  command.header.channelID = 0xFF;
  command.disconnect.data = DEVILS_HOST_TO_NET_32(data);

  if (peer->state == DEVILS_PEER_STATE_CONNECTED || peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER)
    command.header.command |= DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
  else
    command.header.command |= DEVILS_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;

  devils_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

  if (peer->state == DEVILS_PEER_STATE_CONNECTED || peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER)
  {
    devils_peer_on_disconnect(peer);

    peer->state = DEVILS_PEER_STATE_DISCONNECTING;
  }
  else
  {
    devils_host_flush(peer->host);
    devils_peer_reset(peer);
  }
}

/** Request a disconnection from a peer, but only after all queued outgoing packets are sent.
    @param peer peer to request a disconnection
    @param data data describing the disconnection
    @remarks An DEVILS_EVENT_DISCONNECT event will be generated by devils_host_service()
    once the disconnection is complete.
*/
void devils_peer_disconnect_later(devils_peer *peer, devils_uint32 data)
{
  if ((peer->state == DEVILS_PEER_STATE_CONNECTED || peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER) &&
      !(devils_list_empty(&peer->outgoingCommands) &&
        devils_list_empty(&peer->sentReliableCommands)))
  {
    peer->state = DEVILS_PEER_STATE_DISCONNECT_LATER;
    peer->eventData = data;
  }
  else
    devils_peer_disconnect(peer, data);
}

devils_acknowledgement *
devils_peer_queue_acknowledgement(devils_peer *peer, const devils_protocol *command, devils_uint16 sentTime)
{
  devils_acknowledgement *acknowledgement;

  if (command->header.channelID < peer->channelCount)
  {
    devils_channel *channel = &peer->channels[command->header.channelID];
    devils_uint16 reliableWindow = command->header.reliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE,
                  currentWindow = channel->incomingReliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;

    if (command->header.reliableSequenceNumber < channel->incomingReliableSequenceNumber)
      reliableWindow += DEVILS_PEER_RELIABLE_WINDOWS;

    if (reliableWindow >= currentWindow + DEVILS_PEER_FREE_RELIABLE_WINDOWS - 1 && reliableWindow <= currentWindow + DEVILS_PEER_FREE_RELIABLE_WINDOWS)
      return NULL;
  }

  acknowledgement = (devils_acknowledgement *)devils_malloc(sizeof(devils_acknowledgement));
  if (acknowledgement == NULL)
    return NULL;

  peer->outgoingDataTotal += sizeof(devils_protocol_acknowledge);

  acknowledgement->sentTime = sentTime;
  acknowledgement->command = *command;

  devils_list_insert(devils_list_end(&peer->acknowledgements), acknowledgement);

  return acknowledgement;
}

void devils_peer_setup_outgoing_command(devils_peer *peer, devils_outgoing_command *outgoingCommand)
{
  devils_channel *channel = &peer->channels[outgoingCommand->command.header.channelID];

  peer->outgoingDataTotal += devils_protocol_command_size(outgoingCommand->command.header.command) + outgoingCommand->fragmentLength;

  if (outgoingCommand->command.header.channelID == 0xFF)
  {
    ++peer->outgoingReliableSequenceNumber;

    outgoingCommand->reliableSequenceNumber = peer->outgoingReliableSequenceNumber;
    outgoingCommand->unreliableSequenceNumber = 0;
  }
  else if (outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
  {
    ++channel->outgoingReliableSequenceNumber;
    channel->outgoingUnreliableSequenceNumber = 0;

    outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
    outgoingCommand->unreliableSequenceNumber = 0;
  }
  else if (outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_FLAG_UNSEQUENCED)
  {
    ++peer->outgoingUnsequencedGroup;

    outgoingCommand->reliableSequenceNumber = 0;
    outgoingCommand->unreliableSequenceNumber = 0;
  }
  else
  {
    if (outgoingCommand->fragmentOffset == 0)
      ++channel->outgoingUnreliableSequenceNumber;

    outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
    outgoingCommand->unreliableSequenceNumber = channel->outgoingUnreliableSequenceNumber;
  }

  outgoingCommand->sendAttempts = 0;
  outgoingCommand->sentTime = 0;
  outgoingCommand->roundTripTimeout = 0;
  outgoingCommand->roundTripTimeoutLimit = 0;
  outgoingCommand->command.header.reliableSequenceNumber = DEVILS_HOST_TO_NET_16(outgoingCommand->reliableSequenceNumber);

  switch (outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK)
  {
  case DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE:
    outgoingCommand->command.sendUnreliable.unreliableSequenceNumber = DEVILS_HOST_TO_NET_16(outgoingCommand->unreliableSequenceNumber);
    break;

  case DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
    outgoingCommand->command.sendUnsequenced.unsequencedGroup = DEVILS_HOST_TO_NET_16(peer->outgoingUnsequencedGroup);
    break;

  default:
    break;
  }

  devils_list_insert(devils_list_end(&peer->outgoingCommands), outgoingCommand);
}

devils_outgoing_command *
devils_peer_queue_outgoing_command(devils_peer *peer, const devils_protocol *command, devils_packet *packet, devils_uint32 offset, devils_uint16 length)
{
  devils_outgoing_command *outgoingCommand = (devils_outgoing_command *)devils_malloc(sizeof(devils_outgoing_command));
  if (outgoingCommand == NULL)
    return NULL;

  outgoingCommand->command = *command;
  outgoingCommand->fragmentOffset = offset;
  outgoingCommand->fragmentLength = length;
  outgoingCommand->packet = packet;
  if (packet != NULL)
    ++packet->referenceCount;

  devils_peer_setup_outgoing_command(peer, outgoingCommand);

  return outgoingCommand;
}

void devils_peer_dispatch_incoming_unreliable_commands(devils_peer *peer, devils_channel *channel, devils_incoming_command *queuedCommand)
{
  devils_list_iterator droppedCommand, startCommand, currentCommand;

  for (droppedCommand = startCommand = currentCommand = devils_list_begin(&channel->incomingUnreliableCommands);
       currentCommand != devils_list_end(&channel->incomingUnreliableCommands);
       currentCommand = devils_list_next(currentCommand))
  {
    devils_incoming_command *incomingCommand = (devils_incoming_command *)currentCommand;

    if ((incomingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK) == DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
      continue;

    if (incomingCommand->reliableSequenceNumber == channel->incomingReliableSequenceNumber)
    {
      if (incomingCommand->fragmentsRemaining <= 0)
      {
        channel->incomingUnreliableSequenceNumber = incomingCommand->unreliableSequenceNumber;
        continue;
      }

      if (startCommand != currentCommand)
      {
        devils_list_move(devils_list_end(&peer->dispatchedCommands), startCommand, devils_list_previous(currentCommand));

        if (!(peer->flags & DEVILS_PEER_FLAG_NEEDS_DISPATCH))
        {
          devils_list_insert(devils_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

          peer->flags |= DEVILS_PEER_FLAG_NEEDS_DISPATCH;
        }

        droppedCommand = currentCommand;
      }
      else if (droppedCommand != currentCommand)
        droppedCommand = devils_list_previous(currentCommand);
    }
    else
    {
      devils_uint16 reliableWindow = incomingCommand->reliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE,
                    currentWindow = channel->incomingReliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;
      if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
        reliableWindow += DEVILS_PEER_RELIABLE_WINDOWS;
      if (reliableWindow >= currentWindow && reliableWindow < currentWindow + DEVILS_PEER_FREE_RELIABLE_WINDOWS - 1)
        break;

      droppedCommand = devils_list_next(currentCommand);

      if (startCommand != currentCommand)
      {
        devils_list_move(devils_list_end(&peer->dispatchedCommands), startCommand, devils_list_previous(currentCommand));

        if (!(peer->flags & DEVILS_PEER_FLAG_NEEDS_DISPATCH))
        {
          devils_list_insert(devils_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

          peer->flags |= DEVILS_PEER_FLAG_NEEDS_DISPATCH;
        }
      }
    }

    startCommand = devils_list_next(currentCommand);
  }

  if (startCommand != currentCommand)
  {
    devils_list_move(devils_list_end(&peer->dispatchedCommands), startCommand, devils_list_previous(currentCommand));

    if (!(peer->flags & DEVILS_PEER_FLAG_NEEDS_DISPATCH))
    {
      devils_list_insert(devils_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

      peer->flags |= DEVILS_PEER_FLAG_NEEDS_DISPATCH;
    }

    droppedCommand = currentCommand;
  }

  devils_peer_remove_incoming_commands(&channel->incomingUnreliableCommands, devils_list_begin(&channel->incomingUnreliableCommands), droppedCommand, queuedCommand);
}

void devils_peer_dispatch_incoming_reliable_commands(devils_peer *peer, devils_channel *channel, devils_incoming_command *queuedCommand)
{
  devils_list_iterator currentCommand;

  for (currentCommand = devils_list_begin(&channel->incomingReliableCommands);
       currentCommand != devils_list_end(&channel->incomingReliableCommands);
       currentCommand = devils_list_next(currentCommand))
  {
    devils_incoming_command *incomingCommand = (devils_incoming_command *)currentCommand;

    if (incomingCommand->fragmentsRemaining > 0 ||
        incomingCommand->reliableSequenceNumber != (devils_uint16)(channel->incomingReliableSequenceNumber + 1))
      break;

    channel->incomingReliableSequenceNumber = incomingCommand->reliableSequenceNumber;

    if (incomingCommand->fragmentCount > 0)
      channel->incomingReliableSequenceNumber += incomingCommand->fragmentCount - 1;
  }

  if (currentCommand == devils_list_begin(&channel->incomingReliableCommands))
    return;

  channel->incomingUnreliableSequenceNumber = 0;

  devils_list_move(devils_list_end(&peer->dispatchedCommands), devils_list_begin(&channel->incomingReliableCommands), devils_list_previous(currentCommand));

  if (!(peer->flags & DEVILS_PEER_FLAG_NEEDS_DISPATCH))
  {
    devils_list_insert(devils_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

    peer->flags |= DEVILS_PEER_FLAG_NEEDS_DISPATCH;
  }

  if (!devils_list_empty(&channel->incomingUnreliableCommands))
    devils_peer_dispatch_incoming_unreliable_commands(peer, channel, queuedCommand);
}

devils_incoming_command *
devils_peer_queue_incoming_command(devils_peer *peer, const devils_protocol *command, const void *data, size_t dataLength, devils_uint32 flags, devils_uint32 fragmentCount)
{
  static devils_incoming_command dummyCommand;

  devils_channel *channel = &peer->channels[command->header.channelID];
  devils_uint32 unreliableSequenceNumber = 0, reliableSequenceNumber = 0;
  devils_uint16 reliableWindow, currentWindow;
  devils_incoming_command *incomingCommand;
  devils_list_iterator currentCommand;
  devils_packet *packet = NULL;

  if (peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER)
    goto discardCommand;

  if ((command->header.command & DEVILS_PROTOCOL_COMMAND_MASK) != DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
  {
    reliableSequenceNumber = command->header.reliableSequenceNumber;
    reliableWindow = reliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;
    currentWindow = channel->incomingReliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;

    if (reliableSequenceNumber < channel->incomingReliableSequenceNumber)
      reliableWindow += DEVILS_PEER_RELIABLE_WINDOWS;

    if (reliableWindow < currentWindow || reliableWindow >= currentWindow + DEVILS_PEER_FREE_RELIABLE_WINDOWS - 1)
      goto discardCommand;
  }

  switch (command->header.command & DEVILS_PROTOCOL_COMMAND_MASK)
  {
  case DEVILS_PROTOCOL_COMMAND_SEND_FRAGMENT:
  case DEVILS_PROTOCOL_COMMAND_SEND_RELIABLE:
    if (reliableSequenceNumber == channel->incomingReliableSequenceNumber)
      goto discardCommand;

    for (currentCommand = devils_list_previous(devils_list_end(&channel->incomingReliableCommands));
         currentCommand != devils_list_end(&channel->incomingReliableCommands);
         currentCommand = devils_list_previous(currentCommand))
    {
      incomingCommand = (devils_incoming_command *)currentCommand;

      if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
      {
        if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
          continue;
      }
      else if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
        break;

      if (incomingCommand->reliableSequenceNumber <= reliableSequenceNumber)
      {
        if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
          break;

        goto discardCommand;
      }
    }
    break;

  case DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE:
  case DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
    unreliableSequenceNumber = DEVILS_NET_TO_HOST_16(command->sendUnreliable.unreliableSequenceNumber);

    if (reliableSequenceNumber == channel->incomingReliableSequenceNumber &&
        unreliableSequenceNumber <= channel->incomingUnreliableSequenceNumber)
      goto discardCommand;

    for (currentCommand = devils_list_previous(devils_list_end(&channel->incomingUnreliableCommands));
         currentCommand != devils_list_end(&channel->incomingUnreliableCommands);
         currentCommand = devils_list_previous(currentCommand))
    {
      incomingCommand = (devils_incoming_command *)currentCommand;

      if ((command->header.command & DEVILS_PROTOCOL_COMMAND_MASK) == DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
        continue;

      if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
      {
        if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
          continue;
      }
      else if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
        break;

      if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
        break;

      if (incomingCommand->reliableSequenceNumber > reliableSequenceNumber)
        continue;

      if (incomingCommand->unreliableSequenceNumber <= unreliableSequenceNumber)
      {
        if (incomingCommand->unreliableSequenceNumber < unreliableSequenceNumber)
          break;

        goto discardCommand;
      }
    }
    break;

  case DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
    currentCommand = devils_list_end(&channel->incomingUnreliableCommands);
    break;

  default:
    goto discardCommand;
  }

  if (peer->totalWaitingData >= peer->host->maximumWaitingData)
    goto notifyError;

  packet = devils_packet_create(data, dataLength, flags);
  if (packet == NULL)
    goto notifyError;

  incomingCommand = (devils_incoming_command *)devils_malloc(sizeof(devils_incoming_command));
  if (incomingCommand == NULL)
    goto notifyError;

  incomingCommand->reliableSequenceNumber = command->header.reliableSequenceNumber;
  incomingCommand->unreliableSequenceNumber = unreliableSequenceNumber & 0xFFFF;
  incomingCommand->command = *command;
  incomingCommand->fragmentCount = fragmentCount;
  incomingCommand->fragmentsRemaining = fragmentCount;
  incomingCommand->packet = packet;
  incomingCommand->fragments = NULL;

  if (fragmentCount > 0)
  {
    if (fragmentCount <= DEVILS_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
      incomingCommand->fragments = (devils_uint32 *)devils_malloc((fragmentCount + 31) / 32 * sizeof(devils_uint32));
    if (incomingCommand->fragments == NULL)
    {
      devils_free(incomingCommand);

      goto notifyError;
    }
    memset(incomingCommand->fragments, 0, (fragmentCount + 31) / 32 * sizeof(devils_uint32));
  }

  if (packet != NULL)
  {
    ++packet->referenceCount;

    peer->totalWaitingData += packet->dataLength;
  }

  devils_list_insert(devils_list_next(currentCommand), incomingCommand);

  switch (command->header.command & DEVILS_PROTOCOL_COMMAND_MASK)
  {
  case DEVILS_PROTOCOL_COMMAND_SEND_FRAGMENT:
  case DEVILS_PROTOCOL_COMMAND_SEND_RELIABLE:
    devils_peer_dispatch_incoming_reliable_commands(peer, channel, incomingCommand);
    break;

  default:
    devils_peer_dispatch_incoming_unreliable_commands(peer, channel, incomingCommand);
    break;
  }

  return incomingCommand;

discardCommand:
  if (fragmentCount > 0)
    goto notifyError;

  if (packet != NULL && packet->referenceCount == 0)
    devils_packet_destroy(packet);

  return &dummyCommand;

notifyError:
  if (packet != NULL && packet->referenceCount == 0)
    devils_packet_destroy(packet);

  return NULL;
}

/** @} */
