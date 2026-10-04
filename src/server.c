#include "permastream.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_SIZE 4096
#define RESPONSE_SIZE 512

typedef struct {
    int fd;
    char request[REQUEST_SIZE];
    size_t received;
    char response[RESPONSE_SIZE];
    size_t response_size, sent;
    bool parsed, read_closed;
    int output;
    uint64_t cursor;
    int64_t deadline;
} Client;

int server_open(const char *address)
{
    char *host = strdup(address);
    if (!host)
        return -1;
    char *port = strrchr(host, ':');
    if (!port || !port[1]) {
        log_message("listen must be host:port or [IPv6]:port");
        free(host);
        return -1;
    }
    *port++ = '\0';
    char *name = host;
    if (*name == '[') {
        size_t length = strlen(name);
        if (length < 3 || name[length - 1] != ']') {
            free(host);
            return -1;
        }
        name[length - 1] = '\0';
        name++;
    } else if (strchr(name, ':')) {
        log_message("Enclose IPv6 listen addresses in brackets");
        free(host);
        return -1;
    }
    char *end = NULL;
    long port_number = strtol(port, &end, 10);
    if (!*port || *end || port_number < 1 || port_number > 65535) {
        log_message("listen port must be between 1 and 65535");
        free(host);
        return -1;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_PASSIVE | AI_NUMERICSERV };
    struct addrinfo *addresses = NULL;
    int ret = getaddrinfo(*name ? name : NULL, port, &hints, &addresses);
    if (ret) {
        log_message("Cannot resolve listen address: %s", gai_strerror(ret));
        free(host);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *p = addresses; p; p = p->ai_next) {
        fd = socket(p->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (!bind(fd, p->ai_addr, p->ai_addrlen) && !listen(fd, 128))
            break;
        close(fd);
        fd = -1;
    }
    if (fd < 0)
        log_message("Cannot bind %s: %s", address, strerror(errno));
    freeaddrinfo(addresses);
    free(host);
    return fd;
}

static void response(Client *client, int code, const char *reason, const char *body)
{
    client->parsed = true;
    client->output = -1;
    int n = snprintf(client->response, sizeof(client->response),
                     "HTTP/1.0 %d %s\r\nConnection: close\r\n"
                     "Content-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s",
                     code, reason, strlen(body), body);
    client->response_size = (size_t)n;
}

static void parse_request(Client *client, Output *outputs, const Config *config)
{
    char method[16], path[1024], version[16], extra;
    char *line_end = strstr(client->request, "\r\n");
    if (!line_end) {
        response(client, 400, "Bad Request", "Invalid request\n");
        return;
    }
    *line_end = '\0';
    if (sscanf(client->request, "%15s %1023s %15s %c", method, path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1"))) {
        response(client, 400, "Bad Request", "Invalid request\n");
        return;
    }
    bool head = !strcmp(method, "HEAD");
    if (strcmp(method, "GET") && !head) {
        response(client, 405, "Method Not Allowed", "Use GET or HEAD\n");
        return;
    }
    char *query = strchr(path, '?');
    if (query)
        *query = '\0';
    for (int i = 0; i < config->output_count; i++) {
        if (strcmp(path, config->outputs[i].path))
            continue;
        client->parsed = true;
        client->output = head ? -1 : i;
        int n = snprintf(client->response, sizeof(client->response),
                         "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n"
                         "Cache-Control: no-store\r\nConnection: close\r\n"
                         "icy-name: permastream\r\nicy-br: %d\r\n"
                         "Server: permastream/" VERSION "\r\n\r\n", config->outputs[i].bitrate);
        client->response_size = (size_t)n;
        pthread_mutex_lock(&outputs[i].mutex);
        client->cursor = outputs[i].end;
        pthread_mutex_unlock(&outputs[i].mutex);
        return;
    }
    response(client, 404, "Not Found", "Unknown stream path\n");
}

static int read_request(Client *client, Output *outputs, const Config *config)
{
    ssize_t size = recv(client->fd, client->request + client->received,
                        sizeof(client->request) - client->received - 1, 0);
    if (size <= 0)
        return size < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    if (memchr(client->request + client->received, '\0', (size_t)size)) {
        response(client, 400, "Bad Request", "Invalid request\n");
        return 0;
    }
    client->received += (size_t)size;
    client->request[client->received] = '\0';
    if (strstr(client->request, "\r\n\r\n"))
        parse_request(client, outputs, config);
    else if (client->received == sizeof(client->request) - 1)
        response(client, 431, "Request Header Fields Too Large", "Request too large\n");
    return 0;
}

static int write_client(Client *client, Output *outputs)
{
    if (client->sent < client->response_size) {
        ssize_t n = send(client->fd, client->response + client->sent,
                         client->response_size - client->sent, MSG_NOSIGNAL);
        if (n < 0)
            return errno == EAGAIN || errno == EINTR ? 0 : -1;
        if (!n)
            return -1;
        client->sent += (size_t)n;
        if (client->sent < client->response_size)
            return 0;
    }
    if (client->output < 0)
        return -1;
    Output *out = &outputs[client->output];
    /* Copy under the lock, then send without holding up the encoder. */
    unsigned char block[16384];
    pthread_mutex_lock(&out->mutex);
    uint64_t available = out->end - client->cursor;
    if (available > out->capacity) {
        pthread_mutex_unlock(&out->mutex);
        log_message("Dropping slow client on %s", out->config->path);
        return -1;
    }
    size_t size = available > sizeof(block) ? sizeof(block) : (size_t)available;
    size_t pos = (size_t)(client->cursor % out->capacity);
    size_t first = out->capacity - pos;
    if (first > size)
        first = size;
    memcpy(block, out->bytes + pos, first);
    memcpy(block + first, out->bytes, size - first);
    pthread_mutex_unlock(&out->mutex);
    if (!size)
        return 0;
    ssize_t n = send(client->fd, block, size, MSG_NOSIGNAL);
    if (n < 0)
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if (!n)
        return -1;
    client->cursor += (uint64_t)n;
    return 0;
}

void server_run(int listener, Output *outputs, const Config *config)
{
    Client *clients = calloc((size_t)config->max_clients, sizeof(*clients));
    struct pollfd *fds = calloc((size_t)config->max_clients + 1, sizeof(*fds));
    if (!clients || !fds) {
        log_message("Cannot allocate HTTP client buffers");
        atomic_store(&stop_requested, true);
        free(clients);
        free(fds);
        return;
    }
    for (int i = 0; i < config->max_clients; i++)
        clients[i].fd = -1;
    while (!stopping()) {
        fds[0] = (struct pollfd){ .fd = listener, .events = POLLIN };
        for (int i = 0; i < config->max_clients; i++) {
            Client *client = &clients[i];
            short events = client->read_closed ? 0 : POLLIN;
            if (client->parsed) {
                bool pending = client->sent < client->response_size;
                if (client->output >= 0) {
                    Output *out = &outputs[client->output];
                    pthread_mutex_lock(&out->mutex);
                    pending |= out->end > client->cursor;
                    bool expired = out->end - client->cursor > out->capacity;
                    pthread_mutex_unlock(&out->mutex);
                    if (client->fd >= 0 && expired) {
                        log_message("Dropping slow client on %s", out->config->path);
                        close(client->fd);
                        client->fd = -1;
                    }
                }
                if (pending)
                    events |= POLLOUT;
            }
            fds[i + 1] = (struct pollfd){ .fd = client->fd, .events = events };
        }
        int ret = poll(fds, (nfds_t)config->max_clients + 1, 20);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_message("HTTP poll failed: %s", strerror(errno));
            atomic_store(&stop_requested, true);
            break;
        }
        int64_t now = monotonic_ns();
        /* Process existing clients before accepting into vacated slots. */
        for (int i = 0; i < config->max_clients; i++) {
            Client *client = &clients[i];
            if (client->fd < 0)
                continue;
            short events = fds[i + 1].revents;
            bool close_client = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            if ((!client->parsed || client->sent < client->response_size) && now >= client->deadline)
                close_client = true;
            if (!close_client && (events & POLLIN)) {
                if (!client->parsed)
                    close_client = read_request(client, outputs, config) < 0;
                else {
                    char discard[1024];
                    ssize_t n = recv(client->fd, discard, sizeof(discard), 0);
                    /* Streaming connections do not accept pipelined requests. */
                    if (n == 0)
                        client->read_closed = true;
                    else
                        close_client = n > 0 || (errno != EAGAIN && errno != EINTR);
                }
            }
            if (!close_client && (events & POLLOUT))
                close_client = write_client(client, outputs) < 0;
            if (close_client) {
                close(client->fd);
                client->fd = -1;
            }
        }
        if (fds[0].revents & POLLIN) {
            /* Bound accept work so a connection flood cannot starve listeners. */
            for (int accepted = 0; accepted < 16; accepted++) {
                int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0)
                    break;
                int slot = -1;
                for (int i = 0; i < config->max_clients; i++)
                    if (clients[i].fd < 0) {
                        slot = i;
                        break;
                    }
                if (slot < 0) {
                    static const char busy[] = "HTTP/1.0 503 Service Unavailable\r\n"
                        "Connection: close\r\nContent-Length: 0\r\n\r\n";
                    (void)send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
                    close(fd);
                    continue;
                }
                int send_buffer = 16384;
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
                clients[slot] = (Client){ .fd = fd, .output = -1,
                    .deadline = now + INT64_C(5000000000) };
            }
        }
    }
    for (int i = 0; i < config->max_clients; i++)
        if (clients[i].fd >= 0)
            close(clients[i].fd);
    free(clients);
    free(fds);
}
