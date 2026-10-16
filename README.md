# Tundra Chat Protocol

A high-performance, enterprise-grade chat protocol implementation with advanced features for real-time communication, AI agent coordination, and scalable messaging systems.

## 🌟 Overview

The Tundra Chat Protocol is a production-ready communication system that goes far beyond simple chat functionality. It's designed for real-time applications requiring reliable message delivery, session management, and AI agent integration.

## 🚀 Key Features

### Advanced Protocol Design
- **Custom Binary Protocol**: Sophisticated 7-byte header format with version, flags, sequence numbers, and length encoding
- **High Efficiency**: More efficient than HTTP/JSON-based chat systems
- **12+ Message Types**: Comprehensive message type system for various communication needs

### Enterprise-Grade Session Management
- **UUID-based Connection Tracking**: Unique identifier for each session
- **Session Resume**: reconnect with your session id to keep your name and receive buffered messages
- **Session Timeout Handling**: idle connections dropped after 2 minutes, detached sessions held for 2 minutes (both settable with `-i` / `-g`)
- **Username Conflict Detection**: Prevents duplicate usernames across sessions

### Message Reliability
- **Message Buffering**: Stores up to 50 messages per disconnected user
- **Offline Message Delivery**: Delivers buffered messages on reconnection
- **Bounded Backlog**: a disconnected session buffers up to 50 messages / 1 MiB (64 MiB across all sessions); when it overflows the oldest are dropped and the client is told how many on resume. There are no end-to-end ACKs yet, and a client disconnected for falling behind loses whatever was still queued for it

### Health Monitoring & Performance
- **Built-in Ping/Pong**: 30-second interval heartbeat mechanism
- **Latency Tracking**: Real-time latency monitoring and reporting
- **Connection Statistics**: connection uptime and message counts
- **Health Check System**: Comprehensive connection health reporting

### High-Performance Architecture
- **Epoll-based I/O**: Linux epoll for scalable event-driven networking
- **Up to 1000 Clients**: fixed-size connection and session tables
- **Non-blocking Sockets**: Prevents server blocking on slow clients
- **Efficient Broadcasting**: Optimized message distribution system

### Robust Error Handling
- **Comprehensive Error Codes**: Detailed error classification system
- **Protocol Validation**: Strict message format validation
- **Graceful Degradation**: Continues operation despite individual client failures

### AI Agent Integration
- **Specialized Agent Commands**: Dedicated message types for AI interactions
- **Agent Command Broadcasting**: Efficient AI agent coordination
- **Agent Communication Protocol**: Built-in support for agent-to-agent communication

## 📋 Message Types

| Type | Code | Description |
|------|------|-------------|
| `MSG_REGISTER` | 0x01 | Client registration with username |
| `MSG_MESSAGE` | 0x02 | Standard chat message |
| `MSG_AGENT_COMMAND` | 0x03 | AI agent command |
| `MSG_PONG` | 0x04 | Heartbeat response |
| `MSG_HEALTH_CHECK` | 0x05 | Health status request |
| `MSG_SYSTEM` | 0x10 | System notification |
| `MSG_BROADCAST_MESSAGE` | 0x11 | Broadcast chat message |
| `MSG_BROADCAST_AGENT` | 0x12 | Broadcast agent command |
| `MSG_IMAGE` | 0x13 | Reserved, not implemented |
| `MSG_PING` | 0x14 | Heartbeat request |
| `MSG_ERROR` | 0x15 | Error message |
| `MSG_HEALTH_RESULT` | 0x16 | Health status response |

## 📐 Protocol Specification

`tundra_protocol.h` is the source of truth; this section mirrors it.

### Frame Format
```
[Version:1][Flags:1][Sequence:2][Length:2][Type:1][Payload:Length]
```

- All integers are big-endian.
- **Version**: `0x01`. Frames with any other version get an error and the connection is closed.
- **Flags**: reserved, send `0` (currently ignored by the server).
- **Sequence**: replies (`MSG_SYSTEM` welcome, `MSG_ERROR`, `MSG_HEALTH_RESULT`) echo the sequence number of the request they answer. Frames the server pushes on its own (broadcasts, notices, pings) use a per-session counter.
- **Length**: number of payload bytes *after* the 7-byte header. A frame is always exactly `7 + Length` bytes.
- **Payload**: at most 65535 bytes. Chat text is limited to 65492 bytes so it fits in a broadcast frame.

### Payloads

| Type | Payload |
|------|---------|
| `MSG_REGISTER` | `id:16` `ulen:1` `username:ulen`. Use an all-zero `id` for a new session, or the id from a previous welcome to resume. Usernames are 1 to 32 printable ASCII characters, no spaces. |
| `MSG_MESSAGE` | `mlen:2` `text:mlen` |
| `MSG_AGENT_COMMAND` | `alen:1` `agent:alen` `clen:2` `command:clen` |
| `MSG_PONG` | echo of the `MSG_PING` payload. Required: connections that send nothing for 2 minutes are dropped, and latency is computed from the echoed timestamp. |
| `MSG_HEALTH_CHECK` | empty |
| `MSG_SYSTEM` | `id:16` `mlen:2` `text:mlen`. `id` is your session id in the welcome, all zeros in notices (joins and leaves). |
| `MSG_BROADCAST_MESSAGE` | `ulen:1` `username:ulen` `timestamp_ms:8` `mlen:2` `text:mlen` |
| `MSG_BROADCAST_AGENT` | same as `MSG_BROADCAST_MESSAGE`, text is `"<agent> <command>"` |
| `MSG_PING` | `timestamp_ms:8` |
| `MSG_ERROR` | `code:2` `mlen:2` `text:mlen` |
| `MSG_HEALTH_RESULT` | `timestamp_ms:8` `status:1` `latency_ms:4` `uptime_ms:4` `sent:4` `received:4` |

Every length field is validated against the frame; payloads with missing or trailing bytes are rejected with `ERR_INVALID_FORMAT`.

### Session Lifecycle
1. Register with a zero id. The welcome `MSG_SYSTEM` carries your session id.
2. If the connection drops, the session is held for 2 minutes and messages for it are buffered (up to 50, oldest dropped first).
3. Reconnect and register with the same id and username to resume; the backlog is delivered right after the welcome, paced so a large backlog never overflows the connection. If the old connection is still open, it is replaced. An unknown or expired id, or a different username, gets `ERR_SESSION_EXPIRED`; register again with a zero id.
4. After the hold period, the session is released, its username becomes available, and other users get a "has left the chat" notice. If the session table is full, the longest-detached session is released early to make room.
5. A new connection must register within 10 seconds.

### Error Codes
- `ERR_USERNAME_TAKEN` (0x0001): Username already in use (connection is closed)
- `ERR_INVALID_FORMAT` (0x0002): Malformed message
- `ERR_SESSION_EXPIRED` (0x0003): resume with an unknown or expired session id
- `ERR_SERVER_ERROR` (0x0004): Internal server error, or server full
- `ERR_NOT_REGISTERED` (0x0005): Message sent before registering

## 🏗️ Architecture

### Server Architecture
- **Event-driven Design**: Uses Linux epoll for high concurrency
- **Session Management**: Maintains client sessions with UUID tracking
- **Message Broadcasting**: Efficient multi-client message distribution
- **Automatic Cleanup**: Periodic cleanup of inactive sessions
- **Resource Management**: Proper memory management and cleanup

### Client Architecture
- **Protocol Compliance**: Implements full protocol specification
- **Message Validation**: Comprehensive message format validation
- **Error Handling**: Robust error detection and reporting
- **Connection Management**: Handles connection lifecycle

## 🚀 Getting Started

### Building
```bash
sudo apt-get install uuid-dev   # libuuid headers
make                            # builds build/tundra_chat_server and build/tundra_client
make test                       # end-to-end tests
make asan && ./tests/integration.sh   # same tests under AddressSanitizer + UBSan
```

### Running the Server
```bash
./build/tundra_chat_server [-p port] [-i idle-timeout-s] [-g resume-grace-s] [-v]
```
Listens on port 4242 by default. `-v` logs every frame.

### Running the Client
```bash
# listen for 30 seconds
./build/tundra_client -u alice -w 30

# send two messages
./build/tundra_client -u bob -m "hello" -m "anyone here?"

# resume a previous session with the id printed on the SESSION line
./build/tundra_client -u alice -r 1b4e28ba-2fa1-11d2-883f-0016d3cca427 -w 10
```
The client prints one line per frame (`SESSION`, `SYSTEM`, `MSG`, `AGENT`, `ERROR`, `HEALTH`, `PING`) and answers pings automatically.

## 📖 Usage Examples

### Basic Client Connection
1. Client connects to server on port 4242
2. Sends `MSG_REGISTER` with username
3. Receives welcome `MSG_SYSTEM` message
4. Can send `MSG_MESSAGE` for chat
5. Receives `MSG_BROADCAST_MESSAGE` from other clients

### AI Agent Commands
```c
// Send agent command
send_message(sock, MSG_AGENT_COMMAND, seq, agent_payload, payload_len);
```

### Health Monitoring
```c
// Request health check
send_message(sock, MSG_HEALTH_CHECK, seq, NULL, 0);
// Receive MSG_HEALTH_RESULT with metrics
```

## 🔒 Security Features

- **Connection Validation**: Strict protocol compliance checking
- **Resource Limits**: Maximum message sizes and client limits
- **Session Security**: UUID-based session identification
- **Input Sanitization**: Comprehensive input validation

## 📊 Performance Characteristics

- **Concurrent Connections**: up to 1000 (connections and held sessions share the limit)
- **Memory Efficiency**: Optimized memory usage with proper cleanup

## 🛠️ Configuration

### Server Configuration
- `PORT`: 4242 (default)
- `MAX_CLIENTS`: 1000
- Maximum payload: 65535 bytes (chat text: 65492)
- `IDLE_TIMEOUT`: 120 seconds without any frame before a connection is dropped
- `RESUME_GRACE`: 120 seconds a disconnected session is held
- `PING_INTERVAL`: 30 seconds
- `MAX_BUFFERED_MESSAGES`: 50
- `MAX_OUTBOUND_BYTES`: 1 MiB queued per client; slower clients are disconnected

### Dependencies
- Linux (epoll support required)
- libuuid-dev (for UUID generation)
- Standard C libraries

## 🤝 Use Cases

### Real-time Chat Applications
- High-performance chat servers
- Gaming communication systems
- Collaborative platforms

### AI Agent Coordination
- Multi-agent AI systems
- Distributed AI processing
- Agent command and control

### Enterprise Messaging
- Internal communication systems
- Microservice communication
- Event-driven architectures

### IoT and Monitoring
- Device communication
- Real-time monitoring systems
- Alert and notification systems

## 📈 Monitoring and Metrics

The server provides comprehensive metrics:
- Connection counts and status
- Message throughput and latency
- Error rates and types
- Session duration and activity
- Resource utilization

## 🔧 Development

### Adding New Message Types
1. Define new message type constant
2. Implement message handling in `process_frame()`
3. Add payload validation logic
4. Update client to support new message type

### Extending Features
- Custom authentication mechanisms
- Encryption and security layers
- Database integration for persistence
- Load balancing and clustering

## 📄 License

This is a production-grade communication protocol designed for enterprise use. See license file for details.

---

**Tundra Chat Protocol** - Where performance meets reliability in real-time communication.
