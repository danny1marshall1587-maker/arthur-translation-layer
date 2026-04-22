#include "control-protocol.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace arthur {
namespace ipc {

bool send_message(int fd, MessageType type, uint32_t request_id, uint64_t instance_id,
                  const void* payload, uint32_t payload_size) {
    MessageHeader header;
    header.type = type;
    header.payload_size = payload_size;
    header.request_id = request_id;
    header.instance_id = instance_id;

    // Send header
    ssize_t sent = 0;
    size_t total = sizeof(header);
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&header);
    while (sent < static_cast<ssize_t>(total)) {
        ssize_t n = write(fd, ptr + sent, total - sent);
        if (n <= 0) return false;
        sent += n;
    }

    // Send payload if present
    if (payload && payload_size > 0) {
        sent = 0;
        total = payload_size;
        ptr = reinterpret_cast<const uint8_t*>(payload);
        while (sent < static_cast<ssize_t>(total)) {
            ssize_t n = write(fd, ptr + sent, total - sent);
            if (n <= 0) return false;
            sent += n;
        }
    }

    return true;
}

bool recv_header(int fd, MessageHeader& header) {
    size_t received = 0;
    size_t total = sizeof(header);
    uint8_t* ptr = reinterpret_cast<uint8_t*>(&header);
    while (received < total) {
        ssize_t n = read(fd, ptr + received, total - received);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

bool recv_payload(int fd, void* buffer, uint32_t size) {
    if (size == 0) return true;
    size_t received = 0;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(buffer);
    while (received < size) {
        ssize_t n = read(fd, ptr + received, size - received);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

} // namespace ipc
} // namespace arthur
