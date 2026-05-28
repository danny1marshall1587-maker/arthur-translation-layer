#!/usr/bin/env python3
import math
import random

def generate_mls_10():
    # LFSR state (10 bits)
    state = 0x3FF
    seq = []
    # Order 10 period is 2^10 - 1 = 1023
    for _ in range(1023):
        # Taps for order 10: x^10 + x^3 + 1 (1-indexed: 10 and 3 -> 0-indexed: 9 and 2)
        b9 = (state >> 9) & 1
        b2 = (state >> 2) & 1
        feedback = b9 ^ b2
        state = ((state << 1) | feedback) & 0x3FF
        # Map 0 to -1.0, 1 to 1.0
        seq.append(1.0 if b9 else -1.0)
    return seq

def lagrange_interpolate(x, index):
    i = int(math.floor(index))
    f = index - i
    
    # 4-tap Lagrange interpolation
    y_m1 = x[(i - 1) % len(x)]
    y_0  = x[i % len(x)]
    y_p1 = x[(i + 1) % len(x)]
    y_p2 = x[(i + 2) % len(x)]
    
    c0 = -f * (f - 1) * (f - 2) / 6.0
    c1 = (f + 1) * (f - 1) * (f - 2) / 2.0
    c2 = -(f + 1) * f * (f - 2) / 2.0
    c3 = (f + 1) * f * (f - 1) / 6.0
    
    return c0 * y_m1 + c1 * y_0 + c2 * y_p1 + c3 * y_p2

def cross_correlate(x, y):
    N = len(x)
    r = [0.0] * N
    for lag in range(N):
        s = 0.0
        for i in range(N):
            s += x[i] * y[(i + lag) % N]
        r[lag] = s
    return r

def main():
    print("=== Closed-Loop Latency Sync (CLLS) Proof of Concept ===")
    
    # 1. Generate pilot MLS signal
    mls = generate_mls_10()
    N = len(mls)
    print(f"Generated MLS of length: {N}")
    
    # Check autocorrelation at zero lag vs other lags
    auto_corr = cross_correlate(mls, mls)
    print(f"Autocorrelation at lag 0: {auto_corr[0]:.2f}")
    print(f"Autocorrelation at lag 1: {auto_corr[1]:.2f} (expected -1.0)")
    
    # 2. Simulate hardware delay (e.g. 45.37 samples) and some white noise
    true_delay = 45.37
    noise_level = 0.05 # -26 dB signal-to-noise ratio
    
    received = []
    for idx in range(N):
        # Sample delayed by true_delay
        delayed_val = lagrange_interpolate(mls, idx - true_delay)
        # Add white noise
        noisy_val = delayed_val + random.gauss(0, noise_level)
        received.append(noisy_val)
        
    print(f"Simulating physical loopback path with delay of {true_delay} samples and +noise (std={noise_level})")
    
    # 3. Perform cross-correlation
    corr = cross_correlate(mls, received)
    
    # 4. Find peak
    peak_val = -1e9
    peak_idx = -1
    for i, val in enumerate(corr):
        if val > peak_val:
            peak_val = val
            peak_idx = i
            
    print(f"Cross-correlation integer peak found at lag: {peak_idx} (Value: {peak_val:.2f})")
    
    # 5. Parabolic interpolation for sub-sample accuracy
    y_m1 = corr[(peak_idx - 1) % N]
    y_0  = corr[peak_idx]
    y_p1 = corr[(peak_idx + 1) % N]
    
    denom = 2.0 * (y_m1 - 2.0 * y_0 + y_p1)
    if abs(denom) > 1e-9:
        delta = (y_m1 - y_p1) / denom
    else:
        delta = 0.0
        
    measured_delay = peak_idx + delta
    if measured_delay > N / 2:
        measured_delay -= N # Handle negative lags if peak is near end of buffer
        
    error = measured_delay - true_delay
    print(f"Measured Delay: {measured_delay:.6f} samples")
    print(f"Measurement Error: {error:.6f} samples (approx {error * 100:.4f}% of a sample)")
    print(f"At 48kHz, this error is {error / 48.0 * 1000:.6f} microseconds (or {error / 48000.0 * 1e9:.2f} nanoseconds)!")

if __name__ == "__main__":
    main()
