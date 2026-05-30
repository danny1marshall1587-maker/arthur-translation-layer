#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <array>

// Helper to run command and get output
std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::vector<std::string> queryPipeWirePorts() {
    std::vector<std::string> ports;
    try {
        std::string stdoutStr = exec("pw-link -io");
        std::stringstream ss(stdoutStr);
        std::string line;
        while (std::getline(ss, line)) {
            // trim
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (!line.empty()) {
                ports.push_back(line);
            }
        }
    } catch (...) {}
    return ports;
}

// Split helper
std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

std::string getMatchingInputInterface(const std::string &activeInterface, const std::vector<std::string> &allPorts) {
    if (activeInterface.rfind("alsa_input", 0) == 0) return activeInterface;
    
    std::vector<std::string> parts = split(activeInterface, '.');
    if (parts.size() >= 2) {
        std::string cardId = parts[1];
        std::string targetPrefix = "alsa_input." + cardId;
        for (const auto &port : allPorts) {
            if (port.rfind(targetPrefix, 0) == 0) {
                size_t colonIdx = port.find(':');
                if (colonIdx != std::string::npos) {
                    return port.substr(0, colonIdx);
                } else {
                    return port;
                }
            }
        }
    }
    
    // Fallback
    std::string inputInterface = activeInterface;
    size_t pos = 0;
    while ((pos = inputInterface.find("alsa_output", pos)) != std::string::npos) {
        inputInterface.replace(pos, 11, "alsa_input");
        pos += 10;
    }
    pos = 0;
    while ((pos = inputInterface.find("output", pos)) != std::string::npos) {
        inputInterface.replace(pos, 6, "input");
        pos += 5;
    }
    return inputInterface;
}

int main() {
    std::vector<std::string> allPorts = queryPipeWirePorts();
    std::cout << "All ports count: " << allPorts.size() << std::endl;
    
    std::string activeInterface = "alsa_output.usb-Audient_EVO4-00.pro-output-0";
    std::cout << "Active Playback Interface: " << activeInterface << std::endl;
    
    std::string inputInterface = getMatchingInputInterface(activeInterface, allPorts);
    std::cout << "Resolved Input Interface: " << inputInterface << std::endl;
    
    std::cout << "\n--- Matching Playback Ports ---" << std::endl;
    for (const auto &port : allPorts) {
        if (port.rfind(activeInterface, 0) == 0 && port.find("playback") != std::string::npos) {
            std::cout << "  " << port << std::endl;
        }
    }
    
    std::cout << "\n--- Matching Capture Ports ---" << std::endl;
    for (const auto &port : allPorts) {
        if (port.rfind(inputInterface, 0) == 0 && port.find("capture") != std::string::npos) {
            std::cout << "  " << port << std::endl;
        }
    }
    
    std::cout << "\n--- All Ports matching inputInterface prefix ---" << std::endl;
    for (const auto &port : allPorts) {
        if (port.rfind(inputInterface, 0) == 0) {
            std::cout << "  " << port << std::endl;
        }
    }

    return 0;
}
