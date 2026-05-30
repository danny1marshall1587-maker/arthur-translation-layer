#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/base/ustring.h>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include "AudioIPC.h"
#include <iostream>

#ifndef SMTG_EXPORT_SYMBOL
#define SMTG_EXPORT_SYMBOL __attribute__ ((visibility ("default")))
#endif

using namespace Steinberg;

// Global identity that changes based on the filename
static char g_plugin_name[256] = "Arthur Bridge";
static TUID g_plugin_cid = {0};

static void initialize_identity() {
    Dl_info info;
    if (dladdr((void*)initialize_identity, &info) && info.dli_fname) {
        char* bname = strdup(info.dli_fname);
        char* fname = basename(bname);
        
        strncpy(g_plugin_name, fname, sizeof(g_plugin_name)-1);
        char* dot = strrchr(g_plugin_name, '.');
        if (dot) *dot = '\0';
        
        memset(g_plugin_cid, 0, 16);
        uint32_t hash = 0x811c9dc5;
        for (int i = 0; fname[i] != '\0'; i++) {
            hash ^= (uint8_t)fname[i];
            hash *= 0x01000193;
        }
        memcpy(g_plugin_cid, &hash, 4);
        memcpy(g_plugin_cid + 4, "ARTHURBRIDGE", 12);
        
        free(bname);
    }
}

class SimpleComponent : public Vst::IComponent {
public:
    SimpleComponent() : ref_count(1) {}
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Vst::IComponent::iid, 16) == 0 || memcmp(_iid, FUnknown::iid, 16) == 0) {
            *obj = (Vst::IComponent*)this; addRef(); return kResultOk;
        }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++ref_count; }
    uint32 PLUGIN_API release() override { if (--ref_count == 0) { delete this; return 0; } return ref_count; }
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
    tresult PLUGIN_API setIoMode(Vst::IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(Vst::MediaType, Vst::BusDirection) override { return 1; }
    tresult PLUGIN_API getBusInfo(Vst::MediaType type, Vst::BusDirection dir, int32 index, Vst::BusInfo& info) override {
        if (index != 0) return kResultFalse;
        memset(&info, 0, sizeof(info));
        info.mediaType = type;
        info.direction = dir;
        info.channelCount = 2;
        UString(info.name, 128).fromAscii("Main IO");
        info.busType = Vst::kMain;
        info.flags = Vst::BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) override { return kResultFalse; }
    tresult PLUGIN_API activateBus(Vst::MediaType, Vst::BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }
private:
    uint32 ref_count;
};

class SimpleProcessor : public Vst::IAudioProcessor {
public:
    SimpleProcessor() : ref_count(1), layout(nullptr) {}
    ~SimpleProcessor() {
        disconnectFromDaemon();
    }
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Vst::IAudioProcessor::iid, 16) == 0 || memcmp(_iid, FUnknown::iid, 16) == 0) {
            *obj = (Vst::IAudioProcessor*)this; addRef(); return kResultOk;
        }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++ref_count; }
    uint32 PLUGIN_API release() override { if (--ref_count == 0) { delete this; return 0; } return ref_count; }
    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* in, int32 numIn, Vst::SpeakerArrangement* out, int32 numOut) override { return kResultOk; }
    tresult PLUGIN_API getBusArrangement(Vst::BusDirection, int32, Vst::SpeakerArrangement&) override { return kResultFalse; }
    tresult PLUGIN_API canProcessSampleSize(int32) override { return kResultOk; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) override { 
        this->sampleRate = setup.sampleRate;
        this->maxSamplesPerBlock = setup.maxSamplesPerBlock;
        return kResultOk; 
    }
    tresult PLUGIN_API setProcessing(TBool state) override { 
        if (state) {
            connectToDaemon();
        } else {
            disconnectFromDaemon();
        }
        return kResultOk; 
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) override {
        if (!layout) return kResultOk;
        
        layout->sample_rate = this->sampleRate;
        
        // 1. Detect thread priority and block type
        int policy = SCHED_OTHER;
        struct sched_param param;
        pthread_getschedparam(pthread_self(), &policy, &param);
        
        // A block is considered playback/pre-fetch if it is not a real-time priority thread
        // or if the block size is larger than the 16-sample ultra-low-latency real-time size.
        bool is_playback = (policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE || data.numSamples > 16);

        if (is_playback) {
            // Playback pre-fetch path: segment into 32-sample sub-blocks
            uint32_t total_samples = (uint32_t)data.numSamples;
            uint32_t sub_block_size = 32;
            uint32_t num_sub_blocks = total_samples / sub_block_size;
            if (num_sub_blocks == 0) num_sub_blocks = 1; // Fallback if block size is smaller than 32

            uint64_t block_start_time_ns = arthur::get_monotonic_time_ns();
            int64_t block_start_pos = data.processContext ? data.processContext->projectTimeSamples : -1; // Playhead position from DAW if available

            for (uint32_t sb = 0; sb < num_sub_blocks; ++sb) {
                uint32_t offset = sb * sub_block_size;
                uint32_t current_count = std::min(sub_block_size, total_samples - offset);

                layout->sample_count = current_count;
                layout->num_inputs = 0;
                layout->num_outputs = 0;

                // Copy input sub-block to SHM
                if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
                    layout->num_inputs = std::min((uint32_t)data.inputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                    for (uint32_t c = 0; c < layout->num_inputs; ++c) {
                        memcpy(layout->input_buffers[c], &data.inputs[0].channelBuffers32[c][offset], current_count * sizeof(float));
                    }
                }

                if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                    layout->num_outputs = std::min((uint32_t)data.outputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                }

                // Trigger Guest
                layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);

                // Busy wait for Guest with timeout
                int timeout = 100000;
                while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED && --timeout > 0) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    #if defined(__x86_64__) || defined(_M_X64)
                    asm volatile("pause" ::: "memory");
                    #endif
                }

                if (timeout == 0) {
                    layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    break; // Bypass rest on timeout
                }

                // Copy output sub-block from SHM to DAW output buffer and pre-rendered playback ring buffer
                if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                    for (uint32_t c = 0; c < layout->num_outputs; ++c) {
                        memcpy(&data.outputs[0].channelBuffers32[c][offset], layout->output_buffers[c], current_count * sizeof(float));
                        
                        // Write to pre-rendered playback buffer for summing in real-time thread
                        arthur::write_pre_rendered_playback(layout, c, offset, layout->output_buffers[c], current_count, block_start_time_ns, block_start_pos);
                    }
                }

                // Release control back to IDLE
                layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
        } else {
            // Real-time live monitoring path: process synchronously (e.g. 16 samples)
            layout->sample_count = (uint32_t)data.numSamples;
            layout->num_inputs = 0;
            layout->num_outputs = 0;

            if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
                layout->num_inputs = std::min((uint32_t)data.inputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                for (uint32_t c = 0; c < layout->num_inputs; ++c) {
                    memcpy(layout->input_buffers[c], data.inputs[0].channelBuffers32[c], data.numSamples * sizeof(float));
                }
            }

            if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                layout->num_outputs = std::min((uint32_t)data.outputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
            }

            // Trigger Guest
            layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Busy wait for Guest
            int timeout = 100000;
            while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED && --timeout > 0) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                #if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
                #endif
            }

            if (timeout == 0) {
                layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                return kResultOk;
            }

            // Copy output from SHM and sum with pre-rendered playback
            uint64_t now_ns = arthur::get_monotonic_time_ns();
            int64_t current_playhead_pos = data.processContext ? data.processContext->projectTimeSamples : -1;

            if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                for (uint32_t c = 0; c < layout->num_outputs; ++c) {
                    // Start by getting live monitoring output
                    memcpy(data.outputs[0].channelBuffers32[c], layout->output_buffers[c], data.numSamples * sizeof(float));
                    
                    // Sum with aligned pre-rendered playback blocks
                    arthur::sum_aligned_playback(layout, c, data.outputs[0].channelBuffers32[c], (uint32_t)data.numSamples, now_ns, current_playhead_pos);
                }
            }

            // Release control back to IDLE
            layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }

        return kResultOk;
    }

    uint32 PLUGIN_API getTailSamples() override { return 0; }

private:
    void connectToDaemon() {
        if (layout) return;

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock == -1) return;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/arthur.sock", sizeof(addr.sun_path)-1);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(sock);
            std::cerr << "[ERROR] Arthur Host failed to connect to daemon socket." << std::endl;
            return;
        }

        std::string msg = "GET_SHM " + std::string(g_plugin_name);
        send(sock, msg.c_str(), msg.length(), 0);

        char buffer[256];
        int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
        close(sock);

        if (bytes <= 0) {
            std::cerr << "[ERROR] Arthur Host received empty response from daemon." << std::endl;
            return;
        }
        buffer[bytes] = '\0';
        std::string resp(buffer);

        if (resp.find("SHM ") == 0) {
            std::string shm_name = resp.substr(4);

            if (!shm_transport.attach(shm_name)) {
                std::cerr << "[ERROR] Arthur Host failed to attach to shared memory: " << shm_name << std::endl;
                return;
            }

            layout = shm_transport.get();
            if (layout) {
                shm_name_used = shm_name;
                std::cout << "[OK] Arthur Host attached to: " << shm_name << std::endl;
            }
        } else {
            std::cerr << "[ERROR] Arthur Host received error response from daemon: " << resp << std::endl;
        }
    }

    void disconnectFromDaemon() {
        if (!layout) return;

        if (!shm_name_used.empty()) {
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock != -1) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, "/tmp/arthur.sock", sizeof(addr.sun_path)-1);
                if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != -1) {
                    std::string msg = "RELEASE " + shm_name_used;
                    send(sock, msg.c_str(), msg.length(), 0);
                }
                close(sock);
            }
            shm_name_used = "";
        }

        shm_transport.detach();
        layout = nullptr;
    }

    uint32 ref_count;
    double sampleRate;
    int32 maxSamplesPerBlock;
    arthur::AudioTransport shm_transport;
    arthur::AudioSharedMemory* layout;
    std::string shm_name_used;
};

class SimpleFactory : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, IPluginFactory::iid, 16) == 0 || memcmp(_iid, IPluginFactory2::iid, 16) == 0 || memcmp(_iid, FUnknown::iid, 16) == 0) {
            *obj = (IPluginFactory2*)this; return kResultOk;
        }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        memset(info, 0, sizeof(PFactoryInfo)); strncpy(info->vendor, "Arthur", PFactoryInfo::kNameSize); return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0) return kResultFalse;
        memset(info, 0, sizeof(PClassInfo));
        memcpy(info->cid, g_plugin_cid, 16);
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize);
        strncpy(info->name, g_plugin_name, PClassInfo::kNameSize);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (memcmp(iid, Vst::IComponent::iid, 16) == 0) { *obj = (Vst::IComponent*)new SimpleComponent(); return kResultOk; }
        if (memcmp(iid, Vst::IAudioProcessor::iid, 16) == 0) { *obj = (Vst::IAudioProcessor*)new SimpleProcessor(); return kResultOk; }
        return kNoInterface;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (index != 0) return kResultFalse;
        memset(info, 0, sizeof(PClassInfo2));
        memcpy(info->cid, g_plugin_cid, 16);
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->category, kVstAudioEffectClass, sizeof(info->category));
        strncpy(info->name, g_plugin_name, sizeof(info->name));
        strncpy(info->vendor, "Arthur", sizeof(info->vendor));
        strncpy(info->version, "1.0.0", sizeof(info->version));
        strncpy(info->sdkVersion, "VST 3.7.0", sizeof(info->sdkVersion));
        return kResultOk;
    }
};

static SimpleFactory gFactory;

extern "C" {
    SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* GetPluginFactory() { 
        if (g_plugin_cid[0] == 0) initialize_identity();
        return &gFactory; 
    }
    SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { 
        initialize_identity();
        return true; 
    }
    SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
}
