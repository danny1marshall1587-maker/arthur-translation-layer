#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <sys/socket.h>
  #include <unistd.h>
#endif
#include "AudioIPC.h"
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

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

    // Signal host daemon that the guest is fully connected and ready
    layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 4. Real-Time Processing Loop
    while (true) {
        if (layout->state.load(std::memory_order_seq_cst) == arthur::TransportState::STATE_HOST_WRITTEN) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            
            // --- WRAPPER FOR WINDOWS VST3 PROCESS() ---
            // For now, simple pass-through with gain to prove it's alive
            float gain = 0.5f; 
            for (uint32_t c = 0; c < layout->num_inputs; ++c) {
                for (uint32_t s = 0; s < layout->sample_count; ++s) {
                    layout->output_buffers[c][s] = layout->input_buffers[c][s] * gain;
                }
            }
            // ------------------------------------------

            layout->state.store(arthur::TransportState::STATE_GUEST_PROCESSED, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        
        Sleep(0); // Relinquish CPU slice
    }

    UnmapViewOfFile(layout);
    CloseHandle(hMap);
    CloseHandle(hFile);
    if (hModule) FreeLibrary(hModule);
    return 0;
}
