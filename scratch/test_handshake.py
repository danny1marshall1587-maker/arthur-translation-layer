import socket
import subprocess
import time
import sys
import os

SOCKET_PATH = "/tmp/arthur.sock"

def run_test(scenario_name, expect_timeout=False):
    print(f"\n--- Running Scenario: {scenario_name} ---")
    
    # 1. Ensure no existing socket or daemon is running
    if os.path.exists(SOCKET_PATH):
        try:
            os.unlink(SOCKET_PATH)
        except Exception as e:
            print(f"Error unlinking socket: {e}")

    # 2. Start the arthur-daemon process
    # We run it from the root workspace directory
    daemon = subprocess.Popen(["./build/arthur-daemon"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    
    # Wait for socket to appear
    for _ in range(20):
        if os.path.exists(SOCKET_PATH):
            break
        time.sleep(0.1)
    else:
        print("ERROR: Daemon socket not created in time.")
        daemon.terminate()
        return False

    # 3. Connect to the socket and send the command
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(SOCKET_PATH)
        cmd = "GET_SHM dummy_plugin.dll"
        print(f"Sending to daemon: '{cmd}'")
        start_time = time.time()
        s.sendall(cmd.encode())
        
        # 4. Wait for response
        resp = s.recv(1024).decode()
        end_time = time.time()
        elapsed = end_time - start_time
        
        print(f"Received response: '{resp}'")
        print(f"Time elapsed: {elapsed:.3f} seconds")
        
        # 5. Check correctness
        if expect_timeout:
            if "Timeout" in resp and 4.9 <= elapsed <= 5.5:
                print("SUCCESS: Timeout occurred as expected in ~5 seconds.")
                success = True
            else:
                print(f"FAILURE: Expected ~5s timeout, got {elapsed:.3f}s with response: '{resp}'")
                success = False
        else:
            if "SHM" in resp and elapsed < 5.0:
                print("SUCCESS: Handshake completed successfully.")
                success = True
            else:
                print(f"FAILURE: Expected successful connection within 5s, got {elapsed:.3f}s with response: '{resp}'")
                success = False
    except Exception as e:
        print(f"Socket connection error: {e}")
        success = False
    finally:
        s.close()
        daemon.terminate()
        try:
            daemon.wait(timeout=2)
        except subprocess.TimeoutExpired:
            daemon.kill()
        
        # Dump daemon logs
        stdout, stderr = daemon.communicate()
        if stdout:
            print("[Daemon stdout]:")
            print(stdout)
        if stderr:
            print("[Daemon stderr]:")
            print(stderr)
            
    return success

if __name__ == "__main__":
    # Test Scenario 1: Timeout (Guest binaries renamed/disabled)
    print("Pre-requisite: Renaming guest binaries in build/ to simulate missing/delayed guest...")
    has_agent = os.path.exists("build/win_guest_agent.exe")
    has_guest = os.path.exists("build/arthur-guest.exe")
    
    if has_agent:
        os.rename("build/win_guest_agent.exe", "build/win_guest_agent.exe.bak")
    if has_guest:
        os.rename("build/arthur-guest.exe", "build/arthur-guest.exe.bak")
        
    s1_ok = run_test("Watchdog Timeout Test (No Guest running)", expect_timeout=True)
    
    # Restore binaries
    if has_agent:
        os.rename("build/win_guest_agent.exe.bak", "build/win_guest_agent.exe")
    if has_guest:
        os.rename("build/arthur-guest.exe.bak", "build/arthur-guest.exe")
        
    # Test Scenario 2: Successful Handshake (Guest binaries present)
    s2_ok = run_test("Normal Successful Handshake Test (Guest runs normally)", expect_timeout=False)
    
    if s1_ok and s2_ok:
        print("\nALL HANDSHAKE TESTS PASSED!")
        sys.exit(0)
    else:
        print("\nSOME TESTS FAILED.")
        sys.exit(1)
