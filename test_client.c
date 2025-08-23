#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4242
#define MAX_MESSAGE_SIZE 65535

void send_message(int sock, uint8_t type, uint16_t seq, const char *payload, size_t payload_len) {
    if (payload_len > MAX_MESSAGE_SIZE - 7) {
        fprintf(stderr, "Error: Payload too large: %zu bytes (max %d)\n", payload_len, MAX_MESSAGE_SIZE - 7);
        return;
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
    if (payload && payload_len > 0) {
        memcpy(packet + 7, payload, payload_len);
    }

    printf("Sending %zu bytes: ", packet_len);
    for (size_t i = 0; i < packet_len && i < 32; i++) printf("%02x ", (unsigned char)packet[i]);
    printf("\n");

    ssize_t sent = send(sock, packet, packet_len, 0);
    if (sent != (ssize_t)packet_len) {
        fprintf(stderr, "Send failed: %s\n", strerror(errno));
    } else {
        printf("Sent %zu bytes successfully\n", sent);
    }
}

int receive_message(int sock, char *buffer, size_t buffer_size) {
    size_t buffer_pos = 0;

    // Read header (6 bytes: version, flags, seq, length)
    while (buffer_pos < 6) {
        ssize_t bytes = recv(sock, buffer + buffer_pos, buffer_size - buffer_pos, 0);
        if (bytes < 0) {
            fprintf(stderr, "Receive failed: %s\n", strerror(errno));
            return -1;
        }
        if (bytes == 0) {
            fprintf(stderr, "Connection closed by server\n");
            return -1;
        }
        buffer_pos += bytes;
        printf("Received %zd bytes for header, total %zu\n", bytes, buffer_pos);
    }

    // Validate version and length
    if (buffer[0] != 0x01) {
        fprintf(stderr, "Invalid version: %d\n", buffer[0]);
        return -1;
    }
    uint16_t payload_len = ((unsigned char)buffer[4] << 8) | (unsigned char)buffer[5];
    if (payload_len > buffer_size - 7 || payload_len == 0) {
        fprintf(stderr, "Invalid payload length: %u bytes (max %zu)\n", payload_len, buffer_size - 7);
        return -1;
    }

    // Read type (1 byte) and payload
    while (buffer_pos < 7 + payload_len) {
        ssize_t bytes = recv(sock, buffer + buffer_pos, buffer_size - buffer_pos, 0);
        if (bytes < 0) {
            fprintf(stderr, "Receive failed: %s\n", strerror(errno));
            return -1;
        }
        if (bytes == 0) {
            fprintf(stderr, "Connection closed by server\n");
            return -1;
        }
        buffer_pos += bytes;
        printf("Received %zd bytes for payload, total %zu\n", bytes, buffer_pos);
    }

    // Validate message type
    uint8_t type = buffer[6];
    if (type != 0x10 && type != 0x11 && type != 0x15) {
        fprintf(stderr, "Unexpected message type: %02x\n", type);
        return -1;
    }

    // Log received message
    printf("Received complete message (%zu bytes): ", buffer_pos);
    for (size_t i = 0; i < buffer_pos && i < 32; i++) printf("%02x ", (unsigned char)buffer[i]);
    printf("\n");

    // Validate and print payload
    if (type == 0x10) { // SYSTEM: 16-byte UUID + 2-byte len + message
        if (buffer_pos < 25) {
            fprintf(stderr, "SYSTEM message too short: %zu bytes\n", buffer_pos);
            return -1;
        }
        uint16_t msg_len = ((unsigned char)buffer[23] << 8) | (unsigned char)buffer[24];
        if (buffer_pos != 25 + msg_len) {
            fprintf(stderr, "SYSTEM message length mismatch: expected %u, got %zu\n", 25 + msg_len, buffer_pos);
            return -1;
        }
        printf("SYSTEM message content: %.*s\n", msg_len, buffer + 25);
    } else if (type == 0x11) { // BROADCAST_MESSAGE: 1-byte username len + username + 8-byte timestamp + 2-byte msg len + message
        if (buffer_pos < 19) {
            fprintf(stderr, "BROADCAST_MESSAGE too short: %zu bytes\n", buffer_pos);
            return -1;
        }
        uint8_t username_len = buffer[7];
        if (username_len == 0 || username_len > 255) {
            fprintf(stderr, "Invalid username length: %u\n", username_len);
            return -1;
        }
        uint16_t msg_len = ((unsigned char)buffer[16 + username_len] << 8) | (unsigned char)buffer[17 + username_len];
        if (buffer_pos != 18 + username_len + msg_len) {
            fprintf(stderr, "BROADCAST_MESSAGE length mismatch: expected %u, got %zu\n", 18 + username_len + msg_len, buffer_pos);
            return -1;
        }
        printf("BROADCAST_MESSAGE content: %.*s\n", msg_len, buffer + 18 + username_len);
    } else if (type == 0x15) { // ERROR: 2-byte error code + 2-byte msg len + message
        if (buffer_pos < 11) {
            fprintf(stderr, "ERROR message too short: %zu bytes\n", buffer_pos);
            return -1;
        }
        uint16_t error_code = ((unsigned char)buffer[7] << 8) | (unsigned char)buffer[8];
        uint16_t msg_len = ((unsigned char)buffer[9] << 8) | (unsigned char)buffer[10];
        if (buffer_pos != 11 + msg_len) {
            fprintf(stderr, "ERROR message length mismatch: expected %u, got %zu\n", 11 + msg_len, buffer_pos);
            return -1;
        }
        printf("ERROR message (code %u): %.*s\n", error_code, msg_len, buffer + 11);
    }

    return buffer_pos;
}

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid server address");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return 1;
    }

    // Send REGISTER message (username: "TestUser")
    char reg_payload[24] = {0}; // 16-byte zeroed connection ID, 1-byte username length, 8-byte username
    reg_payload[16] = 8; // Username length
    memcpy(reg_payload + 17, "TestUser", 8);
    send_message(sock, 0x01, 0, reg_payload, 24);

    // Receive server response
    char buffer[MAX_MESSAGE_SIZE];
    if (receive_message(sock, buffer, MAX_MESSAGE_SIZE) < 0) {
        close(sock);
        return 1;
    }

    // Send MESSAGE ("Hello, world!")
    char msg_payload[15] = {0};
    msg_payload[0] = 0; // Message length (2 bytes)
    msg_payload[1] = 13;
    memcpy(msg_payload + 2, "Hello, world!", 13);
    send_message(sock, 0x02, 1, msg_payload, 15);

    // Receive server response
    if (receive_message(sock, buffer, MAX_MESSAGE_SIZE) < 0) {
        close(sock);
        return 1;
    }

    close(sock);
    return 0;
}
