#pragma once

#include <cstdint>
#include <cstring>

namespace arthur {
namespace ipc {

/**
 * Arthur IPC Control Protocol
 * 
 * Messages are exchanged between the Linux bridge (.so) and the Wine guest (.exe)
 * over a Unix domain socket. Each message has a fixed-size header followed by
 * a type-specific payload.
 * 
 * Phase 1: Factory queries only (enough to pass a DAW scan).
 * Phase 2: Audio processing messages.
 * Phase 3: GUI messages.
 * Phase 4: ARA2 messages.
 */

// --- Message Types ---

enum class MessageType : uint32_t {
    // Phase 1: Scan
    REQUEST_FACTORY_INFO    = 0x0001,
    RESPONSE_FACTORY_INFO   = 0x0002,
    REQUEST_CLASS_INFO      = 0x0003,
    RESPONSE_CLASS_INFO     = 0x0004,
    REQUEST_CREATE_INSTANCE = 0x0005,
    RESPONSE_CREATE_INSTANCE= 0x0006,
    REQUEST_DESTROY_INSTANCE= 0x0007,
    RESPONSE_DESTROY_INSTANCE=0x0008,

    // Lifecycle
    GUEST_READY             = 0x0010,
    SHUTDOWN                = 0x00FF,
    ERROR_RESPONSE          = 0xFFFF,
};

// --- Message Header ---

struct MessageHeader {
    MessageType type;
    uint32_t payload_size;  // Size of the payload that follows this header
    uint32_t request_id;    // For matching responses to requests
};

// --- Phase 1 Payloads ---

/**
 * Response to REQUEST_FACTORY_INFO.
 * Contains the vendor info and number of plugin classes in this module.
 */
struct FactoryInfoResponse {
    char vendor[128];
    char url[128];
    char email[128];
    int32_t num_classes;
};

/**
 * Request for REQUEST_CLASS_INFO.
 * Ask for info about a specific class by index.
 */
struct ClassInfoRequest {
    int32_t index;
};

/**
 * Response to REQUEST_CLASS_INFO.
 * Contains the plugin's identity: name, category, and unique Component ID.
 */
struct ClassInfoResponse {
    uint8_t cid[16];          // 128-bit Steinberg TUID
    char name[128];
    char category[64];
    int32_t cardinality;       // kManyInstances = 0x7FFFFFFF
};

/**
 * Request for REQUEST_CREATE_INSTANCE.
 * Create an instance of a specific class.
 */
struct CreateInstanceRequest {
    uint8_t cid[16];           // Which class to instantiate
    uint8_t iid[16];           // Which interface to query for
};

/**
 * Response to REQUEST_CREATE_INSTANCE.
 * Returns a handle to the Wine-side instance.
 */
struct CreateInstanceResponse {
    uint64_t instance_id;      // Opaque handle to the Wine-side object
    int32_t result;            // Steinberg::tresult
};

// --- Helpers ---

/**
 * Send a complete message (header + payload) over a file descriptor.
 * Returns true on success.
 */
bool send_message(int fd, MessageType type, uint32_t request_id,
                  const void* payload, uint32_t payload_size);

/**
 * Receive a message header. Returns true on success.
 */
bool recv_header(int fd, MessageHeader& header);

/**
 * Receive a payload of exactly `size` bytes. Returns true on success.
 */
bool recv_payload(int fd, void* buffer, uint32_t size);

} // namespace ipc
} // namespace arthur
