#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/base/ustring.h>
#include <cstring>
#include <string>
#include <atomic>
#include <dlfcn.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cmath>
#include <sys/types.h>
#include <signal.h>
#include "AudioIPC.h"
#include <iostream>
#include <sstream>
#ifndef SMTG_EXPORT_SYMBOL
#define SMTG_EXPORT_SYMBOL __attribute__ ((visibility ("default")))
#endif

using namespace Steinberg;

using CLLSSyncLayout = arthur::CLLSSyncLayout;

static inline float get_delayed_sample(const float *history, int write_idx, float delay) {
    if (delay < 1.0f) delay = 1.0f;
    int I = (int)std::floor(delay);
    float d = delay - (float)I;

    int idx_m1 = (write_idx - I + 1 + 8192) % 8192;
    int idx_0  = (write_idx - I     + 8192) % 8192;
    int idx_p1 = (write_idx - I - 1 + 8192) % 8192;
    int idx_p2 = (write_idx - I - 2 + 8192) % 8192;

    float s_m1 = history[idx_m1];
    float s_0  = history[idx_0];
    float s_p1 = history[idx_p1];
    float s_p2 = history[idx_p2];

    float c_m1 = -d * (d - 1.0f) * (d - 2.0f) / 6.0f;
    float c_0  = (d + 1.0f) * (d - 1.0f) * (d - 2.0f) / 2.0f;
    float c_p1 = -(d + 1.0f) * d * (d - 2.0f) / 2.0f;
    float c_p2 = (d + 1.0f) * d * (d - 1.0f) / 6.0f;

    return c_m1 * s_m1 + c_0 * s_0 + c_p1 * s_p1 + c_p2 * s_p2;
}

// Global identity that changes based on the filename
static char g_plugin_name[256] = "Arthur Bridge";
static TUID g_plugin_cid = {0};

static void initialize_identity() {
    Dl_info info;
    if (dladdr((void*)initialize_identity, &info) && info.dli_fname) {
        char* bname = strdup(info.dli_fname);
        if (!bname) return;
        char* fname = basename(bname);
        
        strncpy(g_plugin_name, fname, sizeof(g_plugin_name)-1);
        g_plugin_name[sizeof(g_plugin_name)-1] = '\0';
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

// Helper to resolve the daemon socket path
static std::string get_socket_path() {
    const char* custom = getenv("ARTHUR_SOCK");
    if (custom) return std::string(custom);
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) return std::string(xdg) + "/arthur.sock";
    return "/tmp/arthur.sock";
}

class ArthurBridgePlugin;

// ---------------------------------------------------------------------------
// ArthurBridgeView: Custom IPlugView implementing host-side X11 reparenting
// ---------------------------------------------------------------------------
class ArthurBridgeView : public IPlugView {
public:
    ArthurBridgeView(ArthurBridgePlugin* plugin);
    virtual ~ArthurBridgeView();

    // --- FUnknown ---
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, IPlugView::iid, 16) == 0 || memcmp(_iid, FUnknown::iid, 16) == 0) {
            *obj = static_cast<IPlugView*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override {
        return ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32 PLUGIN_API release() override;

    // --- IPlugView ---
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        if (strcmp(type, kPlatformTypeX11EmbedWindowID) == 0) {
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override;
    tresult PLUGIN_API removed() override;
    tresult PLUGIN_API onWheel(float distance) override { return kResultOk; }
    tresult PLUGIN_API onKeyDown(char16 key, int16 keyCode, int16 modifiers) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16 key, int16 keyCode, int16 modifiers) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect* size) override;
    tresult PLUGIN_API onSize(ViewRect* newSize) override;
    tresult PLUGIN_API onFocus(TBool state) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override {
        this->frame = frame;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultFalse; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override { return kResultOk; }

private:
    std::atomic<uint32_t> ref_count;
    ArthurBridgePlugin* plugin;
    IPlugFrame* frame;
    uint64_t host_xid;
    uint64_t guest_xid;
};

static void serialize_vst3_events_to_shm(Vst::IEventList* inputEvents, arthur::AudioSharedMemory* layout, uint32_t start_offset, uint32_t end_offset) {
    if (!inputEvents || !layout) return;
    int32 count = inputEvents->getEventCount();
    if (count <= 0) {
        layout->midi_in_count.store(0, std::memory_order_release);
        return;
    }
    uint32_t shm_count = 0;
    for (int32 i = 0; i < count && shm_count < 256; ++i) {
        Vst::Event ev;
        if (inputEvents->getEvent(i, ev) == kResultOk) {
            if ((uint32_t)ev.sampleOffset >= start_offset && (uint32_t)ev.sampleOffset < end_offset) {
                arthur::ShmMidiEvent& shm_ev = layout->midi_in[shm_count];
                shm_ev.sample_offset = ev.sampleOffset - start_offset;
                shm_ev.size = 0;

                if (ev.type == Vst::Event::kNoteOnEvent) {
                    shm_ev.size = 3;
                    shm_ev.data[0] = 0x90 | (ev.noteOn.channel & 0x0F);
                    shm_ev.data[1] = ev.noteOn.pitch & 0x7F;
                    shm_ev.data[2] = (uint8_t)(ev.noteOn.velocity * 127.0f) & 0x7F;
                    shm_count++;
                } else if (ev.type == Vst::Event::kNoteOffEvent) {
                    shm_ev.size = 3;
                    shm_ev.data[0] = 0x80 | (ev.noteOff.channel & 0x0F);
                    shm_ev.data[1] = ev.noteOff.pitch & 0x7F;
                    shm_ev.data[2] = (uint8_t)(ev.noteOff.velocity * 127.0f) & 0x7F;
                    shm_count++;
                } else if (ev.type == Vst::Event::kPolyPressureEvent) {
                    shm_ev.size = 3;
                    shm_ev.data[0] = 0xA0 | (ev.polyPressure.channel & 0x0F);
                    shm_ev.data[1] = ev.polyPressure.pitch & 0x7F;
                    shm_ev.data[2] = (uint8_t)(ev.polyPressure.pressure * 127.0f) & 0x7F;
                    shm_count++;
                } else if (ev.type == Vst::Event::kDataEvent && ev.data.type == Vst::DataEvent::kMidiSysEx) {
                    uint32 size = std::min(ev.data.size, 16u);
                    shm_ev.size = size;
                    memcpy(shm_ev.data, ev.data.bytes, size);
                    shm_count++;
                }
            }
        }
    }
    layout->midi_in_count.store(shm_count, std::memory_order_release);
}

static void deserialize_shm_events_to_vst3(arthur::AudioSharedMemory* layout, Vst::IEventList* outputEvents, uint32_t start_offset) {
    if (!layout || !outputEvents) return;
    uint32_t count = layout->midi_out_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        const arthur::ShmMidiEvent& shm_ev = layout->midi_out[i];
        if (shm_ev.size >= 3) {
            uint8_t status = shm_ev.data[0] & 0xF0;
            uint8_t channel = shm_ev.data[0] & 0x0F;
            Vst::Event ev;
            memset(&ev, 0, sizeof(Vst::Event));
            ev.sampleOffset = shm_ev.sample_offset + start_offset;
            ev.busIndex = 0;

            if (status == 0x90 && shm_ev.data[2] > 0) {
                ev.type = Vst::Event::kNoteOnEvent;
                ev.noteOn.channel = channel;
                ev.noteOn.pitch = shm_ev.data[1] & 0x7F;
                ev.noteOn.velocity = (float)shm_ev.data[2] / 127.0f;
                ev.noteOn.tuning = 0.0f;
                ev.noteOn.noteId = -1;
                outputEvents->addEvent(ev);
            } else if (status == 0x80 || (status == 0x90 && shm_ev.data[2] == 0)) {
                ev.type = Vst::Event::kNoteOffEvent;
                ev.noteOff.channel = channel;
                ev.noteOff.pitch = shm_ev.data[1] & 0x7F;
                ev.noteOff.velocity = (float)shm_ev.data[2] / 127.0f;
                ev.noteOff.tuning = 0.0f;
                ev.noteOff.noteId = -1;
                outputEvents->addEvent(ev);
            } else if (status == 0xA0) {
                ev.type = Vst::Event::kPolyPressureEvent;
                ev.polyPressure.channel = channel;
                ev.polyPressure.pitch = shm_ev.data[1] & 0x7F;
                ev.polyPressure.pressure = (float)shm_ev.data[2] / 127.0f;
                ev.polyPressure.noteId = -1;
                outputEvents->addEvent(ev);
            }
        }
    }
    layout->midi_out_count.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// ArthurBridgePlugin: Merged IComponent + IAudioProcessor + IEditController
// ---------------------------------------------------------------------------
class ArthurBridgePlugin : public Vst::IComponent, public Vst::IAudioProcessor, public Vst::IEditController {
public:
    ArthurBridgePlugin()
        : ref_count(1)
        , sampleRate(48000.0)
        , maxSamplesPerBlock(512)
        , numChannels(2)
        , layout(nullptr)
        , componentHandler(nullptr)
        , clls_shm_fd(-1)
        , clls_sync_layout(nullptr)
        , history_write_idx(0)
        , target_rtt_initialized(false)
        , static_target_rtt(0.0f)
    {
        memset(history_buffers, 0, sizeof(history_buffers));
        memset(smooth_delays, 0, sizeof(smooth_delays));
    }

    ~ArthurBridgePlugin() {
        disconnectFromDaemon();
        detach_clls_sync();
    }

    // --- FUnknown ---
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, Vst::IComponent::iid, 16) == 0 || memcmp(_iid, FUnknown::iid, 16) == 0) {
            *obj = static_cast<Vst::IComponent*>(this);
            addRef();
            return kResultOk;
        }
        if (memcmp(_iid, Vst::IAudioProcessor::iid, 16) == 0) {
            *obj = static_cast<Vst::IAudioProcessor*>(this);
            addRef();
            return kResultOk;
        }
        if (memcmp(_iid, Vst::IEditController::iid, 16) == 0) {
            *obj = static_cast<Vst::IEditController*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override {
        return ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32 PLUGIN_API release() override {
        uint32 newCount = ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newCount == 0) {
            delete this;
            return 0;
        }
        return newCount;
    }

    arthur::AudioSharedMemory* getLayout() const { return layout; }

    // --- IComponent ---
    tresult PLUGIN_API initialize(FUnknown*) override { 
        connectToDaemon();
        return kResultOk; 
    }
    tresult PLUGIN_API terminate() override { 
        disconnectFromDaemon();
        return kResultOk; 
    }
    tresult PLUGIN_API getControllerClassId(TUID classId) override;
    tresult PLUGIN_API setIoMode(Vst::IoMode) override { return kResultOk; }

    int32 PLUGIN_API getBusCount(Vst::MediaType type, Vst::BusDirection dir) override {
        if (type == Vst::MediaTypes::kAudio) {
            return (dir == Vst::kOutput) ? 2 : 1;
        }
        return 0;
    }

    tresult PLUGIN_API getBusInfo(Vst::MediaType type, Vst::BusDirection dir, int32 index, Vst::BusInfo& info) override {
        if (type != Vst::MediaTypes::kAudio) return kResultFalse;
        if (dir == Vst::kInput && index != 0) return kResultFalse;
        if (dir == Vst::kOutput && index > 1) return kResultFalse;

        memset(&info, 0, sizeof(info));
        info.mediaType = type;
        info.direction = dir;
        info.channelCount = numChannels;
        
        if (dir == Vst::kInput) {
            UString(info.name, 128).fromAscii("Main Input");
            info.busType = Vst::kMain;
            info.flags = Vst::BusInfo::kDefaultActive;
        } else {
            if (index == 0) {
                UString(info.name, 128).fromAscii("Main Output");
                info.busType = Vst::kMain;
                info.flags = Vst::BusInfo::kDefaultActive;
            } else {
                UString(info.name, 128).fromAscii("Monitor Output");
                info.busType = Vst::kAux;
                info.flags = 0;
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) override { return kResultFalse; }
    tresult PLUGIN_API activateBus(Vst::MediaType, Vst::BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    // --- IAudioProcessor ---
    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* in, int32 numIn, Vst::SpeakerArrangement* out, int32 numOut) override {
        // Extract channel count from the input arrangement if available
        if (numIn > 0 && in) {
            int32 chCount = Vst::SpeakerArr::getChannelCount(in[0]);
            if (chCount > 0) numChannels = chCount;
        } else if (numOut > 0 && out) {
            int32 chCount = Vst::SpeakerArr::getChannelCount(out[0]);
            if (chCount > 0) numChannels = chCount;
        }
        return kResultOk;
    }

    tresult PLUGIN_API getBusArrangement(Vst::BusDirection, int32, Vst::SpeakerArrangement&) override { return kResultFalse; }
    tresult PLUGIN_API canProcessSampleSize(int32) override { return kResultOk; }

    // Report real latency: the sub-block size is the minimum processing granularity,
    // plus any SHM round-trip overhead. Returning 0 causes the DAW to misalign audio.
    uint32 PLUGIN_API getLatencySamples() override {
        uint32_t p_latency = 0;
        if (layout) {
            p_latency = layout->plugin_latency.load(std::memory_order_acquire);
        }
        
        if (target_rtt_initialized) {
            return p_latency + (uint32_t)static_target_rtt;
        }
        
        if (clls_sync_layout) {
            float current_global = clls_sync_layout->global_target_rtt.load(std::memory_order_acquire);
            if (current_global > 0.0f) {
                static_target_rtt = current_global;
                target_rtt_initialized = true;
                return p_latency + (uint32_t)static_target_rtt;
            }
            
            float max_rtt = 0.0f;
            for (int i = 0; i < 8; ++i) {
                int32_t pid = clls_sync_layout->owner_pid[i].load(std::memory_order_acquire);
                if (pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH)) {
                    float other_rtt = clls_sync_layout->measured_rtt[i].load(std::memory_order_acquire);
                    if (other_rtt > max_rtt) {
                        max_rtt = other_rtt;
                    }
                }
            }
            if (max_rtt > 0.0f) {
                float target_rtt = max_rtt + 128.0f;
                if (target_rtt > 7936.0f) target_rtt = 7936.0f;
                return p_latency + (uint32_t)target_rtt;
            }
        }
        return p_latency;
    }

    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) override { 
        this->sampleRate = setup.sampleRate;
        this->maxSamplesPerBlock = setup.maxSamplesPerBlock;
        return kResultOk; 
    }

    tresult PLUGIN_API setProcessing(TBool state) override { 
        return kResultOk; 
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) override {
        if (!layout) return kResultOk;
        
        layout->sample_rate.store(this->sampleRate, std::memory_order_relaxed);
        
        // 1. Detect thread priority and block type
        int policy = SCHED_OTHER;
        struct sched_param param;
        pthread_getschedparam(pthread_self(), &policy, &param);
        
        // A block is considered playback/pre-fetch if it is not a real-time priority thread
        bool is_playback = (policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE);

        if (is_playback) {
            // Playback pre-fetch path: segment into sub-blocks
            uint32_t total_samples = (uint32_t)data.numSamples;
            // Ceiling division: ensures ALL samples are processed even when
            // total_samples is not a multiple of SUB_BLOCK_SIZE.
            uint32_t num_sub_blocks = (total_samples + SUB_BLOCK_SIZE - 1) / SUB_BLOCK_SIZE;
            if (num_sub_blocks == 0) num_sub_blocks = 1;

            uint64_t block_start_time_ns = arthur::get_monotonic_time_ns();
            int64_t block_start_pos = data.processContext ? data.processContext->projectTimeSamples : -1; // Playhead position from DAW if available

            for (uint32_t sb = 0; sb < num_sub_blocks; ++sb) {
                uint32_t offset = sb * SUB_BLOCK_SIZE;
                uint32_t current_count = std::min(SUB_BLOCK_SIZE, total_samples - offset);

                layout->sample_count.store(current_count, std::memory_order_relaxed);
                layout->num_inputs.store(0, std::memory_order_relaxed);
                layout->num_outputs.store(0, std::memory_order_relaxed);

                // Copy input sub-block to SHM
                if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
                    uint32_t nIn = std::min((uint32_t)data.inputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                    layout->num_inputs.store(nIn, std::memory_order_relaxed);
                    for (uint32_t c = 0; c < nIn; ++c) {
                        memcpy(layout->input_buffers[c], &data.inputs[0].channelBuffers32[c][offset], current_count * sizeof(float));
                    }
                }

                if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                    uint32_t nOut = std::min((uint32_t)data.outputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                    layout->num_outputs.store(nOut, std::memory_order_relaxed);
                }

                // Serialize MIDI events
                serialize_vst3_events_to_shm(data.inputEvents, layout, offset, offset + current_count);

                // Trigger Guest
                layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);

                // Time-based timeout scaled to 80% of the active buffer block duration
                uint64_t spin_start = arthur::get_monotonic_time_ns();
                uint64_t timeout_ns = (uint64_t)((current_count * 800000000.0) / this->sampleRate);
                if (timeout_ns == 0) timeout_ns = 500000; // fallback to 500us

                while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED) {
                    if (arthur::get_monotonic_time_ns() - spin_start > timeout_ns) {
                        break;
                    }
                    #if defined(__x86_64__) || defined(_M_X64)
                    asm volatile("pause" ::: "memory");
                    #endif
                }

                if (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED) {
                    layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    break; // Bypass rest on timeout
                }

                // Deserialize MIDI events
                deserialize_shm_events_to_vst3(layout, data.outputEvents, offset);

                // Copy output sub-block from SHM to DAW output buffer and pre-rendered playback ring buffer
                if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                    uint32_t nOut = layout->num_outputs.load(std::memory_order_relaxed);
                    for (uint32_t c = 0; c < nOut; ++c) {
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
            layout->sample_count.store((uint32_t)data.numSamples, std::memory_order_relaxed);
            layout->num_inputs.store(0, std::memory_order_relaxed);
            layout->num_outputs.store(0, std::memory_order_relaxed);

            if (data.numInputs > 0 && data.inputs[0].numChannels > 0) {
                uint32_t nIn = std::min((uint32_t)data.inputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                layout->num_inputs.store(nIn, std::memory_order_relaxed);
                for (uint32_t c = 0; c < nIn; ++c) {
                    memcpy(layout->input_buffers[c], data.inputs[0].channelBuffers32[c], data.numSamples * sizeof(float));
                }
            }

            if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                uint32_t nOut = std::min((uint32_t)data.outputs[0].numChannels, arthur::SHM_MAX_CHANNELS);
                layout->num_outputs.store(nOut, std::memory_order_relaxed);
            }

            // Serialize MIDI events
            serialize_vst3_events_to_shm(data.inputEvents, layout, 0, (uint32_t)data.numSamples);

            // Trigger Guest
            layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Time-based timeout for real-time live path
            uint64_t spin_start = arthur::get_monotonic_time_ns();
            uint64_t timeout_ns = (uint64_t)((data.numSamples * 800000000.0) / this->sampleRate);
            if (timeout_ns == 0) timeout_ns = 500000;

            while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED) {
                if (arthur::get_monotonic_time_ns() - spin_start > timeout_ns) {
                    break;
                }
                #if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
                #endif
            }

            if (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED) {
                layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                return kResultOk;
            }

            // Deserialize MIDI events
            deserialize_shm_events_to_vst3(layout, data.outputEvents, 0);

            // Copy output from SHM and sum with pre-rendered playback
            uint64_t now_ns = arthur::get_monotonic_time_ns();
            int64_t current_playhead_pos = data.processContext ? data.processContext->projectTimeSamples : -1;

            // Compute current CLLS target and measured RTT
            float max_rtt = 0.0f;
            if (clls_sync_layout) {
                for (int i = 0; i < 8; ++i) {
                    int32_t pid = clls_sync_layout->owner_pid[i].load(std::memory_order_acquire);
                    if (pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH)) {
                        float other_rtt = clls_sync_layout->measured_rtt[i].load(std::memory_order_acquire);
                        if (other_rtt > max_rtt) {
                            max_rtt = other_rtt;
                        }
                    }
                }

                float current_global = clls_sync_layout->global_target_rtt.load(std::memory_order_acquire);
                if (current_global == 0.0f && max_rtt > 0.0f) {
                    float new_target = max_rtt + 128.0f;
                    if (new_target > 7936.0f) new_target = 7936.0f;
                    clls_sync_layout->global_target_rtt.store(new_target, std::memory_order_release);
                    current_global = new_target;
                }

                if (current_global > 0.0f && (!target_rtt_initialized || static_target_rtt != current_global)) {
                    static_target_rtt = current_global;
                    target_rtt_initialized = true;
                    if (componentHandler) {
                        componentHandler->restartComponent(Vst::kLatencyChanged);
                    }
                }
            }

            float target_rtt = target_rtt_initialized ? static_target_rtt : 0.0f;
            float target_delay = target_rtt > max_rtt ? target_rtt - max_rtt : 0.0f;
            static constexpr float SMOOTH_COEFF = 0.001f;

            if (data.numOutputs > 0 && data.outputs[0].numChannels > 0) {
                uint32_t nOut = layout->num_outputs.load(std::memory_order_relaxed);
                bool has_monitor_bus = (data.numOutputs > 1 && data.outputs[1].numChannels > 0);
                uint32_t mode = layout->console_mode.load(std::memory_order_acquire);

                for (uint32_t c = 0; c < nOut; ++c) {
                    float* main_out_buf = data.outputs[0].channelBuffers32[c];
                    float* monitor_out_buf = has_monitor_bus ? data.outputs[1].channelBuffers32[c] : nullptr;
                    float* live_buf = layout->output_buffers[c];
                    
                    // Apply Lagrange fractional delay line to the live monitoring buffer (Decoupled Timing Horizon)
                    if (target_delay > 0.0f && c < 32) {
                        int w_idx = history_write_idx;
                        for (int32_t i = 0; i < data.numSamples; ++i) {
                            smooth_delays[c] += SMOOTH_COEFF * (target_delay - smooth_delays[c]);
                            history_buffers[c][w_idx] = live_buf[i];
                            live_buf[i] = get_delayed_sample(history_buffers[c], w_idx, smooth_delays[c]);
                            w_idx = (w_idx + 1) % 8192;
                        }
                    }

                    if (mode == 1) { // MODE_RECORD_DRY_MONITOR_WET
                        // Route raw input (dry) to Main Output for recording
                        if (data.numInputs > 0 && c < (uint32_t)data.inputs[0].numChannels) {
                            memcpy(main_out_buf, data.inputs[0].channelBuffers32[c], data.numSamples * sizeof(float));
                        } else {
                            memset(main_out_buf, 0, data.numSamples * sizeof(float));
                        }
                        // Route processed wet output to Monitor Output
                        if (monitor_out_buf) {
                            memcpy(monitor_out_buf, live_buf, data.numSamples * sizeof(float));
                        }
                    } else { // MODE_PLAYBACK (0) or MODE_RECORD_WET (2)
                        // Route processed wet output to Main Output
                        memcpy(main_out_buf, live_buf, data.numSamples * sizeof(float));
                        // Route wet output to Monitor Output as well
                        if (monitor_out_buf) {
                            memcpy(monitor_out_buf, live_buf, data.numSamples * sizeof(float));
                        }
                    }
                    
                    // Sum main output with aligned pre-rendered playback blocks
                    arthur::sum_aligned_playback(layout, c, main_out_buf, (uint32_t)data.numSamples, now_ns, current_playhead_pos);
                }
                
                // Update history write index once for all channels
                if (target_delay > 0.0f) {
                    history_write_idx = (history_write_idx + data.numSamples) % 8192;
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
    static constexpr uint32_t SUB_BLOCK_SIZE = 32;

    void attach_clls_sync() {
        if (clls_sync_layout) return;
        clls_shm_fd = shm_open("/arthur_clls_sync", O_RDONLY, 0600);
        if (clls_shm_fd >= 0) {
            struct stat st;
            if (fstat(clls_shm_fd, &st) == 0 && st.st_size >= (off_t)sizeof(CLLSSyncLayout)) {
                void *ptr = mmap(NULL, sizeof(CLLSSyncLayout), PROT_READ, MAP_SHARED, clls_shm_fd, 0);
                if (ptr != MAP_FAILED) {
                    clls_sync_layout = static_cast<CLLSSyncLayout*>(ptr);
                    std::cout << "[OK] Arthur Bridge attached to CLLS Sync shared memory." << std::endl;
                }
            }
        }
    }

    void detach_clls_sync() {
        if (clls_sync_layout) {
            munmap(clls_sync_layout, sizeof(CLLSSyncLayout));
            clls_sync_layout = nullptr;
        }
        if (clls_shm_fd >= 0) {
            close(clls_shm_fd);
            clls_shm_fd = -1;
        }
    }

    void connectToDaemon() {
        if (layout) return;

        attach_clls_sync();

        std::string sock_path = get_socket_path();

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock == -1) return;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path)-1);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(sock);
            std::cerr << "[ERROR] Arthur Host failed to connect to daemon socket at " << sock_path << std::endl;
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
        } else if (resp.find("VDC_SLOT ") == 0) {
            std::stringstream ss(resp.substr(9));
            std::string shm_name;
            uint32_t slot_idx = 0;
            uint64_t stride = 0;
            if (ss >> shm_name >> slot_idx >> stride) {
                if (!shm_transport.attach(shm_name)) {
                    std::cerr << "[ERROR] Arthur Host failed to attach to VDC shared memory: " << shm_name << std::endl;
                    return;
                }
                char* base_ptr = (char*)shm_transport.get();
                layout = (arthur::AudioSharedMemory*)(base_ptr + slot_idx * stride);
                shm_name_used = shm_name;
                std::cout << "[OK] Arthur Host mapped VDC Slot " << slot_idx << " at offset " << slot_idx * stride << " inside " << shm_name << std::endl;
            }
        } else {
            std::cerr << "[ERROR] Arthur Host received error response from daemon: " << resp << std::endl;
        }
    }

    void disconnectFromDaemon() {
        if (!layout) return;

        detach_clls_sync();

        if (!shm_name_used.empty()) {
            std::string sock_path = get_socket_path();
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock != -1) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path)-1);
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

    // --- IEditController ---
    tresult PLUGIN_API setComponentState(IBStream* state) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 0; }
    tresult PLUGIN_API getParameterInfo(int32 paramIndex, Vst::ParameterInfo& info) override { return kResultFalse; }
    tresult PLUGIN_API getParamStringByValue(Vst::ParamID id, Vst::ParamValue valueNormalized, Vst::String128 string) override { return kResultFalse; }
    tresult PLUGIN_API getParamValueByString(Vst::ParamID id, Vst::TChar* string, Vst::ParamValue& valueNormalized) override { return kResultFalse; }
    Vst::ParamValue PLUGIN_API normalizedParamToPlain(Vst::ParamID id, Vst::ParamValue valueNormalized) override { return valueNormalized; }
    Vst::ParamValue PLUGIN_API plainParamToNormalized(Vst::ParamID id, Vst::ParamValue plainValue) override { return plainValue; }
    Vst::ParamValue PLUGIN_API getParamNormalized(Vst::ParamID id) override { return 0.0; }
    tresult PLUGIN_API setParamNormalized(Vst::ParamID id, Vst::ParamValue value) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(Vst::IComponentHandler* handler) override {
        componentHandler = handler;
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override;

    std::atomic<int32_t> ref_count;
    double sampleRate;
    int32 maxSamplesPerBlock;
    int32 numChannels;
    arthur::AudioTransport shm_transport;
    arthur::AudioSharedMemory* layout;
    std::string shm_name_used;

    Vst::IComponentHandler* componentHandler;
    int clls_shm_fd;
    CLLSSyncLayout* clls_sync_layout;
    float history_buffers[32][8192];
    float smooth_delays[32];
    int history_write_idx;
    bool target_rtt_initialized;
    float static_target_rtt;
};

// ---------------------------------------------------------------------------
// ArthurBridgeView Method Implementations
// ---------------------------------------------------------------------------
ArthurBridgeView::ArthurBridgeView(ArthurBridgePlugin* plugin)
    : ref_count(1), plugin(plugin), frame(nullptr), host_xid(0), guest_xid(0) {
    if (plugin) plugin->addRef();
}

ArthurBridgeView::~ArthurBridgeView() {
    if (plugin) plugin->release();
}

Steinberg::uint32 PLUGIN_API ArthurBridgeView::release() {
    uint32 newCount = ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (newCount == 0) {
        delete this;
        return 0;
    }
    return newCount;
}

tresult PLUGIN_API ArthurBridgeView::attached(void* parent, FIDString type) {
    if (strcmp(type, kPlatformTypeX11EmbedWindowID) != 0) {
        return kResultFalse;
    }
    host_xid = (uint64_t)(uintptr_t)parent;
    auto* layout = plugin->getLayout();
    if (!layout) return kResultFalse;

    // Signal guest to open editor
    layout->host_window_xid.store(host_xid, std::memory_order_release);
    layout->request_close_editor.store(0, std::memory_order_release);
    layout->request_open_editor.store(1, std::memory_order_release);

    // Spin-wait for guest to report open status (up to 2 seconds)
    uint64_t start_time = arthur::get_monotonic_time_ns();
    bool ready = false;
    while (arthur::get_monotonic_time_ns() - start_time < 2000000000ULL) {
        if (layout->editor_open_status.load(std::memory_order_acquire) == 1) {
            ready = true;
            break;
        }
        #if defined(__x86_64__) || defined(_M_X64)
        asm volatile("pause" ::: "memory");
        #endif
    }

    if (!ready) {
        std::cerr << "[ERROR] Arthur View connection timeout from guest agent." << std::endl;
        return kResultFalse;
    }

    guest_xid = layout->guest_window_xid.load(std::memory_order_acquire);
    if (guest_xid == 0) return kResultFalse;

    // Dynamic load of libX11 to perform reparenting
    void* x11_lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11_lib) {
        std::cerr << "[ERROR] Failed to load libX11.so.6 dynamically: " << dlerror() << std::endl;
        return kResultFalse;
    }

    typedef void* (*XOpenDisplayProc)(const char*);
    typedef int (*XReparentWindowProc)(void*, uint64_t, uint64_t, int, int);
    typedef int (*XMapWindowProc)(void*, uint64_t);
    typedef int (*XCloseDisplayProc)(void*);

    auto x_open_display = (XOpenDisplayProc)dlsym(x11_lib, "XOpenDisplay");
    auto x_reparent = (XReparentWindowProc)dlsym(x11_lib, "XReparentWindow");
    auto x_map = (XMapWindowProc)dlsym(x11_lib, "XMapWindow");
    auto x_close_display = (XCloseDisplayProc)dlsym(x11_lib, "XCloseDisplay");

    if (x_open_display && x_reparent && x_map && x_close_display) {
        void* display = x_open_display(nullptr);
        if (display) {
            x_reparent(display, guest_xid, host_xid, 0, 0);
            x_map(display, guest_xid);
            x_close_display(display);
            std::cout << "[OK] Arthur View successfully reparented Guest XID: " << guest_xid 
                      << " into Host XID: " << host_xid << std::endl;
        }
    }
    dlclose(x11_lib);

    return kResultOk;
}

tresult PLUGIN_API ArthurBridgeView::removed() {
    auto* layout = plugin->getLayout();
    if (layout) {
        layout->request_open_editor.store(0, std::memory_order_release);
        layout->request_close_editor.store(1, std::memory_order_release);
        layout->editor_open_status.store(0, std::memory_order_release);
    }
    guest_xid = 0;
    host_xid = 0;
    return kResultOk;
}

tresult PLUGIN_API ArthurBridgeView::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    auto* layout = plugin->getLayout();
    if (layout) {
        uint32_t w = layout->editor_width.load(std::memory_order_acquire);
        uint32_t h = layout->editor_height.load(std::memory_order_acquire);
        if (w > 0 && h > 0) {
            size->left = 0;
            size->top = 0;
            size->right = w;
            size->bottom = h;
            return kResultOk;
        }
    }
    size->left = 0;
    size->top = 0;
    size->right = 800;
    size->bottom = 600;
    return kResultOk;
}

tresult PLUGIN_API ArthurBridgeView::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;
    auto* layout = plugin->getLayout();
    if (layout) {
        layout->editor_width.store(newSize->getWidth(), std::memory_order_release);
        layout->editor_height.store(newSize->getHeight(), std::memory_order_release);
    }
    return kResultOk;
}

// --- ArthurBridgePlugin IEditController Method Implementations ---
IPlugView* PLUGIN_API ArthurBridgePlugin::createView(FIDString name) {
    if (strcmp(name, "editor") == 0 || strcmp(name, Vst::ViewType::kEditor) == 0) {
        return new ArthurBridgeView(this);
    }
    return nullptr;
}

tresult PLUGIN_API ArthurBridgePlugin::getControllerClassId(TUID classId) {
    memcpy(classId, g_plugin_cid, 16);
    return kResultOk;
}

class SimpleFactory : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (memcmp(_iid, IPluginFactory::iid, 16) == 0 || memcmp(_iid, IPluginFactory2::iid, 16) == 0 || memcmp(_iid, FUnknown::iid, 16) == 0) {
            *obj = (IPluginFactory2*)this;
            addRef();  // Must addRef before returning per COM contract
            return kResultOk;
        }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }  // Global singleton — fixed refcount
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
        // Both IComponent, IAudioProcessor, and IEditController are served by the same merged class.
        if (memcmp(iid, Vst::IComponent::iid, 16) == 0 ||
            memcmp(iid, Vst::IAudioProcessor::iid, 16) == 0 ||
            memcmp(iid, Vst::IEditController::iid, 16) == 0) {
            auto* plugin = new ArthurBridgePlugin();
            if (memcmp(iid, Vst::IComponent::iid, 16) == 0) {
                *obj = static_cast<Vst::IComponent*>(plugin);
            } else if (memcmp(iid, Vst::IAudioProcessor::iid, 16) == 0) {
                *obj = static_cast<Vst::IAudioProcessor*>(plugin);
            } else {
                *obj = static_cast<Vst::IEditController*>(plugin);
            }
            return kResultOk;
        }
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
