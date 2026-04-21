#pragma once

#include <string>
#include <pluginterfaces/vst/ivstattributes.h>

namespace arthur {

class AraPathTranslator {
public:
    /**
     * Converts a Linux Unix path (e.g., /home/user/audio.wav) 
     * to a Windows DOS path (e.g., Z:\home\user\audio.wav)
     */
    static std::string unix_to_dos(const std::string& unix_path);

    /**
     * Converts a Windows DOS path (e.g., Z:\home\user\audio.wav) 
     * to a Linux Unix path (e.g., /home/user/audio.wav)
     */
    static std::string dos_to_unix(const std::string& dos_path);
};

// Wrapper for VST3 IAttributeList to intercept ARA2 file paths
class WaylandAttributeList : public Steinberg::Vst::IAttributeList {
public:
    WaylandAttributeList(Steinberg::Vst::IAttributeList* host_list);
    virtual ~WaylandAttributeList();

    // IUnknown methods (Delegated to host_list)
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;

    // IAttributeList methods
    Steinberg::tresult PLUGIN_API setInt(AttrID id, Steinberg::int64 value) override;
    Steinberg::tresult PLUGIN_API getInt(AttrID id, Steinberg::int64& value) override;
    Steinberg::tresult PLUGIN_API setFloat(AttrID id, double value) override;
    Steinberg::tresult PLUGIN_API getFloat(AttrID id, double& value) override;
    Steinberg::tresult PLUGIN_API setString(AttrID id, const Steinberg::Vst::TChar* string) override;
    Steinberg::tresult PLUGIN_API getString(AttrID id, Steinberg::Vst::TChar* string, Steinberg::uint32 sizeInBytes) override;
    Steinberg::tresult PLUGIN_API setBinary(AttrID id, const void* data, Steinberg::uint32 sizeInBytes) override;
    Steinberg::tresult PLUGIN_API getBinary(AttrID id, const void*& data, Steinberg::uint32& sizeInBytes) override;

private:
    Steinberg::Vst::IAttributeList* host_list_;
};

} // namespace arthur
