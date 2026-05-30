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
    alignas(64) volatile std::atomic<TransportState> state;
    volatile uint32_t sample_count;
    volatile uint32_t num_inputs;
    volatile uint32_t num_outputs;
    volatile double sample_rate;
    volatile int64_t playhead_pos;

    float input_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];
    float output_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];

    // Hybrid playback pre-rendered buffer (for ASIO-Guard style)
    float pre_rendered_playback[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];
    volatile std::atomic<uint64_t> playback_ring_start_time_ns;
    volatile std::atomic<int64_t> playback_ring_start_pos;
    volatile std::atomic<uint32_t> playback_ring_write_len;

    volatile uint32_t midi_in_count;
    ShmMidiEvent midi_in[256];
    volatile uint32_t midi_out_count;
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

#ifndef _WIN32
#include <pthread.h>
#include <time.h>

inline uint64_t get_monotonic_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

inline void write_pre_rendered_playback(AudioSharedMemory* layout, uint32_t channel, uint32_t offset, const float* data, uint32_t count, uint64_t block_start_time_ns, int64_t block_start_pos) {
    if (channel >= SHM_MAX_CHANNELS) return;
    
    uint32_t target_idx = offset % SHM_MAX_SAMPLES;
    for (uint32_t i = 0; i < count; ++i) {
        layout->pre_rendered_playback[channel][(target_idx + i) % SHM_MAX_SAMPLES] = data[i];
    }
    
    if (offset == 0) {
        layout->playback_ring_start_time_ns.store(block_start_time_ns, std::memory_order_release);
        layout->playback_ring_start_pos.store(block_start_pos, std::memory_order_release);
        layout->playback_ring_write_len.store(count, std::memory_order_release);
    } else {
        uint32_t current_len = layout->playback_ring_write_len.load(std::memory_order_acquire);
        layout->playback_ring_write_len.store(current_len + count, std::memory_order_release);
    }
}

inline void sum_aligned_playback(AudioSharedMemory* layout, uint32_t channel, float* output_buffer, uint32_t count, uint64_t realtime_now_ns, int64_t realtime_pos) {
    if (channel >= SHM_MAX_CHANNELS) return;
    
    uint64_t play_start_ns = layout->playback_ring_start_time_ns.load(std::memory_order_acquire);
    int64_t play_start_pos = layout->playback_ring_start_pos.load(std::memory_order_acquire);
    uint32_t write_len = layout->playback_ring_write_len.load(std::memory_order_acquire);
    double sr = layout->sample_rate > 0.0 ? layout->sample_rate : 48000.0;

    for (uint32_t s = 0; s < count; ++s) {
        int64_t idx = -1;
        
        if (play_start_pos >= 0 && realtime_pos >= 0) {
            idx = (realtime_pos + s) - play_start_pos;
        }
        
        if ((idx < 0 || idx >= (int64_t)write_len) && play_start_ns != 0) {
            uint64_t sample_time_ns = realtime_now_ns + (uint64_t)(s * (1000000000.0 / sr));
            if (sample_time_ns >= play_start_ns) {
                idx = (int64_t)(((sample_time_ns - play_start_ns) * sr) / 1000000000.0);
            }
        }

        if (idx >= 0 && idx < (int64_t)write_len && idx < SHM_MAX_SAMPLES) {
            output_buffer[s] += layout->pre_rendered_playback[channel][idx % SHM_MAX_SAMPLES];
        }
    }
}
#endif

} // namespace arthur
