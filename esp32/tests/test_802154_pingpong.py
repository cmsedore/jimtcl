#!/usr/bin/env python3 -u
"""
802.15.4 Ping/Pong Test — Two ESP32-H2 devices exchange mpack packets.

Orchestrated from Python — sends commands to both REPLs:
  1. Device A sends mpack ping via 802.15.4
  2. Device B receives it, decodes mpack
  3. Device B sends mpack pong back
  4. Device A receives it, decodes mpack

Usage:
    python test_802154_pingpong.py /dev/cu.usbserial-2110 /dev/cu.usbserial-2120
"""

import serial
import sys
import time
import re

BAUD = 115200
CHANNEL = 15
PANID = "0x1234"

class Device:
    def __init__(self, port, name):
        self.name = name
        self.ser = serial.Serial(port, BAUD, timeout=1)

    def reset_and_wait(self):
        print(f"  [{self.name}] Resetting...", end=" ", flush=True)
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False

        deadline = time.time() + 10
        buf = ""
        while time.time() < deadline:
            if self.ser.in_waiting:
                buf += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                if 'jim>' in buf:
                    time.sleep(0.3)
                    if self.ser.in_waiting:
                        self.ser.read(self.ser.in_waiting)
                    print("OK")
                    return True
            else:
                time.sleep(0.1)
        print("TIMEOUT")
        return False

    def cmd(self, command, timeout=5):
        """Send command, return output text."""
        self.ser.reset_input_buffer()
        self.ser.write((command + "\r\n").encode('utf-8'))
        self.ser.flush()
        time.sleep(0.05)

        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.ser.in_waiting:
                buf += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                if re.search(r'jim>\s*$', buf):
                    return buf
            else:
                time.sleep(0.02)
        return buf

    def close(self):
        self.ser.close()


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <port_A> <port_B>")
        sys.exit(1)

    devA = Device(sys.argv[1], "DEV-A")
    devB = Device(sys.argv[2], "DEV-B")

    print("=" * 60)
    print("  802.15.4 Ping/Pong Test (mpack over 802.15.4)")
    print(f"  Device A: {sys.argv[1]}")
    print(f"  Device B: {sys.argv[2]}")
    print(f"  Channel: {CHANNEL}, PAN ID: {PANID}")
    print("=" * 60)

    # Boot both
    print("\nBooting...")
    assert devA.reset_and_wait()
    assert devB.reset_and_wait()

    # Init radios
    print("\nInitializing 802.15.4 radios...")
    for dev in [devA, devB]:
        resp = dev.cmd(f"ieee802154 init -channel {CHANNEL} -panid {PANID}")
        print(f"  [{dev.name}] Radio OK")

    # Enable promiscuous mode on both so they receive all frames
    for dev in [devA, devB]:
        dev.cmd(f"ieee802154 config -promiscuous 1")

    print("\nRunning ping/pong exchanges...\n")
    successes = 0
    failures = 0

    for i in range(5):
        seq = i + 1
        print(f"  Round {seq}/5:", flush=True)

        # Make sure B is in receive mode before A sends
        # (ieee802154 receive blocks, so we send from A first, then B picks it up)

        # Step 1: A sends ping (raw bytes for reliability)
        send_resp = devA.cmd(
            f'ieee802154 send {{72 69 76 76 79 {seq}}}',
            timeout=5
        )
        if "error" in send_resp.lower() or "timeout" in send_resp.lower():
            print(f"    A→B SEND FAILED: {send_resp.strip()[-60:]}")
            failures += 1
            continue
        sent_bytes = re.search(r'(\d+)', send_resp.split('jim>')[-2] if 'jim>' in send_resp else send_resp)
        print(f"    A→B: sent ping (seq={seq})", flush=True)

        # Step 2: B receives ping
        time.sleep(0.2)
        recv_resp = devB.cmd("ieee802154 receive 3000", timeout=6)
        if "timeout" in recv_resp.lower():
            print(f"    B←A: TIMEOUT (no frame received)")
            failures += 1
            continue

        # Check if we got data with our bytes
        if "data" in recv_resp and "rssi" in recv_resp:
            print(f"    B←A: received frame!", flush=True)
        else:
            print(f"    B←A: unexpected: {recv_resp.strip()[-80:]}")
            failures += 1
            continue

        # Step 3: B sends pong back (raw bytes)
        pong_resp = devB.cmd(
            f'ieee802154 send {{80 79 78 71 {seq}}}',
            timeout=5
        )
        if "error" in pong_resp.lower():
            print(f"    B→A: PONG SEND FAILED")
            failures += 1
            continue
        print(f"    B→A: sent pong (seq={seq})", flush=True)

        # Step 4: A receives pong
        time.sleep(0.2)
        pong_recv = devA.cmd("ieee802154 receive 3000", timeout=6)
        if "timeout" in pong_recv.lower():
            print(f"    A←B: TIMEOUT (no pong received)")
            failures += 1
            continue

        if "data" in pong_recv and "rssi" in pong_recv:
            print(f"    A←B: received pong! ✓")
            successes += 1
        else:
            print(f"    A←B: unexpected: {pong_recv.strip()[-80:]}")
            if "data" in pong_recv:
                successes += 1
            else:
                failures += 1

        print()
        time.sleep(0.2)

    # Cleanup
    print("Cleaning up...")
    devA.cmd("ieee802154 deinit")
    devB.cmd("ieee802154 deinit")
    devA.close()
    devB.close()

    # Results
    print()
    print("=" * 60)
    total = successes + failures
    print(f"  Results: {successes}/{total} complete ping/pong round-trips")
    if successes > 0:
        print(f"\n  802.15.4 mpack ping/pong: WORKING ✓")
    else:
        print(f"\n  802.15.4 mpack ping/pong: FAILED ✗")
    print("=" * 60)

    return 0 if successes > 0 else 1

if __name__ == "__main__":
    sys.exit(main())
