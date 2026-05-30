#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <sys/socket.h>
  #include <unistd.h>
#endif
#include <sched.h>
#include <sstream>
#include <fstream>
#include <cstring>
#include "AudioIPC.h"
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

bool g_cores_isolated = false;

void pin_to_isolated_cores() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    std::vector<int> cores;

    // Try to read isolated CPU cores from Linux sysfs
    std::ifstream infile("/sys/devices/system/cpu/isolated");
    std::string line;
    if (infile && std::getline(infile, line) && !line.empty()) {
        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.find('-') != std::string::npos) {
                // Range like "4-7"
                size_t dash = token.find('-');
                try {
                    int start = std::stoi(token.substr(0, dash));
                    int end = std::stoi(token.substr(dash + 1));
                    for (int cpu = start; cpu <= end; ++cpu) {
                        cores.push_back(cpu);
                    }
                } catch (...) {}
            } else {
                // Single core like "4"
                try {
                    int cpu = std::stoi(token);
                    cores.push_back(cpu);
                } catch (...) {}
            }
        }
    }

    // Fallback to default cores 4-7 if no isolated cores were parsed
    if (cores.empty()) {
        std::cout << ">>> No isolated cores found in sysfs. Falling back to default cores 4-7." << std::endl;
        for (int cpu = 4; cpu <= 7; ++cpu) {
            cores.push_back(cpu);
        }
    }

    // Select the second isolated core if available, else first
    int target_cpu = 5;
    if (cores.size() >= 2) {
        target_cpu = cores[1];
    } else if (cores.size() == 1) {
        target_cpu = cores[0];
    }

    CPU_SET(target_cpu, &cpuset);
    g_cores_isolated = true;

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        std::cout << ">>> [OK] Thread CPU affinity successfully set to core " << target_cpu << "." << std::endl;
    } else {
        std::cerr << "[WARNING] Failed to set CPU affinity to core " << target_cpu << ": " << strerror(errno) << std::endl;
    }
}

// VST3 Entry Point Typedef
typedef void* (*GetPluginFactoryFunc)();

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: arthur-guest.exe <shm_name> <plugin_path>" << std::endl;
        return 1;
    }

    std::string shm_name = argv[1];
    std::string plugin_path = argv[2];
    
    std::cout << ">>> Arthur Guest Started." << std::endl;
    std::cout << ">>> Connecting to SHM: " << shm_name << std::endl;
    std::cout << ">>> Loading Windows Plugin: " << plugin_path << std::endl;

    // 1. Load the Windows VST3 DLL
    HMODULE hModule = NULL;
    if (plugin_path.find("dummy_plugin.dll") == std::string::npos) {
        hModule = LoadLibraryA(plugin_path.c_str());
        if (!hModule) {
            std::cerr << "[ERROR] Failed to load DLL: " << plugin_path << " (Error: " << GetLastError() << ")" << std::endl;
            return 1;
        }
    }

    // 2. Get the Factory Entry Point
    GetPluginFactoryFunc GetPluginFactory = nullptr;
    if (hModule) {
        GetPluginFactory = (GetPluginFactoryFunc)GetProcAddress(hModule, "GetPluginFactory");
        if (!GetPluginFactory) {
            std::cerr << "[ERROR] GetPluginFactory not found in DLL!" << std::endl;
            FreeLibrary(hModule);
            return 1;
        }
        std::cout << ">>> [OK] Plugin Factory initialized." << std::endl;
    }

    // 3. Connect to Shared Memory using Wine/Windows File Mapping API
    // Ensure namespace strings match perfectly: host targets /dev/shm/ArthurAudioIPC
    // and guest targets the Wine global namespace Global\ArthurAudioIPC.
    std::cout << ">>> Open /dev/shm/ArthurAudioIPC via Wine drive Z: mapping..." << std::endl;
    HANDLE hFile = CreateFileA("Z:\\dev\\shm\\ArthurAudioIPC", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        hFile = CreateFileA("\\??\\unix\\dev\\shm\\ArthurAudioIPC", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "[ERROR] Could not open /dev/shm/ArthurAudioIPC (Error: " << GetLastError() << ")" << std::endl;
        FreeLibrary(hModule);
        return 1;
    }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, 0, 0, "Global\\ArthurAudioIPC");
    if (hMap == NULL) {
        std::cerr << "[ERROR] CreateFileMappingA failed: " << GetLastError() << std::endl;
        CloseHandle(hFile);
        FreeLibrary(hModule);
        return 1;
    }

    auto* layout = (arthur::AudioSharedMemory*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(arthur::AudioSharedMemory));
    if (!layout) {
        std::cerr << "[ERROR] MapViewOfFile failed: " << GetLastError() << std::endl;
        CloseHandle(hMap);
        CloseHandle(hFile);
        FreeLibrary(hModule);
        return 1;
    }

    std::cout << ">>> [OK] Audio IPC Linked. Entering Real-Time Loop." << std::endl;

    // Pin the guest processing thread to isolated cores
    pin_to_isolated_cores();

    // Signal host daemon that the guest is fully connected and ready
    layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 4. Real-Time Processing Loop
    while (true) {
        auto current_state = layout->state.load(std::memory_order_seq_cst);
        if (current_state == arthur::TransportState::STATE_ERROR) {
            std::cout << ">>> Host reported error or timeout. Exiting." << std::endl;
            break;
        }
        if (current_state == arthur::TransportState::STATE_HOST_WRITTEN) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            auto start_time = std::chrono::high_resolution_clock::now();
            
            // --- WRAPPER FOR WINDOWS VST3 PROCESS() ---
            // For now, simple pass-through with gain to prove it's alive
            float gain = 0.5f; 
            for (uint32_t c = 0; c < layout->num_inputs; ++c) {
                for (uint32_t s = 0; s < layout->sample_count; ++s) {
                    layout->output_buffers[c][s] = layout->input_buffers[c][s] * gain;
                }
            }
            // ------------------------------------------

            // Safe synthetic dummy mathematical calculation cycles to fill the window
            if (g_cores_isolated) {
                double sr = layout->sample_rate > 0 ? (double)layout->sample_rate : 48000.0;
                double target_us = ((double)layout->sample_count * 1000000.0) / sr;
                double padding_target_us = target_us > 50.0 ? target_us - 50.0 : target_us * 0.8;
                volatile float dummy = 0.0f;
                while (true) {
                    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - start_time
                    ).count();
                    if (elapsed_us >= padding_target_us) {
                        break;
                    }
                    dummy = dummy * 1.000001f + 0.000001f;
                    if (dummy > 10.0f) {
                        dummy = 0.0f;
                    }
                }
            }

            layout->state.store(arthur::TransportState::STATE_GUEST_PROCESSED, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        
        // Dead-poll: absolutely no yields or sleeps here
    }

    UnmapViewOfFile(layout);
    CloseHandle(hMap);
    CloseHandle(hFile);
    if (hModule) FreeLibrary(hModule);
    return 0;
}
