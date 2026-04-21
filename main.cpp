#include "AraPathTranslator.h"
#include "WaylandPluginWindow.h"
#include "AudioIPC.h"
#include "ipc/control-protocol.h"

#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <sys/stat.h>
#include <atomic>
#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/base/ipluginbase.h>

#ifndef SMTG_EXPORT_SYMBOL
#define SMTG_EXPORT_SYMBOL __attribute__ ((visibility ("default")))
#endif

using namespace Steinberg;
using namespace arthur::ipc;

namespace arthur {

/**
 * Manages the connection to the Wine-side Guest process.
 * Handles launching the Guest, establishing the IPC socket, and 
 * sending/receiving messages.
 */
class GuestConnection {
public:
    GuestConnection() = default;
    ~GuestConnection() { shutdown(); }

    /**
     * Launch the Guest process and establish the IPC connection via FIFOs.
     * @param plugin_dll_path Path to the Windows .dll inside the Wine prefix
     * @return true if the Guest launched and connected successfully
     */
    bool launch(const std::string& plugin_dll_path) {
        // Create unique FIFO base path
        fifo_base_ = "/tmp/arthur-" + std::to_string(getpid()) + "-" +
                      std::to_string(reinterpret_cast<uintptr_t>(this));

        std::string req_path = fifo_base_ + ".req";
        std::string resp_path = fifo_base_ + ".resp";

        // Remove stale FIFOs
        unlink(req_path.c_str());
        unlink(resp_path.c_str());

        // Create the FIFOs
        if (mkfifo(req_path.c_str(), 0600) < 0) {
            std::cerr << "[Arthur Bridge] Failed to create request FIFO" << std::endl;
            return false;
        }
        if (mkfifo(resp_path.c_str(), 0600) < 0) {
            std::cerr << "[Arthur Bridge] Failed to create response FIFO" << std::endl;
            unlink(req_path.c_str());
            return false;
        }

        // Find the Guest executable
        std::string guest_path = find_guest_exe();
        if (guest_path.empty()) {
            std::cerr << "[Arthur Bridge] Could not find arthur-guest.exe" << std::endl;
            return false;
        }

        // Launch: wine arthur-guest.exe <plugin_path> <fifo_base>
        guest_pid_ = fork();
        if (guest_pid_ == 0) {
            // Child process
            std::string prefix = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.arthur-wine";
            setenv("WINEPREFIX", prefix.c_str(), 1);
            setenv("WINEDLLOVERRIDES", "mscoree=d;mshtml=d", 1);

            execlp("wine", "wine", guest_path.c_str(),
                   plugin_dll_path.c_str(), fifo_base_.c_str(), nullptr);
            _exit(1);
        }

        if (guest_pid_ < 0) {
            std::cerr << "[Arthur Bridge] Failed to fork Guest" << std::endl;
            return false;
        }

        // Open FIFOs (these block until the Guest opens the other end)
        // Bridge writes requests, reads responses
        req_fd_ = open(req_path.c_str(), O_WRONLY);
        if (req_fd_ < 0) {
            std::cerr << "[Arthur Bridge] Failed to open request FIFO for writing" << std::endl;
            kill(guest_pid_, SIGTERM);
            waitpid(guest_pid_, nullptr, 0);
            return false;
        }

        resp_fd_ = open(resp_path.c_str(), O_RDONLY);
        if (resp_fd_ < 0) {
            std::cerr << "[Arthur Bridge] Failed to open response FIFO for reading" << std::endl;
            close(req_fd_);
            kill(guest_pid_, SIGTERM);
            waitpid(guest_pid_, nullptr, 0);
            return false;
        }

        // Wait for GUEST_READY signal
        MessageHeader header;
        if (!recv_header(resp_fd_, header) || header.type != MessageType::GUEST_READY) {
            std::cerr << "[Arthur Bridge] Did not receive GUEST_READY" << std::endl;
            shutdown();
            return false;
        }

        connected_ = true;
        std::cerr << "[Arthur Bridge] Guest connected and ready." << std::endl;
        return true;
    }

    /**
     * Send a request and wait for a response.
     */
    bool request(MessageType req_type, uint32_t req_id,
                 const void* req_payload, uint32_t req_size,
                 MessageType expected_resp, void* resp_payload, uint32_t resp_size) {
        if (!connected_) return false;

        if (!send_message(req_fd_, req_type, req_id, req_payload, req_size)) {
            return false;
        }

        MessageHeader header;
        if (!recv_header(resp_fd_, header)) return false;
        if (header.type != expected_resp) return false;

        if (resp_payload && resp_size > 0) {
            return recv_payload(resp_fd_, resp_payload, resp_size);
        }
        return true;
    }

    bool is_connected() const { return connected_; }

    void shutdown() {
        if (req_fd_ >= 0) {
            send_message(req_fd_, MessageType::SHUTDOWN, 0, nullptr, 0);
            close(req_fd_);
            req_fd_ = -1;
        }
        if (resp_fd_ >= 0) {
            close(resp_fd_);
            resp_fd_ = -1;
        }
        if (guest_pid_ > 0) {
            kill(guest_pid_, SIGTERM);
            waitpid(guest_pid_, nullptr, 0);
            guest_pid_ = -1;
        }
        if (!fifo_base_.empty()) {
            unlink((fifo_base_ + ".req").c_str());
            unlink((fifo_base_ + ".resp").c_str());
            fifo_base_.clear();
        }
        connected_ = false;
    }

private:
    /**
     * Find arthur-guest.exe by looking relative to this .so file.
     * Search order:
     *   1. Same directory as the .so
     *   2. ~/.local/lib/arthur/
     *   3. /usr/lib/arthur/
     */
    std::string find_guest_exe() {
        Dl_info info;
        if (dladdr((void*)find_guest_exe_anchor, &info) && info.dli_fname) {
            char* path_copy = strdup(info.dli_fname);
            char* dir = dirname(path_copy);
            
            // Check same directory
            std::string candidate = std::string(dir) + "/arthur-guest.exe";
            if (access(candidate.c_str(), F_OK) == 0) {
                free(path_copy);
                return candidate;
            }

            // Check parent's parent (inside .vst3 bundle structure)
            // .vst3/Contents/x86_64-linux/Plugin.so → ../../arthur-guest.exe
            candidate = std::string(dir) + "/../../arthur-guest.exe";
            if (access(candidate.c_str(), F_OK) == 0) {
                free(path_copy);
                return candidate;
            }

            free(path_copy);
        }

        // System paths
        const char* fallbacks[] = {
            "~/.local/lib/arthur/arthur-guest.exe",
            "/usr/lib/arthur/arthur-guest.exe",
            nullptr
        };
        for (int i = 0; fallbacks[i]; i++) {
            if (access(fallbacks[i], F_OK) == 0) return fallbacks[i];
        }

        return "";
    }

    static void find_guest_exe_anchor() {}

    std::string fifo_base_;
    int req_fd_ = -1;
    int resp_fd_ = -1;
    pid_t guest_pid_ = -1;
    bool connected_ = false;
};

/**
 * Proxy IPluginFactory that forwards all calls to the Wine Guest.
 * This is what REAPER sees when it scans the plugin.
 */
class ArthurPluginFactory : public IPluginFactory {
public:
    ArthurPluginFactory(GuestConnection* conn) : conn_(conn) {
        // Fetch factory info from Guest
        conn_->request(MessageType::REQUEST_FACTORY_INFO, 1,
                       nullptr, 0,
                       MessageType::RESPONSE_FACTORY_INFO,
                       &factory_info_, sizeof(factory_info_));

        // Pre-fetch all class infos
        for (int32_t i = 0; i < factory_info_.num_classes; i++) {
            ClassInfoRequest req;
            req.index = i;
            ClassInfoResponse resp;
            conn_->request(MessageType::REQUEST_CLASS_INFO, 2 + i,
                           &req, sizeof(req),
                           MessageType::RESPONSE_CLASS_INFO,
                           &resp, sizeof(resp));
            class_infos_.push_back(resp);
        }

        std::cerr << "[Arthur Bridge] Factory: " << factory_info_.vendor
                  << " (" << factory_info_.num_classes << " classes)" << std::endl;
    }

    virtual ~ArthurPluginFactory() {}

    // --- FUnknown ---
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        if (std::memcmp(_iid, FUnknown::iid, sizeof(TUID)) == 0 ||
            std::memcmp(_iid, IPluginFactory::iid, sizeof(TUID)) == 0) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++ref_count_; }
    uint32 PLUGIN_API release() override {
        if (--ref_count_ == 0) { delete this; return 0; }
        return ref_count_;
    }

    // --- IPluginFactory ---
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        std::memset(info, 0, sizeof(PFactoryInfo));
        std::strncpy(info->vendor, factory_info_.vendor, sizeof(info->vendor) - 1);
        std::strncpy(info->url, factory_info_.url, sizeof(info->url) - 1);
        std::strncpy(info->email, factory_info_.email, sizeof(info->email) - 1);
        info->flags = 0;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override {
        return factory_info_.num_classes;
    }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index < 0 || index >= static_cast<int32>(class_infos_.size())) {
            return kInvalidArgument;
        }

        const auto& ci = class_infos_[index];
        std::memset(info, 0, sizeof(PClassInfo));
        std::memcpy(info->cid, ci.cid, sizeof(TUID));
        info->cardinality = ci.cardinality;
        std::strncpy(info->category, ci.category, sizeof(info->category) - 1);
        std::strncpy(info->name, ci.name, sizeof(info->name) - 1);
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) override {
        // Phase 1: Return kNoInterface for now.
        // Phase 2 will implement full instance proxy with audio processing.
        std::cerr << "[Arthur Bridge] createInstance called (Phase 2 needed)" << std::endl;
        *obj = nullptr;
        return kNoInterface;
    }

private:
    GuestConnection* conn_;
    FactoryInfoResponse factory_info_{};
    std::vector<ClassInfoResponse> class_infos_;
    std::atomic<int32_t> ref_count_{1};
};

// --- Module state ---
static GuestConnection* g_connection = nullptr;
static ArthurPluginFactory* g_factory = nullptr;

/**
 * Determine the Windows .dll path for this bridge instance.
 * The bridge .so is named like "FabFilter Pro-Q 3.so"
 * We look for the corresponding .dll in the Wine prefix.
 */
static std::string find_plugin_dll() {
    Dl_info info;
    if (!dladdr((void*)find_plugin_dll, &info) || !info.dli_fname) {
        return "";
    }

    // Get the plugin name from our .so filename
    char* path_copy = strdup(info.dli_fname);
    char* base = basename(path_copy);
    std::string name(base);

    // Strip .so extension
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    free(path_copy);

    // Search for the Windows .vst3 or .dll in the Wine prefix
    // Standard Wine VST3 paths:
    const char* home = getenv("HOME");
    if (!home) return "";

    std::string prefix = std::string(home) + "/.arthur-wine";
    
    // Check common Windows VST3 install locations
    std::vector<std::string> search_paths = {
        prefix + "/drive_c/Program Files/Common Files/VST3/" + name + ".vst3",
        prefix + "/drive_c/Program Files/Common Files/VST3/" + name + ".vst3/Contents/x86_64-win/" + name + ".vst3",
        prefix + "/drive_c/Program Files/Common Files/VST3/" + name + "/" + name + ".dll",
    };

    for (const auto& path : search_paths) {
        if (access(path.c_str(), F_OK) == 0) {
            return path;
        }
    }

    // If not found, return a best-guess path that the Guest can report as missing
    return prefix + "/drive_c/Program Files/Common Files/VST3/" + name + ".vst3";
}

} // namespace arthur

// --- VST3 Entry Points ---

extern "C" {

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!arthur::g_factory && arthur::g_connection && arthur::g_connection->is_connected()) {
        arthur::g_factory = new arthur::ArthurPluginFactory(arthur::g_connection);
    }
    return arthur::g_factory;
}

SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleEntry(void* sharedLibraryHandle) {
    std::cerr << "[Arthur Bridge] ModuleEntry called." << std::endl;

    if (arthur::g_connection) return true;  // Already initialized

    // Find the Windows plugin DLL for this bridge instance
    std::string dll_path = arthur::find_plugin_dll();
    if (dll_path.empty()) {
        std::cerr << "[Arthur Bridge] Could not determine plugin DLL path." << std::endl;
        return false;
    }

    std::cerr << "[Arthur Bridge] Plugin DLL: " << dll_path << std::endl;

    // Launch the Guest process
    arthur::g_connection = new arthur::GuestConnection();
    if (!arthur::g_connection->launch(dll_path)) {
        std::cerr << "[Arthur Bridge] Failed to launch Guest." << std::endl;
        delete arthur::g_connection;
        arthur::g_connection = nullptr;
        return false;
    }

    return true;
}

SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleExit() {
    std::cerr << "[Arthur Bridge] ModuleExit called." << std::endl;

    if (arthur::g_factory) {
        arthur::g_factory = nullptr;  // Will be cleaned up by release()
    }

    if (arthur::g_connection) {
        arthur::g_connection->shutdown();
        delete arthur::g_connection;
        arthur::g_connection = nullptr;
    }

    return true;
}

// Legacy entry points
bool arthur_init_bridge() { return true; }
void arthur_shutdown_bridge() { }

} // extern "C"
