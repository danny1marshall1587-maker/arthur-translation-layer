#include "WaylandPluginWindow.h"
#include <iostream>
#include <cstring>

namespace arthur {

static const struct wl_registry_listener registry_listener = {
    WaylandPluginWindow::registry_handler,
    WaylandPluginWindow::registry_remover
};

WaylandPluginWindow::WaylandPluginWindow(wl_surface* daw_surface, int width, int height)
    : daw_surface_(daw_surface), width_(width), height_(height) {
    
    // Connect to the Wayland display
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::cerr << "[WaylandPluginWindow] Failed to connect to Wayland display." << std::endl;
        return;
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);

    // Roundtrip to get compositor and subcompositor
    wl_display_roundtrip(display_);

    if (!compositor_ || !subcompositor_) {
        std::cerr << "[WaylandPluginWindow] Wayland compositor/subcompositor not found!" << std::endl;
        return;
    }

    // Create our plugin's dedicated surface
    plugin_surface_ = wl_compositor_create_surface(compositor_);
}

WaylandPluginWindow::~WaylandPluginWindow() {
    if (subsurface_) wl_subsurface_destroy(subsurface_);
    if (plugin_surface_) wl_surface_destroy(plugin_surface_);
    if (subcompositor_) wl_subcompositor_destroy(subcompositor_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

void WaylandPluginWindow::registry_handler(void* data, wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    WaylandPluginWindow* self = static_cast<WaylandPluginWindow*>(data);
    
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, id, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1));
    }
}

void WaylandPluginWindow::registry_remover(void* data, wl_registry* registry, uint32_t id) {
    // Handle destruction of globals if necessary
}

bool WaylandPluginWindow::attach_hwnd(HWND plugin_hwnd) {
    if (!plugin_surface_ || !daw_surface_ || !subcompositor_) return false;

    hwnd_ = plugin_hwnd;

    // Create the subsurface binding our plugin surface as a child of the DAW's surface
    subsurface_ = wl_subcompositor_get_subsurface(subcompositor_, plugin_surface_, daw_surface_);
    
    // Set synchronized mode to true initially to prevent tearing during initial mapping
    wl_subsurface_set_sync(subsurface_);

    // Position it at 0,0 relative to the DAW surface (or apply offset as needed)
    wl_subsurface_set_position(subsurface_, 0, 0);

    /* 
     * THE MAGIC WINE 11 WAYLAND HOOK:
     * In the new Wine Wayland driver, we map the HWND to the Wayland surface.
     * We trigger a SetWindowPos or use Wine's internal Wayland extensions 
     * to bind the DIB/Vulkan context of the HWND to `plugin_surface_`.
     */
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, width_.load(), height_.load(), SWP_SHOWWINDOW);

    // Commit changes to the parent
    wl_surface_commit(daw_surface_);
    return true;
}

void WaylandPluginWindow::resize(int width, int height) {
    width_.store(width, std::memory_order_relaxed);
    height_.store(height, std::memory_order_relaxed);

    if (hwnd_) {
        // Trigger a Win32 resize message on the HWND. 
        // This runs asynchronously so it doesn't block the DAW.
        PostMessageA(hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
    }
}

void WaylandPluginWindow::pump_events() {
    if (display_) {
        // Non-blocking event dispatch for the Wayland queue
        wl_display_dispatch_pending(display_);
        wl_display_flush(display_);
    }
}

} // namespace arthur
