document.addEventListener("DOMContentLoaded", () => {
    const navButtons = document.querySelectorAll(".nav-btn");
    console.log("Arthur UI Initialized.");

    // Simple tab switching logic for demonstration
    navButtons.forEach(btn => {
        btn.addEventListener("click", () => {
            navButtons.forEach(b => b.classList.remove("active"));
            btn.classList.add("active");
            const tabName = btn.getAttribute("data-tab");
            console.log(`Switched to tab: ${tabName}`);
            // In a fully-implemented Tauri app, this swaps DOM elements or triggers navigation
        });
    });

    // Simulate real-time jitter graph movement
    const bars = document.querySelectorAll(".latency-graph .bar");
    setInterval(() => {
        bars.forEach(bar => {
            const currentHeight = parseFloat(bar.style.height);
            const fluctuation = (Math.random() - 0.5) * 15;
            let newHeight = currentHeight + fluctuation;
            newHeight = Math.max(10, Math.min(newHeight, 90));
            bar.style.height = `${newHeight}%`;
        });
    }, 500);
});
