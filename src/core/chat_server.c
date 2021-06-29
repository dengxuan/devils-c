#include <stdio.h>
#include <enet/enet.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

#define INET_ADDRSTRLEN 16

enum pkt_type
{
    PKT_TYPE_INIT = 0,
    PKT_TYPE_CONTENT,

    PKT_TYPE_MAX,
};

struct packet
{
    enum pkt_type type;
    char data[256];
    size_t len;
};

static volatile sig_atomic_t G__running = false;

static void
htop(unsigned int ip4, unsigned short port, char *buf, size_t len)
{
    uint8_t *octets = (uint8_t *)&ip4;
    snprintf(buf, len, "%u.%u.%u.%u:%u",
             octets[0], octets[1], octets[2], octets[3], port);
}

static void
signal_handler(int signal)
{
    if (signal == SIGINT)
    {
        printf("sigint received\n");
        G__running = false;
    }
}

static void
usage(void)
{
    printf("Usage:\n\n");
    printf("  chat.srv <port>\n\n");
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        usage();
        return -1;
    }

    int port = atoi(argv[1]);

    if (signal(SIGINT, signal_handler) != SIG_ERR &&
        enet_initialize() == 0)
    {
        ENetHost *server;

        ENetAddress address;

        /* Bind the server to the default localhost.     */
        /* A specific host address can be specified by   */
        /* enet_address_set_host (& address, "x.x.x.x"); */
        address.host = ENET_HOST_ANY;

        /* Bind the server to port 1234. */
        address.port = port;
        server = enet_host_create(&address, /* the address to bind the server host to */
                                  32,       /* allow up to 32 clients and/or outgoing connections */
                                  2,        /* allow up to 2 channels to be used, 0 and 1 */
                                  0,        /* assume any amount of incoming bandwidth */
                                  0);       /* assume any amount of outgoing bandwidth */
        if (server)
        {
            G__running = true;
            printf("Starting chat server [%d]\n", port);

            while (G__running)
            {
                ENetEvent event;
                int ret = enet_host_service(server, &event, 100);
                if (ret > 0)
                {
                    ENetPacket *packet;
                    char buf[256] = {0};

                    printf("event type: %d\n", event.type);
                    switch (event.type)
                    {
                    case ENET_EVENT_TYPE_CONNECT:
                    {
                        char ip[INET_ADDRSTRLEN];

                        htop(event.peer->address.host, event.peer->address.port, ip, sizeof(ip));

                        printf("A new client connected [%s] id %u\n", ip, event.peer->incomingPeerID);

                        // event.peer->data = strdup (ip);
                    }
                    break;
                    case ENET_EVENT_TYPE_RECEIVE:
                    {
                        char *ip = (char *)event.peer->data;
                        uint8_t *data = (uint8_t *)event.packet->data;
                        char str[256] = {0};

                        printf("pkt> dataLength        : %zu\n", event.packet->dataLength);
                        printf("pkt> struct packet len : %zu\n", sizeof(struct packet));

                        if (event.packet->dataLength == sizeof(struct packet))
                        {
                            struct packet *pkt = (struct packet *)data;

                            printf("pkt> type  : %d\n", pkt->type);
                            printf("pkt> data  : %s\n", pkt->data);
                            printf("pkt> len   : %zu\n", pkt->len);

                            switch (pkt->type)
                            {
                            case PKT_TYPE_INIT:
                            {
                                event.peer->data = strdup(pkt->data);
                            }
                            break;
                            case PKT_TYPE_CONTENT:
                            {
                                snprintf(str, sizeof(str), "%s", pkt->data);
                            }
                            break;
                            default:
                            {
                                printf("Unkown packet type: %d\n", pkt->type);
                            }
                            break;
                            }
                        }

                        if (ip && ip[0] != '\0' &&
                            str && str[0] != '\0')
                        {
                            printf("[%s]: %s (ping=%u)\n", ip, str, event.peer->roundTripTime);

                            if (strncmp(str, "!who", 4) == 0)
                            {
                                snprintf(buf, sizeof(buf), "[SERVER] connected peers: %zu/%zu",
                                         server->connectedPeers, server->peerCount);
                            }
                            else
                            {
                                snprintf(buf, sizeof(buf), "[%s]: %s", ip, str);
                            }
                        }

                        if (buf[0] != '\0')
                        {
                            packet = enet_packet_create(buf, strlen(buf) + 1,
                                                        ENET_PACKET_FLAG_RELIABLE);
                            enet_host_broadcast(server, 0, packet);
                            enet_host_flush(server);

                            enet_packet_destroy(event.packet);
                        }
                    }
                    break;
                    case ENET_EVENT_TYPE_DISCONNECT:
                    {
                        if (event.peer->data)
                        {
                            printf("%s disconnected.\n", (char *)event.peer->data);

                            // free (event.peer->data);
                            // event.peer->data = NULL;
                        }
                        else
                        {
                            printf("%s disconnected.\n", "???");
                        }
                    }
                    break;
                    default:
                    {
                        printf("unknown event type: %d\n", event.type);
                    }
                    break;
                    }
                }
                else if (ret < 0)
                {
                    printf("An error occured trying to pump messages\n");
                }
            }

            printf("exiting\n");
            // TODO: disconnect all peers

            enet_host_destroy(server);
        }
        else
        {
            printf("An error occurred while trying to create an ENet server host.\n");
        }
    }

    enet_deinitialize();

    return 0;
}
