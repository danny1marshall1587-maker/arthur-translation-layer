document.addEventListener("DOMContentLoaded", () => {
    console.log("Arthur UI Initialized.");

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
            bars.forEach(bar => {
                const currentHeight = parseFloat(bar.style.height) || 40;
                const fluctuation = (Math.random() - 0.5) * 15;
                let newHeight = currentHeight + fluctuation;
                newHeight = Math.max(10, Math.min(newHeight, 90));
                bar.style.height = `${newHeight}%`;
            });
        }, 500);
    }

    // --- Core Isolation Slider ---
    const coreSlider = document.getElementById("core-slider");
    const sliderStatus = document.getElementById("slider-status");
    if (coreSlider && sliderStatus) {
        coreSlider.addEventListener("input", (e) => {
            const cores = e.target.value;
            const isolatedCores = `4-${cores}`;
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

    // Prevent default drag behaviors
    ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
        window.addEventListener(eventName, preventDefaults, false);
    });

    function preventDefaults(e) {
        e.preventDefault();
        e.stopPropagation();
    }

    // Highlight drop zone when item is dragged over it
    if (dropZone) {
        ['dragenter', 'dragover'].forEach(eventName => {
            dropZone.addEventListener(eventName, () => dropZone.classList.add('dragover'), false);
        });

        ['dragleave', 'drop'].forEach(eventName => {
            dropZone.addEventListener(eventName, () => dropZone.classList.remove('dragover'), false);
        });

        // Handle dropped files
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
            // Trigger file dialog
            // In Tauri, we can open file dialog. In standard browser, we can simulate.
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
                // Browser simulation: Prompt for file name
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
        // Toggle view states
        if (dropZone) dropZone.classList.add("hidden");
        if (statusCard) statusCard.classList.add("hidden");
        if (progressCard) progressCard.classList.remove("hidden");

        installTitle.textContent = `Installing ${fileName}...`;
        consoleLog.textContent = `Initializing installation pipeline for: ${filePath}\n`;

        if (window.__TAURI__) {
            // Run through Tauri backend IPC
            runTauriInstallation(fileName, filePath);
        } else {
            // Simulated install bar for debugging/standalone UI
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

            window.__TAURI__.invoke('install_vst_plugin', { installerPath: filePath })
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
});
