#include "AraPathTranslator.h"
#include <pluginterfaces/vst/vstpresetkeys.h>
#include <string>
#include <vector>
#include <algorithm>
#include <codecvt>
#include <locale>

#ifdef __WINE__
#include <windows.h>
// In a real Wine build, wine_get_unix_file_name and wine_get_dos_file_name 
// can be loaded via GetProcAddress from kernel32.dll
typedef char* (*wine_get_unix_file_name_ptr)(const WCHAR* dosW);
typedef WCHAR* (*wine_get_dos_file_name_ptr)(const char* unix_name);
#endif

namespace arthur {

// Helper to convert UTF-16 (Vst::TChar) to UTF-8
static std::string tchar_to_utf8(const Steinberg::Vst::TChar* tchar_str) {
    std::u16string u16str(reinterpret_cast<const char16_t*>(tchar_str));
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
    return convert.to_bytes(u16str);
}

// Helper to convert UTF-8 to UTF-16 (Vst::TChar)
static std::u16string utf8_to_tchar(const std::string& utf8_str) {
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
    return convert.from_bytes(utf8_str);
}

std::string AraPathTranslator::unix_to_dos(const std::string& unix_path) {
    if (unix_path.empty()) return "";
    
    // Fast path Wine C-API Translation if compiled inside Winelib
#ifdef __WINE__
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        auto wine_get_dos = (wine_get_dos_file_name_ptr)GetProcAddress(kernel32, "wine_get_dos_file_name");
        if (wine_get_dos) {
            WCHAR* dos_w = wine_get_dos(unix_path.c_str());
            if (dos_w) {
                // Convert WCHAR to utf8 string
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, dos_w, -1, NULL, 0, NULL, NULL);
                std::string dos_str(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, dos_w, -1, &dos_str[0], size_needed, NULL, NULL);
                HeapFree(GetProcessHeap(), 0, dos_w); // Wine allocates with HeapAlloc
                // Remove null terminator added by WideCharToMultiByte
                if (!dos_str.empty() && dos_str.back() == '\0') dos_str.pop_back();
                return dos_str;
            }
        }
    }
#endif

    // Fallback Manual Translation: Prepend Z: and flip slashes
    std::string dos_path = "Z:" + unix_path;
    std::replace(dos_path.begin(), dos_path.end(), '/', '\\');
    return dos_path;
}

std::string AraPathTranslator::dos_to_unix(const std::string& dos_path) {
    if (dos_path.empty()) return "";

#ifdef __WINE__
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        auto wine_get_unix = (wine_get_unix_file_name_ptr)GetProcAddress(kernel32, "wine_get_unix_file_name");
        if (wine_get_unix) {
            // Convert utf8 to WCHAR
            int size_needed = MultiByteToWideChar(CP_UTF8, 0, dos_path.c_str(), -1, NULL, 0);
            std::vector<WCHAR> dos_w(size_needed);
            MultiByteToWideChar(CP_UTF8, 0, dos_path.c_str(), -1, dos_w.data(), size_needed);
            
            char* unix_str = wine_get_unix(dos_w.data());
            if (unix_str) {
                std::string result(unix_str);
                HeapFree(GetProcessHeap(), 0, unix_str);
                return result;
            }
        }
    }
#endif

    // Fallback Manual Translation: Strip Z: and flip backslashes
    std::string unix_path = dos_path;
    if (unix_path.length() >= 2 && unix_path[1] == ':') {
        unix_path = unix_path.substr(2); // Strip drive letter
    }
    std::replace(unix_path.begin(), unix_path.end(), '\\', '/');
    return unix_path;
}

WaylandAttributeList::WaylandAttributeList(Steinberg::Vst::IAttributeList* host_list) 
    : host_list_(host_list) {
    if (host_list_) host_list_->addRef();
}

WaylandAttributeList::~WaylandAttributeList() {
    if (host_list_) host_list_->release();
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::queryInterface(const Steinberg::TUID _iid, void** obj) {
    return host_list_ ? host_list_->queryInterface(_iid, obj) : Steinberg::kNoInterface;
}

Steinberg::uint32 PLUGIN_API WaylandAttributeList::addRef() {
    return host_list_ ? host_list_->addRef() : 1;
}

Steinberg::uint32 PLUGIN_API WaylandAttributeList::release() {
    return host_list_ ? host_list_->release() : 1;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::setInt(AttrID id, Steinberg::int64 value) {
    return host_list_ ? host_list_->setInt(id, value) : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::getInt(AttrID id, Steinberg::int64& value) {
    return host_list_ ? host_list_->getInt(id, value) : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::setFloat(AttrID id, double value) {
    return host_list_ ? host_list_->setFloat(id, value) : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::getFloat(AttrID id, double& value) {
    return host_list_ ? host_list_->getFloat(id, value) : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::setString(AttrID id, const Steinberg::Vst::TChar* string) {
    if (!host_list_ || !string) return Steinberg::kInvalidArgument;

    // INTERCEPT: If the DAW is sending a file path, we translate Unix -> DOS
    if (std::string(id) == Steinberg::Vst::PresetAttributes::kFilePathStringType) {
        std::string unix_path = tchar_to_utf8(string);
        std::string dos_path = AraPathTranslator::unix_to_dos(unix_path);
        std::u16string tchar_dos = utf8_to_tchar(dos_path);
        
        return host_list_->setString(id, reinterpret_cast<const Steinberg::Vst::TChar*>(tchar_dos.c_str()));
    }

    return host_list_->setString(id, string);
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::getString(AttrID id, Steinberg::Vst::TChar* string, Steinberg::uint32 sizeInBytes) {
    if (!host_list_ || !string) return Steinberg::kInvalidArgument;

    Steinberg::tresult result = host_list_->getString(id, string, sizeInBytes);
    if (result != Steinberg::kResultOk) return result;

    // INTERCEPT: If the Plugin is returning a file path, we translate DOS -> Unix
    if (std::string(id) == Steinberg::Vst::PresetAttributes::kFilePathStringType) {
        std::string dos_path = tchar_to_utf8(string);
        std::string unix_path = AraPathTranslator::dos_to_unix(dos_path);
        std::u16string tchar_unix = utf8_to_tchar(unix_path);

        // Safely copy back into the provided buffer
        size_t max_chars = (sizeInBytes / sizeof(Steinberg::Vst::TChar)) - 1;
        size_t chars_to_copy = std::min(tchar_unix.length(), max_chars);
        
        auto* dest = reinterpret_cast<char16_t*>(string);
        std::copy_n(tchar_unix.begin(), chars_to_copy, dest);
        dest[chars_to_copy] = 0; // Null terminate
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::setBinary(AttrID id, const void* data, Steinberg::uint32 sizeInBytes) {
    return host_list_ ? host_list_->setBinary(id, data, sizeInBytes) : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::getBinary(AttrID id, const void*& data, Steinberg::uint32& sizeInBytes) {
    return host_list_ ? host_list_->getBinary(id, data, sizeInBytes) : Steinberg::kResultFalse;
}

} // namespace arthur
