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

    // Phase 2: Audio
    REQUEST_SETUP_PROCESSING = 0x0101,
    RESPONSE_SETUP_PROCESSING= 0x0102,
    REQUEST_SET_BUS_CONFIG   = 0x0103,
    RESPONSE_SET_BUS_CONFIG  = 0x0104,
    REQUEST_SET_STATE        = 0x0105, // Active/Inactive
    RESPONSE_SET_STATE       = 0x0106,
    REQUEST_SET_BYPASS       = 0x0107,
    RESPONSE_SET_BYPASS      = 0x0108,
    REQUEST_PROCESS_AUDIO    = 0x0109, // Manual trigger if not using SHM state

    // Phase 3: GUI
    REQUEST_OPEN_EDITOR      = 0x0201,
    RESPONSE_OPEN_EDITOR     = 0x0202,
    REQUEST_CLOSE_EDITOR     = 0x0203,
    RESPONSE_CLOSE_EDITOR    = 0x0204,
    REQUEST_ATTACH_WINDOW    = 0x0205, // Pass Wayland handle or X11 ID
    RESPONSE_ATTACH_WINDOW   = 0x0206,
    NOTIFY_WINDOW_RESIZE     = 0x0207,
    NOTIFY_WINDOW_CLOSE      = 0x0208,

    // Lifecycle
    GUEST_READY             = 0x0010,
    SHUTDOWN                = 0x00FF,
    ERROR_RESPONSE          = 0xFFFF,
};

// --- Message Header ---

struct MessageHeader {
    MessageType type;
    uint32_t payload_size;
    uint32_t request_id;
    uint64_t instance_id;
};

// --- Phase 1 Payloads ---
// ... (omitting unchanged for brevity but I will include them in final write)

/**
 * Request for REQUEST_SETUP_PROCESSING.
 * Configures the processing environment and tells the Guest about the SHM region.
 */
struct SetupProcessingRequest {
    double sample_rate;
    int32_t max_block_size;
    int32_t process_mode;      // kRealtime, kPrefetch, etc.
    char shm_name[64];         // Name of the /dev/shm region
};

/**
 * Request for REQUEST_SET_BUS_CONFIG.
 */
struct SetBusConfigRequest {
    int32_t num_inputs;
    int32_t num_outputs;
    uint64_t input_arrangements[8];
    uint64_t output_arrangements[8];
};

/**
 * Request for REQUEST_SET_STATE.
 */
struct SetStateRequest {
    bool state;                // true = active, false = inactive
};

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

// --- Phase 3 Payloads ---

struct OpenEditorRequest {
    // No payload needed initially, just triggers opening
};

struct OpenEditorResponse {
    int32_t width;
    int32_t height;
};

struct AttachWindowRequest {
    char handle[256];          // Wayland handle (e.g. "wayland:...") or X11 window ID
};

struct WindowResizeNotification {
    int32_t width;
    int32_t height;
};

// --- Helpers ---

/**
 * Send a complete message (header + payload) over a file descriptor.
 * Returns true on success.
 */
bool send_message(int fd, MessageType type, uint32_t request_id, uint64_t instance_id,
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
