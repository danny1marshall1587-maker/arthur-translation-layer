#pragma once

#include <wayland-client.h>
#include <atomic>
#include <mutex>
#include <windows.h>

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
     * Handles resize events from the DAW without blocking the audio or main Wine thread.
     */
    void resize(int width, int height);

    /**
     * Process pending Wayland events. Should be called periodically on the UI thread.
     */
    void pump_events();

private:
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_subcompositor* subcompositor_ = nullptr;

    wl_surface* daw_surface_ = nullptr;
    wl_surface* plugin_surface_ = nullptr;
    wl_subsurface* subsurface_ = nullptr;

    HWND hwnd_ = nullptr;

    std::atomic<int> width_{0};
    std::atomic<int> height_{0};

    // Wayland Registry listeners
    static void registry_handler(void* data, wl_registry* registry, uint32_t id, const char* interface, uint32_t version);
    static void registry_remover(void* data, wl_registry* registry, uint32_t id);
};

} // namespace arthur
