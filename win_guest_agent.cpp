#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <sched.h>
#include <sstream>
#include <fstream>
#include <cstring>

#include "AudioIPC.h"

void pin_to_isolated_cores() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    bool set_any = false;

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
                        CPU_SET(cpu, &cpuset);
                        set_any = true;
                    }
                } catch (...) {}
            } else {
                // Single core like "4"
                try {
                    int cpu = std::stoi(token);
                    CPU_SET(cpu, &cpuset);
                    set_any = true;
                } catch (...) {}
            }
        }
    }

    // Fallback to default cores 4-7 if no isolated cores were parsed
    if (!set_any) {
        std::cout << ">>> No isolated cores found in sysfs. Falling back to default cores 4-7." << std::endl;
        for (int cpu = 4; cpu <= 7; ++cpu) {
            CPU_SET(cpu, &cpuset);
        }
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        std::cout << ">>> [OK] Thread CPU affinity successfully set." << std::endl;
    } else {
        std::cerr << "[WARNING] Failed to set CPU affinity: " << strerror(errno) << std::endl;
    }
}
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

// IVSHMEM Device IOCTL Definitions
#define FILE_DEVICE_UNKNOWN 0x00000022
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

// VST3 Entry Point Typedef
typedef Steinberg::IPluginFactory* (PLUGIN_API *GetPluginFactoryProc)();

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace arthur;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: win_guest_agent.exe <shm_path_or_device> <vst3_dll_path>\n"
                  << "Example (VM mode):   win_guest_agent.exe \\\\.\\IVSHMEM C:\\Plugins\\Synth.vst3\n"
                  << "Example (Wine mode): win_guest_agent.exe arthur_shm C:\\Plugins\\Synth.vst3\n";
        return 1;
    }

    std::string shm_path = argv[1];
    std::string plugin_path = argv[2];

    std::cout << ">>> Arthur Windows Guest Agent Started." << std::endl;
    std::cout << ">>> VST3 Plugin: " << plugin_path << std::endl;

    // 1. Map Shared Memory
    AudioSharedMemory* layout = nullptr;
    HANDLE hDevice = INVALID_HANDLE_VALUE;
    HANDLE hMapFile = NULL;

    if (shm_path.find("\\\\.\\") == 0) {
        // IVSHMEM Device Link Mode (Real VM)
        std::cout << ">>> Real VM mode: Opening device " << shm_path << std::endl;
        hDevice = CreateFileA(shm_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) {
            std::cerr << "[ERROR] Failed to open IVSHMEM device: " << GetLastError() << std::endl;
            return 1;
        }

        IVSHMEM_MMAP_CONFIG config;
        config.cacheMode = IVSHMEM_CACHE_NONCACHED;
        IVSHMEM_MMAP mmap_info;
        DWORD bytesReturned = 0;

        if (DeviceIoControl(hDevice, IOCTL_IVSHMEM_REQUEST_MMAP, &config, sizeof(config), &mmap_info, sizeof(mmap_info), &bytesReturned, NULL)) {
            layout = (AudioSharedMemory*)mmap_info.ptr;
            std::cout << ">>> [OK] IVSHMEM mapped at " << layout << " (Size: " << mmap_info.size << ")" << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to map IVSHMEM device memory: " << GetLastError() << std::endl;
            CloseHandle(hDevice);
            return 1;
        }
    } else {
        // Named Shared Memory Mode (Wine / local testing fallback)
        std::cout << ">>> Local/Wine mode: Opening /dev/shm/ArthurAudioIPC via Wine Z: drive mapping..." << std::endl;
        HANDLE hFile = CreateFileA("Z:\\dev\\shm\\ArthurAudioIPC", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            hFile = CreateFileA("\\??\\unix\\dev\\shm\\ArthurAudioIPC", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (hFile == INVALID_HANDLE_VALUE) {
            std::cerr << "[ERROR] Could not open /dev/shm/ArthurAudioIPC (Error: " << GetLastError() << ")" << std::endl;
            return 1;
        }

        hMapFile = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, 0, 0, "Global\\ArthurAudioIPC");
        if (hMapFile == NULL) {
            std::cerr << "[ERROR] CreateFileMappingA failed: " << GetLastError() << std::endl;
            CloseHandle(hFile);
            return 1;
        }

        layout = (AudioSharedMemory*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioSharedMemory));
        if (!layout) {
            std::cerr << "[ERROR] MapViewOfFile failed: " << GetLastError() << std::endl;
            CloseHandle(hMapFile);
            CloseHandle(hFile);
            return 1;
        }
        std::cout << ">>> [OK] Global\\ArthurAudioIPC mapped at " << layout << std::endl;
    }

    if (plugin_path.find("dummy_plugin.dll") != std::string::npos) {
        std::cout << ">>> [OK] Running in dummy test mode." << std::endl;
        pin_to_isolated_cores();
        layout->state.store(TransportState::STATE_IDLE, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        while (true) {
            auto current_state = layout->state.load(std::memory_order_seq_cst);
            if (current_state == TransportState::STATE_ERROR) {
                std::cout << ">>> Host reported error or timeout. Exiting." << std::endl;
                break;
            }
            if (current_state == TransportState::STATE_HOST_WRITTEN) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                auto start_time = std::chrono::high_resolution_clock::now();
                
                // Copy inputs to outputs with gain of 0.5f (consistent with guest.cpp dummy)
                float gain = 0.5f;
                uint32_t active_channels = std::min(layout->num_inputs, layout->num_outputs);
                for (uint32_t c = 0; c < active_channels; ++c) {
                    for (uint32_t s = 0; s < layout->sample_count; ++s) {
                        layout->output_buffers[c][s] = layout->input_buffers[c][s] * gain;
                    }
                }
                
                // Safe synthetic dummy mathematical calculation cycles to fill the window
                double sr = layout->sample_rate > 0 ? (double)layout->sample_rate : 48000.0;
                double target_us = ((double)layout->sample_count * 1000000.0) / sr;
                volatile float dummy = 0.0f;
                while (true) {
                    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - start_time
                    ).count();
                    if (elapsed_us >= target_us) {
                        break;
                    }
                    dummy = dummy * 1.000001f + 0.000001f;
                    if (dummy > 10.0f) {
                        dummy = 0.0f;
                    }
                }

                layout->state.store(TransportState::STATE_GUEST_PROCESSED, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            // Dead-poll: absolutely no yields or sleeps here
        }
        
        if (hDevice != INVALID_HANDLE_VALUE) {
            DeviceIoControl(hDevice, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL);
            CloseHandle(hDevice);
        }
        if (hMapFile) {
            UnmapViewOfFile(layout);
            CloseHandle(hMapFile);
        }
        return 0;
    }

    // 2. Load the Windows VST3 DLL
    HMODULE hModule = LoadLibraryA(plugin_path.c_str());
    if (!hModule) {
        std::cerr << "[ERROR] Failed to load plugin DLL: " << plugin_path << " (Error: " << GetLastError() << ")" << std::endl;
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    GetPluginFactoryProc GetPluginFactory = (GetPluginFactoryProc)GetProcAddress(hModule, "GetPluginFactory");
    if (!GetPluginFactory) {
        std::cerr << "[ERROR] Failed to find GetPluginFactory entry point!" << std::endl;
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    IPluginFactory* factory = GetPluginFactory();
    if (!factory) {
        std::cerr << "[ERROR] GetPluginFactory returned NULL!" << std::endl;
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }
    std::cout << ">>> [OK] VST3 Factory initialized." << std::endl;

    // 3. Query Component Description & Find Audio Effect Class
    TUID classId;
    bool found = false;
    int32 classCount = factory->countClasses();
    std::cout << ">>> Factory contains " << classCount << " classes." << std::endl;
    
    for (int32 i = 0; i < classCount; ++i) {
        PClassInfo info;
        if (factory->getClassInfo(i, &info) == kResultOk) {
            std::cout << "  - Class [" << i << "]: Name=\"" << info.name 
                      << "\", Category=\"" << info.category << "\"" << std::endl;
            if (strcmp(info.category, "Audio Effect") == 0) {
                memcpy(classId, info.cid, sizeof(TUID));
                found = true;
                std::cout << "    => Selected class: \"" << info.name << "\"" << std::endl;
                break;
            }
        }
    }

    if (!found) {
        std::cerr << "[ERROR] No Class with category 'Audio Effect' found in this plugin!" << std::endl;
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    // 4. Create VST3 Component
    IComponent* component = nullptr;
    if (factory->createInstance(classId, IComponent::iid, (void**)&component) != kResultOk || !component) {
        std::cerr << "[ERROR] Failed to create IComponent instance!" << std::endl;
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }
    std::cout << ">>> [OK] IComponent created." << std::endl;

    // Initialize component (Host context is NULL for now)
    if (component->initialize(nullptr) != kResultOk) {
        std::cerr << "[ERROR] Component initialization failed!" << std::endl;
        component->release();
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    // 5. Query Audio Processor interface
    IAudioProcessor* audioProcessor = nullptr;
    if (component->queryInterface(IAudioProcessor::iid, (void**)&audioProcessor) != kResultOk || !audioProcessor) {
        std::cerr << "[ERROR] Plugin does not support IAudioProcessor interface!" << std::endl;
        component->release();
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }
    std::cout << ">>> [OK] IAudioProcessor retrieved." << std::endl;

    // 6. Setup Audio Processing configuration
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = SHM_MAX_SAMPLES;
    setup.sampleRate = layout->sample_rate > 0.0 ? layout->sample_rate : 48000.0;

    if (audioProcessor->setupProcessing(setup) != kResultOk) {
        std::cerr << "[ERROR] setupProcessing failed!" << std::endl;
        audioProcessor->release();
        component->release();
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    // Activate inputs and outputs buses
    int32 inputBuses = component->getBusCount(kAudio, kInput);
    for (int32 i = 0; i < inputBuses; ++i) {
        component->activateBus(kAudio, kInput, i, true);
    }
    int32 outputBuses = component->getBusCount(kAudio, kOutput);
    for (int32 i = 0; i < outputBuses; ++i) {
        component->activateBus(kAudio, kOutput, i, true);
    }

    if (component->setActive(true) != kResultOk) {
        std::cerr << "[ERROR] setActive(true) failed!" << std::endl;
        audioProcessor->release();
        component->release();
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    if (audioProcessor->setProcessing(true) != kResultOk) {
        std::cerr << "[ERROR] setProcessing(true) failed!" << std::endl;
        component->setActive(false);
        audioProcessor->release();
        component->release();
        FreeLibrary(hModule);
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) { UnmapViewOfFile(layout); CloseHandle(hMapFile); }
        return 1;
    }

    std::cout << ">>> [OK] Plugin fully activated. Entering Real-Time Processing Loop." << std::endl;

    // Pin the guest processing thread to isolated cores
    pin_to_isolated_cores();

    // Reset layout state to IDLE
    layout->state.store(TransportState::STATE_IDLE, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 7. Real-Time IPC processing loop
    while (true) {
        auto current_state = layout->state.load(std::memory_order_seq_cst);
        if (current_state == TransportState::STATE_ERROR) {
            std::cout << ">>> Host reported error or timeout. Exiting." << std::endl;
            break;
        }
        // Spin lock waiting for the host to write the next buffer
        if (current_state == TransportState::STATE_HOST_WRITTEN) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            auto start_time = std::chrono::high_resolution_clock::now();
            
            // Set up ProcessData buffers
            ProcessData processData;
            processData.processMode = kRealtime;
            processData.symbolicSampleSize = kSample32;
            processData.numSamples = layout->sample_count;
            processData.numInputs = inputBuses;
            processData.numOutputs = outputBuses;

            // Map inputs
            std::vector<AudioBusBuffers> inputBusesBuffers(inputBuses);
            std::vector<float*> inputChannelPtrs(layout->num_inputs);
            if (inputBuses > 0) {
                // Map channels to the first audio bus
                for (uint32_t c = 0; c < layout->num_inputs; ++c) {
                    inputChannelPtrs[c] = layout->input_buffers[c];
                }
                inputBusesBuffers[0].numChannels = layout->num_inputs;
                inputBusesBuffers[0].channelBuffers32 = inputChannelPtrs.data();
                inputBusesBuffers[0].silenceFlags = 0;
                processData.inputs = inputBusesBuffers.data();
            } else {
                processData.inputs = nullptr;
            }

            // Map outputs
            std::vector<AudioBusBuffers> outputBusesBuffers(outputBuses);
            std::vector<float*> outputChannelPtrs(layout->num_outputs);
            if (outputBuses > 0) {
                // Map channels to the first audio bus
                for (uint32_t c = 0; c < layout->num_outputs; ++c) {
                    outputChannelPtrs[c] = layout->output_buffers[c];
                }
                outputBusesBuffers[0].numChannels = layout->num_outputs;
                outputBusesBuffers[0].channelBuffers32 = outputChannelPtrs.data();
                outputBusesBuffers[0].silenceFlags = 0;
                processData.outputs = outputBusesBuffers.data();
            } else {
                processData.outputs = nullptr;
            }

            // Handlers for parameters and events (NULL for now)
            processData.inputEvents = nullptr;
            processData.outputEvents = nullptr;
            processData.inputParameterChanges = nullptr;
            processData.outputParameterChanges = nullptr;

            // Process audio through the VST3 plugin!
            audioProcessor->process(processData);

            // Safe synthetic dummy mathematical calculation cycles to fill the window
            double sr = layout->sample_rate > 0 ? (double)layout->sample_rate : 48000.0;
            double target_us = ((double)layout->sample_count * 1000000.0) / sr;
            volatile float dummy = 0.0f;
            while (true) {
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - start_time
                ).count();
                if (elapsed_us >= target_us) {
                    break;
                }
                dummy = dummy * 1.000001f + 0.000001f;
                if (dummy > 10.0f) {
                    dummy = 0.0f;
                }
            }

            // Notify Host
            layout->state.store(TransportState::STATE_GUEST_PROCESSED, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }

        // Dead-poll: absolutely no yields or sleeps here
    }

    // 8. Clean up
    audioProcessor->setProcessing(false);
    component->setActive(false);
    audioProcessor->release();
    component->release();
    FreeLibrary(hModule);

    if (hDevice != INVALID_HANDLE_VALUE) {
        DeviceIoControl(hDevice, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL);
        CloseHandle(hDevice);
    }
    if (hMapFile) {
        UnmapViewOfFile(layout);
        CloseHandle(hMapFile);
    }

    return 0;
}
