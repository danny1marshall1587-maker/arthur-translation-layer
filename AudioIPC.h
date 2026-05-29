#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>

#ifndef _WIN32
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace arthur {

// Constants for performance and capacity
constexpr uint32_t SHM_MAX_CHANNELS = 32;
constexpr uint32_t SHM_MAX_SAMPLES = 4096;

/**
 * State machine for the audio transport.
 */
enum class TransportState : uint32_t {
    STATE_IDLE = 0,
    STATE_HOST_WRITTEN = 1,
    STATE_GUEST_PROCESSED = 2,
    STATE_ERROR = 3
};

/**
 * MIDI Event structure for transport
 */
struct ShmMidiEvent {
    uint32_t sample_offset;
    uint32_t size;
    uint8_t data[16]; 
};

/**
 * The actual memory layout for Shared Memory.
 */
struct AudioSharedMemory {
    alignas(64) std::atomic<TransportState> state;
    uint32_t sample_count;
    uint32_t num_inputs;
    uint32_t num_outputs;
    double sample_rate;
    int64_t playhead_pos;

    float input_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];
    float output_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];

    uint32_t midi_in_count;
    ShmMidiEvent midi_in[256];
    uint32_t midi_out_count;
    ShmMidiEvent midi_out[256];
};

#ifndef _WIN32
/**
 * Helper to manage the SHM lifecycle on Linux.
 */
class AudioTransport {
public:
    AudioTransport() : shm_ptr_(nullptr), fd_(-1), is_owner_(false) {}
    
    ~AudioTransport() {
        detach();
    }

    bool create(const std::string& name) {
        name_ = "/" + name;
        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return false;

        if (ftruncate(fd_, sizeof(AudioSharedMemory)) < 0) return false;

        shm_ptr_ = (AudioSharedMemory*)mmap(nullptr, sizeof(AudioSharedMemory), 
                                           PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        
        if (shm_ptr_ == MAP_FAILED) {
            shm_ptr_ = nullptr;
            return false;
        }

        shm_ptr_->state.store(TransportState::STATE_IDLE);
        is_owner_ = true;
        return true;
    }

    bool attach(const std::string& name) {
        name_ = "/" + name;
        fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
        if (fd_ < 0) return false;

        shm_ptr_ = (AudioSharedMemory*)mmap(nullptr, sizeof(AudioSharedMemory), 
                                           PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        
        if (shm_ptr_ == MAP_FAILED) {
            shm_ptr_ = nullptr;
            return false;
        }
        is_owner_ = false;
        return true;
    }

    void detach() {
        if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
            munmap(shm_ptr_, sizeof(AudioSharedMemory));
            shm_ptr_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (!name_.empty()) {
            if (is_owner_) {
                shm_unlink(name_.c_str());
            }
            name_ = "";
        }
    }

    AudioSharedMemory* get() { return shm_ptr_; }

private:
    AudioSharedMemory* shm_ptr_;
    int fd_;
    std::string name_;
    bool is_owner_;
};
#endif

} // namespace arthur
