/**
 * Arthur Guest — Wine-side Plugin Host
 * 
 * This executable runs inside Wine. It loads a Windows VST3 plugin DLL,
 * queries its factory for class information, and communicates back to the
 * Linux-side bridge over a Unix domain socket.
 * 
 * Usage: wine arthur-guest.exe <plugin_dll_path> <socket_path>
 * 
 * Architecture:
 *   REAPER (Linux) → arthur_bridge.so → Unix Socket → arthur-guest.exe → Plugin.dll
 * 
 * NOTE: We must include winsock2.h BEFORE windows.h to avoid header collisions.
 * The Guest uses POSIX file I/O (read/write on fd) which Wine supports natively,
 * but we avoid mixing Linux sys/socket.h with Wine's windows.h.
 */

// Wine/Windows headers MUST come first
// NOMINMAX prevents windows.h from defining min/max macros that break std::min/std::max
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ole2.h>
#undef near
#undef far

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include "AudioIPC.h"

// POSIX — only use headers that don't conflict with Wine
#include <unistd.h>
#include <fcntl.h>

// VST3 SDK
#define INIT_CLASS_IID
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstplugview.h>
#ifdef interface
#undef interface
#endif
#include "WaylandPluginWindow.h"

#include "ipc/control-protocol.h"

using namespace Steinberg;
using namespace arthur;
using namespace arthur::ipc;

// --- Typedefs for VST3 entry points ---
typedef IPluginFactory* (PLUGIN_API *GetPluginFactoryFunc)();
typedef bool (PLUGIN_API *ModuleEntryFunc)(void*);
typedef bool (PLUGIN_API *ModuleExitFunc)();

// --- Global state ---
static HMODULE g_plugin_module = nullptr;
static IPluginFactory* g_factory = nullptr;
static std::map<uint64_t, FUnknown*> g_instances;
static uint64_t g_next_instance_id = 1;

struct InstanceContext {
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    IPlugView* view = nullptr;
    HWND hwnd = nullptr;
    WaylandPluginWindow* wayland_window = nullptr;

    AudioTransport transport;
    std::thread audio_thread;
    std::atomic<bool> thread_running{false};

    ~InstanceContext() {
        thread_running = false;
        if (audio_thread.joinable()) audio_thread.join();
        if (wayland_window) delete wayland_window;
        if (view) {
            view->removed();
            view->release();
        }
        if (hwnd) DestroyWindow(hwnd);
        if (controller) controller->release();
        if (processor) processor->release();
    }
};

static std::map<uint64_t, InstanceContext*> g_instance_contexts;

void audio_thread_loop(uint64_t instance_id, InstanceContext* ctx) {
    auto* shm = ctx->transport.get();
    if (!shm) return;

    std::cerr << "[Arthur Guest] Audio thread started for instance " << instance_id << std::endl;

    while (ctx->thread_running) {
        // 1. Wait for Host to provide data
        while (shm->state.load(std::memory_order_acquire) != TransportState::STATE_HOST_WRITTEN) {
            if (!ctx->thread_running) return;
            // Short yield to avoid 100% CPU on idle, but keep latency low
            std::this_thread::yield();
        }

        // 2. Prepare Vst::ProcessData
        Vst::ProcessData data;
        data.numSamples = shm->sample_count;
        data.numInputs = shm->num_inputs;
        data.numOutputs = shm->num_outputs;
        
        // We need to map our planar buffers to the VST3 channel buffers
        Vst::AudioBusBuffers inputs[SHM_MAX_CHANNELS];
        Vst::AudioBusBuffers outputs[SHM_MAX_CHANNELS];
        
        for (int i = 0; i < shm->num_inputs; ++i) {
            inputs[i].numChannels = 2; // For now assume stereo
            inputs[i].channelBuffers32 = new float*[2];
            inputs[i].channelBuffers32[0] = shm->input_buffers[0];
            inputs[i].channelBuffers32[1] = shm->input_buffers[1];
        }
        for (int i = 0; i < shm->num_outputs; ++i) {
            outputs[i].numChannels = 2;
            outputs[i].channelBuffers32 = new float*[2];
            outputs[i].channelBuffers32[0] = shm->output_buffers[0];
            outputs[i].channelBuffers32[1] = shm->output_buffers[1];
        }

        data.inputs = inputs;
        data.numInputs = shm->num_inputs;
        data.outputs = outputs;
        data.numOutputs = shm->num_outputs;

        // 3. Process
        if (ctx->processor) {
            ctx->processor->process(data);
        }

        // Cleanup temp buffers
        for (int i = 0; i < shm->num_inputs; ++i) delete[] inputs[i].channelBuffers32;
        for (int i = 0; i < shm->num_outputs; ++i) delete[] outputs[i].channelBuffers32;

        // 4. Signal Host
        shm->state.store(TransportState::STATE_GUEST_PROCESSED, std::memory_order_release);
    }
}

/**
 * Load a Windows VST3 DLL and extract its factory.
 */
bool load_plugin(const std::string& dll_path) {
    // Convert to wide string for LoadLibraryW
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dll_path.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, dll_path.c_str(), -1, wpath.data(), wlen);

    g_plugin_module = LoadLibraryW(wpath.data());
    if (!g_plugin_module) {
        std::cerr << "[Arthur Guest] Failed to load DLL: " << dll_path
                  << " (error: " << GetLastError() << ")" << std::endl;
        return false;
    }

    // Call ModuleEntry if available
    auto module_entry = (ModuleEntryFunc)GetProcAddress(g_plugin_module, "ModuleEntry");
    if (module_entry) {
        module_entry(g_plugin_module);
    }

    // Get the plugin factory
    auto get_factory = (GetPluginFactoryFunc)GetProcAddress(g_plugin_module, "GetPluginFactory");
    if (!get_factory) {
        std::cerr << "[Arthur Guest] No GetPluginFactory in: " << dll_path << std::endl;
        FreeLibrary(g_plugin_module);
        g_plugin_module = nullptr;
        return false;
    }

    g_factory = get_factory();
    if (!g_factory) {
        std::cerr << "[Arthur Guest] GetPluginFactory returned null" << std::endl;
        FreeLibrary(g_plugin_module);
        g_plugin_module = nullptr;
        return false;
    }

    std::cerr << "[Arthur Guest] Plugin loaded: " << dll_path << std::endl;
    return true;
}

/**
 * Connect to the Unix domain socket created by the Linux bridge.
 * 
 * Wine supports Unix domain sockets through its POSIX layer.
 * We open the socket file directly using open() and communicate
 * via read()/write() on the file descriptor.
 * 
 * However, Unix domain sockets require the socket() syscall.
 * Under Winelib, we can use Wine's internal ntdll Unix calls.
 * The simplest portable approach: use a named pipe (FIFO) or 
 * let Wine's wineserver proxy the connection.
 * 
 * For Phase 1, we use a pair of FIFOs (one for each direction)
 * which avoids all Winsock/POSIX header conflicts.
 */

// We use simple file-based IPC via two FIFOs:
//   <socket_path>.req  (bridge → guest)
//   <socket_path>.resp (guest → bridge)
static int g_req_fd = -1;   // Read requests from bridge
static int g_resp_fd = -1;  // Write responses to bridge

bool connect_to_bridge(const std::string& socket_path) {
    std::string req_path = socket_path + ".req";
    std::string resp_path = socket_path + ".resp";

    // Open the request pipe (reading)
    // The bridge will have created these FIFOs before launching us
    g_req_fd = open(req_path.c_str(), O_RDONLY);
    if (g_req_fd < 0) {
        std::cerr << "[Arthur Guest] Failed to open request pipe: " << req_path << std::endl;
        return false;
    }

    // Open the response pipe (writing)
    g_resp_fd = open(resp_path.c_str(), O_WRONLY);
    if (g_resp_fd < 0) {
        std::cerr << "[Arthur Guest] Failed to open response pipe: " << resp_path << std::endl;
        close(g_req_fd);
        g_req_fd = -1;
        return false;
    }

    std::cerr << "[Arthur Guest] Connected to bridge via FIFOs." << std::endl;
    return true;
}

/**
 * Handle a single IPC message from the Linux bridge.
 * Returns false if the connection should be closed.
 */
bool handle_message(const MessageHeader& header) {
    switch (header.type) {

        case MessageType::REQUEST_FACTORY_INFO: {
            FactoryInfoResponse resp;
            std::memset(&resp, 0, sizeof(resp));

            if (g_factory) {
                PFactoryInfo info;
                if (g_factory->getFactoryInfo(&info) == kResultOk) {
                    std::strncpy(resp.vendor, info.vendor, sizeof(resp.vendor) - 1);
                    std::strncpy(resp.url, info.url, sizeof(resp.url) - 1);
                    std::strncpy(resp.email, info.email, sizeof(resp.email) - 1);
                }
                resp.num_classes = g_factory->countClasses();
            }

            send_message(g_resp_fd, MessageType::RESPONSE_FACTORY_INFO,
                         header.request_id, header.instance_id, &resp, sizeof(resp));
            return true;
        }

        case MessageType::REQUEST_CLASS_INFO: {
            ClassInfoRequest req;
            if (!recv_payload(g_req_fd, &req, header.payload_size)) return false;

            ClassInfoResponse resp;
            std::memset(&resp, 0, sizeof(resp));

            if (g_factory) {
                PClassInfo info;
                if (g_factory->getClassInfo(req.index, &info) == kResultOk) {
                    std::memcpy(resp.cid, info.cid, sizeof(resp.cid));
                    std::strncpy(resp.name, info.name, sizeof(resp.name) - 1);
                    std::strncpy(resp.category, info.category, sizeof(resp.category) - 1);
                    resp.cardinality = info.cardinality;
                }
            }

            send_message(g_resp_fd, MessageType::RESPONSE_CLASS_INFO,
                         header.request_id, header.instance_id, &resp, sizeof(resp));
            return true;
        }

        case MessageType::REQUEST_CREATE_INSTANCE: {
            CreateInstanceRequest req;
            if (!recv_payload(g_req_fd, &req, header.payload_size)) return false;

            CreateInstanceResponse resp;
            std::memset(&resp, 0, sizeof(resp));

            if (g_factory) {
                void* obj = nullptr;
                resp.result = g_factory->createInstance(
                    reinterpret_cast<const char*>(req.cid),
                    reinterpret_cast<const char*>(req.iid),
                    &obj);

                if (obj) {
                    uint64_t id = g_next_instance_id++;
                    auto* ctx = new InstanceContext();
                    g_instances[id] = static_cast<FUnknown*>(obj);
                    g_instance_contexts[id] = ctx;

                    // Query for Audio Processor
                    static_cast<FUnknown*>(obj)->queryInterface(Vst::IAudioProcessor::iid, (void**)&ctx->processor);
                    // Query for Edit Controller
                    static_cast<FUnknown*>(obj)->queryInterface(Vst::IEditController::iid, (void**)&ctx->controller);

                    resp.instance_id = id;
                    resp.result = kResultOk;
                    std::cerr << "[Arthur Guest] Created instance " << id 
                              << " (Proc: " << (ctx->processor ? "YES" : "NO")
                              << ", Ctrl: " << (ctx->controller ? "YES" : "NO") << ")" << std::endl;
                }
            }

            send_message(g_resp_fd, MessageType::RESPONSE_CREATE_INSTANCE,
                         header.request_id, header.instance_id, &resp, sizeof(resp));
            return true;
        }

        case MessageType::REQUEST_DESTROY_INSTANCE: {
            uint64_t id = header.instance_id;
            auto it = g_instances.find(id);
            if (it != g_instances.end()) {
                if (it->second) it->second->release();
                g_instances.erase(it);
            }

            auto ait = g_instance_contexts.find(id);
            if (ait != g_instance_contexts.end()) {
                delete ait->second;
                g_instance_contexts.erase(ait);
            }

            std::cerr << "[Arthur Guest] Destroyed instance " << id << std::endl;
            send_message(g_resp_fd, MessageType::RESPONSE_DESTROY_INSTANCE,
                         header.request_id, header.instance_id, nullptr, 0);
            return true;
        }

        case MessageType::REQUEST_SETUP_PROCESSING: {
            SetupProcessingRequest req;
            if (!recv_payload(g_req_fd, &req, header.payload_size)) return false;

            auto* ctx = g_instance_contexts[header.instance_id];
            if (ctx && ctx->transport.attach(req.shm_name)) {
                Vst::ProcessSetup setup;
                setup.sampleRate = req.sample_rate;
                setup.maxSamplesPerBlock = req.max_block_size;
                setup.processMode = req.process_mode;
                ctx->processor->setupProcessing(setup);
                
                if (!ctx->thread_running) {
                    ctx->thread_running = true;
                    ctx->audio_thread = std::thread(audio_thread_loop, header.instance_id, ctx);
                }
            }

            send_message(g_resp_fd, MessageType::RESPONSE_SETUP_PROCESSING,
                         header.request_id, header.instance_id, nullptr, 0);
            return true;
        }

        case MessageType::REQUEST_SET_BUS_CONFIG: {
            SetBusConfigRequest req;
            if (!recv_payload(g_req_fd, &req, header.payload_size)) return false;

            auto* ctx = g_instance_contexts[header.instance_id];
            if (ctx) {
                ctx->processor->setBusArrangements(req.input_arrangements, req.num_inputs,
                                                 req.output_arrangements, req.num_outputs);
            }

            send_message(g_resp_fd, MessageType::RESPONSE_SET_BUS_CONFIG,
                         header.request_id, header.instance_id, nullptr, 0);
            return true;
        }

        case MessageType::REQUEST_SET_STATE: {
            SetStateRequest req;
            if (!recv_payload(g_req_fd, &req, header.payload_size)) return false;

            auto* ctx = g_instance_contexts[header.instance_id];
            if (ctx) {
                ctx->processor->setProcessing(req.state);
            }

            send_message(g_resp_fd, MessageType::RESPONSE_SET_STATE,
                         header.request_id, header.instance_id, nullptr, 0);
            return true;
        }

        case MessageType::REQUEST_OPEN_EDITOR: {
            auto* ctx = g_instance_contexts[header.instance_id];
            OpenEditorResponse resp{0, 0};
            if (ctx && ctx->controller) {
                if (!ctx->view) {
                    ctx->view = ctx->controller->createView("Editor");
                }
                if (ctx->view) {
                    ViewRect rect;
                    if (ctx->view->getSize(&rect) == kResultOk) {
                        resp.width = rect.right - rect.left;
                        resp.height = rect.bottom - rect.top;
                    } else {
                        resp.width = 800; resp.height = 600; // Fallback
                    }
                }
            }
            send_message(g_resp_fd, MessageType::RESPONSE_OPEN_EDITOR,
                         header.request_id, header.instance_id, &resp, sizeof(resp));
            return true;
        }

        case MessageType::REQUEST_ATTACH_WINDOW: {
            AttachWindowRequest req;
            if (!recv_payload(g_req_fd, &req, header.payload_size)) return false;

            auto* ctx = g_instance_contexts[header.instance_id];
            if (ctx && ctx->view) {
                if (!ctx->hwnd) {
                    ctx->hwnd = CreateWindowExA(0, "Static", "Arthur Plugin Window",
                                               WS_CHILD | WS_VISIBLE, 0, 0, 800, 600,
                                               GetDesktopWindow(), nullptr, nullptr, nullptr);
                }

                // If handle looks like an xdg-foreign handle (starts with 'wayland:')
                if (std::strncmp(req.handle, "wayland:", 8) != 0 && std::strncmp(req.handle, "x11:", 4) != 0) {
                    if (!ctx->wayland_window) {
                        ctx->wayland_window = new WaylandPluginWindow(nullptr, 800, 600);
                    }
                    if (ctx->wayland_window->import_surface(req.handle)) {
                         std::cerr << "[Arthur Guest] Imported DAW surface via xdg-foreign handle: " << req.handle << std::endl;
                         ctx->wayland_window->attach_hwnd(ctx->hwnd);
                    }
                }

                ctx->view->attached(ctx->hwnd, "HWND");
                std::cerr << "[Arthur Guest] Attached view to HWND: " << ctx->hwnd << std::endl;
            }

            send_message(g_resp_fd, MessageType::RESPONSE_ATTACH_WINDOW,
                         header.request_id, header.instance_id, nullptr, 0);
            return true;
        }

        case MessageType::SHUTDOWN: {
            std::cerr << "[Arthur Guest] Shutdown requested." << std::endl;
            return false;
        }

        default: {
            std::cerr << "[Arthur Guest] Unknown message type: "
                      << static_cast<uint32_t>(header.type) << std::endl;
            return true;
        }
    }
}

/**
 * Main entry point.
 * Usage: wine arthur-guest.exe <plugin_dll_path> <fifo_base_path>
 */
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: arthur-guest.exe <plugin_dll_path> <fifo_base_path>" << std::endl;
        return 1;
    }

    std::string plugin_path = argv[1];
    std::string fifo_base = argv[2];

    std::cerr << "[Arthur Guest] Starting..." << std::endl;
    std::cerr << "[Arthur Guest] Plugin: " << plugin_path << std::endl;

    // Initialize COM (some plugins require this)
    OleInitialize(nullptr);

    // Load the Windows plugin
    if (!load_plugin(plugin_path)) {
        return 1;
    }

    // Connect to the Linux bridge via FIFOs
    if (!connect_to_bridge(fifo_base)) {
        return 1;
    }

    // Send GUEST_READY signal
    send_message(g_resp_fd, MessageType::GUEST_READY, 0, 0, nullptr, 0);

    // Message loop — handle requests from the bridge
    while (true) {
        MessageHeader header;
        if (!recv_header(g_req_fd, header)) {
            std::cerr << "[Arthur Guest] Connection lost." << std::endl;
            break;
        }

        if (!handle_message(header)) {
            break;
        }
    }

    // Cleanup
    if (g_req_fd >= 0) close(g_req_fd);
    if (g_resp_fd >= 0) close(g_resp_fd);

    for (auto& [id, obj] : g_instances) {
        if (obj) obj->release();
    }
    g_instances.clear();

    if (g_plugin_module) {
        auto module_exit = (ModuleExitFunc)GetProcAddress(g_plugin_module, "ModuleExit");
        if (module_exit) module_exit();
        FreeLibrary(g_plugin_module);
    }

    OleUninitialize();

    std::cerr << "[Arthur Guest] Shutdown complete." << std::endl;
    return 0;
}
