#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <sys/socket.h>
  #include <unistd.h>
#endif
#include "ipc/shm_buffer.h"
#include <iostream>
#include <string>
#include <vector>

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
    HMODULE hModule = LoadLibraryA(plugin_path.c_str());
    if (!hModule) {
        std::cerr << "[ERROR] Failed to load DLL: " << plugin_path << " (Error: " << GetLastError() << ")" << std::endl;
        return 1;
    }

    // 2. Get the Factory Entry Point
    GetPluginFactoryFunc GetPluginFactory = (GetPluginFactoryFunc)GetProcAddress(hModule, "GetPluginFactory");
    if (!GetPluginFactory) {
        std::cerr << "[ERROR] GetPluginFactory not found in DLL!" << std::endl;
        FreeLibrary(hModule);
        return 1;
    }

    std::cout << ">>> [OK] Plugin Factory initialized." << std::endl;

    // 3. Connect to Shared Memory
    Arthur::ShmBuffer shm(shm_name, false);
    if (!shm.isValid()) {
        std::cerr << "[ERROR] Could not connect to Shared Memory!" << std::endl;
        FreeLibrary(hModule);
        return 1;
    }

    auto* layout = shm.get();
    std::cout << ">>> [OK] Audio IPC Linked. Entering Real-Time Loop." << std::endl;

    // 4. Real-Time Processing Loop
    while (true) {
        if (layout->header.hostReady.load()) {
            
            // --- WRAPPER FOR WINDOWS VST3 PROCESS() ---
            // For now, simple pass-through with gain to prove it's alive
            float gain = 0.5f; 
            for (uint32_t c = 0; c < layout->header.channels; ++c) {
                for (uint32_t s = 0; s < layout->header.bufferSize; ++s) {
                    layout->output[c][s] = layout->input[c][s] * gain;
                }
            }
            // ------------------------------------------

            layout->header.guestReady.store(true);
            layout->header.hostReady.store(false);
        }
        
        Sleep(0); // Relinquish CPU slice
    }

    FreeLibrary(hModule);
    return 0;
}
