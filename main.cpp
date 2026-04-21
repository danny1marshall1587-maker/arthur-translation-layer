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

#ifndef SMTG_EXPORT_SYMBOL
#define SMTG_EXPORT_SYMBOL __attribute__ ((visibility ("default")))
#endif

using namespace arthur;

namespace arthur {

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
        std::memset(info, 0, sizeof(Steinberg::PFactoryInfo));
        std::strncpy(info->vendor, "Arthur Audio", sizeof(info->vendor) - 1);
        std::strncpy(info->url, "https://arthur.audio", sizeof(info->url) - 1);
        std::strncpy(info->email, "support@arthur.audio", sizeof(info->email) - 1);
        info->flags = 0;
        return Steinberg::kResultOk;
    }

    Steinberg::int32 PLUGIN_API countClasses() override { return 1; }

    Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info) override {
        if (index != 0) return Steinberg::kInvalidArgument;
        
        std::memset(info, 0, sizeof(Steinberg::PClassInfo));
        std::memcpy(info->cid, plugin_cid_, sizeof(Steinberg::TUID));
        info->cardinality = Steinberg::PClassInfo::kManyInstances;
        std::strncpy(info->category, "Audio Module Class", sizeof(info->category) - 1);
        std::strncpy(info->name, plugin_name_.c_str(), sizeof(info->name) - 1);
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

    void update_identity() {
        Dl_info info;
        if (dladdr((void*)update_identity_static, &info) && info.dli_fname) {
            char* path_copy = strdup(info.dli_fname);
            char* base = basename(path_copy);
            
            std::string name(base);
            size_t last_dot = name.find_last_of(".");
            if (last_dot != std::string::npos) {
                name = name.substr(0, last_dot);
            }
            
            plugin_name_ = "Arthur: " + name;
            
            // Generate a professional 128-bit GUID
            std::memset(plugin_cid_, 0, sizeof(Steinberg::TUID));
            
            // Fixed Prefix for Arthur
            plugin_cid_[0] = 0x41; // A
            plugin_cid_[1] = 0x52; // R
            plugin_cid_[2] = 0x54; // T
            plugin_cid_[3] = 0x48; // H
            
            uint32_t hash = 0x811c9dc5;
            for (char c : name) {
                hash ^= static_cast<uint32_t>(c);
                hash *= 0x01000193;
            }
            
            // Inject hash into multiple segments of the GUID to ensure uniqueness
            std::memcpy(plugin_cid_ + 4, &hash, sizeof(hash));
            uint32_t hash2 = hash ^ 0xAAAAAAAA;
            std::memcpy(plugin_cid_ + 8, &hash2, sizeof(hash2));
            uint32_t hash3 = hash ^ 0x55555555;
            std::memcpy(plugin_cid_ + 12, &hash3, sizeof(hash3));
            
            free(path_copy);
            std::cout << "[Arthur Bridge] Self-Identified as: " << plugin_name_ << std::endl;
        }
    }

    static void update_identity_static() {}
};

static ArthurPluginFactory* gFactory = nullptr;

} // namespace arthur

// --- VST3 Entry Points ---

extern "C" {

/**
 * MANDATORY: The primary factory entry point.
 */
SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!arthur::gFactory) {
        arthur::gFactory = new arthur::ArthurPluginFactory();
    }
    return arthur::gFactory;
}

/**
 * OPTIONAL BUT RECOMMENDED: Module initialization.
 */
SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleEntry(void* sharedLibraryHandle) {
    std::cout << "[Arthur Bridge] ModuleEntry called." << std::endl;
    return true;
}

/**
 * OPTIONAL BUT RECOMMENDED: Module cleanup.
 */
SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleExit() {
    std::cout << "[Arthur Bridge] ModuleExit called." << std::endl;
    return true;
}

bool arthur_init_bridge() { return true; }
void arthur_shutdown_bridge() { }

} // extern "C"
