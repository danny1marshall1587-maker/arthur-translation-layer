#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#ifdef __WINE__
#include <sched.h>
#endif
#include <sstream>
#include <fstream>
#include <cstring>

#include "AudioIPC.h"

bool g_cores_isolated = false;

void pin_to_isolated_cores() {
#ifdef __WINE__
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
#else
    // Native Windows CPU Affinity fallback
    DWORD_PTR mask = 1 << 5; // Default fallback to core 5
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        std::cerr << "[WARNING] Failed to set CPU affinity on Windows: " << GetLastError() << std::endl;
    } else {
        std::cout << ">>> [OK] Thread CPU affinity successfully set via Windows API." << std::endl;
        g_cores_isolated = true;
    }
#endif
}
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

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

#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <atomic>

class ArthurHostApplication : public Steinberg::Vst::IHostApplication {
public:
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IHostApplication_iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    virtual Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    virtual Steinberg::uint32 PLUGIN_API release() override { return 1; }

    virtual Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* src = "Arthur Translation Layer";
        int i = 0;
        for (; i < 127 && src[i] != '\0'; ++i) {
            name[i] = src[i];
        }
        name[i] = '\0';
        return Steinberg::kResultOk;
    }
    virtual Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID _iid, void** obj) override {
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
};

class ShmEventList : public Steinberg::Vst::IEventList {
public:
    ShmEventList() : ref_count(1), count(0) {}

    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IEventList_iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    virtual Steinberg::uint32 PLUGIN_API addRef() override { return ++ref_count; }
    virtual Steinberg::uint32 PLUGIN_API release() override {
        uint32_t c = --ref_count;
        return c;
    }

    virtual Steinberg::int32 PLUGIN_API getEventCount() override { return count; }
    virtual Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& e) override {
        if (index < 0 || index >= count) return Steinberg::kResultFalse;
        e = events[index];
        return Steinberg::kResultOk;
    }
    virtual Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& e) override {
        if (count >= 256) return Steinberg::kResultFalse;
        events[count++] = e;
        return Steinberg::kResultOk;
    }

    void clear() { count = 0; }

private:
    std::atomic<uint32_t> ref_count;
    int32_t count;
    Steinberg::Vst::Event events[256];
};

static void deserialize_shm_to_guest_events(AudioSharedMemory* layout, ShmEventList& inputEvents) {
    inputEvents.clear();
    uint32_t count = layout->midi_in_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        const ShmMidiEvent& shm_ev = layout->midi_in[i];
        if (shm_ev.size >= 3) {
            uint8_t status = shm_ev.data[0] & 0xF0;
            uint8_t channel = shm_ev.data[0] & 0x0F;
            Event ev;
            memset(&ev, 0, sizeof(Event));
            ev.sampleOffset = shm_ev.sample_offset;
            ev.busIndex = 0;

            if (status == 0x90 && shm_ev.data[2] > 0) {
                ev.type = Event::kNoteOnEvent;
                ev.noteOn.channel = channel;
                ev.noteOn.pitch = shm_ev.data[1] & 0x7F;
                ev.noteOn.velocity = (float)shm_ev.data[2] / 127.0f;
                ev.noteOn.tuning = 0.0f;
                ev.noteOn.noteId = -1;
                inputEvents.addEvent(ev);
            } else if (status == 0x80 || (status == 0x90 && shm_ev.data[2] == 0)) {
                ev.type = Event::kNoteOffEvent;
                ev.noteOff.channel = channel;
                ev.noteOff.pitch = shm_ev.data[1] & 0x7F;
                ev.noteOff.velocity = (float)shm_ev.data[2] / 127.0f;
                ev.noteOff.tuning = 0.0f;
                ev.noteOff.noteId = -1;
                inputEvents.addEvent(ev);
            } else if (status == 0xA0) {
                ev.type = Event::kPolyPressureEvent;
                ev.polyPressure.channel = channel;
                ev.polyPressure.pitch = shm_ev.data[1] & 0x7F;
                ev.polyPressure.pressure = (float)shm_ev.data[2] / 127.0f;
                ev.polyPressure.noteId = -1;
                inputEvents.addEvent(ev);
            }
        }
    }
}

static void serialize_guest_events_to_shm(ShmEventList& outputEvents, AudioSharedMemory* layout) {
    int32 count = outputEvents.getEventCount();
    if (count <= 0) {
        layout->midi_out_count.store(0, std::memory_order_release);
        return;
    }
    uint32_t shm_count = 0;
    for (int32 i = 0; i < count && shm_count < 256; ++i) {
        Event ev;
        if (outputEvents.getEvent(i, ev) == kResultOk) {
            ShmMidiEvent& shm_ev = layout->midi_out[shm_count];
            shm_ev.sample_offset = ev.sampleOffset;
            shm_ev.size = 0;

            if (ev.type == Event::kNoteOnEvent) {
                shm_ev.size = 3;
                shm_ev.data[0] = 0x90 | (ev.noteOn.channel & 0x0F);
                shm_ev.data[1] = ev.noteOn.pitch & 0x7F;
                shm_ev.data[2] = (uint8_t)(ev.noteOn.velocity * 127.0f) & 0x7F;
                shm_count++;
            } else if (ev.type == Event::kNoteOffEvent) {
                shm_ev.size = 3;
                shm_ev.data[0] = 0x80 | (ev.noteOff.channel & 0x0F);
                shm_ev.data[1] = ev.noteOff.pitch & 0x7F;
                shm_ev.data[2] = (uint8_t)(ev.noteOff.velocity * 127.0f) & 0x7F;
                shm_count++;
            } else if (ev.type == Event::kPolyPressureEvent) {
                shm_ev.size = 3;
                shm_ev.data[0] = 0xA0 | (ev.polyPressure.channel & 0x0F);
                shm_ev.data[1] = ev.polyPressure.pitch & 0x7F;
                shm_ev.data[2] = (uint8_t)(ev.polyPressure.pressure * 127.0f) & 0x7F;
                shm_count++;
            }
        }
    }
    layout->midi_out_count.store(shm_count, std::memory_order_release);
}

class EmptyParameterChanges : public Steinberg::Vst::IParameterChanges {
public:
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IParameterChanges_iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    virtual Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    virtual Steinberg::uint32 PLUGIN_API release() override { return 1; }

    virtual Steinberg::int32 PLUGIN_API getParameterCount() override { return 0; }
    virtual Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override { return nullptr; }
    virtual Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override { return nullptr; }
};

int main(int argc, char* argv[]) {

    ArthurHostApplication hostApp;
    ShmEventList emptyInputEvents;
    ShmEventList emptyOutputEvents;
    EmptyParameterChanges emptyInputParams;
    EmptyParameterChanges emptyOutputParams;
    if (argc < 3) {
        std::cout << "Usage: win_guest_agent.exe <shm_path_or_device> <vst3_dll_path> [slot_idx] [stride]\n"
                  << "Example (VM mode):   win_guest_agent.exe \\\\.\\IVSHMEM C:\\Plugins\\Synth.vst3 0 2097152\n"
                  << "Example (Wine mode): win_guest_agent.exe arthur_shm C:\\Plugins\\Synth.vst3 0 2097152\n";
        return 1;
    }

    std::string shm_path = argv[1];
    std::string plugin_path = argv[2];
    uint32_t slot_idx = 0;
    uint64_t stride = 0;
    if (argc >= 5) {
        try {
            slot_idx = (uint32_t)std::stoul(argv[3]);
            stride = (uint64_t)std::stoull(argv[4]);
        } catch (...) {
            std::cerr << "[WARNING] Failed to parse slot_idx or stride. Using defaults." << std::endl;
        }
    }

    std::cout << ">>> Arthur Windows Guest Agent Started." << std::endl;
    std::cout << ">>> VST3 Plugin: " << plugin_path << std::endl;
    std::cout << ">>> VDC Slot index: " << slot_idx << " | Stride: " << stride << " bytes" << std::endl;

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
            char* base_ptr = (char*)mmap_info.ptr;
            layout = (AudioSharedMemory*)(base_ptr + (uint64_t)slot_idx * stride);
            std::cout << ">>> [OK] IVSHMEM base mapped at " << mmap_info.ptr << ", VDC layout set at " << layout << " (Offset: " << (uint64_t)slot_idx * stride << ")" << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to map IVSHMEM device memory: " << GetLastError() << std::endl;
            CloseHandle(hDevice);
            return 1;
        }
    } else {
        // Named Shared Memory Mode (Wine / local testing fallback)
        std::string unix_shm_path = "Z:\\dev\\shm\\" + shm_path;
        std::string direct_unix_path = "\\??\\unix\\dev\\shm\\" + shm_path;
        std::string global_mapping_name = "Global\\" + shm_path;

        std::cout << ">>> Local/Wine mode: Opening " << unix_shm_path << " via Wine Z: drive mapping..." << std::endl;
        HANDLE hFile = CreateFileA(unix_shm_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            hFile = CreateFileA(direct_unix_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (hFile == INVALID_HANDLE_VALUE) {
            std::cerr << "[ERROR] Could not open shared memory file " << unix_shm_path << " (Error: " << GetLastError() << ")" << std::endl;
            return 1;
        }

        hMapFile = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
        if (hMapFile == NULL) {
            std::cerr << "[ERROR] CreateFileMappingA failed: " << GetLastError() << std::endl;
            CloseHandle(hFile);
            return 1;
        }

        uint64_t offset = (uint64_t)slot_idx * stride;
        DWORD offset_high = (DWORD)(offset >> 32);
        DWORD offset_low = (DWORD)(offset & 0xFFFFFFFF);

        layout = (AudioSharedMemory*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, offset_high, offset_low, sizeof(AudioSharedMemory));
        if (!layout) {
            std::cerr << "[ERROR] MapViewOfFile failed: " << GetLastError() << std::endl;
            CloseHandle(hMapFile);
            CloseHandle(hFile);
            return 1;
        }
        std::cout << ">>> [OK] " << global_mapping_name << " mapped at " << layout << " with offset " << offset << std::endl;
    }

    if (layout->version != AudioSharedMemory::SHM_VERSION) {
        std::cerr << "[ERROR] SHM Layout Version Mismatch! Expected " << AudioSharedMemory::SHM_VERSION << " but got " << layout->version << std::endl;
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        if (hMapFile) CloseHandle(hMapFile);
        return 1;
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
            std::string cat = info.category;
            if (cat == "Audio Effect" || cat == "Audio Module Class" || cat == "Instrument" ||
                cat.find("Effect") != std::string::npos || cat.find("Instrument") != std::string::npos) {
                memcpy(classId, info.cid, sizeof(TUID));
                found = true;
                std::cout << "    => Selected class: \"" << info.name << "\" (Category: " << info.category << ")" << std::endl;
                break;
            }
        }
    }

    if (!found) {
        std::cerr << "[ERROR] No Class with compatible category ('Audio Effect', 'Audio Module Class', or 'Instrument') found in this plugin!" << std::endl;
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

    // Initialize component with host context
    if (component->initialize(&hostApp) != kResultOk) {
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

    // Spin-wait for valid sample rate (with 5s timeout)
    std::cout << ">>> Waiting for valid sample rate from host..." << std::endl;
    auto spin_start = std::chrono::high_resolution_clock::now();
    double rate = 0.0;
    while (true) {
        rate = layout->sample_rate.load(std::memory_order_seq_cst);
        if (rate > 0.0) {
            break;
        }
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - spin_start).count() > 5000) {
            std::cout << ">>> [WARNING] Timeout waiting for host sample rate. Defaulting to 48000.0." << std::endl;
            rate = 48000.0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << ">>> Host sample rate: " << rate << " Hz" << std::endl;

    // 6. Setup Audio Processing configuration
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = SHM_MAX_SAMPLES;
    setup.sampleRate = rate;

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

    uint32_t p_latency = audioProcessor->getLatencySamples();
    layout->plugin_latency.store(p_latency, std::memory_order_release);
    std::cout << ">>> [OK] Plugin fully activated (Internal Latency: " << p_latency << " samples). Entering Real-Time Processing Loop." << std::endl;

    // Pin the guest processing thread to isolated cores
    pin_to_isolated_cores();

    // Set high thread priority for real-time processing under Wine
    #ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    #endif

    // GUI View tracking variables
    IPlugView* pluginView = nullptr;
    HWND dummyHwnd = NULL;
    bool editor_open = false;

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

        // GUI Editor check when idle
        if (current_state == TransportState::STATE_IDLE) {
            // Check for open request
            if (layout->request_open_editor.load(std::memory_order_acquire) == 1 && !editor_open) {
                // Query EditController from IComponent
                IEditController* editController = nullptr;
                if (component->queryInterface(IEditController::iid, (void**)&editController) == kResultOk && editController) {
                    pluginView = editController->createView(ViewType::kEditor);
                    if (!pluginView) {
                        pluginView = editController->createView("editor");
                    }
                    if (pluginView) {
                        if (pluginView->isPlatformTypeSupported(kPlatformTypeHWND) == kResultOk) {
                            dummyHwnd = CreateWindowExA(
                                0, "STATIC", "ArthurPluginParent", WS_POPUP,
                                0, 0, 800, 600,
                                NULL, NULL, GetModuleHandle(NULL), NULL
                            );
                            if (dummyHwnd) {
                                if (pluginView->attached(dummyHwnd, kPlatformTypeHWND) == kResultOk) {
                                    // Retrieve wine X11 window
                                    uint64_t wine_xid = 0;
                                    int retries = 0;
                                    while (wine_xid == 0 && retries < 10) {
                                        wine_xid = (uint64_t)(uintptr_t)GetPropA(dummyHwnd, "__wine_x11_whole_window");
                                        if (wine_xid == 0) {
                                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                            retries++;
                                        }
                                    }
                                    if (wine_xid != 0) {
                                        ViewRect rect;
                                        if (pluginView->getSize(&rect) == kResultOk) {
                                            uint32_t w = rect.getWidth();
                                            uint32_t h = rect.getHeight();
                                            layout->guest_window_xid.store(wine_xid, std::memory_order_release);
                                            layout->editor_width.store(w, std::memory_order_release);
                                            layout->editor_height.store(h, std::memory_order_release);
                                            layout->editor_open_status.store(1, std::memory_order_release);
                                            editor_open = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    editController->release();
                }
                if (!editor_open) {
                    layout->editor_open_status.store(0, std::memory_order_release);
                    if (pluginView) { pluginView->release(); pluginView = nullptr; }
                    if (dummyHwnd) { DestroyWindow(dummyHwnd); dummyHwnd = NULL; }
                }
            }

            // Check for close request
            if (layout->request_close_editor.load(std::memory_order_acquire) == 1 && editor_open) {
                if (pluginView) {
                    pluginView->removed();
                    pluginView->release();
                    pluginView = nullptr;
                }
                if (dummyHwnd) {
                    DestroyWindow(dummyHwnd);
                    dummyHwnd = NULL;
                }
                layout->guest_window_xid.store(0, std::memory_order_release);
                layout->editor_open_status.store(0, std::memory_order_release);
                editor_open = false;
            }

            std::this_thread::yield();
        }

        // Spin lock waiting for the host to write the next buffer
        if (current_state == TransportState::STATE_HOST_WRITTEN) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            
            // Set up ProcessData buffers
            ProcessData processData;
            processData.processMode = kRealtime;
            processData.symbolicSampleSize = kSample32;
            processData.numSamples = layout->sample_count.load(std::memory_order_relaxed);
            processData.numInputs = inputBuses;
            processData.numOutputs = outputBuses;

            // Stack arrays for real-time safety (no std::vector allocation)
            AudioBusBuffers inputBusesBuffers[32]; // 32 input buses max
            float* inputChannelPtrs[SHM_MAX_CHANNELS];
            
            uint32_t numInputs = std::min((uint32_t)layout->num_inputs.load(std::memory_order_relaxed), SHM_MAX_CHANNELS);
            uint32_t effectiveInputBuses = std::min((uint32_t)inputBuses, 32u);

            if (effectiveInputBuses > 0) {
                for (uint32_t b = 0; b < effectiveInputBuses; ++b) {
                    inputBusesBuffers[b].numChannels = 0;
                    inputBusesBuffers[b].channelBuffers32 = nullptr;
                    inputBusesBuffers[b].silenceFlags = 0;
                }
                for (uint32_t c = 0; c < numInputs; ++c) {
                    inputChannelPtrs[c] = layout->input_buffers[c];
                }
                inputBusesBuffers[0].numChannels = numInputs;
                inputBusesBuffers[0].channelBuffers32 = inputChannelPtrs;
                processData.inputs = inputBusesBuffers;
            } else {
                processData.inputs = nullptr;
            }

            // Stack arrays for output
            AudioBusBuffers outputBusesBuffers[32]; // 32 output buses max
            float* outputChannelPtrs[SHM_MAX_CHANNELS];

            uint32_t numOutputs = std::min((uint32_t)layout->num_outputs.load(std::memory_order_relaxed), SHM_MAX_CHANNELS);
            uint32_t effectiveOutputBuses = std::min((uint32_t)outputBuses, 32u);

            if (effectiveOutputBuses > 0) {
                for (uint32_t b = 0; b < effectiveOutputBuses; ++b) {
                    outputBusesBuffers[b].numChannels = 0;
                    outputBusesBuffers[b].channelBuffers32 = nullptr;
                    outputBusesBuffers[b].silenceFlags = 0;
                }
                for (uint32_t c = 0; c < numOutputs; ++c) {
                    outputChannelPtrs[c] = layout->output_buffers[c];
                }
                outputBusesBuffers[0].numChannels = numOutputs;
                outputBusesBuffers[0].channelBuffers32 = outputChannelPtrs;
                processData.outputs = outputBusesBuffers;
            } else {
                processData.outputs = nullptr;
            }

            // Handlers for parameters and events with MIDI deserialization/serialization
            ShmEventList inputEvents;
            ShmEventList outputEvents;
            deserialize_shm_to_guest_events(layout, inputEvents);

            processData.inputEvents = &inputEvents;
            processData.outputEvents = &outputEvents;
            processData.inputParameterChanges = &emptyInputParams;
            processData.outputParameterChanges = &emptyOutputParams;

            // Process audio through the VST3 plugin!
            audioProcessor->process(processData);

            serialize_guest_events_to_shm(outputEvents, layout);

            // Notify Host
            layout->state.store(TransportState::STATE_GUEST_PROCESSED, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    // 8. Clean up
    audioProcessor->setProcessing(false);
    component->setActive(false);
    component->terminate();
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
