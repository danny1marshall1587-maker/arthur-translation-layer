#include "WaylandPluginWindow.h"
#include <iostream>
#include <cstring>
#include "xdg-foreign-unstable-v2-protocol.c"

namespace arthur {

static const struct wl_registry_listener registry_listener = {
    WaylandPluginWindow::registry_handler,
    WaylandPluginWindow::registry_remover
};

static const struct zxdg_exported_v2_listener exported_listener = {
    WaylandPluginWindow::handle_exported
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

    if (!compositor_) {
        std::cerr << "[WaylandPluginWindow] Wayland compositor not found!" << std::endl;
        return;
    }

    // Create our plugin's dedicated surface
    plugin_surface_ = wl_compositor_create_surface(compositor_);
}

WaylandPluginWindow::~WaylandPluginWindow() {
    if (imported_surface_) zxdg_imported_v2_destroy(imported_surface_);
    if (exported_surface_) zxdg_exported_v2_destroy(exported_surface_);
    if (importer_) zxdg_importer_v2_destroy(importer_);
    if (exporter_) zxdg_exporter_v2_destroy(exporter_);
    if (subsurface_) wl_subsurface_destroy(subsurface_);
    if (plugin_surface_) wl_surface_destroy(plugin_surface_);
    if (subcompositor_) wl_subcompositor_destroy(subcompositor_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

void WaylandPluginWindow::registry_handler(void* data, wl_registry* registry, uint32_t id, const char* iface, uint32_t version) {
    WaylandPluginWindow* self = static_cast<WaylandPluginWindow*>(data);
    
    if (std::strcmp(iface, "wl_compositor") == 0) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, id, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(iface, "wl_subcompositor") == 0) {
        self->subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(iface, "zxdg_exporter_v2") == 0) {
        self->exporter_ = static_cast<zxdg_exporter_v2*>(
            wl_registry_bind(registry, id, &zxdg_exporter_v2_interface, 1));
    } else if (std::strcmp(iface, "zxdg_importer_v2") == 0) {
        self->importer_ = static_cast<zxdg_importer_v2*>(
            wl_registry_bind(registry, id, &zxdg_importer_v2_interface, 1));
    }
}

void WaylandPluginWindow::registry_remover(void* data, wl_registry* registry, uint32_t id) {
}

void WaylandPluginWindow::handle_exported(void* data, zxdg_exported_v2* exported, const char* handle) {
    auto* self = static_cast<WaylandPluginWindow*>(data);
    self->handle_ = handle;
    std::cerr << "[WaylandPluginWindow] Surface exported with handle: " << handle << std::endl;
}

bool WaylandPluginWindow::attach_hwnd(HWND plugin_hwnd) {
    if (!plugin_surface_ || !daw_surface_ || !subcompositor_) return false;

    hwnd_ = plugin_hwnd;

    // Create the subsurface binding our plugin surface as a child of the DAW's surface
    subsurface_ = wl_subcompositor_get_subsurface(subcompositor_, plugin_surface_, daw_surface_);
    
    // Set synchronized mode to true initially to prevent tearing during initial mapping
    wl_subsurface_set_sync(subsurface_);
    wl_subsurface_set_position(subsurface_, 0, 0);

#ifdef __WINE__
    /* 
     * In the Wine Wayland driver, we map the HWND to the Wayland surface.
     */
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, width_.load(), height_.load(), SWP_SHOWWINDOW);
#endif

    wl_surface_commit(daw_surface_);
    return true;
}

bool WaylandPluginWindow::export_surface() {
    if (!exporter_ || !daw_surface_) return false;
    
    exported_surface_ = zxdg_exporter_v2_export_toplevel(exporter_, daw_surface_);
    zxdg_exported_v2_add_listener(exported_surface_, &exported_listener, this);
    
    // Wait for handle
    wl_display_roundtrip(display_);
    return !handle_.empty();
}

bool WaylandPluginWindow::import_surface(const std::string& handle) {
    if (!importer_) return false;
    
    imported_surface_ = zxdg_importer_v2_import_toplevel(importer_, handle.c_str());
    // In a real implementation we would also listen for the imported surface's events
    wl_display_roundtrip(display_);
    return true;
}

void WaylandPluginWindow::resize(int width, int height) {
    width_.store(width, std::memory_order_relaxed);
    height_.store(height, std::memory_order_relaxed);

#ifdef __WINE__
    if (hwnd_) {
        PostMessageA(hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
    }
#endif
}

void WaylandPluginWindow::pump_events() {
    if (display_) {
        wl_display_dispatch_pending(display_);
        wl_display_flush(display_);
    }
}

} // namespace arthur
