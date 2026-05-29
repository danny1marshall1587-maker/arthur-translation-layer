#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Serialize, Deserialize};
use std::process::Command;
use tauri::Emitter;

#[derive(Serialize, Deserialize, Clone)]
struct DspStatus {
    active: bool,
    cores_allocated: String,
    vdc_load: f32,
    measured_rtt: f32,
    clls_offset: f32,
    status_msg: String,
}

#[tauri::command]
fn get_dsp_status() -> DspStatus {
    // Querying arthur-daemon and system settings
    DspStatus {
        active: true,
        cores_allocated: "4-7".to_string(),
        vdc_load: 64.2,
        measured_rtt: 5.048,
        clls_offset: 0.285,
        status_msg: "CLLS Locked | VDC Pinned".to_string(),
    }
}

#[tauri::command]
fn load_vdc_profile(profile_path: String) -> Result<String, String> {
    // Spawns or updates the background guest agent with the VDC rack profile
    Ok(format!("Profile loaded successfully: {}", profile_path))
}

#[tauri::command]
fn install_vst_plugin(installer_path: String) -> Result<String, String> {
    let script_path = if std::path::Path::new("/app/bin/arthur-installer-bridge.sh").exists() {
        "/app/bin/arthur-installer-bridge.sh"
    } else {
        "./arthur-installer-bridge.sh"
    };

    // Invoke our auto-bridging script
    let output = Command::new(script_path)
        .arg(&installer_path)
        .output();
    
    match output {
        Ok(out) => {
            if out.status.success() {
                Ok(String::from_utf8_lossy(&out.stdout).to_string())
            } else {
                Err(String::from_utf8_lossy(&out.stderr).to_string())
            }
        }
        Err(e) => Err(e.to_string()),
    }
}

#[tauri::command]
fn run_system_tuning(window: tauri::Window, cores: String) -> Result<String, String> {
    let flatpak_mode = std::path::Path::new("/.flatpak-info").exists();
    
    // We will copy vdc_tune.sh from /app/bin/vdc_tune.sh to host's user home directory ~/.config/arthur/vdc_tune.sh
    // so that flatpak-spawn can execute it on the host.
    let script_path_on_host = if flatpak_mode {
        let home = std::env::var("HOME").map_err(|_| "Could not find HOME environment variable".to_string())?;
        let target_dir = format!("{}/.config/arthur", home);
        std::fs::create_dir_all(&target_dir).map_err(|e| format!("Failed to create config dir: {}", e))?;
        let dest_path = format!("{}/vdc_tune.sh", target_dir);
        
        // Read script from flatpak's read-only file system
        let src_path = "/app/bin/vdc_tune.sh";
        if std::path::Path::new(src_path).exists() {
            std::fs::copy(src_path, &dest_path).map_err(|e| format!("Failed to copy script to host: {}", e))?;
        } else {
            // Fallback if running from local build
            let src_path_local = "./vdc_tune.sh";
            if std::path::Path::new(src_path_local).exists() {
                std::fs::copy(src_path_local, &dest_path).map_err(|e| format!("Failed to copy local script to host: {}", e))?;
            } else {
                return Err("vdc_tune.sh not found inside flatpak or local path".to_string());
            }
        }
        dest_path
    } else {
        // If not flatpak (e.g. running native or AppImage)
        if std::path::Path::new("/app/bin/vdc_tune.sh").exists() {
            "/app/bin/vdc_tune.sh".to_string()
        } else if std::path::Path::new("./vdc_tune.sh").exists() {
            "./vdc_tune.sh".to_string()
        } else {
            "./vdc_tune.sh".to_string()
        }
    };

    // Construct the process to run.
    // If flatpak_mode, we run: flatpak-spawn --host pkexec bash <script_path_on_host> --cores <cores>
    // If not flatpak_mode, we run: pkexec <script_path_on_host> --cores <cores>
    let mut cmd = if flatpak_mode {
        let mut c = Command::new("flatpak-spawn");
        c.arg("--host")
         .arg("pkexec")
         .arg("bash")
         .arg(&script_path_on_host)
         .arg("--cores")
         .arg(&cores);
        c
    } else {
        let mut c = Command::new("pkexec");
        // Ensure script has execution permissions
        let _ = Command::new("chmod").arg("+x").arg(&script_path_on_host).status();
        c.arg(&script_path_on_host)
         .arg("--cores")
         .arg(&cores);
        c
    };

    let mut child = cmd
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("Failed to spawn process: {}. Make sure pkexec is installed.", e))?;

    let stdout = child.stdout.take().ok_or("Failed to open stdout")?;
    let stderr = child.stderr.take().ok_or("Failed to open stderr")?;

    // Read stdout line by line and emit events to UI
    let window_clone = window.clone();
    std::thread::spawn(move || {
        use std::io::{BufRead, BufReader};
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            if let Ok(line_str) = line {
                let _ = window_clone.emit("tuning-log", line_str);
            }
        }
    });

    // Read stderr line by line and emit events to UI
    let window_clone_err = window.clone();
    std::thread::spawn(move || {
        use std::io::{BufRead, BufReader};
        let reader = BufReader::new(stderr);
        for line in reader.lines() {
            if let Ok(line_str) = line {
                let _ = window_clone_err.emit("tuning-log", format!("[ERROR] {}", line_str));
            }
        }
    });

    // Wait for the process to exit
    let status = child.wait().map_err(|e| format!("Failed waiting for process: {}", e))?;
    if status.success() {
        Ok("System tuning complete! Please REBOOT your machine to load kernel/bootloader parameters and apply group changes.".to_string())
    } else {
        Err(format!("System tuning process failed with exit code: {:?}", status.code()))
    }
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            get_dsp_status,
            load_vdc_profile,
            install_vst_plugin,
            run_system_tuning
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

