/** 
 @file  protocol.c
 @brief ENet protocol functions
*/
#include <stdio.h>
#include <string.h>
#define DEVILS_BUILDING_LIB 1
#include "include/devils_utility.h"
#include "include/devils_time.h"
#include "include/devils.h"

static size_t commandSizes[DEVILS_PROTOCOL_COMMAND_COUNT] =
    {
        0,
        sizeof(devils_protocol_acknowledge),
        sizeof(devils_protocol_connect),
        sizeof(devils_protocol_verify_connect),
        sizeof(devils_protocol_disconnect),
        sizeof(devils_protocol_ping),
        sizeof(devils_protocol_send_reliable),
        sizeof(devils_protocol_send_unreliable),
        sizeof(devils_protocol_send_fragment),
        sizeof(devils_protocol_send_unsequenced),
        sizeof(devils_protocol_bandwidth_limit),
        sizeof(devils_protocol_throttle_configure),
        sizeof(devils_protocol_send_fragment)};

size_t
devils_protocol_command_size(devils_uint8 commandNumber)
{
  return commandSizes[commandNumber & DEVILS_PROTOCOL_COMMAND_MASK];
}

static void
devils_protocol_change_state(devils_host *host, devils_peer *peer, devils_peer_state state)
{
  if (state == DEVILS_PEER_STATE_CONNECTED || state == DEVILS_PEER_STATE_DISCONNECT_LATER)
    devils_peer_on_connect(peer);
  else
    devils_peer_on_disconnect(peer);

  peer->state = state;
}

static void
devils_protocol_dispatch_state(devils_host *host, devils_peer *peer, devils_peer_state state)
{
  devils_protocol_change_state(host, peer, state);

  if (!(peer->flags & DEVILS_PEER_FLAG_NEEDS_DISPATCH))
  {
    devils_list_insert(devils_list_end(&host->dispatchQueue), &peer->dispatchList);

    peer->flags |= DEVILS_PEER_FLAG_NEEDS_DISPATCH;
  }
}

static int
devils_protocol_dispatch_incoming_commands(devils_host *host, devils_event *event)
{
  while (!devils_list_empty(&host->dispatchQueue))
  {
    devils_peer *peer = (devils_peer *)devils_list_remove(devils_list_begin(&host->dispatchQueue));

    peer->flags &= ~DEVILS_PEER_FLAG_NEEDS_DISPATCH;

    switch (peer->state)
    {
    case DEVILS_PEER_STATE_CONNECTION_PENDING:
    case DEVILS_PEER_STATE_CONNECTION_SUCCEEDED:
      devils_protocol_change_state(host, peer, DEVILS_PEER_STATE_CONNECTED);

      event->type = DEVILS_EVENT_TYPE_CONNECT;
      event->peer = peer;
      event->data = peer->eventData;

      return 1;

    case DEVILS_PEER_STATE_ZOMBIE:
      host->recalculateBandwidthLimits = 1;

      event->type = DEVILS_EVENT_TYPE_DISCONNECT;
      event->peer = peer;
      event->data = peer->eventData;

      devils_peer_reset(peer);

      return 1;

    case DEVILS_PEER_STATE_CONNECTED:
      if (devils_list_empty(&peer->dispatchedCommands))
        continue;

      event->packet = devils_peer_receive(peer, &event->channelID);
      if (event->packet == NULL)
        continue;

      event->type = DEVILS_EVENT_TYPE_RECEIVE;
      event->peer = peer;

      if (!devils_list_empty(&peer->dispatchedCommands))
      {
        peer->flags |= DEVILS_PEER_FLAG_NEEDS_DISPATCH;

        devils_list_insert(devils_list_end(&host->dispatchQueue), &peer->dispatchList);
      }

      return 1;

    default:
      break;
    }
  }

  return 0;
}

static void
devils_protocol_notify_connect(devils_host *host, devils_peer *peer, devils_event *event)
{
  host->recalculateBandwidthLimits = 1;

  if (event != NULL)
  {
    devils_protocol_change_state(host, peer, DEVILS_PEER_STATE_CONNECTED);

    event->type = DEVILS_EVENT_TYPE_CONNECT;
    event->peer = peer;
    event->data = peer->eventData;
  }
  else
    devils_protocol_dispatch_state(host, peer, peer->state == DEVILS_PEER_STATE_CONNECTING ? DEVILS_PEER_STATE_CONNECTION_SUCCEEDED : DEVILS_PEER_STATE_CONNECTION_PENDING);
}

static void
devils_protocol_notify_disconnect(devils_host *host, devils_peer *peer, devils_event *event)
{
  if (peer->state >= DEVILS_PEER_STATE_CONNECTION_PENDING)
    host->recalculateBandwidthLimits = 1;

  if (peer->state != DEVILS_PEER_STATE_CONNECTING && peer->state < DEVILS_PEER_STATE_CONNECTION_SUCCEEDED)
    devils_peer_reset(peer);
  else if (event != NULL)
  {
    event->type = DEVILS_EVENT_TYPE_DISCONNECT;
    event->peer = peer;
    event->data = 0;

    devils_peer_reset(peer);
  }
  else
  {
    peer->eventData = 0;

    devils_protocol_dispatch_state(host, peer, DEVILS_PEER_STATE_ZOMBIE);
  }
}

static void
devils_protocol_remove_sent_unreliable_commands(devils_peer *peer)
{
  devils_outgoing_command *outgoingCommand;

  if (devils_list_empty(&peer->sentUnreliableCommands))
    return;

  do
  {
    outgoingCommand = (devils_outgoing_command *)devils_list_front(&peer->sentUnreliableCommands);

    devils_list_remove(&outgoingCommand->outgoingCommandList);

    if (outgoingCommand->packet != NULL)
    {
      --outgoingCommand->packet->referenceCount;

      if (outgoingCommand->packet->referenceCount == 0)
      {
        outgoingCommand->packet->flags |= DEVILS_PACKET_FLAG_SENT;

        devils_packet_destroy(outgoingCommand->packet);
      }
    }

    devils_free(outgoingCommand);
  } while (!devils_list_empty(&peer->sentUnreliableCommands));

  if (peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER &&
      devils_list_empty(&peer->outgoingCommands) &&
      devils_list_empty(&peer->sentReliableCommands))
    devils_peer_disconnect(peer, peer->eventData);
}

static devils_protocol_command
devils_protocol_remove_sent_reliable_command(devils_peer *peer, devils_uint16 reliableSequenceNumber, devils_uint8 channelID)
{
  devils_outgoing_command *outgoingCommand = NULL;
  devils_list_iterator currentCommand;
  devils_protocol_command commandNumber;
  int wasSent = 1;

  for (currentCommand = devils_list_begin(&peer->sentReliableCommands);
       currentCommand != devils_list_end(&peer->sentReliableCommands);
       currentCommand = devils_list_next(currentCommand))
  {
    outgoingCommand = (devils_outgoing_command *)currentCommand;

    if (outgoingCommand->reliableSequenceNumber == reliableSequenceNumber &&
        outgoingCommand->command.header.channelID == channelID)
      break;
  }

  if (currentCommand == devils_list_end(&peer->sentReliableCommands))
  {
    for (currentCommand = devils_list_begin(&peer->outgoingCommands);
         currentCommand != devils_list_end(&peer->outgoingCommands);
         currentCommand = devils_list_next(currentCommand))
    {
      outgoingCommand = (devils_outgoing_command *)currentCommand;

      if (!(outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))
        continue;

      if (outgoingCommand->sendAttempts < 1)
        return DEVILS_PROTOCOL_COMMAND_NONE;

      if (outgoingCommand->reliableSequenceNumber == reliableSequenceNumber &&
          outgoingCommand->command.header.channelID == channelID)
        break;
    }

    if (currentCommand == devils_list_end(&peer->outgoingCommands))
      return DEVILS_PROTOCOL_COMMAND_NONE;

    wasSent = 0;
  }

  if (outgoingCommand == NULL)
    return DEVILS_PROTOCOL_COMMAND_NONE;

  if (channelID < peer->channelCount)
  {
    devils_channel *channel = &peer->channels[channelID];
    devils_uint16 reliableWindow = reliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;
    if (channel->reliableWindows[reliableWindow] > 0)
    {
      --channel->reliableWindows[reliableWindow];
      if (!channel->reliableWindows[reliableWindow])
        channel->usedReliableWindows &= ~(1 << reliableWindow);
    }
  }

  commandNumber = (devils_protocol_command)(outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK);

  devils_list_remove(&outgoingCommand->outgoingCommandList);

  if (outgoingCommand->packet != NULL)
  {
    if (wasSent)
      peer->reliableDataInTransit -= outgoingCommand->fragmentLength;

    --outgoingCommand->packet->referenceCount;

    if (outgoingCommand->packet->referenceCount == 0)
    {
      outgoingCommand->packet->flags |= DEVILS_PACKET_FLAG_SENT;

      devils_packet_destroy(outgoingCommand->packet);
    }
  }

  devils_free(outgoingCommand);

  if (devils_list_empty(&peer->sentReliableCommands))
    return commandNumber;

  outgoingCommand = (devils_outgoing_command *)devils_list_front(&peer->sentReliableCommands);

  peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

  return commandNumber;
}

static devils_peer *
devils_protocol_handle_connect(devils_host *host, devils_protocol_header *header, devils_protocol *command)
{
  devils_uint8 incomingSessionID, outgoingSessionID;
  devils_uint32 mtu, windowSize;
  devils_channel *channel;
  size_t channelCount, duplicatePeers = 0;
  devils_peer *currentPeer, *peer = NULL;
  devils_protocol verifyCommand;

  channelCount = DEVILS_NET_TO_HOST_32(command->connect.channelCount);

  if (channelCount < DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT ||
      channelCount > DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
    return NULL;

  for (currentPeer = host->peers;
       currentPeer < &host->peers[host->peerCount];
       ++currentPeer)
  {
    if (currentPeer->state == DEVILS_PEER_STATE_DISCONNECTED)
    {
      if (peer == NULL)
        peer = currentPeer;
    }
    else if (currentPeer->state != DEVILS_PEER_STATE_CONNECTING &&
             currentPeer->address.host == host->receivedAddress.host)
    {
      if (currentPeer->address.port == host->receivedAddress.port &&
          currentPeer->connectID == command->connect.connectID)
        return NULL;

      ++duplicatePeers;
    }
  }

  if (peer == NULL || duplicatePeers >= host->duplicatePeers)
    return NULL;

  if (channelCount > host->channelLimit)
    channelCount = host->channelLimit;
  peer->channels = (devils_channel *)devils_malloc(channelCount * sizeof(devils_channel));
  if (peer->channels == NULL)
    return NULL;
  peer->channelCount = channelCount;
  peer->state = DEVILS_PEER_STATE_ACKNOWLEDGING_CONNECT;
  peer->connectID = command->connect.connectID;
  peer->address = host->receivedAddress;
  peer->outgoingPeerID = DEVILS_NET_TO_HOST_16(command->connect.outgoingPeerID);
  peer->incomingBandwidth = DEVILS_NET_TO_HOST_32(command->connect.incomingBandwidth);
  peer->outgoingBandwidth = DEVILS_NET_TO_HOST_32(command->connect.outgoingBandwidth);
  peer->packetThrottleInterval = DEVILS_NET_TO_HOST_32(command->connect.packetThrottleInterval);
  peer->packetThrottleAcceleration = DEVILS_NET_TO_HOST_32(command->connect.packetThrottleAcceleration);
  peer->packetThrottleDeceleration = DEVILS_NET_TO_HOST_32(command->connect.packetThrottleDeceleration);
  peer->eventData = DEVILS_NET_TO_HOST_32(command->connect.data);

  incomingSessionID = command->connect.incomingSessionID == 0xFF ? peer->outgoingSessionID : command->connect.incomingSessionID;
  incomingSessionID = (incomingSessionID + 1) & (DEVILS_PROTOCOL_HEADER_SESSION_MASK >> DEVILS_PROTOCOL_HEADER_SESSION_SHIFT);
  if (incomingSessionID == peer->outgoingSessionID)
    incomingSessionID = (incomingSessionID + 1) & (DEVILS_PROTOCOL_HEADER_SESSION_MASK >> DEVILS_PROTOCOL_HEADER_SESSION_SHIFT);
  peer->outgoingSessionID = incomingSessionID;

  outgoingSessionID = command->connect.outgoingSessionID == 0xFF ? peer->incomingSessionID : command->connect.outgoingSessionID;
  outgoingSessionID = (outgoingSessionID + 1) & (DEVILS_PROTOCOL_HEADER_SESSION_MASK >> DEVILS_PROTOCOL_HEADER_SESSION_SHIFT);
  if (outgoingSessionID == peer->incomingSessionID)
    outgoingSessionID = (outgoingSessionID + 1) & (DEVILS_PROTOCOL_HEADER_SESSION_MASK >> DEVILS_PROTOCOL_HEADER_SESSION_SHIFT);
  peer->incomingSessionID = outgoingSessionID;

  for (channel = peer->channels;
       channel < &peer->channels[channelCount];
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

  mtu = DEVILS_NET_TO_HOST_32(command->connect.mtu);

  if (mtu < DEVILS_PROTOCOL_MINIMUM_MTU)
    mtu = DEVILS_PROTOCOL_MINIMUM_MTU;
  else if (mtu > DEVILS_PROTOCOL_MAXIMUM_MTU)
    mtu = DEVILS_PROTOCOL_MAXIMUM_MTU;

  peer->mtu = mtu;

  if (host->outgoingBandwidth == 0 &&
      peer->incomingBandwidth == 0)
    peer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;
  else if (host->outgoingBandwidth == 0 ||
           peer->incomingBandwidth == 0)
    peer->windowSize = (DEVILS_MAX(host->outgoingBandwidth, peer->incomingBandwidth) /
                        DEVILS_PEER_WINDOW_SIZE_SCALE) *
                       DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;
  else
    peer->windowSize = (DEVILS_MIN(host->outgoingBandwidth, peer->incomingBandwidth) /
                        DEVILS_PEER_WINDOW_SIZE_SCALE) *
                       DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;

  if (peer->windowSize < DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE)
    peer->windowSize = DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;
  else if (peer->windowSize > DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE)
    peer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;

  if (host->incomingBandwidth == 0)
    windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;
  else
    windowSize = (host->incomingBandwidth / DEVILS_PEER_WINDOW_SIZE_SCALE) *
                 DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;

  if (windowSize > DEVILS_NET_TO_HOST_32(command->connect.windowSize))
    windowSize = DEVILS_NET_TO_HOST_32(command->connect.windowSize);

  if (windowSize < DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE)
    windowSize = DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;
  else if (windowSize > DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE)
    windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;

  verifyCommand.header.command = DEVILS_PROTOCOL_COMMAND_VERIFY_CONNECT | DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
  verifyCommand.header.channelID = 0xFF;
  verifyCommand.verifyConnect.outgoingPeerID = DEVILS_HOST_TO_NET_16(peer->incomingPeerID);
  verifyCommand.verifyConnect.incomingSessionID = incomingSessionID;
  verifyCommand.verifyConnect.outgoingSessionID = outgoingSessionID;
  verifyCommand.verifyConnect.mtu = DEVILS_HOST_TO_NET_32(peer->mtu);
  verifyCommand.verifyConnect.windowSize = DEVILS_HOST_TO_NET_32(windowSize);
  verifyCommand.verifyConnect.channelCount = DEVILS_HOST_TO_NET_32(channelCount);
  verifyCommand.verifyConnect.incomingBandwidth = DEVILS_HOST_TO_NET_32(host->incomingBandwidth);
  verifyCommand.verifyConnect.outgoingBandwidth = DEVILS_HOST_TO_NET_32(host->outgoingBandwidth);
  verifyCommand.verifyConnect.packetThrottleInterval = DEVILS_HOST_TO_NET_32(peer->packetThrottleInterval);
  verifyCommand.verifyConnect.packetThrottleAcceleration = DEVILS_HOST_TO_NET_32(peer->packetThrottleAcceleration);
  verifyCommand.verifyConnect.packetThrottleDeceleration = DEVILS_HOST_TO_NET_32(peer->packetThrottleDeceleration);
  verifyCommand.verifyConnect.connectID = peer->connectID;

  devils_peer_queue_outgoing_command(peer, &verifyCommand, NULL, 0, 0);

  return peer;
}

static int
devils_protocol_handle_send_reliable(devils_host *host, devils_peer *peer, const devils_protocol *command, devils_uint8 **currentData)
{
  size_t dataLength;

  if (command->header.channelID >= peer->channelCount ||
      (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER))
    return -1;

  dataLength = DEVILS_NET_TO_HOST_16(command->sendReliable.dataLength);
  *currentData += dataLength;
  if (dataLength > host->maximumPacketSize ||
      *currentData < host->receivedData ||
      *currentData > &host->receivedData[host->receivedDataLength])
    return -1;

  if (devils_peer_queue_incoming_command(peer, command, (const devils_uint8 *)command + sizeof(devils_protocol_send_reliable), dataLength, DEVILS_PACKET_FLAG_RELIABLE, 0) == NULL)
    return -1;

  return 0;
}

static int
devils_protocol_handle_send_unsequenced(devils_host *host, devils_peer *peer, const devils_protocol *command, devils_uint8 **currentData)
{
  devils_uint32 unsequencedGroup, index;
  size_t dataLength;

  if (command->header.channelID >= peer->channelCount ||
      (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER))
    return -1;

  dataLength = DEVILS_NET_TO_HOST_16(command->sendUnsequenced.dataLength);
  *currentData += dataLength;
  if (dataLength > host->maximumPacketSize ||
      *currentData < host->receivedData ||
      *currentData > &host->receivedData[host->receivedDataLength])
    return -1;

  unsequencedGroup = DEVILS_NET_TO_HOST_16(command->sendUnsequenced.unsequencedGroup);
  index = unsequencedGroup % DEVILS_PEER_UNSEQUENCED_WINDOW_SIZE;

  if (unsequencedGroup < peer->incomingUnsequencedGroup)
    unsequencedGroup += 0x10000;

  if (unsequencedGroup >= (devils_uint32)peer->incomingUnsequencedGroup + DEVILS_PEER_FREE_UNSEQUENCED_WINDOWS * DEVILS_PEER_UNSEQUENCED_WINDOW_SIZE)
    return 0;

  unsequencedGroup &= 0xFFFF;

  if (unsequencedGroup - index != peer->incomingUnsequencedGroup)
  {
    peer->incomingUnsequencedGroup = unsequencedGroup - index;

    memset(peer->unsequencedWindow, 0, sizeof(peer->unsequencedWindow));
  }
  else if (peer->unsequencedWindow[index / 32] & (1 << (index % 32)))
    return 0;

  if (devils_peer_queue_incoming_command(peer, command, (const devils_uint8 *)command + sizeof(devils_protocol_send_unsequenced), dataLength, DEVILS_PACKET_FLAG_UNSEQUENCED, 0) == NULL)
    return -1;

  peer->unsequencedWindow[index / 32] |= 1 << (index % 32);

  return 0;
}

static int
devils_protocol_handle_send_unreliable(devils_host *host, devils_peer *peer, const devils_protocol *command, devils_uint8 **currentData)
{
  size_t dataLength;

  if (command->header.channelID >= peer->channelCount ||
      (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER))
    return -1;

  dataLength = DEVILS_NET_TO_HOST_16(command->sendUnreliable.dataLength);
  *currentData += dataLength;
  if (dataLength > host->maximumPacketSize ||
      *currentData < host->receivedData ||
      *currentData > &host->receivedData[host->receivedDataLength])
    return -1;

  if (devils_peer_queue_incoming_command(peer, command, (const devils_uint8 *)command + sizeof(devils_protocol_send_unreliable), dataLength, 0, 0) == NULL)
    return -1;

  return 0;
}

static int
devils_protocol_handle_send_fragment(devils_host *host, devils_peer *peer, const devils_protocol *command, devils_uint8 **currentData)
{
  devils_uint32 fragmentNumber,
      fragmentCount,
      fragmentOffset,
      fragmentLength,
      startSequenceNumber,
      totalLength;
  devils_channel *channel;
  devils_uint16 startWindow, currentWindow;
  devils_list_iterator currentCommand;
  devils_incoming_command *startCommand = NULL;

  if (command->header.channelID >= peer->channelCount ||
      (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER))
    return -1;

  fragmentLength = DEVILS_NET_TO_HOST_16(command->sendFragment.dataLength);
  *currentData += fragmentLength;
  if (fragmentLength > host->maximumPacketSize ||
      *currentData < host->receivedData ||
      *currentData > &host->receivedData[host->receivedDataLength])
    return -1;

  channel = &peer->channels[command->header.channelID];
  startSequenceNumber = DEVILS_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);
  startWindow = startSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;
  currentWindow = channel->incomingReliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;

  if (startSequenceNumber < channel->incomingReliableSequenceNumber)
    startWindow += DEVILS_PEER_RELIABLE_WINDOWS;

  if (startWindow < currentWindow || startWindow >= currentWindow + DEVILS_PEER_FREE_RELIABLE_WINDOWS - 1)
    return 0;

  fragmentNumber = DEVILS_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
  fragmentCount = DEVILS_NET_TO_HOST_32(command->sendFragment.fragmentCount);
  fragmentOffset = DEVILS_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
  totalLength = DEVILS_NET_TO_HOST_32(command->sendFragment.totalLength);

  if (fragmentCount > DEVILS_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
      fragmentNumber >= fragmentCount ||
      totalLength > host->maximumPacketSize ||
      fragmentOffset >= totalLength ||
      fragmentLength > totalLength - fragmentOffset)
    return -1;

  for (currentCommand = devils_list_previous(devils_list_end(&channel->incomingReliableCommands));
       currentCommand != devils_list_end(&channel->incomingReliableCommands);
       currentCommand = devils_list_previous(currentCommand))
  {
    devils_incoming_command *incomingCommand = (devils_incoming_command *)currentCommand;

    if (startSequenceNumber >= channel->incomingReliableSequenceNumber)
    {
      if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
        continue;
    }
    else if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
      break;

    if (incomingCommand->reliableSequenceNumber <= startSequenceNumber)
    {
      if (incomingCommand->reliableSequenceNumber < startSequenceNumber)
        break;

      if ((incomingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK) != DEVILS_PROTOCOL_COMMAND_SEND_FRAGMENT ||
          totalLength != incomingCommand->packet->dataLength ||
          fragmentCount != incomingCommand->fragmentCount)
        return -1;

      startCommand = incomingCommand;
      break;
    }
  }

  if (startCommand == NULL)
  {
    devils_protocol hostCommand = *command;

    hostCommand.header.reliableSequenceNumber = startSequenceNumber;

    startCommand = devils_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength, DEVILS_PACKET_FLAG_RELIABLE, fragmentCount);
    if (startCommand == NULL)
      return -1;
  }

  if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
  {
    --startCommand->fragmentsRemaining;

    startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

    if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
      fragmentLength = startCommand->packet->dataLength - fragmentOffset;

    memcpy(startCommand->packet->data + fragmentOffset,
           (devils_uint8 *)command + sizeof(devils_protocol_send_fragment),
           fragmentLength);

    if (startCommand->fragmentsRemaining <= 0)
      devils_peer_dispatch_incoming_reliable_commands(peer, channel, NULL);
  }

  return 0;
}

static int
devils_protocol_handle_send_unreliable_fragment(devils_host *host, devils_peer *peer, const devils_protocol *command, devils_uint8 **currentData)
{
  devils_uint32 fragmentNumber,
      fragmentCount,
      fragmentOffset,
      fragmentLength,
      reliableSequenceNumber,
      startSequenceNumber,
      totalLength;
  devils_uint16 reliableWindow, currentWindow;
  devils_channel *channel;
  devils_list_iterator currentCommand;
  devils_incoming_command *startCommand = NULL;

  if (command->header.channelID >= peer->channelCount ||
      (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER))
    return -1;

  fragmentLength = DEVILS_NET_TO_HOST_16(command->sendFragment.dataLength);
  *currentData += fragmentLength;
  if (fragmentLength > host->maximumPacketSize ||
      *currentData < host->receivedData ||
      *currentData > &host->receivedData[host->receivedDataLength])
    return -1;

  channel = &peer->channels[command->header.channelID];
  reliableSequenceNumber = command->header.reliableSequenceNumber;
  startSequenceNumber = DEVILS_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);

  reliableWindow = reliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;
  currentWindow = channel->incomingReliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;

  if (reliableSequenceNumber < channel->incomingReliableSequenceNumber)
    reliableWindow += DEVILS_PEER_RELIABLE_WINDOWS;

  if (reliableWindow < currentWindow || reliableWindow >= currentWindow + DEVILS_PEER_FREE_RELIABLE_WINDOWS - 1)
    return 0;

  if (reliableSequenceNumber == channel->incomingReliableSequenceNumber &&
      startSequenceNumber <= channel->incomingUnreliableSequenceNumber)
    return 0;

  fragmentNumber = DEVILS_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
  fragmentCount = DEVILS_NET_TO_HOST_32(command->sendFragment.fragmentCount);
  fragmentOffset = DEVILS_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
  totalLength = DEVILS_NET_TO_HOST_32(command->sendFragment.totalLength);

  if (fragmentCount > DEVILS_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
      fragmentNumber >= fragmentCount ||
      totalLength > host->maximumPacketSize ||
      fragmentOffset >= totalLength ||
      fragmentLength > totalLength - fragmentOffset)
    return -1;

  for (currentCommand = devils_list_previous(devils_list_end(&channel->incomingUnreliableCommands));
       currentCommand != devils_list_end(&channel->incomingUnreliableCommands);
       currentCommand = devils_list_previous(currentCommand))
  {
    devils_incoming_command *incomingCommand = (devils_incoming_command *)currentCommand;

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

    if (incomingCommand->unreliableSequenceNumber <= startSequenceNumber)
    {
      if (incomingCommand->unreliableSequenceNumber < startSequenceNumber)
        break;

      if ((incomingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK) != DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT ||
          totalLength != incomingCommand->packet->dataLength ||
          fragmentCount != incomingCommand->fragmentCount)
        return -1;

      startCommand = incomingCommand;
      break;
    }
  }

  if (startCommand == NULL)
  {
    startCommand = devils_peer_queue_incoming_command(peer, command, NULL, totalLength, DEVILS_PACKET_FLAG_UNRELIABLE_FRAGMENT, fragmentCount);
    if (startCommand == NULL)
      return -1;
  }

  if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
  {
    --startCommand->fragmentsRemaining;

    startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

    if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
      fragmentLength = startCommand->packet->dataLength - fragmentOffset;

    memcpy(startCommand->packet->data + fragmentOffset,
           (devils_uint8 *)command + sizeof(devils_protocol_send_fragment),
           fragmentLength);

    if (startCommand->fragmentsRemaining <= 0)
      devils_peer_dispatch_incoming_unreliable_commands(peer, channel, NULL);
  }

  return 0;
}

static int
devils_protocol_handle_ping(devils_host *host, devils_peer *peer, const devils_protocol *command)
{
  if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
    return -1;

  return 0;
}

static int
devils_protocol_handle_bandwidth_limit(devils_host *host, devils_peer *peer, const devils_protocol *command)
{
  if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
    return -1;

  if (peer->incomingBandwidth != 0)
    --host->bandwidthLimitedPeers;

  peer->incomingBandwidth = DEVILS_NET_TO_HOST_32(command->bandwidthLimit.incomingBandwidth);
  peer->outgoingBandwidth = DEVILS_NET_TO_HOST_32(command->bandwidthLimit.outgoingBandwidth);

  if (peer->incomingBandwidth != 0)
    ++host->bandwidthLimitedPeers;

  if (peer->incomingBandwidth == 0 && host->outgoingBandwidth == 0)
    peer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;
  else if (peer->incomingBandwidth == 0 || host->outgoingBandwidth == 0)
    peer->windowSize = (DEVILS_MAX(peer->incomingBandwidth, host->outgoingBandwidth) /
                        DEVILS_PEER_WINDOW_SIZE_SCALE) *
                       DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;
  else
    peer->windowSize = (DEVILS_MIN(peer->incomingBandwidth, host->outgoingBandwidth) /
                        DEVILS_PEER_WINDOW_SIZE_SCALE) *
                       DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;

  if (peer->windowSize < DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE)
    peer->windowSize = DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;
  else if (peer->windowSize > DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE)
    peer->windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;

  return 0;
}

static int
devils_protocol_handle_throttle_configure(devils_host *host, devils_peer *peer, const devils_protocol *command)
{
  if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
    return -1;

  peer->packetThrottleInterval = DEVILS_NET_TO_HOST_32(command->throttleConfigure.packetThrottleInterval);
  peer->packetThrottleAcceleration = DEVILS_NET_TO_HOST_32(command->throttleConfigure.packetThrottleAcceleration);
  peer->packetThrottleDeceleration = DEVILS_NET_TO_HOST_32(command->throttleConfigure.packetThrottleDeceleration);

  return 0;
}

static int
devils_protocol_handle_disconnect(devils_host *host, devils_peer *peer, const devils_protocol *command)
{
  if (peer->state == DEVILS_PEER_STATE_DISCONNECTED || peer->state == DEVILS_PEER_STATE_ZOMBIE || peer->state == DEVILS_PEER_STATE_ACKNOWLEDGING_DISCONNECT)
    return 0;

  devils_peer_reset_queues(peer);

  if (peer->state == DEVILS_PEER_STATE_CONNECTION_SUCCEEDED || peer->state == DEVILS_PEER_STATE_DISCONNECTING || peer->state == DEVILS_PEER_STATE_CONNECTING)
    devils_protocol_dispatch_state(host, peer, DEVILS_PEER_STATE_ZOMBIE);
  else if (peer->state != DEVILS_PEER_STATE_CONNECTED && peer->state != DEVILS_PEER_STATE_DISCONNECT_LATER)
  {
    if (peer->state == DEVILS_PEER_STATE_CONNECTION_PENDING)
      host->recalculateBandwidthLimits = 1;

    devils_peer_reset(peer);
  }
  else if (command->header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
    devils_protocol_change_state(host, peer, DEVILS_PEER_STATE_ACKNOWLEDGING_DISCONNECT);
  else
    devils_protocol_dispatch_state(host, peer, DEVILS_PEER_STATE_ZOMBIE);

  if (peer->state != DEVILS_PEER_STATE_DISCONNECTED)
    peer->eventData = DEVILS_NET_TO_HOST_32(command->disconnect.data);

  return 0;
}

static int
devils_protocol_handle_acknowledge(devils_host *host, devils_event *event, devils_peer *peer, const devils_protocol *command)
{
  devils_uint32 roundTripTime,
      receivedSentTime,
      receivedReliableSequenceNumber;
  devils_protocol_command commandNumber;

  if (peer->state == DEVILS_PEER_STATE_DISCONNECTED || peer->state == DEVILS_PEER_STATE_ZOMBIE)
    return 0;

  receivedSentTime = DEVILS_NET_TO_HOST_16(command->acknowledge.receivedSentTime);
  receivedSentTime |= host->serviceTime & 0xFFFF0000;
  if ((receivedSentTime & 0x8000) > (host->serviceTime & 0x8000))
    receivedSentTime -= 0x10000;

  if (DEVILS_TIME_LESS(host->serviceTime, receivedSentTime))
    return 0;

  roundTripTime = DEVILS_TIME_DIFFERENCE(host->serviceTime, receivedSentTime);
  roundTripTime = DEVILS_MAX(roundTripTime, 1);

  if (peer->lastReceiveTime > 0)
  {
    devils_peer_throttle(peer, roundTripTime);

    peer->roundTripTimeVariance -= peer->roundTripTimeVariance / 4;

    if (roundTripTime >= peer->roundTripTime)
    {
      devils_uint32 diff = roundTripTime - peer->roundTripTime;
      peer->roundTripTimeVariance += diff / 4;
      peer->roundTripTime += diff / 8;
    }
    else
    {
      devils_uint32 diff = peer->roundTripTime - roundTripTime;
      peer->roundTripTimeVariance += diff / 4;
      peer->roundTripTime -= diff / 8;
    }
  }
  else
  {
    peer->roundTripTime = roundTripTime;
    peer->roundTripTimeVariance = (roundTripTime + 1) / 2;
  }

  if (peer->roundTripTime < peer->lowestRoundTripTime)
    peer->lowestRoundTripTime = peer->roundTripTime;

  if (peer->roundTripTimeVariance > peer->highestRoundTripTimeVariance)
    peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;

  if (peer->packetThrottleEpoch == 0 ||
      DEVILS_TIME_DIFFERENCE(host->serviceTime, peer->packetThrottleEpoch) >= peer->packetThrottleInterval)
  {
    peer->lastRoundTripTime = peer->lowestRoundTripTime;
    peer->lastRoundTripTimeVariance = DEVILS_MAX(peer->highestRoundTripTimeVariance, 1);
    peer->lowestRoundTripTime = peer->roundTripTime;
    peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;
    peer->packetThrottleEpoch = host->serviceTime;
  }

  peer->lastReceiveTime = DEVILS_MAX(host->serviceTime, 1);
  peer->earliestTimeout = 0;

  receivedReliableSequenceNumber = DEVILS_NET_TO_HOST_16(command->acknowledge.receivedReliableSequenceNumber);

  commandNumber = devils_protocol_remove_sent_reliable_command(peer, receivedReliableSequenceNumber, command->header.channelID);

  switch (peer->state)
  {
  case DEVILS_PEER_STATE_ACKNOWLEDGING_CONNECT:
    if (commandNumber != DEVILS_PROTOCOL_COMMAND_VERIFY_CONNECT)
      return -1;

    devils_protocol_notify_connect(host, peer, event);
    break;

  case DEVILS_PEER_STATE_DISCONNECTING:
    if (commandNumber != DEVILS_PROTOCOL_COMMAND_DISCONNECT)
      return -1;

    devils_protocol_notify_disconnect(host, peer, event);
    break;

  case DEVILS_PEER_STATE_DISCONNECT_LATER:
    if (devils_list_empty(&peer->outgoingCommands) &&
        devils_list_empty(&peer->sentReliableCommands))
      devils_peer_disconnect(peer, peer->eventData);
    break;

  default:
    break;
  }

  return 0;
}

static int
devils_protocol_handle_verify_connect(devils_host *host, devils_event *event, devils_peer *peer, const devils_protocol *command)
{
  devils_uint32 mtu, windowSize;
  size_t channelCount;

  if (peer->state != DEVILS_PEER_STATE_CONNECTING)
    return 0;

  channelCount = DEVILS_NET_TO_HOST_32(command->verifyConnect.channelCount);

  if (channelCount < DEVILS_PROTOCOL_MINIMUM_CHANNEL_COUNT || channelCount > DEVILS_PROTOCOL_MAXIMUM_CHANNEL_COUNT ||
      DEVILS_NET_TO_HOST_32(command->verifyConnect.packetThrottleInterval) != peer->packetThrottleInterval ||
      DEVILS_NET_TO_HOST_32(command->verifyConnect.packetThrottleAcceleration) != peer->packetThrottleAcceleration ||
      DEVILS_NET_TO_HOST_32(command->verifyConnect.packetThrottleDeceleration) != peer->packetThrottleDeceleration ||
      command->verifyConnect.connectID != peer->connectID)
  {
    peer->eventData = 0;

    devils_protocol_dispatch_state(host, peer, DEVILS_PEER_STATE_ZOMBIE);

    return -1;
  }

  devils_protocol_remove_sent_reliable_command(peer, 1, 0xFF);

  if (channelCount < peer->channelCount)
    peer->channelCount = channelCount;

  peer->outgoingPeerID = DEVILS_NET_TO_HOST_16(command->verifyConnect.outgoingPeerID);
  peer->incomingSessionID = command->verifyConnect.incomingSessionID;
  peer->outgoingSessionID = command->verifyConnect.outgoingSessionID;

  mtu = DEVILS_NET_TO_HOST_32(command->verifyConnect.mtu);

  if (mtu < DEVILS_PROTOCOL_MINIMUM_MTU)
    mtu = DEVILS_PROTOCOL_MINIMUM_MTU;
  else if (mtu > DEVILS_PROTOCOL_MAXIMUM_MTU)
    mtu = DEVILS_PROTOCOL_MAXIMUM_MTU;

  if (mtu < peer->mtu)
    peer->mtu = mtu;

  windowSize = DEVILS_NET_TO_HOST_32(command->verifyConnect.windowSize);

  if (windowSize < DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE)
    windowSize = DEVILS_PROTOCOL_MINIMUM_WINDOW_SIZE;

  if (windowSize > DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE)
    windowSize = DEVILS_PROTOCOL_MAXIMUM_WINDOW_SIZE;

  if (windowSize < peer->windowSize)
    peer->windowSize = windowSize;

  peer->incomingBandwidth = DEVILS_NET_TO_HOST_32(command->verifyConnect.incomingBandwidth);
  peer->outgoingBandwidth = DEVILS_NET_TO_HOST_32(command->verifyConnect.outgoingBandwidth);

  devils_protocol_notify_connect(host, peer, event);
  return 0;
}

static int
devils_protocol_handle_incoming_commands(devils_host *host, devils_event *event)
{
  devils_protocol_header *header;
  devils_protocol *command;
  devils_peer *peer;
  devils_uint8 *currentData;
  size_t headerSize;
  devils_uint16 peerID, flags;
  devils_uint8 sessionID;

  if (host->receivedDataLength < (size_t) & ((devils_protocol_header *)0)->sentTime)
    return 0;

  header = (devils_protocol_header *)host->receivedData;

  peerID = DEVILS_NET_TO_HOST_16(header->peerID);
  sessionID = (peerID & DEVILS_PROTOCOL_HEADER_SESSION_MASK) >> DEVILS_PROTOCOL_HEADER_SESSION_SHIFT;
  flags = peerID & DEVILS_PROTOCOL_HEADER_FLAG_MASK;
  peerID &= ~(DEVILS_PROTOCOL_HEADER_FLAG_MASK | DEVILS_PROTOCOL_HEADER_SESSION_MASK);

  headerSize = (flags & DEVILS_PROTOCOL_HEADER_FLAG_SENT_TIME ? sizeof(devils_protocol_header) : (size_t) & ((devils_protocol_header *)0)->sentTime);
  if (host->checksum != NULL)
    headerSize += sizeof(devils_uint32);

  if (peerID == DEVILS_PROTOCOL_MAXIMUM_PEER_ID)
    peer = NULL;
  else if (peerID >= host->peerCount)
    return 0;
  else
  {
    peer = &host->peers[peerID];

    if (peer->state == DEVILS_PEER_STATE_DISCONNECTED ||
        peer->state == DEVILS_PEER_STATE_ZOMBIE ||
        ((host->receivedAddress.host != peer->address.host ||
          host->receivedAddress.port != peer->address.port) &&
         peer->address.host != DEVILS_HOST_BROADCAST) ||
        (peer->outgoingPeerID < DEVILS_PROTOCOL_MAXIMUM_PEER_ID &&
         sessionID != peer->incomingSessionID))
      return 0;
  }

  if (flags & DEVILS_PROTOCOL_HEADER_FLAG_COMPRESSED)
  {
    size_t originalSize;
    if (host->compressor.context == NULL || host->compressor.decompress == NULL)
      return 0;

    originalSize = host->compressor.decompress(host->compressor.context,
                                               host->receivedData + headerSize,
                                               host->receivedDataLength - headerSize,
                                               host->packetData[1] + headerSize,
                                               sizeof(host->packetData[1]) - headerSize);
    if (originalSize <= 0 || originalSize > sizeof(host->packetData[1]) - headerSize)
      return 0;

    memcpy(host->packetData[1], header, headerSize);
    host->receivedData = host->packetData[1];
    host->receivedDataLength = headerSize + originalSize;
  }

  if (host->checksum != NULL)
  {
    devils_uint32 *checksum = (devils_uint32 *)&host->receivedData[headerSize - sizeof(devils_uint32)],
                  desiredChecksum = *checksum;
    devils_buffer buffer;

    *checksum = peer != NULL ? peer->connectID : 0;

    buffer.data = host->receivedData;
    buffer.dataLength = host->receivedDataLength;

    if (host->checksum(&buffer, 1) != desiredChecksum)
      return 0;
  }

  if (peer != NULL)
  {
    peer->address.host = host->receivedAddress.host;
    peer->address.port = host->receivedAddress.port;
    peer->incomingDataTotal += host->receivedDataLength;
  }

  currentData = host->receivedData + headerSize;

  while (currentData < &host->receivedData[host->receivedDataLength])
  {
    devils_uint8 commandNumber;
    size_t commandSize;

    command = (devils_protocol *)currentData;

    if (currentData + sizeof(devils_protocol_command_header) > &host->receivedData[host->receivedDataLength])
      break;

    commandNumber = command->header.command & DEVILS_PROTOCOL_COMMAND_MASK;
    if (commandNumber >= DEVILS_PROTOCOL_COMMAND_COUNT)
      break;

    commandSize = commandSizes[commandNumber];
    if (commandSize == 0 || currentData + commandSize > &host->receivedData[host->receivedDataLength])
      break;

    currentData += commandSize;

    if (peer == NULL && commandNumber != DEVILS_PROTOCOL_COMMAND_CONNECT)
      break;

    command->header.reliableSequenceNumber = DEVILS_NET_TO_HOST_16(command->header.reliableSequenceNumber);

    switch (commandNumber)
    {
    case DEVILS_PROTOCOL_COMMAND_ACKNOWLEDGE:
      if (devils_protocol_handle_acknowledge(host, event, peer, command))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_CONNECT:
      if (peer != NULL)
        goto commandError;
      peer = devils_protocol_handle_connect(host, header, command);
      if (peer == NULL)
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_VERIFY_CONNECT:
      if (devils_protocol_handle_verify_connect(host, event, peer, command))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_DISCONNECT:
      if (devils_protocol_handle_disconnect(host, peer, command))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_PING:
      if (devils_protocol_handle_ping(host, peer, command))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_SEND_RELIABLE:
      if (devils_protocol_handle_send_reliable(host, peer, command, &currentData))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE:
      if (devils_protocol_handle_send_unreliable(host, peer, command, &currentData))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
      if (devils_protocol_handle_send_unsequenced(host, peer, command, &currentData))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_SEND_FRAGMENT:
      if (devils_protocol_handle_send_fragment(host, peer, command, &currentData))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_BANDWIDTH_LIMIT:
      if (devils_protocol_handle_bandwidth_limit(host, peer, command))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_THROTTLE_CONFIGURE:
      if (devils_protocol_handle_throttle_configure(host, peer, command))
        goto commandError;
      break;

    case DEVILS_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
      if (devils_protocol_handle_send_unreliable_fragment(host, peer, command, &currentData))
        goto commandError;
      break;

    default:
      goto commandError;
    }

    if (peer != NULL &&
        (command->header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0)
    {
      devils_uint16 sentTime;

      if (!(flags & DEVILS_PROTOCOL_HEADER_FLAG_SENT_TIME))
        break;

      sentTime = DEVILS_NET_TO_HOST_16(header->sentTime);

      switch (peer->state)
      {
      case DEVILS_PEER_STATE_DISCONNECTING:
      case DEVILS_PEER_STATE_ACKNOWLEDGING_CONNECT:
      case DEVILS_PEER_STATE_DISCONNECTED:
      case DEVILS_PEER_STATE_ZOMBIE:
        break;

      case DEVILS_PEER_STATE_ACKNOWLEDGING_DISCONNECT:
        if ((command->header.command & DEVILS_PROTOCOL_COMMAND_MASK) == DEVILS_PROTOCOL_COMMAND_DISCONNECT)
          devils_peer_queue_acknowledgement(peer, command, sentTime);
        break;

      default:
        devils_peer_queue_acknowledgement(peer, command, sentTime);
        break;
      }
    }
  }

commandError:
  if (event != NULL && event->type != DEVILS_EVENT_TYPE_NONE)
    return 1;

  return 0;
}

static int
devils_protocol_receive_incoming_commands(devils_host *host, devils_event *event)
{
  int packets;

  for (packets = 0; packets < 256; ++packets)
  {
    int receivedLength;
    devils_buffer buffer;

    buffer.data = host->packetData[0];
    buffer.dataLength = sizeof(host->packetData[0]);

    receivedLength = devils_socket_receive(host->socket,
                                           &host->receivedAddress,
                                           &buffer,
                                           1);

    if (receivedLength < 0)
      return -1;

    if (receivedLength == 0)
      return 0;

    host->receivedData = host->packetData[0];
    host->receivedDataLength = receivedLength;

    host->totalReceivedData += receivedLength;
    host->totalReceivedPackets++;

    if (host->intercept != NULL)
    {
      switch (host->intercept(host, event))
      {
      case 1:
        if (event != NULL && event->type != DEVILS_EVENT_TYPE_NONE)
          return 1;

        continue;

      case -1:
        return -1;

      default:
        break;
      }
    }

    switch (devils_protocol_handle_incoming_commands(host, event))
    {
    case 1:
      return 1;

    case -1:
      return -1;

    default:
      break;
    }
  }

  return 0;
}

static void
devils_protocol_send_acknowledgements(devils_host *host, devils_peer *peer)
{
  devils_protocol *command = &host->commands[host->commandCount];
  devils_buffer *buffer = &host->buffers[host->bufferCount];
  devils_acknowledgement *acknowledgement;
  devils_list_iterator currentAcknowledgement;
  devils_uint16 reliableSequenceNumber;

  currentAcknowledgement = devils_list_begin(&peer->acknowledgements);

  while (currentAcknowledgement != devils_list_end(&peer->acknowledgements))
  {
    if (command >= &host->commands[sizeof(host->commands) / sizeof(devils_protocol)] ||
        buffer >= &host->buffers[sizeof(host->buffers) / sizeof(devils_buffer)] ||
        peer->mtu - host->packetSize < sizeof(devils_protocol_acknowledge))
    {
      host->continueSending = 1;

      break;
    }

    acknowledgement = (devils_acknowledgement *)currentAcknowledgement;

    currentAcknowledgement = devils_list_next(currentAcknowledgement);

    buffer->data = command;
    buffer->dataLength = sizeof(devils_protocol_acknowledge);

    host->packetSize += buffer->dataLength;

    reliableSequenceNumber = DEVILS_HOST_TO_NET_16(acknowledgement->command.header.reliableSequenceNumber);

    command->header.command = DEVILS_PROTOCOL_COMMAND_ACKNOWLEDGE;
    command->header.channelID = acknowledgement->command.header.channelID;
    command->header.reliableSequenceNumber = reliableSequenceNumber;
    command->acknowledge.receivedReliableSequenceNumber = reliableSequenceNumber;
    command->acknowledge.receivedSentTime = DEVILS_HOST_TO_NET_16(acknowledgement->sentTime);

    if ((acknowledgement->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK) == DEVILS_PROTOCOL_COMMAND_DISCONNECT)
      devils_protocol_dispatch_state(host, peer, DEVILS_PEER_STATE_ZOMBIE);

    devils_list_remove(&acknowledgement->acknowledgementList);
    devils_free(acknowledgement);

    ++command;
    ++buffer;
  }

  host->commandCount = command - host->commands;
  host->bufferCount = buffer - host->buffers;
}

static int
devils_protocol_check_timeouts(devils_host *host, devils_peer *peer, devils_event *event)
{
  devils_outgoing_command *outgoingCommand;
  devils_list_iterator currentCommand, insertPosition;

  currentCommand = devils_list_begin(&peer->sentReliableCommands);
  insertPosition = devils_list_begin(&peer->outgoingCommands);

  while (currentCommand != devils_list_end(&peer->sentReliableCommands))
  {
    outgoingCommand = (devils_outgoing_command *)currentCommand;

    currentCommand = devils_list_next(currentCommand);

    if (DEVILS_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) < outgoingCommand->roundTripTimeout)
      continue;

    if (peer->earliestTimeout == 0 ||
        DEVILS_TIME_LESS(outgoingCommand->sentTime, peer->earliestTimeout))
      peer->earliestTimeout = outgoingCommand->sentTime;

    if (peer->earliestTimeout != 0 &&
        (DEVILS_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMaximum ||
         (outgoingCommand->roundTripTimeout >= outgoingCommand->roundTripTimeoutLimit &&
          DEVILS_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMinimum)))
    {
      devils_protocol_notify_disconnect(host, peer, event);

      return 1;
    }

    if (outgoingCommand->packet != NULL)
      peer->reliableDataInTransit -= outgoingCommand->fragmentLength;

    ++peer->packetsLost;

    outgoingCommand->roundTripTimeout *= 2;

    devils_list_insert(insertPosition, devils_list_remove(&outgoingCommand->outgoingCommandList));

    if (currentCommand == devils_list_begin(&peer->sentReliableCommands) &&
        !devils_list_empty(&peer->sentReliableCommands))
    {
      outgoingCommand = (devils_outgoing_command *)currentCommand;

      peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
    }
  }

  return 0;
}

static int
devils_protocol_check_outgoing_commands(devils_host *host, devils_peer *peer)
{
  devils_protocol *command = &host->commands[host->commandCount];
  devils_buffer *buffer = &host->buffers[host->bufferCount];
  devils_outgoing_command *outgoingCommand;
  devils_list_iterator currentCommand;
  devils_channel *channel = NULL;
  devils_uint16 reliableWindow = 0;
  size_t commandSize;
  int windowExceeded = 0, windowWrap = 0, canPing = 1;

  currentCommand = devils_list_begin(&peer->outgoingCommands);

  while (currentCommand != devils_list_end(&peer->outgoingCommands))
  {
    outgoingCommand = (devils_outgoing_command *)currentCommand;

    if (outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
    {
      channel = outgoingCommand->command.header.channelID < peer->channelCount ? &peer->channels[outgoingCommand->command.header.channelID] : NULL;
      reliableWindow = outgoingCommand->reliableSequenceNumber / DEVILS_PEER_RELIABLE_WINDOW_SIZE;
      if (channel != NULL)
      {
        if (!windowWrap &&
            outgoingCommand->sendAttempts < 1 &&
            !(outgoingCommand->reliableSequenceNumber % DEVILS_PEER_RELIABLE_WINDOW_SIZE) &&
            (channel->reliableWindows[(reliableWindow + DEVILS_PEER_RELIABLE_WINDOWS - 1) % DEVILS_PEER_RELIABLE_WINDOWS] >= DEVILS_PEER_RELIABLE_WINDOW_SIZE ||
             channel->usedReliableWindows & ((((1 << (DEVILS_PEER_FREE_RELIABLE_WINDOWS + 2)) - 1) << reliableWindow) |
                                             (((1 << (DEVILS_PEER_FREE_RELIABLE_WINDOWS + 2)) - 1) >> (DEVILS_PEER_RELIABLE_WINDOWS - reliableWindow)))))
          windowWrap = 1;
        if (windowWrap)
        {
          currentCommand = devils_list_next(currentCommand);

          continue;
        }
      }

      if (outgoingCommand->packet != NULL)
      {
        if (!windowExceeded)
        {
          devils_uint32 windowSize = (peer->packetThrottle * peer->windowSize) / DEVILS_PEER_PACKET_THROTTLE_SCALE;

          if (peer->reliableDataInTransit + outgoingCommand->fragmentLength > DEVILS_MAX(windowSize, peer->mtu))
            windowExceeded = 1;
        }
        if (windowExceeded)
        {
          currentCommand = devils_list_next(currentCommand);

          continue;
        }
      }

      canPing = 0;
    }

    commandSize = commandSizes[outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_MASK];
    if (command >= &host->commands[sizeof(host->commands) / sizeof(devils_protocol)] ||
        buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(devils_buffer)] ||
        peer->mtu - host->packetSize < commandSize ||
        (outgoingCommand->packet != NULL &&
         (devils_uint16)(peer->mtu - host->packetSize) < (devils_uint16)(commandSize + outgoingCommand->fragmentLength)))
    {
      host->continueSending = 1;

      break;
    }

    currentCommand = devils_list_next(currentCommand);

    if (outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
    {
      if (channel != NULL && outgoingCommand->sendAttempts < 1)
      {
        channel->usedReliableWindows |= 1 << reliableWindow;
        ++channel->reliableWindows[reliableWindow];
      }

      ++outgoingCommand->sendAttempts;

      if (outgoingCommand->roundTripTimeout == 0)
      {
        outgoingCommand->roundTripTimeout = peer->roundTripTime + 4 * peer->roundTripTimeVariance;
        outgoingCommand->roundTripTimeoutLimit = peer->timeoutLimit * outgoingCommand->roundTripTimeout;
      }

      if (devils_list_empty(&peer->sentReliableCommands))
        peer->nextTimeout = host->serviceTime + outgoingCommand->roundTripTimeout;

      devils_list_insert(devils_list_end(&peer->sentReliableCommands),
                         devils_list_remove(&outgoingCommand->outgoingCommandList));

      outgoingCommand->sentTime = host->serviceTime;

      host->headerFlags |= DEVILS_PROTOCOL_HEADER_FLAG_SENT_TIME;

      peer->reliableDataInTransit += outgoingCommand->fragmentLength;
    }
    else
    {
      if (outgoingCommand->packet != NULL && outgoingCommand->fragmentOffset == 0)
      {
        peer->packetThrottleCounter += DEVILS_PEER_PACKET_THROTTLE_COUNTER;
        peer->packetThrottleCounter %= DEVILS_PEER_PACKET_THROTTLE_SCALE;

        if (peer->packetThrottleCounter > peer->packetThrottle)
        {
          devils_uint16 reliableSequenceNumber = outgoingCommand->reliableSequenceNumber,
                        unreliableSequenceNumber = outgoingCommand->unreliableSequenceNumber;
          for (;;)
          {
            --outgoingCommand->packet->referenceCount;

            if (outgoingCommand->packet->referenceCount == 0)
              devils_packet_destroy(outgoingCommand->packet);

            devils_list_remove(&outgoingCommand->outgoingCommandList);
            devils_free(outgoingCommand);

            if (currentCommand == devils_list_end(&peer->outgoingCommands))
              break;

            outgoingCommand = (devils_outgoing_command *)currentCommand;
            if (outgoingCommand->reliableSequenceNumber != reliableSequenceNumber ||
                outgoingCommand->unreliableSequenceNumber != unreliableSequenceNumber)
              break;

            currentCommand = devils_list_next(currentCommand);
          }

          continue;
        }
      }

      devils_list_remove(&outgoingCommand->outgoingCommandList);

      if (outgoingCommand->packet != NULL)
        devils_list_insert(devils_list_end(&peer->sentUnreliableCommands), outgoingCommand);
    }

    buffer->data = command;
    buffer->dataLength = commandSize;

    host->packetSize += buffer->dataLength;

    *command = outgoingCommand->command;

    if (outgoingCommand->packet != NULL)
    {
      ++buffer;

      buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
      buffer->dataLength = outgoingCommand->fragmentLength;

      host->packetSize += outgoingCommand->fragmentLength;
    }
    else if (!(outgoingCommand->command.header.command & DEVILS_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))
      devils_free(outgoingCommand);

    ++peer->packetsSent;

    ++command;
    ++buffer;
  }

  host->commandCount = command - host->commands;
  host->bufferCount = buffer - host->buffers;

  if (peer->state == DEVILS_PEER_STATE_DISCONNECT_LATER &&
      devils_list_empty(&peer->outgoingCommands) &&
      devils_list_empty(&peer->sentReliableCommands) &&
      devils_list_empty(&peer->sentUnreliableCommands))
    devils_peer_disconnect(peer, peer->eventData);

  return canPing;
}

static int
devils_protocol_send_outgoing_commands(devils_host *host, devils_event *event, int checkForTimeouts)
{
  devils_uint8 headerData[sizeof(devils_protocol_header) + sizeof(devils_uint32)];
  devils_protocol_header *header = (devils_protocol_header *)headerData;
  devils_peer *currentPeer;
  int sentLength;
  size_t shouldCompress = 0;

  host->continueSending = 1;

  while (host->continueSending)
    for (host->continueSending = 0,
        currentPeer = host->peers;
         currentPeer < &host->peers[host->peerCount];
         ++currentPeer)
    {
      if (currentPeer->state == DEVILS_PEER_STATE_DISCONNECTED ||
          currentPeer->state == DEVILS_PEER_STATE_ZOMBIE)
        continue;

      host->headerFlags = 0;
      host->commandCount = 0;
      host->bufferCount = 1;
      host->packetSize = sizeof(devils_protocol_header);

      if (!devils_list_empty(&currentPeer->acknowledgements))
        devils_protocol_send_acknowledgements(host, currentPeer);

      if (checkForTimeouts != 0 &&
          !devils_list_empty(&currentPeer->sentReliableCommands) &&
          DEVILS_TIME_GREATER_EQUAL(host->serviceTime, currentPeer->nextTimeout) &&
          devils_protocol_check_timeouts(host, currentPeer, event) == 1)
      {
        if (event != NULL && event->type != DEVILS_EVENT_TYPE_NONE)
          return 1;
        else
          continue;
      }

      if ((devils_list_empty(&currentPeer->outgoingCommands) ||
           devils_protocol_check_outgoing_commands(host, currentPeer)) &&
          devils_list_empty(&currentPeer->sentReliableCommands) &&
          DEVILS_TIME_DIFFERENCE(host->serviceTime, currentPeer->lastReceiveTime) >= currentPeer->pingInterval &&
          currentPeer->mtu - host->packetSize >= sizeof(devils_protocol_ping))
      {
        devils_peer_ping(currentPeer);
        devils_protocol_check_outgoing_commands(host, currentPeer);
      }

      if (host->commandCount == 0)
        continue;

      if (currentPeer->packetLossEpoch == 0)
        currentPeer->packetLossEpoch = host->serviceTime;
      else if (DEVILS_TIME_DIFFERENCE(host->serviceTime, currentPeer->packetLossEpoch) >= DEVILS_PEER_PACKET_LOSS_INTERVAL &&
               currentPeer->packetsSent > 0)
      {
        devils_uint32 packetLoss = currentPeer->packetsLost * DEVILS_PEER_PACKET_LOSS_SCALE / currentPeer->packetsSent;

#ifdef DEVILS_DEBUG
        printf("peer %u: %f%%+-%f%% packet loss, %u+-%u ms round trip time, %f%% throttle, %u outgoing, %u/%u incoming\n", currentPeer->incomingPeerID, currentPeer->packetLoss / (float)DEVILS_PEER_PACKET_LOSS_SCALE, currentPeer->packetLossVariance / (float)DEVILS_PEER_PACKET_LOSS_SCALE, currentPeer->roundTripTime, currentPeer->roundTripTimeVariance, currentPeer->packetThrottle / (float)DEVILS_PEER_PACKET_THROTTLE_SCALE, devils_list_size(&currentPeer->outgoingCommands), currentPeer->channels != NULL ? devils_list_size(&currentPeer->channels->incomingReliableCommands) : 0, currentPeer->channels != NULL ? devils_list_size(&currentPeer->channels->incomingUnreliableCommands) : 0);
#endif

        currentPeer->packetLossVariance = (currentPeer->packetLossVariance * 3 + DEVILS_DIFFERENCE(packetLoss, currentPeer->packetLoss)) / 4;
        currentPeer->packetLoss = (currentPeer->packetLoss * 7 + packetLoss) / 8;

        currentPeer->packetLossEpoch = host->serviceTime;
        currentPeer->packetsSent = 0;
        currentPeer->packetsLost = 0;
      }

      host->buffers->data = headerData;
      if (host->headerFlags & DEVILS_PROTOCOL_HEADER_FLAG_SENT_TIME)
      {
        header->sentTime = DEVILS_HOST_TO_NET_16(host->serviceTime & 0xFFFF);

        host->buffers->dataLength = sizeof(devils_protocol_header);
      }
      else
        host->buffers->dataLength = (size_t) & ((devils_protocol_header *)0)->sentTime;

      shouldCompress = 0;
      if (host->compressor.context != NULL && host->compressor.compress != NULL)
      {
        size_t originalSize = host->packetSize - sizeof(devils_protocol_header),
               compressedSize = host->compressor.compress(host->compressor.context,
                                                          &host->buffers[1], host->bufferCount - 1,
                                                          originalSize,
                                                          host->packetData[1],
                                                          originalSize);
        if (compressedSize > 0 && compressedSize < originalSize)
        {
          host->headerFlags |= DEVILS_PROTOCOL_HEADER_FLAG_COMPRESSED;
          shouldCompress = compressedSize;
#ifdef DEVILS_DEBUG_COMPRESS
          printf("peer %u: compressed %u -> %u (%u%%)\n", currentPeer->incomingPeerID, originalSize, compressedSize, (compressedSize * 100) / originalSize);
#endif
        }
      }

      if (currentPeer->outgoingPeerID < DEVILS_PROTOCOL_MAXIMUM_PEER_ID)
        host->headerFlags |= currentPeer->outgoingSessionID << DEVILS_PROTOCOL_HEADER_SESSION_SHIFT;
      header->peerID = DEVILS_HOST_TO_NET_16(currentPeer->outgoingPeerID | host->headerFlags);
      if (host->checksum != NULL)
      {
        devils_uint32 *checksum = (devils_uint32 *)&headerData[host->buffers->dataLength];
        *checksum = currentPeer->outgoingPeerID < DEVILS_PROTOCOL_MAXIMUM_PEER_ID ? currentPeer->connectID : 0;
        host->buffers->dataLength += sizeof(devils_uint32);
        *checksum = host->checksum(host->buffers, host->bufferCount);
      }

      if (shouldCompress > 0)
      {
        host->buffers[1].data = host->packetData[1];
        host->buffers[1].dataLength = shouldCompress;
        host->bufferCount = 2;
      }

      currentPeer->lastSendTime = host->serviceTime;

      sentLength = devils_socket_send(host->socket, &currentPeer->address, host->buffers, host->bufferCount);

      devils_protocol_remove_sent_unreliable_commands(currentPeer);

      if (sentLength < 0)
        return -1;

      host->totalSentData += sentLength;
      host->totalSentPackets++;
    }

  return 0;
}

/** Sends any queued packets on the host specified to its designated peers.

    @param host   host to flush
    @remarks this function need only be used in circumstances where one wishes to send queued packets earlier than in a call to devils_host_service().
    @ingroup host
*/
void devils_host_flush(devils_host *host)
{
  host->serviceTime = devils_time_get();

  devils_protocol_send_outgoing_commands(host, NULL, 0);
}

/** Checks for any queued events on the host and dispatches one if available.

    @param host    host to check for events
    @param event   an event structure where event details will be placed if available
    @retval > 0 if an event was dispatched
    @retval 0 if no events are available
    @retval < 0 on failure
    @ingroup host
*/
int devils_host_check_events(devils_host *host, devils_event *event)
{
  if (event == NULL)
    return -1;

  event->type = DEVILS_EVENT_TYPE_NONE;
  event->peer = NULL;
  event->packet = NULL;

  return devils_protocol_dispatch_incoming_commands(host, event);
}

/** Waits for events on the host specified and shuttles packets between
    the host and its peers.

    @param host    host to service
    @param event   an event structure where event details will be placed if one occurs
                   if event == NULL then no events will be delivered
    @param timeout number of milliseconds that ENet should wait for events
    @retval > 0 if an event occurred within the specified time limit
    @retval 0 if no event occurred
    @retval < 0 on failure
    @remarks devils_host_service should be called fairly regularly for adequate performance
    @ingroup host
*/
int devils_host_service(devils_host *host, devils_event *event, devils_uint32 timeout)
{
  devils_uint32 waitCondition;

  if (event != NULL)
  {
    event->type = DEVILS_EVENT_TYPE_NONE;
    event->peer = NULL;
    event->packet = NULL;

    switch (devils_protocol_dispatch_incoming_commands(host, event))
    {
    case 1:
      return 1;

    case -1:
#ifdef DEVILS_DEBUG
      perror("Error dispatching incoming packets");
#endif

      return -1;

    default:
      break;
    }
  }

  host->serviceTime = devils_time_get();

  timeout += host->serviceTime;

  do
  {
    if (DEVILS_TIME_DIFFERENCE(host->serviceTime, host->bandwidthThrottleEpoch) >= DEVILS_HOST_BANDWIDTH_THROTTLE_INTERVAL)
      devils_host_bandwidth_throttle(host);

    switch (devils_protocol_send_outgoing_commands(host, event, 1))
    {
    case 1:
      return 1;

    case -1:
#ifdef DEVILS_DEBUG
      perror("Error sending outgoing packets");
#endif

      return -1;

    default:
      break;
    }

    switch (devils_protocol_receive_incoming_commands(host, event))
    {
    case 1:
      return 1;

    case -1:
#ifdef DEVILS_DEBUG
      perror("Error receiving incoming packets");
#endif

      return -1;

    default:
      break;
    }

    switch (devils_protocol_send_outgoing_commands(host, event, 1))
    {
    case 1:
      return 1;

    case -1:
#ifdef DEVILS_DEBUG
      perror("Error sending outgoing packets");
#endif

      return -1;

    default:
      break;
    }

    if (event != NULL)
    {
      switch (devils_protocol_dispatch_incoming_commands(host, event))
      {
      case 1:
        return 1;

      case -1:
#ifdef DEVILS_DEBUG
        perror("Error dispatching incoming packets");
#endif

        return -1;

      default:
        break;
      }
    }

    if (DEVILS_TIME_GREATER_EQUAL(host->serviceTime, timeout))
      return 0;

    do
    {
      host->serviceTime = devils_time_get();

      if (DEVILS_TIME_GREATER_EQUAL(host->serviceTime, timeout))
        return 0;

      waitCondition = DEVILS_SOCKET_WAIT_RECEIVE | DEVILS_SOCKET_WAIT_INTERRUPT;

      if (devils_socket_wait(host->socket, &waitCondition, DEVILS_TIME_DIFFERENCE(timeout, host->serviceTime)) != 0)
        return -1;
    } while (waitCondition & DEVILS_SOCKET_WAIT_INTERRUPT);

    host->serviceTime = devils_time_get();
  } while (waitCondition & DEVILS_SOCKET_WAIT_RECEIVE);

  return 0;
}
