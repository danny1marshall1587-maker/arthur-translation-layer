#include "AraPathTranslator.h"
#include <pluginterfaces/vst/vstpresetkeys.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

#ifdef __WINE__
#include <windows.h>
// In a real Wine build, wine_get_unix_file_name and wine_get_dos_file_name 
// can be loaded via GetProcAddress from kernel32.dll
typedef char* (*wine_get_unix_file_name_ptr)(const WCHAR* dosW);
typedef WCHAR* (*wine_get_dos_file_name_ptr)(const char* unix_name);
#endif

namespace arthur {

// Helper to convert UTF-16 (Vst::TChar) to UTF-8 (deprecated-free)
static std::string tchar_to_utf8(const Steinberg::Vst::TChar* tchar_str) {
    if (!tchar_str) return "";
    std::string result;
    for (int i = 0; tchar_str[i] != 0; ++i) {
        uint16_t code = (uint16_t)tchar_str[i];
        if (code < 0x80) {
            result.push_back((char)code);
        } else if (code < 0x800) {
            result.push_back((char)(0xC0 | (code >> 6)));
            result.push_back((char)(0x80 | (code & 0x3F)));
        } else {
            result.push_back((char)(0xE0 | (code >> 12)));
            result.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            result.push_back((char)(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

// Helper to convert UTF-8 to UTF-16 (Vst::TChar) (deprecated-free)
static std::u16string utf8_to_tchar(const std::string& utf8_str) {
    std::u16string result;
    for (size_t i = 0; i < utf8_str.length(); ) {
        uint8_t c = utf8_str[i];
        uint32_t code = 0;
        if (c < 0x80) {
            code = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < utf8_str.length()) {
                code = ((c & 0x1F) << 6) | (utf8_str[i+1] & 0x3F);
                i += 2;
            } else { i += 1; }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < utf8_str.length()) {
                code = ((c & 0x0F) << 12) | ((utf8_str[i+1] & 0x3F) << 6) | (utf8_str[i+2] & 0x3F);
                i += 3;
            } else { i += 1; }
        } else {
            i += 1;
        }
        if (code != 0) {
            result.push_back((char16_t)code);
        }
    }
    return result;
}

std::string AraPathTranslator::unix_to_dos(const std::string& unix_path) {
    if (unix_path.empty()) return "";
    
    std::string clean_path = unix_path;
    // Strip file URI schema if present
    if (clean_path.find("file://") == 0) {
        clean_path = clean_path.substr(7);
    }
    
#ifdef __WINE__
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32) {
        auto wine_get_dos = (wine_get_dos_file_name_ptr)GetProcAddress(kernel32, "wine_get_dos_file_name");
        if (wine_get_dos) {
            WCHAR* dos_w = wine_get_dos(clean_path.c_str());
            if (dos_w) {
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, dos_w, -1, NULL, 0, NULL, NULL);
                std::string dos_str(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, dos_w, -1, &dos_str[0], size_needed, NULL, NULL);
                HeapFree(GetProcessHeap(), 0, dos_w);
                if (!dos_str.empty() && dos_str.back() == '\0') dos_str.pop_back();
                return dos_str;
            }
        }
    }
#endif

    // Fallback Manual Translation: Prepend Z: and flip slashes
    std::string dos_path = "Z:" + clean_path;
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
    : ref_count(1), host_list_(host_list) {
    if (host_list_) host_list_->addRef();
}

WaylandAttributeList::~WaylandAttributeList() {
    if (host_list_) host_list_->release();
}

Steinberg::tresult PLUGIN_API WaylandAttributeList::queryInterface(const Steinberg::TUID _iid, void** obj) {
    if (std::memcmp(_iid, Steinberg::Vst::IAttributeList::iid, 16) == 0 ||
        std::memcmp(_iid, Steinberg::FUnknown::iid, 16) == 0) {
        *obj = static_cast<Steinberg::Vst::IAttributeList*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    return host_list_ ? host_list_->queryInterface(_iid, obj) : Steinberg::kNoInterface;
}

Steinberg::uint32 PLUGIN_API WaylandAttributeList::addRef() {
    return ++ref_count;
}

Steinberg::uint32 PLUGIN_API WaylandAttributeList::release() {
    Steinberg::uint32 count = --ref_count;
    if (count == 0) {
        delete this;
        return 0;
    }
    return count;
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

    // INTERCEPT: If the DAW is sending a file path, we translate Unix -> DOS (safe id null-checking)
    if (id && std::strcmp(id, Steinberg::Vst::PresetAttributes::kFilePathStringType) == 0) {
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

    // INTERCEPT: If the Plugin is returning a file path, we translate DOS -> Unix (safe id null-checking)
    if (id && std::strcmp(id, Steinberg::Vst::PresetAttributes::kFilePathStringType) == 0) {
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
