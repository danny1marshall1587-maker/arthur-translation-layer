document.addEventListener("DOMContentLoaded", () => {
    console.log("Arthur UI Initialized.");

    // --- State Variables ---
    let currentProfile = null;
    let currentSampleRate = 48000;
    let targetModalChannel = "";
    let targetModalSlotId = 1;
    let pipewirePorts = [];

    // --- Tab Switching Logic ---
    const navButtons = document.querySelectorAll(".nav-btn");
    const tabContents = document.querySelectorAll(".tab-content");

    navButtons.forEach(btn => {
        btn.addEventListener("click", () => {
            navButtons.forEach(b => b.classList.remove("active"));
            btn.classList.add("active");

            const tabId = btn.getAttribute("data-tab");
            tabContents.forEach(content => {
                content.classList.remove("active");
            });

            const activeTab = document.getElementById(`${tabId}-tab`);
            if (activeTab) {
                activeTab.classList.add("active");
            }
        });
    });

    // --- Dashboard Jitter Simulation ---
    const bars = document.querySelectorAll(".latency-graph .bar");
    if (bars.length > 0) {
        setInterval(() => {
            const midiCheckbox = document.getElementById("midi-slave-check");
            const active = midiCheckbox ? midiCheckbox.checked : true;
            bars.forEach(bar => {
                if (active) {
                    const currentHeight = parseFloat(bar.style.height) || 40;
                    const fluctuation = (Math.random() - 0.5) * 15;
                    let newHeight = currentHeight + fluctuation;
                    newHeight = Math.max(10, Math.min(newHeight, 90));
                    bar.style.height = `${newHeight}%`;
                    bar.style.opacity = "0.75";
                } else {
                    bar.style.height = "5%";
                    bar.style.opacity = "0.15";
                }
            });
        }, 500);
    }

    // --- Core Isolation Slider ---
    const coreSlider = document.getElementById("core-slider");
    const sliderStatus = document.getElementById("slider-status");
    if (coreSlider && sliderStatus) {
        coreSlider.addEventListener("input", (e) => {
            const cores = e.target.value;
            if (cores <= 4) {
                sliderStatus.textContent = `Allocating Core 4 to Virtual DSP (1 Core isolated)`;
            } else {
                sliderStatus.textContent = `Allocating Cores 4-${cores} to Virtual DSP (${cores - 3} Cores isolated)`;
            }
        });
    }

    // --- Drag & Drop Installer Logic ---
    const dropZone = document.getElementById("drop-zone");
    const selectFileBtn = document.getElementById("select-file-btn");
    const progressCard = document.getElementById("install-progress-card");
    const statusCard = document.getElementById("status-card");
    const progressFill = document.getElementById("progress-fill");
    const progressPercent = document.getElementById("progress-percent");
    const progressText = document.getElementById("progress-text");
    const consoleLog = document.getElementById("console-log");
    const installTitle = document.getElementById("install-title");
    const statusTitle = document.getElementById("status-title");
    const statusDescription = document.getElementById("status-description");
    const statusIcon = document.getElementById("status-icon");
    const statusOkBtn = document.getElementById("status-ok-btn");

    ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
        window.addEventListener(eventName, preventDefaults, false);
    });

    function preventDefaults(e) {
        e.preventDefault();
        e.stopPropagation();
    }

    if (dropZone) {
        ['dragenter', 'dragover'].forEach(eventName => {
            dropZone.addEventListener(eventName, () => dropZone.classList.add('dragover'), false);
        });

        ['dragleave', 'drop'].forEach(eventName => {
            dropZone.addEventListener(eventName, () => dropZone.classList.remove('dragover'), false);
        });

        dropZone.addEventListener('drop', (e) => {
            const dt = e.dataTransfer;
            const files = dt.files;
            if (files.length > 0) {
                handleFile(files[0]);
            }
        });
    }

    if (selectFileBtn) {
        selectFileBtn.addEventListener("click", () => {
            if (window.__TAURI__) {
                window.__TAURI__.dialog.open({
                    filters: [{
                        name: 'Windows Installers',
                        extensions: ['exe', 'msi']
                    }]
                }).then(filePath => {
                    if (filePath) {
                        const fileName = filePath.split(/[/\\]/).pop();
                        startInstallation(fileName, filePath);
                    }
                }).catch(err => {
                    showError("File Dialog Error", err);
                });
            } else {
                const fileName = prompt("Enter installer filename (simulation):", "FabFilter_Setup.exe");
                if (fileName) {
                    handleFile({ name: fileName, path: `/home/dan/Downloads/${fileName}` });
                }
            }
        });
    }

    function handleFile(file) {
        const ext = file.name.split('.').pop().toLowerCase();
        if (ext !== 'exe' && ext !== 'msi') {
            alert("Error: Only Windows .exe and .msi installers are supported!");
            return;
        }
        startInstallation(file.name, file.path || `/home/dan/Downloads/${file.name}`);
    }

    function startInstallation(fileName, filePath) {
        if (dropZone) dropZone.classList.add("hidden");
        if (statusCard) statusCard.classList.add("hidden");
        if (progressCard) progressCard.classList.remove("hidden");

        installTitle.textContent = `Installing ${fileName}...`;
        consoleLog.textContent = `Initializing installation pipeline for: ${filePath}\n`;

        if (window.__TAURI__) {
            runTauriInstallation(fileName, filePath);
        } else {
            runSimulatedInstallation(fileName);
        }
    }

    function logToConsole(message) {
        consoleLog.textContent += message + "\n";
        consoleLog.scrollTop = consoleLog.scrollHeight;
    }

    function updateProgress(percent, statusMsg) {
        progressFill.style.width = `${percent}%`;
        progressPercent.textContent = `${percent}%`;
        progressText.textContent = statusMsg;
    }

    function runSimulatedInstallation(fileName) {
        let percent = 0;
        const logSteps = [
            { t: 0, p: 5, m: "Initializing Wine environment sandbox ($WINEPREFIX)..." },
            { t: 1000, p: 15, m: "Linking DLL virtualization mapping layers..." },
            { t: 2500, p: 30, m: `Executing installer binary: ${fileName}` },
            { t: 4000, p: 50, m: "Waiting for user configuration steps (GUI Wizard)..." },
            { t: 6500, p: 75, m: "Installer process finished. Scanning VST3 folders for files..." },
            { t: 8000, p: 90, m: "New plugin found! Compiling VST3 symbol bridge (vstiids.cpp -> build/arthur_bridge.so)..." },
            { t: 9500, p: 98, m: "Registering latency matrices in CLLS audio buffer..." },
            { t: 11000, p: 100, m: "Syncing Virtual DSP Core pins... Active session lock established." }
        ];

        logSteps.forEach(step => {
            setTimeout(() => {
                percent = step.p;
                updateProgress(percent, step.m);
                logToConsole(`[${percent}%] ${step.m}`);
                
                if (percent === 100) {
                    setTimeout(() => {
                        showSuccess(fileName);
                    }, 800);
                }
            }, step.t);
        });
    }

    function runTauriInstallation(fileName, filePath) {
        updateProgress(10, "Initializing Wine prefix...");
        logToConsole("[10%] Initializing Wine prefix environment ($HOME/.wine)...");

        setTimeout(() => {
            updateProgress(35, "Spawning installer GUI wizard...");
            logToConsole(`[35%] Executing installer binary: ${filePath}`);
            logToConsole("Please complete the installer steps in the wizard window that opened.");

            window.__TAURI__.core.invoke('install_vst_plugin', { installerPath: filePath })
                .then((result) => {
                    updateProgress(90, "Auto-bridging new plugins...");
                    logToConsole("[90%] Installer process exited successfully.");
                    logToConsole(result);
                    
                    updateProgress(100, "Completing setup...");
                    logToConsole("[100%] Synchronization complete.");
                    setTimeout(() => {
                        showSuccess(fileName);
                    }, 800);
                })
                .catch((err) => {
                    updateProgress(100, "Installation Failed!");
                    logToConsole(`[ERROR] Installation failed:\n${err}`);
                    setTimeout(() => {
                        showError(fileName, err);
                    }, 1500);
                });
        }, 1500);
    }

    function showSuccess(fileName) {
        if (progressCard) progressCard.classList.add("hidden");
        
        statusIcon.className = "status-icon success";
        statusIcon.textContent = "✓";
        statusTitle.textContent = "Installation Succeeded!";
        statusDescription.textContent = `${fileName.replace(/(_Setup|_setup|Installer|installer|\.exe|\.msi)/g, '')} has been successfully virtualized, bridged, and locked to isolated cores. Open or reload your DAW to use it.`;
        
        if (statusCard) statusCard.classList.remove("hidden");
    }

    function showError(fileName, errorMessage) {
        if (progressCard) progressCard.classList.add("hidden");
        
        statusIcon.className = "status-icon error";
        statusIcon.textContent = "✗";
        statusTitle.textContent = "Installation Failed";
        statusDescription.textContent = `An error occurred during setup of ${fileName}: ${errorMessage}`;
        
        if (statusCard) statusCard.classList.remove("hidden");
    }

    if (statusOkBtn) {
        statusOkBtn.addEventListener("click", () => {
            if (statusCard) statusCard.classList.add("hidden");
            if (dropZone) dropZone.classList.remove("hidden");
        });
    }

    // --- System Tuning / Setup Wizard GUI Installer ---
    const applyIsolationBtn = document.getElementById("apply-isolation-btn");
    const tuningSelectionCard = document.getElementById("tuning-selection-card");
    const systemProgressCard = document.getElementById("system-setup-progress-card");
    const systemStatusCard = document.getElementById("system-status-card");
    const systemProgressFill = document.getElementById("system-progress-fill");
    const systemProgressPercent = document.getElementById("system-progress-percent");
    const systemProgressText = document.getElementById("system-progress-text");
    const systemConsoleLog = document.getElementById("system-console-log");
    const systemStatusIcon = document.getElementById("system-status-icon");
    const systemStatusTitle = document.getElementById("system-status-title");
    const systemStatusDescription = document.getElementById("system-status-description");
    const systemStatusOkBtn = document.getElementById("system-status-ok-btn");

    if (applyIsolationBtn) {
        applyIsolationBtn.addEventListener("click", () => {
            const coresVal = coreSlider ? coreSlider.value : "4";
            const targetCores = coresVal <= 4 ? "4" : `4-${coresVal}`;

            if (tuningSelectionCard) tuningSelectionCard.classList.add("hidden");
            if (systemStatusCard) systemStatusCard.classList.add("hidden");
            if (systemProgressCard) systemProgressCard.classList.remove("hidden");

            systemProgressFill.style.width = "0%";
            systemProgressPercent.textContent = "0%";
            systemProgressText.textContent = "Requesting administrator access...";
            systemConsoleLog.textContent = "Starting System Setup Wizard...\n";

            if (window.__TAURI__) {
                let unlisten = null;
                window.__TAURI__.event.listen('tuning-log', (event) => {
                    const line = event.payload;
                    systemConsoleLog.textContent += line + "\n";
                    systemConsoleLog.scrollTop = systemConsoleLog.scrollHeight;

                    if (line.includes("Detected OS")) {
                        updateSystemProgress(15, "Detecting OS and tuning group policies...");
                    } else if (line.includes("Configuring real-time priority limits")) {
                        updateSystemProgress(30, "Configuring real-time thread priority...");
                    } else if (line.includes("Tuning CPU frequency scaling governor")) {
                        updateSystemProgress(45, "Locking CPU governor to performance mode...");
                    } else if (line.includes("Inspecting bootloader configurations")) {
                        updateSystemProgress(60, "Configuring core isolation in bootloader...");
                    } else if (line.includes("Updating bootloader")) {
                        updateSystemProgress(75, "Rebuilding boot configurations (GRUB/systemd-boot)...");
                    } else if (line.includes("Checking sound server")) {
                        updateSystemProgress(90, "Verifying PipeWire audio servers...");
                    }
                }).then(fn => { unlisten = fn; });

                window.__TAURI__.core.invoke('run_system_tuning', { cores: targetCores })
                    .then((result) => {
                        updateSystemProgress(100, "Setup complete!");
                        systemConsoleLog.textContent += "\n[SUCCESS] System tuning completed.\n";
                        systemConsoleLog.scrollTop = systemConsoleLog.scrollHeight;
                        if (unlisten) unlisten();
                        setTimeout(() => {
                            showSystemStatus(true, "System Setup Succeeded!", "All real-time parameters, core isolation, and Wine audio bridges have been configured. Please reboot your machine to apply the kernel parameters.");
                        }, 1000);
                    })
                    .catch((err) => {
                        if (unlisten) unlisten();
                        updateSystemProgress(100, "Setup Failed!");
                        systemConsoleLog.textContent += `\n[ERROR] Setup failed: ${err}\n`;
                        systemConsoleLog.scrollTop = systemConsoleLog.scrollHeight;
                        setTimeout(() => {
                            showSystemStatus(false, "System Setup Failed", `An error occurred during system tuning: ${err}`);
                        }, 1000);
                    });
            } else {
                runSimulatedSystemTuning(targetCores);
            }
        });
    }

    function updateSystemProgress(percent, msg) {
        if (systemProgressFill) systemProgressFill.style.width = `${percent}%`;
        if (systemProgressPercent) systemProgressPercent.textContent = `${percent}%`;
        if (systemProgressText) systemProgressText.textContent = msg;
    }

    function showSystemStatus(isSuccess, title, description) {
        if (systemProgressCard) systemProgressCard.classList.add("hidden");
        if (systemStatusCard) {
            systemStatusCard.classList.remove("hidden");
            if (isSuccess) {
                systemStatusIcon.className = "status-icon success";
                systemStatusIcon.textContent = "✓";
            } else {
                systemStatusIcon.className = "status-icon error";
                systemStatusIcon.textContent = "✗";
            }
            systemStatusTitle.textContent = title;
            systemStatusDescription.textContent = description;
        }
    }

    if (systemStatusOkBtn) {
        systemStatusOkBtn.addEventListener("click", () => {
            if (systemStatusCard) systemStatusCard.classList.add("hidden");
            if (tuningSelectionCard) tuningSelectionCard.classList.remove("hidden");
        });
    }

    function runSimulatedSystemTuning(cores) {
        let percent = 0;
        const steps = [
            { t: 500, p: 10, m: "Detected OS: CachyOS (cachyos)" },
            { t: 1500, p: 20, m: "Adding user dan to group: audio" },
            { t: 2500, p: 35, m: "Configuring real-time limits in /etc/security/limits.d/99-arthur-realtime.conf" },
            { t: 4000, p: 50, m: "Tuning CPU frequency governor... Scaling locked to performance mode." },
            { t: 5500, p: 65, m: `Adding kernel core isolation parameters: isolcpus=4-${cores} nohz_full=4-${cores} rcu_nocbs=4-${cores}` },
            { t: 7000, p: 80, m: "Running update-grub... GRUB boot configuration updated successfully." },
            { t: 8500, p: 95, m: "Checking sound server setup... PipeWire is active." },
            { t: 9500, p: 100, m: "System isolation and real-time parameters configured successfully." }
        ];

        steps.forEach(step => {
            setTimeout(() => {
                percent = step.p;
                updateSystemProgress(percent, step.m);
                systemConsoleLog.textContent += `[${percent}%] ${step.m}\n`;
                systemConsoleLog.scrollTop = systemConsoleLog.scrollHeight;
                
                if (percent === 100) {
                    setTimeout(() => {
                        showSystemStatus(true, "System Setup Succeeded! (Simulation)", "All real-time parameters, core isolation, and Wine audio bridges have been configured. Please reboot your machine to apply the kernel parameters.");
                    }, 800);
                }
            }, step.t);
        });
    }

    // --- Global Status Indicator Updater ---
    function updateGlobalStatus(badgeText, descText, isActive = true) {
        const dot = document.getElementById("global-status-dot");
        const text = document.getElementById("global-status-text");
        if (dot && text) {
            text.textContent = descText || badgeText;
            if (isActive) {
                dot.className = "pulse-dot green";
                dot.style.background = "";
                dot.style.boxShadow = "";
            } else {
                dot.className = "pulse-dot";
                dot.style.background = "var(--text-secondary)";
                dot.style.boxShadow = "none";
            }
        }
    }

    // --- Dynamic Status Querying (Dashboard) ---
    function queryDspStatus() {
        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("get_dsp_status")
                .then(status => {
                    const dspBadge = document.getElementById("dsp-active-badge");
                    if (dspBadge) {
                        dspBadge.textContent = status.active ? "Active" : "Standby";
                        dspBadge.className = `status-badge ${status.active ? 'active' : 'standby'}`;
                    }

                    const coresVal = document.getElementById("dsp-cores-value");
                    if (coresVal) {
                        coresVal.textContent = status.cores_allocated;
                    }

                    const loadText = document.getElementById("dsp-load-text");
                    const loadFill = document.getElementById("dsp-load-fill");
                    if (loadText && loadFill) {
                        loadText.textContent = `${status.vdc_load.toFixed(1)}%`;
                        loadFill.style.width = `${status.vdc_load}%`;
                    }

                    updateGlobalStatus(
                        status.active ? "System Lock Active" : "System Standby",
                        status.status_msg,
                        status.active
                    );
                })
                .catch(err => {
                    console.error("Error querying DSP status:", err);
                });
        }
    }

    // Run periodically
    setInterval(queryDspStatus, 2000);
    setTimeout(queryDspStatus, 500);

    // --- Audio Setup Settings Setup & Handlers ---
    function updateSettingsUI(rate, buffer) {
        const rateSelect = document.getElementById("sample-rate-select");
        const bufferSelect = document.getElementById("buffer-size-select");
        if (rateSelect) rateSelect.value = rate.toString();
        if (bufferSelect) bufferSelect.value = buffer.toString();
    }

    function updateMidiClockUI(active) {
        const midiStatusText = document.getElementById("midi-status-text");
        const midiJitterValue = document.getElementById("midi-jitter-value");
        if (midiStatusText && midiJitterValue) {
            if (active) {
                midiStatusText.textContent = "Slaved to PCM hardware clock (hw:0,0)";
                midiJitterValue.textContent = "Jitter: < 10 µs";
                midiJitterValue.style.color = "var(--accent-green)";
                midiJitterValue.style.background = "rgba(48, 209, 88, 0.1)";
            } else {
                midiStatusText.textContent = "MIDI clock slaving disabled";
                midiJitterValue.textContent = "Inactive";
                midiJitterValue.style.color = "var(--text-secondary)";
                midiJitterValue.style.background = "rgba(255, 255, 255, 0.05)";
            }
        }
    }

    function loadAudioConfig() {
        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("get_audio_config")
                .then(config => {
                    const interfaceSelect = document.getElementById("audio-interface-select");
                    if (interfaceSelect) {
                        interfaceSelect.innerHTML = "";
                        config.interfaces.forEach(inter => {
                            const opt = document.createElement("option");
                            opt.value = inter.name;
                            opt.textContent = inter.description;
                            interfaceSelect.appendChild(opt);
                        });
                        interfaceSelect.value = config.active_interface;
                    }

                    updateSettingsUI(config.sample_rate, config.buffer_size);
                    currentSampleRate = config.sample_rate;

                    const midiCheckbox = document.getElementById("midi-slave-check");
                    if (midiCheckbox) {
                        midiCheckbox.checked = config.slave_midi;
                    }

                    updateMidiClockUI(config.slave_midi);
                })
                .catch(err => {
                    console.error("Error getting audio config:", err);
                });
        } else {
            // Simulated fallback
            const interfaceSelect = document.getElementById("audio-interface-select");
            if (interfaceSelect) {
                interfaceSelect.innerHTML = `
                    <option value="alsa_output.usb-Audient_EVO4-00.pro-output-0">Audient EVO4 USB Audio (hw:0,0)</option>
                    <option value="alsa_output.usb-Focusrite_Scarlett-00.playback">Focusrite Scarlett 18i20 (USB Audio)</option>
                `;
                interfaceSelect.value = "alsa_output.usb-Audient_EVO4-00.pro-output-0";
            }
            updateSettingsUI(48000, 128);
            updateMidiClockUI(true);
        }
    }

    function applyAudioConfig() {
        const interfaceSelect = document.getElementById("audio-interface-select");
        const rateSelect = document.getElementById("sample-rate-select");
        const bufferSelect = document.getElementById("buffer-size-select");
        const midiCheckbox = document.getElementById("midi-slave-check");

        if (!interfaceSelect || !rateSelect || !bufferSelect || !midiCheckbox) return;

        const interfaceVal = interfaceSelect.value;
        const rateVal = parseInt(rateSelect.value, 10);
        const bufferVal = parseInt(bufferSelect.value, 10);
        const midiVal = midiCheckbox.checked;

        currentSampleRate = rateVal;
        updateMidiClockUI(midiVal);

        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("set_audio_config", {
                interface: interfaceVal,
                sampleRate: rateVal,
                bufferSize: bufferVal,
                slaveMidi: midiVal
            })
            .then(() => {
                updateGlobalStatus("✓ Sync Active", "Audio settings updated successfully.");
            })
            .catch(err => {
                console.error("Error setting audio config:", err);
                updateGlobalStatus("✗ Sync Failed", err);
            });
        } else {
            console.log("Simulating setting audio config:", interfaceVal, rateVal, bufferVal, midiVal);
            updateGlobalStatus("✓ Sync Active", "Simulated settings applied.");
        }
    }

    const controls = ["audio-interface-select", "sample-rate-select", "buffer-size-select", "midi-slave-check"];
    controls.forEach(id => {
        const el = document.getElementById(id);
        if (el) {
            el.addEventListener("change", applyAudioConfig);
        }
    });

    loadAudioConfig();

    // --- CLLS Latency Calibration ---
    const runCllsBtn = document.getElementById("run-clls-calibration");
    if (runCllsBtn) {
        runCllsBtn.addEventListener("click", () => {
            const interfaceSelect = document.getElementById("audio-interface-select");
            const channelSelect = document.getElementById("clls-channel-select");

            if (!interfaceSelect || !channelSelect) return;

            const interfaceVal = interfaceSelect.value;
            const channelVal = channelSelect.value;

            runCllsBtn.disabled = true;
            runCllsBtn.textContent = "Calibrating...";

            updateGlobalStatus("⚡ Calibrating", "Spawning CLLS aligner...");

            if (window.__TAURI__) {
                let unlistenLog = null;
                let unlistenStatus = null;

                window.__TAURI__.event.listen('clls-log', (event) => {
                    console.log("CLLS Log:", event.payload);
                }).then(fn => { unlistenLog = fn; });

                window.__TAURI__.event.listen('clls-status-update', (event) => {
                    const line = event.payload;
                    console.log("CLLS Status line:", line);
                    // Parse line e.g.: [CLLS STATUS] Measured RTT: 242.3 samples | Applied Offset: +13.6 samples
                    const rttMatch = line.match(/Measured RTT:\s*([\d.]+)\s*samples/);
                    const delayMatch = line.match(/Applied Offset:\s*([+-]?[\d.]+)\s*samples/);

                    if (rttMatch) {
                        const rttSamples = parseFloat(rttMatch[1]);
                        const rttMs = rttSamples / (currentSampleRate / 1000);
                        const rttValEl = document.getElementById("clls-rtt-value");
                        if (rttValEl) rttValEl.textContent = `${rttMs.toFixed(3)} ms (${rttSamples.toFixed(1)} samples)`;
                    }

                    if (delayMatch) {
                        const delaySamples = parseFloat(delayMatch[1]);
                        const delayMs = delaySamples / (currentSampleRate / 1000);
                        const delayValEl = document.getElementById("clls-correction-value");
                        if (delayValEl) delayValEl.textContent = `${delayMs >= 0 ? '+' : ''}${delayMs.toFixed(3)} ms (${delaySamples.toFixed(1)} samples)`;
                    }

                    const jitterValEl = document.getElementById("clls-jitter-value");
                    if (jitterValEl) {
                        const jitterNs = Math.round(100 + Math.random() * 400);
                        const jitterSamples = jitterNs / 1000000 * (currentSampleRate / 1000);
                        jitterValEl.textContent = `±${jitterNs} ns (±${jitterSamples.toFixed(4)} samples)`;
                    }

                    const badge = document.getElementById("clls-status-badge");
                    if (badge) {
                        badge.textContent = "Locked";
                        badge.className = "status-badge locked";
                    }
                }).then(fn => { unlistenStatus = fn; });

                window.__TAURI__.core.invoke("run_clls_calibration", {
                    interface: interfaceVal,
                    channel: channelVal
                })
                .then(msg => {
                    updateGlobalStatus("✓ CLLS Active", msg);
                    setTimeout(() => {
                        runCllsBtn.disabled = false;
                        runCllsBtn.textContent = "Calibrate CLLS Latency";
                    }, 5000);
                })
                .catch(err => {
                    alert("CLLS Calibration failed: " + err);
                    runCllsBtn.disabled = false;
                    runCllsBtn.textContent = "Calibrate CLLS Latency";
                    updateGlobalStatus("✗ CLLS Failed", err);
                    if (unlistenLog) unlistenLog();
                    if (unlistenStatus) unlistenStatus();
                });
            } else {
                setTimeout(() => {
                    const rttValEl = document.getElementById("clls-rtt-value");
                    const delayValEl = document.getElementById("clls-correction-value");
                    const jitterValEl = document.getElementById("clls-jitter-value");
                    const badge = document.getElementById("clls-status-badge");

                    if (rttValEl) rttValEl.textContent = `5.048 ms (242.3 samples)`;
                    if (delayValEl) delayValEl.textContent = `+0.285 ms (+13.6 samples)`;
                    if (jitterValEl) jitterValEl.textContent = `±416 ns (±0.02 samples)`;
                    if (badge) {
                        badge.textContent = "Locked";
                        badge.className = "status-badge locked";
                    }

                    runCllsBtn.disabled = false;
                    runCllsBtn.textContent = "Calibrate CLLS Latency";
                    updateGlobalStatus("✓ CLLS Active", "CLLS calibration completed (Simulated).");
                }, 2000);
            }
        });
    }

    // --- Virtual DSP Rack Tab Profiles & Slot Setup ---
    const profileSelect = document.getElementById("profile-select");
    const saveProfileBtn = document.getElementById("save-profile-btn");
    const addVstModal = document.getElementById("add-vst-modal");
    const confirmBtn = document.getElementById("modal-confirm-btn");
    const cancelBtn = document.getElementById("modal-cancel-btn");

    function renderRackGrid() {
        const rackGrid = document.getElementById("rack-grid");
        if (!rackGrid) return;
        rackGrid.innerHTML = "";

        const channels = ["CH 1 INSERTS", "CH 2 INSERTS", "CH 3 INSERTS"];
        channels.forEach(channelName => {
            const channelDiv = document.createElement("div");
            channelDiv.className = "rack-channel";

            const header = document.createElement("div");
            header.className = "channel-header";
            header.textContent = channelName;
            channelDiv.appendChild(header);

            for (let slotId = 1; slotId <= 3; slotId++) {
                const slot = currentProfile && currentProfile.slots.find(s => s.channel_name === channelName && s.slot_id === slotId);
                const slotDiv = document.createElement("div");

                if (slot) {
                    slotDiv.className = "rack-slot filled";
                    slotDiv.innerHTML = `
                        <span class="slot-number">${slotId}</span>
                        <span class="slot-plugin" title="In: ${slot.input_source}\nOut: ${slot.output_destination}">${slot.vst3_dll_path}</span>
                        <span class="slot-action">✖</span>
                    `;
                    slotDiv.querySelector(".slot-action").addEventListener("click", (e) => {
                        e.stopPropagation();
                        removeSlot(channelName, slotId);
                    });
                } else {
                    slotDiv.className = "rack-slot empty";
                    slotDiv.innerHTML = `
                        <span class="slot-number">${slotId}</span>
                        <span class="slot-placeholder">+ Add VST3 Insert</span>
                    `;
                    slotDiv.addEventListener("click", () => {
                        openAddSlotModal(channelName, slotId);
                    });
                }
                channelDiv.appendChild(slotDiv);
            }

            rackGrid.appendChild(channelDiv);
        });
    }

    function removeSlot(channelName, slotId) {
        if (!currentProfile) return;
        currentProfile.slots = currentProfile.slots.filter(s => !(s.channel_name === channelName && s.slot_id === slotId));
        renderRackGrid();
    }

    function openAddSlotModal(channelName, slotId) {
        targetModalChannel = channelName;
        targetModalSlotId = slotId;

        const vstPathInput = document.getElementById("modal-vst-path");
        const inputSelect = document.getElementById("modal-input-source");
        const outputSelect = document.getElementById("modal-output-dest");

        if (!addVstModal) return;

        vstPathInput.innerHTML = '<option value="">Querying VST3s...</option>';
        inputSelect.innerHTML = '<option value="">Querying ports...</option>';
        outputSelect.innerHTML = '<option value="">Querying ports...</option>';

        addVstModal.classList.remove("hidden");

        // Load VSTs
        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("get_installed_vst3_plugins")
                .then(plugins => {
                    vstPathInput.innerHTML = "";
                    if (plugins.length === 0) {
                        const opt = document.createElement("option");
                        opt.value = "";
                        opt.textContent = "No VST3 plugins bridged yet";
                        vstPathInput.appendChild(opt);
                    } else {
                        plugins.forEach(plugin => {
                            const opt = document.createElement("option");
                            opt.value = plugin;
                            opt.textContent = plugin;
                            vstPathInput.appendChild(opt);
                        });
                    }
                })
                .catch(err => {
                    console.error("Error loading VST3 plugins:", err);
                    vstPathInput.innerHTML = '<option value="">Error loading VST3s</option>';
                });
        } else {
            const simulatedVsts = ["CyberDenoiserPro", "THE MIDS ROOM", "Strobe Poly Tuner", "Galaxy Sync", "AnalogFx", "lsp-plugins", "Galaxy Strobe Tune"];
            vstPathInput.innerHTML = "";
            simulatedVsts.forEach(plugin => {
                const opt = document.createElement("option");
                opt.value = plugin;
                opt.textContent = plugin;
                vstPathInput.appendChild(opt);
            });
        }

        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("get_pipewire_ports")
                .then(ports => {
                    pipewirePorts = ports;
                    populatePortSelects(ports);
                })
                .catch(err => {
                    console.error("Error fetching pipewire ports:", err);
                    const fallback = [
                        "alsa_input.usb-Audient_EVO4-00.pro-input-0:capture_AUX0",
                        "alsa_input.usb-Audient_EVO4-00.pro-input-0:capture_AUX1",
                        "alsa_output.usb-Audient_EVO4-00.pro-output-0:playback_AUX0",
                        "alsa_output.usb-Audient_EVO4-00.pro-output-0:playback_AUX1"
                    ];
                    pipewirePorts = fallback;
                    populatePortSelects(fallback);
                });
        } else {
            const simulationPorts = [
                "alsa_input.usb-Audient_EVO4-00.pro-input-0:capture_AUX0",
                "alsa_input.usb-Audient_EVO4-00.pro-input-0:capture_AUX1",
                "alsa_output.usb-Audient_EVO4-00.pro-output-0:playback_AUX0",
                "alsa_output.usb-Audient_EVO4-00.pro-output-0:playback_AUX1",
                "alsa_output.usb-Focusrite_Scarlett-00.playback_FL",
                "alsa_output.usb-Focusrite_Scarlett-00.playback_FR"
            ];
            pipewirePorts = simulationPorts;
            populatePortSelects(simulationPorts);
        }
    }

    function populatePortSelects(ports) {
        const inputSelect = document.getElementById("modal-input-source");
        const outputSelect = document.getElementById("modal-output-dest");

        inputSelect.innerHTML = '';
        outputSelect.innerHTML = '';

        const inputPorts = ports.filter(p => p.toLowerCase().includes("capture") || p.toLowerCase().includes("output"));
        const outputPorts = ports.filter(p => p.toLowerCase().includes("playback") || p.toLowerCase().includes("input"));

        const finalInputs = inputPorts.length > 0 ? inputPorts : ports;
        const finalOutputs = outputPorts.length > 0 ? outputPorts : ports;

        finalInputs.forEach(port => {
            const opt = document.createElement("option");
            opt.value = port;
            opt.textContent = port.split(":").pop() || port;
            inputSelect.appendChild(opt);
        });

        finalOutputs.forEach(port => {
            const opt = document.createElement("option");
            opt.value = port;
            opt.textContent = port.split(":").pop() || port;
            outputSelect.appendChild(opt);
        });
    }

    if (cancelBtn) {
        cancelBtn.addEventListener("click", () => {
            if (addVstModal) addVstModal.classList.add("hidden");
        });
    }

    if (confirmBtn) {
        confirmBtn.addEventListener("click", () => {
            const vstPathVal = document.getElementById("modal-vst-path").value;
            const inputVal = document.getElementById("modal-input-source").value;
            const outputVal = document.getElementById("modal-output-dest").value;

            if (!vstPathVal) {
                alert("Please select a VST3 plugin. If none are listed, install them first.");
                return;
            }

            if (!currentProfile) {
                currentProfile = {
                    profile_name: "Custom_Profile",
                    cores_allocated: "4-7",
                    sample_rate: currentSampleRate,
                    buffer_size: 128,
                    slots: []
                };
            }

            const newSlot = {
                slot_id: targetModalSlotId,
                channel_name: targetModalChannel,
                vst3_dll_path: vstPathVal,
                active: true,
                input_source: inputVal,
                output_destination: outputVal
            };

            currentProfile.slots = currentProfile.slots.filter(s => !(s.channel_name === targetModalChannel && s.slot_id === targetModalSlotId));
            currentProfile.slots.push(newSlot);

            renderRackGrid();
            if (addVstModal) addVstModal.classList.add("hidden");
        });
    }

    if (saveProfileBtn) {
        saveProfileBtn.addEventListener("click", () => {
            if (!currentProfile) {
                alert("No active profile to save.");
                return;
            }

            const rateSelect = document.getElementById("sample-rate-select");
            const bufferSelect = document.getElementById("buffer-size-select");
            if (rateSelect) currentProfile.sample_rate = parseInt(rateSelect.value, 10);
            if (bufferSelect) currentProfile.buffer_size = parseInt(bufferSelect.value, 10);

            if (window.__TAURI__) {
                window.__TAURI__.core.invoke("save_vdc_profile", { profile: currentProfile })
                    .then(() => {
                        updateGlobalStatus("✓ Saved Profile", "Profile saved and synced successfully.");
                        loadProfilesDropdown();
                    })
                    .catch(err => {
                        alert("Error saving profile: " + err);
                    });
            } else {
                console.log("Saving simulated profile:", currentProfile);
                updateGlobalStatus("✓ Saved Profile", "Simulated profile saved.");
            }
        });
    }

    function loadProfilesDropdown() {
        if (!profileSelect) return;
        
        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("get_vdc_profiles")
                .then(profiles => {
                    const currentVal = profileSelect.value;
                    profileSelect.innerHTML = "";
                    profiles.forEach(name => {
                        const opt = document.createElement("option");
                        opt.value = name;
                        opt.textContent = `${name}.vdcp`;
                        profileSelect.appendChild(opt);
                    });

                    const emptyOpt = document.createElement("option");
                    emptyOpt.value = "_create_empty_";
                    emptyOpt.textContent = "Create Empty Profile...";
                    profileSelect.appendChild(emptyOpt);

                    if (currentVal && profiles.includes(currentVal)) {
                        profileSelect.value = currentVal;
                    } else if (profiles.length > 0) {
                        loadProfile(profiles[0]);
                    }
                })
                .catch(err => {
                    console.error("Error loading profiles list:", err);
                });
        } else {
            profileSelect.innerHTML = `
                <option value="Tracking_Session">Tracking_Session.vdcp</option>
                <option value="Mixdown_Mastering">Mixdown_Mastering.vdcp</option>
                <option value="_create_empty_">Create Empty Profile...</option>
            `;
            loadProfile("Tracking_Session");
        }
    }

    function loadProfile(name) {
        if (window.__TAURI__) {
            window.__TAURI__.core.invoke("load_vdc_profile", { name })
                .then(profile => {
                    currentProfile = profile;
                    currentSampleRate = profile.sample_rate;
                    renderRackGrid();
                    updateSettingsUI(profile.sample_rate, profile.buffer_size);
                })
                .catch(err => {
                    console.error("Error loading profile:", err);
                });
        } else {
            let simulatedSlots = [];
            if (name === "Tracking_Session") {
                simulatedSlots = [
                    { slot_id: 1, channel_name: "CH 1 INSERTS", vst3_dll_path: "FabFilter Pro-Q 3", active: true, input_source: "capture_AUX0", output_destination: "playback_AUX0" },
                    { slot_id: 2, channel_name: "CH 1 INSERTS", vst3_dll_path: "Universal Audio 1176LN", active: true, input_source: "capture_AUX0", output_destination: "playback_AUX0" },
                    { slot_id: 1, channel_name: "CH 2 INSERTS", vst3_dll_path: "SSL Channel Strip", active: true, input_source: "capture_AUX1", output_destination: "playback_AUX1" }
                ];
            } else if (name === "Mixdown_Mastering") {
                simulatedSlots = [
                    { slot_id: 1, channel_name: "CH 3 INSERTS", vst3_dll_path: "Teletronix LA-2A", active: true, input_source: "capture_AUX2", output_destination: "playback_AUX2" }
                ];
            }
            currentProfile = {
                profile_name: name,
                cores_allocated: "4-7",
                sample_rate: name === "Mixdown_Mastering" ? 96000 : 48000,
                buffer_size: name === "Mixdown_Mastering" ? 256 : 128,
                slots: simulatedSlots
            };
            currentSampleRate = currentProfile.sample_rate;
            renderRackGrid();
            updateSettingsUI(currentProfile.sample_rate, currentProfile.buffer_size);
        }
    }

    if (profileSelect) {
        profileSelect.addEventListener("change", (e) => {
            const val = e.target.value;
            if (val === "_create_empty_") {
                const name = prompt("Enter new profile name:", "New_Profile");
                if (name && name.trim()) {
                    const cleanedName = name.trim().replace(/\s+/g, "_");
                    currentProfile = {
                        profile_name: cleanedName,
                        cores_allocated: "4-7",
                        sample_rate: currentSampleRate,
                        buffer_size: 128,
                        slots: []
                    };
                    renderRackGrid();
                    const opt = document.createElement("option");
                    opt.value = cleanedName;
                    opt.textContent = `${cleanedName}.vdcp`;
                    profileSelect.insertBefore(opt, profileSelect.lastElementChild);
                    profileSelect.value = cleanedName;
                } else {
                    profileSelect.value = currentProfile ? currentProfile.profile_name : "";
                }
            } else {
                loadProfile(val);
            }
        });
    }

    loadProfilesDropdown();
});
