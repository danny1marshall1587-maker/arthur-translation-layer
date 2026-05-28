#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

#define FILE_DEVICE_UNKNOWN 0x00000022
#define IOCTL_IVSHMEM_REQUEST_PEERID CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_SIZE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IVSHMEM_CACHE_NONCACHED 0
#define IVSHMEM_CACHE_CACHED 1
#define IVSHMEM_CACHE_WRITECOMBINED 2

typedef struct IVSHMEM_MMAP_CONFIG {
    UINT8 cacheMode; 
} IVSHMEM_MMAP_CONFIG, *PIVSHMEM_MMAP_CONFIG;

typedef UINT16 IVSHMEM_PEERID;
typedef UINT64 IVSHMEM_SIZE;

typedef struct IVSHMEM_MMAP {
    IVSHMEM_PEERID peerID;
    IVSHMEM_SIZE size;
    PVOID ptr;
    UINT16 vectors;
} IVSHMEM_MMAP, *PIVSHMEM_MMAP;

#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Default configuration parameters
#ifdef _WIN32
const char* DEFAULT_PATH = "\\\\.\\IVSHMEM";
#else
const char* DEFAULT_PATH = "/dev/shm/ivshmem";
#endif
const size_t DEFAULT_SIZE = 1024 * 1024; // 1 MB

int main(int argc, char* argv[]) {
    std::string path = DEFAULT_PATH;
    size_t shm_size = DEFAULT_SIZE;
    bool write_mode = false;
    bool read_mode = false;
    bool loop_mode = false;

    // Command line arguments parsing
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            shm_size = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "-w") == 0) {
            write_mode = true;
        } else if (std::strcmp(argv[i], "-r") == 0) {
            read_mode = true;
        } else if (std::strcmp(argv[i], "-l") == 0) {
            loop_mode = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: ivshmem_test [options]\n"
                      << "Options:\n"
                      << "  -p <path>  Path to shared memory file (Linux) or Device link (Windows)\n"
                      << "  -s <size>  Size of memory to map (in bytes)\n"
                      << "  -w         Write test pattern to memory\n"
                      << "  -r         Read and print memory contents\n"
                      << "  -l         Run latency ping-pong loop\n"
                      << "  -h         Show help\n";
            return 0;
        }
    }

    if (!write_mode && !read_mode && !loop_mode) {
        std::cout << "No action specified. Defaulting to read mode.\n";
        read_mode = true;
    }

    void* mapped_ptr = nullptr;
    size_t mapped_size = shm_size;

#ifdef _WIN32
    std::cout << "Windows Build: Opening device " << path << std::endl;
    HANDLE hDevice = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open device link: " << GetLastError() << std::endl;
        return 1;
    }

    // Query size first
    IVSHMEM_SIZE q_size = 0;
    DWORD bytesReturned = 0;
    if (DeviceIoControl(hDevice, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &q_size, sizeof(q_size), &bytesReturned, NULL)) {
        std::cout << "Device reported size: " << q_size << " bytes" << std::endl;
        mapped_size = q_size;
    } else {
        std::cerr << "Warning: Failed to query size, using default size: " << GetLastError() << std::endl;
    }

    // Map memory
    IVSHMEM_MMAP_CONFIG config;
    config.cacheMode = IVSHMEM_CACHE_NONCACHED;
    IVSHMEM_MMAP mmap_info;
    
    if (DeviceIoControl(hDevice, IOCTL_IVSHMEM_REQUEST_MMAP, &config, sizeof(config), &mmap_info, sizeof(mmap_info), &bytesReturned, NULL)) {
        mapped_ptr = mmap_info.ptr;
        std::cout << "Memory mapped successfully at " << mmap_info.ptr 
                  << " (PeerID: " << mmap_info.peerID << ", Vectors: " << mmap_info.vectors << ")" << std::endl;
    } else {
        std::cerr << "Failed to map memory: " << GetLastError() << std::endl;
        CloseHandle(hDevice);
        return 1;
    }

#else
    std::cout << "Linux Build: Opening file " << path << std::endl;
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open shared memory file " << path << ": " << std::strerror(errno) << std::endl;
        std::cerr << "Creating shm file directly in case of local test..." << std::endl;
        fd = open(path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
            std::cerr << "Failed to create shm file: " << std::strerror(errno) << std::endl;
            return 1;
        }
        if (ftruncate(fd, shm_size) < 0) {
            std::cerr << "Failed to set size on file: " << std::strerror(errno) << std::endl;
            close(fd);
            return 1;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            mapped_size = st.st_size;
            std::cout << "File size matches: " << mapped_size << " bytes" << std::endl;
        }
    }

    mapped_ptr = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_ptr == MAP_FAILED) {
        std::cerr << "mmap failed: " << std::strerror(errno) << std::endl;
        close(fd);
        return 1;
    }
    std::cout << "Memory mapped successfully at " << mapped_ptr << std::endl;
#endif

    // Test actions
    volatile char* buffer = static_cast<volatile char*>(mapped_ptr);

    if (write_mode) {
        std::string test_msg = "ARTHUR_IVSHMEM_PING";
        std::cout << "Writing to shared memory: \"" << test_msg << "\"" << std::endl;
        std::memset(const_cast<char*>(buffer), 0, mapped_size);
        std::memcpy(const_cast<char*>(buffer), test_msg.c_str(), test_msg.length());
        std::cout << "Write completed." << std::endl;
    }

    if (read_mode) {
        std::cout << "Reading shared memory content:" << std::endl;
        char read_buf[64] = {0};
        std::memcpy(read_buf, const_cast<char*>(buffer), std::min(sizeof(read_buf) - 1, mapped_size));
        std::cout << "Raw string (first 63 chars): \"" << read_buf << "\"" << std::endl;
    }

    if (loop_mode) {
        std::cout << "Starting latency ping-pong loop. Press Ctrl+C to stop." << std::endl;
        // Ping-pong protocol:
        // Host writes "PING" to buffer[0-3], waits for Guest to change it to "PONG".
        // Guest reads "PING", changes it to "PONG".
        // Let's implement both sides. We assume the writer writes "PING" first.
        
        // Let's detect if we are running as Linux Host (Client A) or Windows Guest (Client B).
        // Since we compile separately, we can specify via write_mode which side initiates.
        // For simplicity: If loop_mode is run:
        // Linux side initiates (writes PING, waits for PONG).
        // Windows side responds (waits for PING, writes PONG).
#ifdef _WIN32
        bool is_initiator = false; // Windows responds
#else
        bool is_initiator = true;  // Linux initiates
#endif

        if (is_initiator) {
            std::cout << "Running as Initiator (Host)..." << std::endl;
            for (int r = 0; r < 100; ++r) {
                auto start = std::chrono::high_resolution_clock::now();
                
                std::memcpy(const_cast<char*>(buffer), "PING", 4);
                
                // Wait for response
                int timeout = 1000000; // Loop counter limit
                while (std::strncmp(const_cast<char*>(buffer), "PONG", 4) != 0 && --timeout > 0) {
                    std::this_thread::yield();
                }
                
                if (timeout == 0) {
                    std::cerr << "Timeout waiting for PONG!" << std::endl;
                    break;
                }
                
                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                std::cout << "Round trip latency: " << duration / 1000.0 << " microseconds" << std::endl;
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            std::cout << "Running as Responder (Guest)..." << std::endl;
            while (true) {
                if (std::strncmp(const_cast<char*>(buffer), "PING", 4) == 0) {
                    std::memcpy(const_cast<char*>(buffer), "PONG", 4);
                }
                std::this_thread::yield();
            }
        }
    }

    // Clean up
#ifdef _WIN32
    DeviceIoControl(hDevice, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, &bytesReturned, NULL);
    CloseHandle(hDevice);
#else
    munmap(mapped_ptr, mapped_size);
    close(fd);
#endif

    std::cout << "Done." << std::endl;
    return 0;
}
