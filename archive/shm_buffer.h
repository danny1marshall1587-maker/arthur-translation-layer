#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace Arthur {

struct AudioBufferHeader {
    std::atomic<uint64_t> sequence;
    uint32_t sampleRate;
    uint32_t bufferSize;
    uint32_t channels;
    std::atomic<bool> hostReady;
    std::atomic<bool> guestReady;
};

class ShmBuffer {
public:
    static constexpr size_t MAX_CHANNELS = 2;
    static constexpr size_t MAX_SAMPLES = 4096;
    
    struct Layout {
        AudioBufferHeader header;
        float input[MAX_CHANNELS][MAX_SAMPLES];
        float output[MAX_CHANNELS][MAX_SAMPLES];
    };

    ShmBuffer(const std::string& name, bool create) : name(name) {
        if (create) {
            fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd != -1) ftruncate(fd, sizeof(Layout));
        } else {
            fd = shm_open(name.c_str(), O_RDWR, 0666);
        }
        
        if (fd != -1) {
            ptr = (Layout*)mmap(NULL, sizeof(Layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
    }

    ~ShmBuffer() {
        if (ptr) munmap(ptr, sizeof(Layout));
        if (fd != -1) close(fd);
    }

    bool isValid() const { return ptr != nullptr; }
    Layout* get() { return ptr; }

private:
    std::string name;
    int fd = -1;
    Layout* ptr = nullptr;
};

} // namespace Arthur
