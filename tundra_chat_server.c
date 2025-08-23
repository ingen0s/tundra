#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>
#include <uuid/uuid.h>
#include <zlib.h>

#define PORT 4242
#define MAX_CLIENTS 1000
#define MAX_MESSAGE_SIZE 65535
#define MAX_USERNAME_LEN 32
#define SESSION_TIMEOUT 120 // 2 minutes
#define PING_INTERVAL 30 // 30 seconds
#define MAX_BUFFERED_MESSAGES 50

// Message types
enum MessageType {
    MSG_REGISTER = 0x01,
    MSG_MESSAGE = 0x02,
    MSG_AGENT_COMMAND = 0x03,
    MSG_PONG = 0x04,
    MSG_HEALTH_CHECK = 0x05,
    MSG_SYSTEM = 0x10,
    MSG_BROADCAST_MESSAGE = 0x11,
    MSG_BROADCAST_AGENT = 0x12,
    MSG_IMAGE = 0x13,
    MSG_PING = 0x14,
    MSG_ERROR = 0x15,
    MSG_HEALTH_RESULT = 0x16
};

// Error codes
enum ErrorCode {
    ERR_USERNAME_TAKEN = 0x0001,
    ERR_INVALID_FORMAT = 0x0002,
    ERR_SESSION_EXPIRED = 0x0003,
    ERR_SERVER_ERROR = 0x0004
};

// Session structure
typedef struct {
    uuid_t connection_id;
    char username[MAX_USERNAME_LEN + 1];
    time_t connected_at;
    time_t last_seen;
    int active;
    char *message_buffer[MAX_BUFFERED_MESSAGES];
    size_t buffer_sizes[MAX_BUFFERED_MESSAGES];
    int buffer_count;
    int fd; // Associated socket
    uint16_t last_seq; // Last sequence number
    time_t last_ping;
    time_t last_pong;
    uint32_t latency;
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t errors;
} Session;

// Client structure
typedef struct {
    int fd;
    Session *session;
    char buffer[MAX_MESSAGE_SIZE];
    size_t buffer_pos;
} Client;

// Global state
Client clients[MAX_CLIENTS];
Session sessions[MAX_CLIENTS];
int session_count = 0;
int epoll_fd;
int server_fd;

// Utility to send a message
int send_message(int fd, uint8_t type, uint16_t seq, const char *payload, size_t payload_len) {
    if (payload_len > MAX_MESSAGE_SIZE - 7) {
        fprintf(stderr, "Error: Payload too large: %zu bytes\n", payload_len);
        return -1;
    }

    char packet[MAX_MESSAGE_SIZE];
    size_t packet_len = 7 + payload_len; // Header (4) + Length (2) + Type (1) + Payload
    packet[0] = 0x01; // Version
    packet[1] = 0x00; // Flags (TCP, no compression)
    packet[2] = (seq >> 8) & 0xFF; // Sequence number
    packet[3] = seq & 0xFF;
    packet[4] = (payload_len + 1) >> 8; // Length (Type + Payload)
    packet[5] = (payload_len + 1) & 0xFF;
    packet[6] = type;
    memcpy(packet + 7, payload, payload_len);

    printf("Sending to fd %d: %zu bytes: ", fd, packet_len);
    for (size_t i = 0; i < packet_len && i < 32; i++) printf("%02x ", (unsigned char)packet[i]);
    printf("\n");

    ssize_t sent = send(fd, packet, packet_len, MSG_NOSIGNAL);
    if (sent != (ssize_t)packet_len) {
        fprintf(stderr, "Send failed on fd %d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

// Send error message
void send_error(int fd, uint16_t seq, uint16_t error_code, const char *msg) {
    char payload[256];
    size_t msg_len = strlen(msg);
    if (msg_len > 252) msg_len = 252; // Prevent overflow
    payload[0] = (error_code >> 8) & 0xFF;
    payload[1] = error_code & 0xFF;
    payload[2] = (msg_len >> 8) & 0xFF;
    payload[3] = msg_len & 0xFF;
    memcpy(payload + 4, msg, msg_len);
    send_message(fd, MSG_ERROR, seq, payload, 4 + msg_len);
}

// Send system message
void send_system(int fd, uint16_t seq, const uuid_t conn_id, const char *msg) {
    char payload[256];
    size_t msg_len = strlen(msg);
    if (msg_len > 238) msg_len = 238; // Prevent overflow
    memcpy(payload, conn_id, 16);
    payload[16] = (msg_len >> 8) & 0xFF;
    payload[17] = msg_len & 0xFF;
    memcpy(payload + 18, msg, msg_len);
    send_message(fd, MSG_SYSTEM, seq, payload, 18 + msg_len);
}

// Broadcast message (excluding sender)
void broadcast_message(uint8_t type, const char *sender, const char *payload, size_t payload_len, int sender_fd) {
    char packet[MAX_MESSAGE_SIZE];
    size_t sender_len = strlen(sender);
    if (sender_len > 255) sender_len = 255; // Prevent overflow
    uint64_t timestamp = (uint64_t)time(NULL) * 1000;
    
    size_t packet_len = 7 + sender_len + 1 + 8 + 2 + payload_len;
    if (packet_len > MAX_MESSAGE_SIZE) {
        fprintf(stderr, "Broadcast message too large: %zu bytes\n", packet_len);
        return;
    }

    packet[0] = 0x01;
    packet[1] = 0x00;
    packet[2] = 0x00; // Sequence number handled per client
    packet[3] = 0x00;
    packet[4] = (payload_len + 11 + sender_len) >> 8;
    packet[5] = (payload_len + 11 + sender_len) & 0xFF;
    packet[6] = type;
    packet[7] = sender_len;
    memcpy(packet + 8, sender, sender_len);
    for (int i = 0; i < 8; i++) packet[8 + sender_len + i] = (timestamp >> ((7 - i) * 8)) & 0xFF;
    packet[16 + sender_len] = (payload_len >> 8) & 0xFF;
    packet[17 + sender_len] = payload_len & 0xFF;
    memcpy(packet + 18 + sender_len, payload, payload_len);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].fd != sender_fd && clients[i].session && clients[i].session->active) {
            packet[2] = (clients[i].session->last_seq >> 8) & 0xFF;
            packet[3] = clients[i].session->last_seq & 0xFF;
            clients[i].session->last_seq++;
            clients[i].session->messages_sent++;
            send(clients[i].fd, packet, packet_len, MSG_NOSIGNAL);
            
            // Buffer for disconnected clients
            for (int j = 0; j < session_count; j++) {
                if (!sessions[j].active && memcmp(sessions[j].username, sender, sender_len) != 0) {
                    if (sessions[j].buffer_count < MAX_BUFFERED_MESSAGES) {
                        sessions[j].message_buffer[sessions[j].buffer_count] = malloc(packet_len);
                        if (!sessions[j].message_buffer[sessions[j].buffer_count]) {
                            fprintf(stderr, "Memory allocation failed for buffer\n");
                            continue;
                        }
                        memcpy(sessions[j].message_buffer[sessions[j].buffer_count], packet, packet_len);
                        sessions[j].buffer_sizes[sessions[j].buffer_count] = packet_len;
                        sessions[j].buffer_count++;
                    }
                }
            }
        }
    }
}

// Process incoming message
void process_message(Client *client, const char *data, size_t len) {
    if (len < 7) {
        send_error(client->fd, 0, ERR_INVALID_FORMAT, "Message too short");
        return;
    }

    uint8_t version = data[0];
    uint8_t flags = data[1];
    uint16_t seq = (data[2] << 8) | data[3];
    uint16_t payload_len = (data[4] << 8) | data[5];
    uint8_t type = data[6];

    if (version != 0x01 || payload_len + 7 > len || payload_len > MAX_MESSAGE_SIZE - 7) {
        send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid version or length");
        return;
    }

    if (!client->session) {
        client->session = &sessions[session_count]; // Temporary assignment for validation
    }
    client->session->messages_received++;
    client->session->last_seen = time(NULL);

    switch (type) {
        case MSG_REGISTER: {
            if (payload_len < 17) {
                send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid register payload");
                return;
            }
            uuid_t conn_id;
            memcpy(conn_id, data + 7, 16);
            uint8_t username_len = data[23];
            if (username_len > MAX_USERNAME_LEN || payload_len < 17 + username_len) {
                send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid username length");
                return;
            }

            char username[MAX_USERNAME_LEN + 1];
            memcpy(username, data + 24, username_len);
            username[username_len] = '\0';

            // Check for existing session (reconnection)
            int session_idx = -1;
            for (int i = 0; i < session_count; i++) {
                if (uuid_compare(sessions[i].connection_id, conn_id) == 0 && sessions[i].active) {
                    session_idx = i;
                    break;
                }
            }

            // Check for username conflict
            int name_taken = 0;
            for (int i = 0; i < session_count; i++) {
                if (sessions[i].active && strcmp(sessions[i].username, username) == 0 && session_idx != i) {
                    name_taken = 1;
                    break;
                }
            }

            if (name_taken && session_idx == -1) {
                send_error(client->fd, seq, ERR_USERNAME_TAKEN, "Username already taken");
                close(client->fd);
                return;
            }

            // Create or update session
            if (session_idx == -1) {
                session_idx = session_count++;
                uuid_generate(sessions[session_idx].connection_id);
                sessions[session_idx].connected_at = time(NULL);
                sessions[session_idx].buffer_count = 0;
            }
            sessions[session_idx].active = 1;
            sessions[session_idx].last_seen = time(NULL);
            sessions[session_idx].fd = client->fd;
            strncpy(sessions[session_idx].username, username, MAX_USERNAME_LEN);
            sessions[session_idx].username[MAX_USERNAME_LEN] = '\0';
            client->session = &sessions[session_idx];

            // Send welcome and buffered messages
            char welcome[64];
            snprintf(welcome, sizeof(welcome), "Welcome, %s!", username);
            send_system(client->fd, seq, sessions[session_idx].connection_id, welcome);

            for (int i = 0; i < sessions[session_idx].buffer_count; i++) {
                send(client->fd, sessions[session_idx].message_buffer[i], sessions[session_idx].buffer_sizes[i], MSG_NOSIGNAL);
                free(sessions[session_idx].message_buffer[i]);
            }
            sessions[session_idx].buffer_count = 0;
            break;
        }

        case MSG_MESSAGE: {
            if (!client->session || !client->session->active) {
                send_error(client->fd, seq, ERR_SESSION_EXPIRED, "Not registered");
                return;
            }
            if (payload_len < 3) {
                send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid message payload");
                return;
            }
            uint16_t msg_len = (data[7] << 8) | data[8];
            if (msg_len + 9 > payload_len) {
                send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid message length");
                return;
            }
            broadcast_message(MSG_BROADCAST_MESSAGE, client->session->username, data + 9, msg_len, client->fd);
            break;
        }

        case MSG_AGENT_COMMAND: {
            if (!client->session || !client->session->active) {
                send_error(client->fd, seq, ERR_SESSION_EXPIRED, "Not registered");
                return;
            }
            if (payload_len < 4) {
                send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid agent command payload");
                return;
            }
            uint8_t agent_len = data[7];
            uint16_t cmd_len = (data[8 + agent_len] << 8) | data[9 + agent_len];
            if (payload_len < 4 + agent_len + cmd_len) {
                send_error(client->fd, seq, ERR_INVALID_FORMAT, "Invalid agent command length");
                return;
            }
            char agent[256], cmd[256];
            memcpy(agent, data + 8, agent_len);
            agent[agent_len] = '\0';
            memcpy(cmd, data + 10 + agent_len, cmd_len);
            cmd[cmd_len] = '\0';

            char payload[512];
            size_t payload_len = snprintf(payload, sizeof(payload), "%s %s", agent, cmd);
            broadcast_message(MSG_BROADCAST_AGENT, client->session->username, payload, payload_len, client->fd);
            break;
        }

        case MSG_PONG: {
            if (!client->session || !client->session->active) return;
            client->session->last_pong = time(NULL);
            if (client->session->last_ping) {
                client->session->latency = (client->session->last_pong - client->session->last_ping) * 1000;
            }
            break;
        }

        case MSG_HEALTH_CHECK: {
            if (!client->session || !client->session->active) return;
            char payload[25];
            uint64_t timestamp = (uint64_t)time(NULL) * 1000;
            for (int i = 0; i < 8; i++) payload[i] = (timestamp >> ((7 - i) * 8)) & 0xFF;
            payload[8] = 0x01; // Connected
            for (int i = 0; i < 4; i++) payload[9 + i] = (client->session->latency >> ((3 - i) * 8)) & 0xFF;
            for (int i = 0; i < 4; i++) payload[13 + i] = ((uint32_t)(time(NULL) - client->session->connected_at) * 1000 >> ((3 - i) * 8)) & 0xFF;
            for (int i = 0; i < 4; i++) payload[17 + i] = (client->session->messages_sent >> ((3 - i) * 8)) & 0xFF;
            for (int i = 0; i < 4; i++) payload[21 + i] = (client->session->messages_received >> ((3 - i) * 8)) & 0xFF;
            send_message(client->fd, MSG_HEALTH_RESULT, seq, payload, 25);
            break;
        }

        default:
            send_error(client->fd, seq, ERR_INVALID_FORMAT, "Unknown message type");
            break;
    }
}

// Cleanup inactive sessions
void cleanup_sessions() {
    time_t now = time(NULL);
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active && now - sessions[i].last_seen > SESSION_TIMEOUT) {
            sessions[i].active = 0;
            close(sessions[i].fd);
            for (int j = 0; j < sessions[i].buffer_count; j++) free(sessions[i].message_buffer[j]);
            sessions[i].buffer_count = 0;
            char msg[64];
            snprintf(msg, sizeof(msg), "%s has left the chat.", sessions[i].username);
            broadcast_message(MSG_SYSTEM, "Server", msg, strlen(msg), -1);
        }
    }
}

// Send pings
void send_pings() {
    char payload[8];
    uint64_t timestamp = (uint64_t)time(NULL) * 1000;
    for (int i = 0; i < 8; i++) payload[i] = (timestamp >> ((7 - i) * 8)) & 0xFF;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].session && clients[i].session->active) {
            clients[i].session->last_ping = time(NULL);
            clients[i].session->last_seq++;
            send_message(clients[i].fd, MSG_PING, clients[i].session->last_seq, payload, 8);
        }
    }
}

int main() {
    // Initialize clients and sessions
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].session = NULL;
        clients[i].buffer_pos = 0;
    }
    session_count = 0;

    // Create server socket
    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    // Set SO_REUSEADDR to avoid "Address already in use"
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("Listen failed");
        close(server_fd);
        return 1;
    }

    // Create epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("Epoll creation failed");
        close(server_fd);
        return 1;
    }

    struct epoll_event ev, events[32];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("Epoll add server failed");
        close(server_fd);
        close(epoll_fd);
        return 1;
    }

    time_t last_cleanup = time(NULL);
    time_t last_ping = time(NULL);

    printf("Server listening on port %d\n", PORT);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, 32, 1000);
        time_t now = time(NULL);

        // Periodic cleanup and pings
        if (now - last_cleanup >= 10) {
            cleanup_sessions();
            last_cleanup = now;
        }
        if (now - last_ping >= PING_INTERVAL) {
            send_pings();
            last_ping = now;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
                if (client_fd < 0) {
                    fprintf(stderr, "Accept failed: %s\n", strerror(errno));
                    continue;
                }

                // Add client to epoll
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    fprintf(stderr, "Epoll add client failed: %s\n", strerror(errno));
                    close(client_fd);
                    continue;
                }

                // Find free client slot
                int client_idx = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (clients[j].fd == -1) {
                        client_idx = j;
                        break;
                    }
                }
                if (client_idx == -1) {
                    fprintf(stderr, "No free client slots\n");
                    close(client_fd);
                    continue;
                }

                clients[client_idx].fd = client_fd;
                clients[client_idx].session = NULL;
                clients[client_idx].buffer_pos = 0;
                printf("Client connected on fd %d\n", client_fd);
            } else {
                int fd = events[i].data.fd;
                int client_idx = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (clients[j].fd == fd) {
                        client_idx = j;
                        break;
                    }
                }
                if (client_idx == -1) continue;

                Client *client = &clients[client_idx];
                ssize_t bytes_read = recv(fd, client->buffer + client->buffer_pos, MAX_MESSAGE_SIZE - client->buffer_pos, 0);
                if (bytes_read <= 0) {
                    if (client->session) client->session->active = 0;
                    close(fd);
                    clients[client_idx].fd = -1;
                    clients[client_idx].session = NULL;
                    clients[client_idx].buffer_pos = 0;
                    printf("Client disconnected on fd %d\n", fd);
                    continue;
                }

                client->buffer_pos += bytes_read;
                printf("Received %zd bytes from fd %d, total %zu\n", bytes_read, fd, client->buffer_pos);

                while (client->buffer_pos >= 6) {
                    uint16_t payload_len = (client->buffer[4] << 8) | client->buffer[5];
                    if (client->buffer_pos < 7 + payload_len) break;

                    process_message(client, client->buffer, 7 + payload_len);
                    memmove(client->buffer, client->buffer + 7 + payload_len, client->buffer_pos - (7 + payload_len));
                    client->buffer_pos -= 7 + payload_len;
                }
            }
        }
    }

    close(epoll_fd);
    close(server_fd);
    return 0;
}
