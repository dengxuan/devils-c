#include <stdio.h>
#include <string.h>
#include "../devils/include/devils.h"

int main(int argc, char **argv)
{
    devils_host *client = NULL;
    devils_address address;
    devils_event event;
    devils_peer *peer;
    devils_packet *packet;
    int retcode;
    unsigned int time_begin;
    const char *host = "localhost";
    unsigned short port = 1234;
    int verbose = 0;
    int packet_count = 10;
    int packet_length = 100;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0)
        {
            if (i + 1 < argc)
            {
                host = argv[i + 1];
            }
            i++;
        }
        else if (strcmp(argv[i], "-p") == 0)
        {
            if (i + 1 < argc)
            {
                port = (unsigned short)atoi(argv[i + 1]);
            }
            i++;
        }
        else if (strcmp(argv[i], "-v") == 0)
        {
            verbose = 1;
        }
        else if (strcmp(argv[i], "-c") == 0)
        {
            if (i + 1 < argc)
            {
                packet_count = atoi(argv[i + 1]);
            }
            i++;
        }
        else if (strcmp(argv[i], "-l") == 0)
        {
            if (i + 1 < argc)
            {
                packet_length = atoi(argv[i + 1]);
            }
            i++;
        }
    }

    if (devils_initialize() != 0)
    {
        fprintf(stderr, "An error occurred while initializing ENet.\n");
        return 1;
    }

    client = devils_host_create(NULL /* create a client host */,
                                1 /* only allow 1 outgoing connection */,
                                2 /* allow up 2 channels to be used, 0 and 1 */,
                                0 /* assume any amount of incoming bandwidth */,
                                0 /* assume any amount of outgoing bandwidth */);
    if (client == NULL)
    {
        fprintf(stderr, "An error occurred while trying to create an ENet client host.\n");
        goto Exit;
    }

    /* Initiate the connection. */
    devils_address_set_host(&address, host);
    address.port = port;
    peer = devils_host_connect(client /* host seeking the connection */,
                               &address /* destination for the connection */,
                               2 /* number of channels to allocate */,
                               0 /* user data supplied to the receiving host */);
    if (peer == NULL)
    {
        fprintf(stderr, "No available peers for initiating an ENet connection.\n");
        goto Exit;
    }

    /* Wait up to 5 seconds for the connection attempt to succeed. */
    if (devils_host_service(client, &event, 5000) > 0 && event.type == DEVILS_EVENT_TYPE_CONNECT)
    {
        fprintf(stdout, "Connect succeeded\n");
    }
    else
    {
        /* Either the 5 seconds are up or a disconnect event was */
        /* received. Reset the peer in the event the 5 seconds   */
        /* had run out without any significant event.            */
        devils_peer_reset(peer);

        fprintf(stderr, "Connect failed\n");
        goto Exit;
    }

    /* Ping-Pong */
    time_begin = devils_time_get();
    for (int i = 0; i < packet_count; i++)
    {
        packet = devils_packet_create(NULL /* initial contents of the packet's data */,
                                      packet_length /* size of the data allocated for this packet */,
                                      DEVILS_PACKET_FLAG_RELIABLE /* flags for this packet */);
        memset(packet->data, '0', packet_length);
        sprintf((char *)packet->data, "packet %d", i);
        devils_peer_send(peer /* destination for the packet */,
                         i % 2 /* channel on which to send */,
                         packet /* packet to send */);

        retcode = devils_host_service(client /* host to service */,
                                      &event /* an event structure where event details will be placed if one occurs */,
                                      1000 /* number of milliseconds that ENet should wait for events */);
        if (retcode > 0 && event.type == DEVILS_EVENT_TYPE_RECEIVE)
        {
            if (verbose)
            {
                fprintf(stdout,
                        "Reply packet %d: channel=%u length=%u data=\"%s\"\n",
                        i,
                        event.channelID,
                        (unsigned int)event.packet->dataLength,
                        (const char *)event.packet->data);
            }
            /* Clean up the packet now that we're done using it. */
            devils_packet_destroy(event.packet);
        }
        else
        {
            /* Something is wrong. */
            fprintf(stderr, "Lost reply packet %d\n", i);
        }
        sleep(1);
    }
    fprintf(stdout, "PacketCount=%d Time=%u\n", packet_count, devils_time_get() - time_begin);

    /* Send a large packet(should be fragmented). */
    packet = devils_packet_create(NULL, 6666, DEVILS_PACKET_FLAG_RELIABLE);
    memset(packet->data, 0, 6666);
    strcpy((char *)packet->data, "This is a large packet...");
    devils_peer_send(peer, 0, packet);

    /* Initiate the disconnection. */
    devils_peer_disconnect_later(peer /* peer to request a disconnection */,
                                 0 /* data describing the disconnection */);

    /* Allow up to 3 seconds for the disconnect to succeed and drop any packets received packets. */
    retcode = 1;
    while (retcode > 0)
    {
        retcode = devils_host_service(client, &event, 3000);
        if (retcode > 0)
        {
            switch (event.type)
            {
            case DEVILS_EVENT_TYPE_RECEIVE:
                devils_packet_destroy(event.packet);
                break;

            case DEVILS_EVENT_TYPE_DISCONNECT:
                fprintf(stdout, "Disconnect succeeded\n");
                retcode = 0;
                break;

            default:;
            }
        }
        else
        {
            /* Timeout or failure */
            fprintf(stderr, "Disconnect failed\n");
            devils_peer_reset(peer);
        }
    }

Exit:
    if (client != NULL)
    {
        devils_host_destroy(client);
    }
    devils_deinitialize();

    return 0;
}
