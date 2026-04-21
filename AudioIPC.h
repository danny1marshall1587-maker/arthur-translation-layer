#pragma once

#include <atomic>
#include <cstdint>
#include <array>
#include <cassert>

namespace arthur {

// Constants defining the IPC limits
constexpr size_t MAX_AUDIO_CHANNELS = 64;
constexpr size_t MAX_SAMPLES_PER_BLOCK = 4096;
constexpr size_t IPC_RING_BUFFER_SIZE = 16; // Must be a power of 2 for fast masking

/**
 * A single block of audio data transferred between the Linux Host and Wine.
 */
struct AudioProcessBlock {
    uint32_t sample_count;
    uint32_t num_channels;
    
    // We use a flat array to ensure it's trivially copyable over shared memory.
    // In a real system, this is allocated in an mmap'ed shared memory region.
    float channels[MAX_AUDIO_CHANNELS][MAX_SAMPLES_PER_BLOCK];
};

/**
 * A Single-Producer, Single-Consumer (SPSC) Lock-Free Ring Buffer.
 * Designed to be allocated in a shared memory region (e.g. via shm_open / mmap).
 * 
 * Crucial Audio Rule: NO MUTEXES. NO ALLOCATIONS. 
 * We rely strictly on atomic acquire/release semantics.
 */
class LockFreeAudioQueue {
public:
    LockFreeAudioQueue() : write_idx_(0), read_idx_(0) {}

    /**
     * Push a block to the queue. (Called by the audio thread producer).
     * Returns true if successful, false if the queue is full (buffer overrun).
     */
    bool push(const AudioProcessBlock& block) {
        const size_t current_tail = write_idx_.load(std::memory_order_relaxed);
        const size_t next_tail = increment(current_tail);

        // If the next write position is the current read position, queue is full.
        if (next_tail == read_idx_.load(std::memory_order_acquire)) {
            return false; 
        }

        buffer_[current_tail] = block;
        
        // Release ensures the data write happens before we update the tail index.
        write_idx_.store(next_tail, std::memory_order_release);
        return true;
    }

    /**
     * Pop a block from the queue. (Called by the audio thread consumer).
     * Returns true if successful, false if the queue is empty.
     */
    bool pop(AudioProcessBlock& out_block) {
        const size_t current_head = read_idx_.load(std::memory_order_relaxed);

        // If head equals tail, queue is empty.
        if (current_head == write_idx_.load(std::memory_order_acquire)) {
            return false;
        }

        out_block = buffer_[current_head];

        // Release ensures data read happens before we update the head index.
        read_idx_.store(increment(current_head), std::memory_order_release);
        return true;
    }

private:
    inline size_t increment(size_t idx) const {
        return (idx + 1) & (IPC_RING_BUFFER_SIZE - 1);
    }

    std::array<AudioProcessBlock, IPC_RING_BUFFER_SIZE> buffer_;
    
    // Cache line padding to prevent false sharing between the producer and consumer cores.
    alignas(64) std::atomic<size_t> write_idx_;
    alignas(64) std::atomic<size_t> read_idx_;
};

} // namespace arthur
