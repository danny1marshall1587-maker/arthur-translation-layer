#include "AraPathTranslator.h"
#include "WaylandPluginWindow.h"
#include "AudioIPC.h"
#include <iostream>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <libgen.h>

#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/base/ipluginbase.h>

using namespace arthur;

namespace arthur {

/**
 * A dynamic factory that generates unique IDs and names based on its own filename.
 */
class ArthurPluginFactory : public Steinberg::IPluginFactory {
public:
    ArthurPluginFactory() {
        update_identity();
    }
    virtual ~ArthurPluginFactory() {}

    // FUnknown
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (std::memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0 ||
            std::memcmp(_iid, Steinberg::IPluginFactory::iid, sizeof(Steinberg::TUID)) == 0) {
            addRef();
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    // IPluginFactory
    Steinberg::tresult PLUGIN_API getFactoryInfo(Steinberg::PFactoryInfo* info) override {
        std::strncpy(info->vendor, "Arthur Audio", sizeof(info->vendor));
        std::strncpy(info->url, "https://github.com/danny1marshall1587-maker/arthur-translation-layer", sizeof(info->url));
        std::strncpy(info->email, "support@arthur.audio", sizeof(info->email));
        info->flags = 0;
        return Steinberg::kResultOk;
    }

    Steinberg::int32 PLUGIN_API countClasses() override { return 1; }

    Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info) override {
        if (index != 0) return Steinberg::kInvalidArgument;
        
        std::memcpy(info->cid, plugin_cid_, sizeof(Steinberg::TUID));
        info->cardinality = Steinberg::PClassInfo::kManyInstances;
        std::strncpy(info->category, "Audio Module Class", sizeof(info->category));
        std::strncpy(info->name, plugin_name_.c_str(), sizeof(info->name));
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString _iid, void** obj) override {
        std::cout << "[Arthur Bridge] createInstance called for: " << plugin_name_ << std::endl;
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

private:
    std::string plugin_name_ = "Arthur Bridged Plugin";
    Steinberg::TUID plugin_cid_ = {0};

    /**
     * Detects our own filename and generates a unique name/CID.
     */
    void update_identity() {
        Dl_info info;
        if (dladdr((void*)GetPluginFactory, &info) && info.dli_fname) {
            char* path_copy = strdup(info.dli_fname);
            char* base = basename(path_copy);
            
            // Strip .so extension
            std::string name(base);
            size_t last_dot = name.find_last_of(".");
            if (last_dot != std::string::npos) {
                name = name.substr(0, last_dot);
            }
            
            plugin_name_ = "Arthur: " + name;
            
            // Generate a simple hash for the CID
            std::memset(plugin_cid_, 0, sizeof(Steinberg::TUID));
            plugin_cid_[0] = 0x41; // 'A'
            plugin_cid_[1] = 0x52; // 'R'
            
            uint32_t hash = 0x811c9dc5;
            for (char c : name) {
                hash ^= static_cast<uint32_t>(c);
                hash *= 0x01000193;
            }
            
            // Inject hash into CID
            std::memcpy(plugin_cid_ + 8, &hash, sizeof(hash));
            
            free(path_copy);
            std::cout << "[Arthur Bridge] Self-Identified as: " << plugin_name_ << std::endl;
        }
    }
};

static ArthurPluginFactory* gFactory = nullptr;

} // namespace arthur

// --- VST3 Entry Points ---

extern "C" {

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!arthur::gFactory) {
        arthur::gFactory = new arthur::ArthurPluginFactory();
    }
    return arthur::gFactory;
}

bool arthur_init_bridge() { return true; }
void arthur_shutdown_bridge() { }

} // extern "C"
