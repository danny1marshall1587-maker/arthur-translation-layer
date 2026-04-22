#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/base/ustring.h>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include "ipc/shm_buffer.h"

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
    SimpleProcessor() : ref_count(1), shm(nullptr) {}
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
        if (state) connectToDaemon();
        return kResultOk; 
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) override {
        if (!shm || !shm->isValid()) return kResultOk;
        
        auto* layout = shm->get();
        layout->header.sampleRate = (uint32_t)this->sampleRate;
        layout->header.bufferSize = (uint32_t)data.numSamples;
        layout->header.channels = 2;

        // Copy input to SHM
        if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
            for (int c = 0; c < std::min(2, (int)data.inputs[0].numChannels); ++c) {
                memcpy(layout->input[c], data.inputs[0].channelBuffers32[c], data.numSamples * sizeof(float));
            }
        }

        // Trigger Guest
        layout->header.hostReady.store(true);
        
        // Busy wait for Guest (with timeout)
        int timeout = 10000;
        while (!layout->header.guestReady.load() && --timeout > 0) {
            asm volatile("pause" ::: "memory");
        }

        // Copy output from SHM
        if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
            for (int c = 0; c < std::min(2, (int)data.outputs[0].numChannels); ++c) {
                memcpy(data.outputs[0].channelBuffers32[c], layout->output[c], data.numSamples * sizeof(float));
            }
        }

        layout->header.hostReady.store(false);
        layout->header.guestReady.store(false);

        return kResultOk;
    }

    uint32 PLUGIN_API getTailSamples() override { return 0; }

private:
    void connectToDaemon() {
        if (shm) return;
        
        std::string shm_name = "/arthur_" + std::string(g_plugin_name);
        shm = new Arthur::ShmBuffer(shm_name, true);
        
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock != -1) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, "/tmp/arthur.sock", sizeof(addr.sun_path)-1);
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != -1) {
                std::string msg = "LOAD " + shm_name + " " + std::string(g_plugin_name);
                send(sock, msg.c_str(), msg.length(), 0);
            }
            close(sock);
        }
    }

    uint32 ref_count;
    double sampleRate;
    int32 maxSamplesPerBlock;
    Arthur::ShmBuffer* shm;
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
