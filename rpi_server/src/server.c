/*
 * RPi5 TCP-Server
 *
 * Akzeptiert Verbindungen von mehreren ESP32-C3 Clients gleichzeitig
 * ueber select(). Empfaengt Nachrichten gemaess protocol.h und
 * kann Befehle an einzelne ESPs zuruecksenden.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "../../common/protocol.h"

#define MAX_CLIENTS 16

typedef struct {
    int      fd;
    uint8_t  device_id;
    char     ip[INET_ADDRSTRLEN];
} client_t;

static client_t clients[MAX_CLIENTS];
static int      client_count = 0;

/* ------------------------------------------------------------------ */

static int server_init(void)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(PROTO_PORT),
    };

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    printf("[Server] Lausche auf Port %d ...\n", PROTO_PORT);
    return sockfd;
}

/* ------------------------------------------------------------------ */

static void accept_client(int server_fd)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int fd = accept(server_fd, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        perror("accept");
        return;
    }

    if (client_count >= MAX_CLIENTS) {
        fprintf(stderr, "[Server] Max Clients erreicht, lehne ab.\n");
        close(fd);
        return;
    }

    client_t *c = &clients[client_count++];
    c->fd = fd;
    c->device_id = 0;
    inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof(c->ip));

    printf("[Server] Neuer Client von %s (fd=%d)\n", c->ip, fd);
}

/* ------------------------------------------------------------------ */

static void remove_client(int idx)
{
    printf("[Server] Client %s (id=%u) getrennt.\n",
           clients[idx].ip, clients[idx].device_id);
    close(clients[idx].fd);
    clients[idx] = clients[--client_count];
}

/* ------------------------------------------------------------------ */

static void send_ack(int fd, uint8_t device_id)
{
    msg_header_t ack = {
        .magic       = PROTO_MAGIC,
        .type        = MSG_ACK,
        .device_id   = device_id,
        .payload_len = 0,
    };
    send(fd, &ack, sizeof(ack), 0);
}

/* ------------------------------------------------------------------ */

static void handle_message(int idx, msg_header_t *hdr, uint8_t *payload)
{
    client_t *c = &clients[idx];

    switch (hdr->type) {

    case MSG_REGISTER:
        c->device_id = hdr->device_id;
        printf("[Server] Geraet %u registriert (%s)\n",
               c->device_id, c->ip);
        send_ack(c->fd, c->device_id);
        break;

    case MSG_HEARTBEAT:
        printf("[Server] Heartbeat von Geraet %u\n", hdr->device_id);
        send_ack(c->fd, hdr->device_id);
        break;

    case MSG_SENSOR_DATA:
        if (hdr->payload_len >= sizeof(sensor_data_t)) {
            sensor_data_t *sd = (sensor_data_t *)payload;
            printf("[Server] Geraet %u -> Temp=%.1f C, Hum=%.1f %%\n",
                   hdr->device_id, sd->temperature, sd->humidity);
        }
        send_ack(c->fd, hdr->device_id);
        break;

    default:
        printf("[Server] Unbekannter Typ 0x%02X von Geraet %u\n",
               hdr->type, hdr->device_id);
        break;
    }
}

/* ------------------------------------------------------------------ */

static void handle_client(int idx)
{
    msg_header_t hdr;
    ssize_t n = recv(clients[idx].fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n <= 0) {
        remove_client(idx);
        return;
    }

    if (hdr.magic != PROTO_MAGIC) {
        fprintf(stderr, "[Server] Ungueltiges Magic von fd=%d\n",
                clients[idx].fd);
        remove_client(idx);
        return;
    }

    uint8_t payload[PROTO_MAX_PAYLOAD] = {0};
    if (hdr.payload_len > 0) {
        if (hdr.payload_len > PROTO_MAX_PAYLOAD) {
            fprintf(stderr, "[Server] Payload zu gross.\n");
            remove_client(idx);
            return;
        }
        n = recv(clients[idx].fd, payload, hdr.payload_len, MSG_WAITALL);
        if (n <= 0) {
            remove_client(idx);
            return;
        }
    }

    handle_message(idx, &hdr, payload);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int server_fd = server_init();
    if (server_fd < 0) return 1;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int maxfd = server_fd;

        for (int i = 0; i < client_count; i++) {
            FD_SET(clients[i].fd, &readfds);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(server_fd, &readfds))
            accept_client(server_fd);

        for (int i = client_count - 1; i >= 0; i--) {
            if (FD_ISSET(clients[i].fd, &readfds))
                handle_client(i);
        }
    }

    close(server_fd);
    return 0;
}
