/** 
 @file  packet.c
 @brief ENet packet management functions
*/
#include <string.h>
#define DEVILS_BUILDING_LIB 1
#include "include/devils.h"

/** @defgroup Packet ENet packet functions 
    @{ 
*/

/** Creates a packet that may be sent to a peer.
    @param data         initial contents of the packet's data; the packet's data will remain uninitialized if data is NULL.
    @param dataLength   size of the data allocated for this packet
    @param flags        flags for this packet as described for the devils_packet structure.
    @returns the packet on success, NULL on failure
*/
devils_packet *
devils_packet_create(const void *data, size_t dataLength, devils_uint32 flags)
{
    devils_packet *packet = (devils_packet *)devils_malloc(sizeof(devils_packet));
    if (packet == NULL)
        return NULL;

    if (flags & DEVILS_PACKET_FLAG_NO_ALLOCATE)
        packet->data = (devils_uint8 *)data;
    else if (dataLength <= 0)
        packet->data = NULL;
    else
    {
        packet->data = (devils_uint8 *)devils_malloc(dataLength);
        if (packet->data == NULL)
        {
            devils_free(packet);
            return NULL;
        }

        if (data != NULL)
            memcpy(packet->data, data, dataLength);
    }

    packet->referenceCount = 0;
    packet->flags = flags;
    packet->dataLength = dataLength;
    packet->freeCallback = NULL;
    packet->userData = NULL;

    return packet;
}

/** Destroys the packet and deallocates its data.
    @param packet packet to be destroyed
*/
void devils_packet_destroy(devils_packet *packet)
{
    if (packet == NULL)
        return;

    if (packet->freeCallback != NULL)
        (*packet->freeCallback)(packet);
    if (!(packet->flags & DEVILS_PACKET_FLAG_NO_ALLOCATE) &&
        packet->data != NULL)
        devils_free(packet->data);
    devils_free(packet);
}

/** Attempts to resize the data in the packet to length specified in the 
    dataLength parameter 
    @param packet packet to resize
    @param dataLength new size for the packet data
    @returns 0 on success, < 0 on failure
*/
int devils_packet_resize(devils_packet *packet, size_t dataLength)
{
    devils_uint8 *newData;

    if (dataLength <= packet->dataLength || (packet->flags & DEVILS_PACKET_FLAG_NO_ALLOCATE))
    {
        packet->dataLength = dataLength;

        return 0;
    }

    newData = (devils_uint8 *)devils_malloc(dataLength);
    if (newData == NULL)
        return -1;

    memcpy(newData, packet->data, packet->dataLength);
    devils_free(packet->data);

    packet->data = newData;
    packet->dataLength = dataLength;

    return 0;
}

static int initializedCRC32 = 0;
static devils_uint32 crcTable[256];

static devils_uint32
reflect_crc(int val, int bits)
{
    int result = 0, bit;

    for (bit = 0; bit < bits; bit++)
    {
        if (val & 1)
            result |= 1 << (bits - 1 - bit);
        val >>= 1;
    }

    return result;
}

static void
initialize_crc32(void)
{
    int byte;

    for (byte = 0; byte < 256; ++byte)
    {
        devils_uint32 crc = reflect_crc(byte, 8) << 24;
        int offset;

        for (offset = 0; offset < 8; ++offset)
        {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04c11db7;
            else
                crc <<= 1;
        }

        crcTable[byte] = reflect_crc(crc, 32);
    }

    initializedCRC32 = 1;
}

devils_uint32
devils_crc32(const devils_buffer *buffers, size_t bufferCount)
{
    devils_uint32 crc = 0xFFFFFFFF;

    if (!initializedCRC32)
        initialize_crc32();

    while (bufferCount-- > 0)
    {
        const devils_uint8 *data = (const devils_uint8 *)buffers->data,
                           *dataEnd = &data[buffers->dataLength];

        while (data < dataEnd)
        {
            crc = (crc >> 8) ^ crcTable[(crc & 0xFF) ^ *data++];
        }

        ++buffers;
    }

    return DEVILS_HOST_TO_NET_32(~crc);
}

/** @} */
