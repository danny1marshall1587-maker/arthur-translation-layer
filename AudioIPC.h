#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>
#include <new>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
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
 * POD-compatible atomic wrapper utilizing compiler built-in intrinsics.
 * This guarantees binary layout compatibility across compilers/environments (Linux/Wine).
 */
template <typename T>
struct ArthurAtomic {
    T value;

    ArthurAtomic() : value{} {}
    ArthurAtomic(T val) : value(val) {}

    T load(std::memory_order order = std::memory_order_seq_cst) const volatile {
        int gcc_order = get_gcc_memory_order(order);
        T res;
        __atomic_load(const_cast<const T*>(&value), &res, gcc_order);
        return res;
    }

    T load(std::memory_order order = std::memory_order_seq_cst) const {
        int gcc_order = get_gcc_memory_order(order);
        T res;
        __atomic_load(&value, &res, gcc_order);
        return res;
    }

    void store(T val, std::memory_order order = std::memory_order_seq_cst) volatile {
        int gcc_order = get_gcc_memory_order(order);
        __atomic_store(const_cast<T*>(&value), &val, gcc_order);
    }

    void store(T val, std::memory_order order = std::memory_order_seq_cst) {
        int gcc_order = get_gcc_memory_order(order);
        __atomic_store(&value, &val, gcc_order);
    }

    T fetch_add(T val, std::memory_order order = std::memory_order_seq_cst) volatile {
        int gcc_order = get_gcc_memory_order(order);
        return __atomic_fetch_add(const_cast<T*>(&value), val, gcc_order);
    }

    T fetch_add(T val, std::memory_order order = std::memory_order_seq_cst) {
        int gcc_order = get_gcc_memory_order(order);
        return __atomic_fetch_add(&value, val, gcc_order);
    }

    // Implicit conversions to mimic std::atomic operator behaviors
    operator T() const { return load(); }
    T operator=(T val) { store(val); return val; }

    static constexpr int get_gcc_memory_order(std::memory_order order) {
        switch (order) {
            case std::memory_order_relaxed: return __ATOMIC_RELAXED;
            case std::memory_order_consume: return __ATOMIC_CONSUME;
            case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
            case std::memory_order_release: return __ATOMIC_RELEASE;
            case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
            case std::memory_order_seq_cst: return __ATOMIC_SEQ_CST;
            default: return __ATOMIC_SEQ_CST;
        }
    }
};

/**
 * The actual memory layout for Shared Memory.
 * 
 * VERSION FIELD: Must be the first field. Any process attaching to this
 * segment validates SHM_VERSION to detect layout mismatches.
 */
struct AudioSharedMemory {
    // Version tag — must be first field for mismatch detection
    static constexpr uint32_t SHM_VERSION = 5; // Bumped version due to console_mode addition
    uint32_t version = SHM_VERSION;

    // Transport state machine — cache-line aligned for performance
    alignas(64) ArthurAtomic<TransportState> state{TransportState::STATE_IDLE};

    // Audio format metadata — all standard types wrapped in ArthurAtomic
    ArthurAtomic<uint32_t> sample_count{0};
    ArthurAtomic<uint32_t> num_inputs{0};
    ArthurAtomic<uint32_t> num_outputs{0};
    ArthurAtomic<double> sample_rate{48000.0};
    ArthurAtomic<int64_t> playhead_pos{0};

    float input_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES]{};
    float output_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES]{};

    // Hybrid playback pre-rendered buffer (for ASIO-Guard style)
    float pre_rendered_playback[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES]{};
    ArthurAtomic<uint64_t> playback_ring_start_time_ns{0};
    ArthurAtomic<int64_t> playback_ring_start_pos{0};
    ArthurAtomic<uint32_t> playback_ring_write_len{0};

    // MIDI transport — counts are atomic to prevent data races
    ArthurAtomic<uint32_t> midi_in_count{0};
    ShmMidiEvent midi_in[256]{};
    ArthurAtomic<uint32_t> midi_out_count{0};
    ShmMidiEvent midi_out[256]{};

    // VST3 internal plugin latency
    ArthurAtomic<uint32_t> plugin_latency{0};

    // VHC Console Mode: 0 = Playback, 1 = Record Dry / Monitor Wet, 2 = Record Wet
    ArthurAtomic<uint32_t> console_mode{0};

    // GUI control variables (X11 / XWayland windowing bridge)
    ArthurAtomic<uint64_t> host_window_xid{0};
    ArthurAtomic<uint64_t> guest_window_xid{0};
    ArthurAtomic<uint32_t> request_open_editor{0};
    ArthurAtomic<uint32_t> request_close_editor{0};
    ArthurAtomic<uint32_t> editor_width{0};
    ArthurAtomic<uint32_t> editor_height{0};
    ArthurAtomic<uint32_t> editor_open_status{0};
};

/**
 * Shared Memory structure for multi-device CLLS aggregation.
 */
struct CLLSSyncLayout {
    static constexpr uint32_t SYNC_VERSION = 4;
    uint32_t version = SYNC_VERSION;
    ArthurAtomic<int32_t> owner_pid[8]{};
    ArthurAtomic<float> measured_rtt[8]{};
    ArthurAtomic<float> global_target_rtt{0.0f};
};


#ifndef _WIN32
/**
 * Helper to manage the SHM lifecycle on Linux.
 * Enforces Rule of 5 to prevent resource double-free on copies.
 */
class AudioTransport {
public:
    AudioTransport() : shm_ptr_(nullptr), fd_(-1), is_owner_(false), mapped_size_(0) {}
    
    ~AudioTransport() {
        detach();
    }

    // Delete copy operations
    AudioTransport(const AudioTransport&) = delete;
    AudioTransport& operator=(const AudioTransport&) = delete;

    // Move constructor
    AudioTransport(AudioTransport&& other) noexcept {
        shm_ptr_ = other.shm_ptr_;
        fd_ = other.fd_;
        name_ = std::move(other.name_);
        is_owner_ = other.is_owner_;
        mapped_size_ = other.mapped_size_;
        other.shm_ptr_ = nullptr;
        other.fd_ = -1;
        other.is_owner_ = false;
        other.mapped_size_ = 0;
    }

    // Move assignment
    AudioTransport& operator=(AudioTransport&& other) noexcept {
        if (this != &other) {
            detach();
            shm_ptr_ = other.shm_ptr_;
            fd_ = other.fd_;
            name_ = std::move(other.name_);
            is_owner_ = other.is_owner_;
            mapped_size_ = other.mapped_size_;
            other.shm_ptr_ = nullptr;
            other.fd_ = -1;
            other.is_owner_ = false;
            other.mapped_size_ = 0;
        }
        return *this;
    }
    
    bool create(const std::string& name) {
        name_ = "/" + name;
        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0600);  // 0600: owner-only (was 0666)
        if (fd_ < 0) return false;

        mapped_size_ = sizeof(AudioSharedMemory);

        if (ftruncate(fd_, mapped_size_) < 0) {
            close(fd_);
            fd_ = -1;
            shm_unlink(name_.c_str());
            return false;
        }

        shm_ptr_ = (AudioSharedMemory*)mmap(nullptr, mapped_size_, 
                                           PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        
        if (shm_ptr_ == MAP_FAILED) {
            shm_ptr_ = nullptr;
            close(fd_);
            fd_ = -1;
            shm_unlink(name_.c_str());
            return false;
        }

        // Placement-new to properly initialize atomics and default values
        new (shm_ptr_) AudioSharedMemory();
        is_owner_ = true;
        return true;
    }

    bool attach(const std::string& name) {
        name_ = "/" + name;
        fd_ = shm_open(name_.c_str(), O_RDWR, 0600);
        if (fd_ < 0) return false;

        // Verify that the file is large enough to map without generating SIGBUS
        struct stat st;
        if (fstat(fd_, &st) < 0 || st.st_size < (off_t)sizeof(AudioSharedMemory)) {
            close(fd_);
            fd_ = -1;
            return false;
        }

        mapped_size_ = st.st_size;

        shm_ptr_ = (AudioSharedMemory*)mmap(nullptr, mapped_size_, 
                                           PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        
        if (shm_ptr_ == MAP_FAILED) {
            shm_ptr_ = nullptr;
            close(fd_);
            fd_ = -1;
            return false;
        }

        // Validate version to detect layout mismatches
        if (shm_ptr_->version != AudioSharedMemory::SHM_VERSION) {
            munmap(shm_ptr_, mapped_size_);
            shm_ptr_ = nullptr;
            close(fd_);
            fd_ = -1;
            return false;
        }

        is_owner_ = false;
        return true;
    }

    void detach() {
        if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
            munmap(shm_ptr_, mapped_size_);
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
        mapped_size_ = 0;
    }

    AudioSharedMemory* get() { return shm_ptr_; }

private:
    AudioSharedMemory* shm_ptr_;
    int fd_;
    std::string name_;
    bool is_owner_;
    size_t mapped_size_;
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
    
    // Only update shared timing & buffer length metadata when processing the first channel.
    // This prevents multiplying write_len by the channel count in multichannel setups.
    if (channel == 0) {
        if (offset == 0) {
            layout->playback_ring_start_time_ns.store(block_start_time_ns, std::memory_order_release);
            layout->playback_ring_start_pos.store(block_start_pos, std::memory_order_release);
            layout->playback_ring_write_len.store(count, std::memory_order_release);
        } else {
            // Use fetch_add to update write length atomically
            layout->playback_ring_write_len.fetch_add(count, std::memory_order_release);
        }
    }
}

inline void sum_aligned_playback(AudioSharedMemory* layout, uint32_t channel, float* output_buffer, uint32_t count, uint64_t realtime_now_ns, int64_t realtime_pos) {
    if (channel >= SHM_MAX_CHANNELS) return;
    
    // Enforce Acquire ordering: Load the synchronization length variable first,
    // establishing memory ordering for timing variables loaded afterwards.
    uint32_t write_len = layout->playback_ring_write_len.load(std::memory_order_acquire);
    uint64_t play_start_ns = layout->playback_ring_start_time_ns.load(std::memory_order_acquire);
    int64_t play_start_pos = layout->playback_ring_start_pos.load(std::memory_order_acquire);
    double sr = layout->sample_rate.load(std::memory_order_relaxed);
    if (sr <= 0.0) sr = 48000.0;

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
