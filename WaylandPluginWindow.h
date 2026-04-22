#pragma once

#ifdef interface
#undef interface
#endif
#include <wayland-client.h>
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include <atomic>
#include <cstdint>
#include <string>

#ifdef __WINE__
#define NOMINMAX
#include <windows.h>
#else
// Stub types for non-Wine builds (Linux-side bridge only)
typedef void* HWND;
#endif

namespace arthur {

/**
 * A native Wayland wrapper that intercepts a Windows HWND (the VST3 plugin UI)
 * and attaches its pixel buffer to a native Wayland wl_subsurface provided by the DAW.
 */
class WaylandPluginWindow {
public:
    /**
     * @param daw_surface The parent wl_surface provided by the Linux Host (DAW)
     * @param width Initial width
     * @param height Initial height
     */
    WaylandPluginWindow(wl_surface* daw_surface, int width, int height);
    ~WaylandPluginWindow();

    /**
     * Attaches the Windows HWND to our managed Wayland subsurface.
     * This relies on Wine's Wayland driver exposing the underlying wl_surface of the HWND.
     */
    bool attach_hwnd(HWND plugin_hwnd);

    /**
     * Export the current surface for another process to import (via xdg-foreign).
     * @return true if export was initiated successfully
     */
    bool export_surface();

    /**
     * Import a surface handle from another process.
     * @param handle The xdg-foreign handle string
     * @return true if import was initiated successfully
     */
    bool import_surface(const std::string& handle);

    /**
     * Returns the exported handle string if available.
     */
    std::string get_exported_handle() const { return handle_; }

    /**
     * Handles resize events from the DAW without blocking the audio or main Wine thread.
     */
    void resize(int width, int height);

    /**
     * Process pending Wayland events. Should be called periodically on the UI thread.
     */
    void pump_events();

    // Wayland Registry listeners
    static void registry_handler(void* data, wl_registry* registry, uint32_t id, const char* iface, uint32_t version);
    static void registry_remover(void* data, wl_registry* registry, uint32_t id);

    // xdg-foreign export listener
    static void handle_exported(void* data, zxdg_exported_v2* exported, const char* handle);

private:
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_subcompositor* subcompositor_ = nullptr;
    zxdg_exporter_v2* exporter_ = nullptr;
    zxdg_importer_v2* importer_ = nullptr;
    zxdg_exported_v2* exported_surface_ = nullptr;
    zxdg_imported_v2* imported_surface_ = nullptr;

    wl_surface* daw_surface_ = nullptr;
    wl_surface* plugin_surface_ = nullptr;
    wl_subsurface* subsurface_ = nullptr;

    HWND hwnd_ = nullptr;

    uint64_t instance_id_ = 0;
    std::string handle_;

    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
};

} // namespace arthur
