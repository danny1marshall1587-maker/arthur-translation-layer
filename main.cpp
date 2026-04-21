#include "AraPathTranslator.h"
#include "WaylandPluginWindow.h"
#include "AudioIPC.h"
#include <iostream>
#include <cstring>

#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/base/ipluginbase.h>

using namespace arthur;

namespace arthur {

/**
 * A stub factory that satisfies the DAW's scan requirements.
 */
class ArthurPluginFactory : public Steinberg::IPluginFactory {
public:
    ArthurPluginFactory() {}
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
        info->flags = 0; // No specific flags needed for a stub
        return Steinberg::kResultOk;
    }

    Steinberg::int32 PLUGIN_API countClasses() override { return 1; }

    Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info) override {
        if (index != 0) return Steinberg::kInvalidArgument;
        
        // This CID should ideally be generated or mirrored from the real plugin
        std::memset(info->cid, 0, sizeof(Steinberg::TUID));
        info->cid[0] = 0x41; // 'A'
        info->cid[1] = 0x52; // 'R'
        
        info->cardinality = Steinberg::PClassInfo::kManyInstances;
        std::strncpy(info->category, "Audio Module Class", sizeof(info->category));
        std::strncpy(info->name, "Arthur Bridged Plugin", sizeof(info->name));
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::FIDString cid, Steinberg::FIDString _iid, void** obj) override {
        std::cout << "[Arthur Bridge] createInstance called for: " << cid << std::endl;
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
};

static ArthurPluginFactory* gFactory = nullptr;

} // namespace arthur

// --- VST3 Entry Points ---

extern "C" {

/**
 * The main factory entry point that hosts look for.
 */
SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!arthur::gFactory) {
        arthur::gFactory = new arthur::ArthurPluginFactory();
    }
    std::cout << "[Arthur Bridge] GetPluginFactory() called by DAW. Returning ArthurPluginFactory." << std::endl;
    return arthur::gFactory;
}

/**
 * Initialize the Arthur Translation Layer.
 */
bool arthur_init_bridge() {
    std::cout << "[Arthur Translation Layer] Initializing Wayland-Native VST3 Bridge..." << std::endl;
    return true;
}

/**
 * Shut down the Arthur Translation Layer.
 */
void arthur_shutdown_bridge() {
    std::cout << "[Arthur Translation Layer] Shutting down..." << std::endl;
}

} // extern "C"
