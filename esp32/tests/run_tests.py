#!/usr/bin/env python3
"""
ESP32 Jim Tcl Test Runner

Sends test scripts to an ESP32 over serial and collects PASS/FAIL/SKIP results.

Usage:
    python run_tests.py /dev/cu.usbserial-2110
    python run_tests.py /dev/cu.usbserial-2110 json.test core.test
    python run_tests.py /dev/cu.usbserial-2110 --all
    python run_tests.py /dev/cu.usbserial-2110 --constraint wifi_available=1
    python run_tests.py /dev/cu.usbserial-2110 --baud 115200 --timeout 30

Exit code: 0 if all tests pass, 1 if any fail.
"""

import argparse
import glob
import os
import re
import serial
import sys
import time


class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    CYAN = '\033[96m'
    BOLD = '\033[1m'
    RESET = '\033[0m'


def colorize(text, color):
    if sys.stdout.isatty():
        return f"{color}{text}{Colors.RESET}"
    return text


class ESP32TestRunner:
    def __init__(self, port, baud=115200, timeout=30, verbose=False):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.verbose = verbose
        self.ser = None

        # Aggregate results
        self.total_pass = 0
        self.total_fail = 0
        self.total_skip = 0
        self.failed_tests = []
        self.suite_results = []

    def connect(self):
        """Open serial connection and wait for the jim> prompt."""
        self.ser = serial.Serial(self.port, self.baud, timeout=1)
        time.sleep(0.5)
        # Drain any boot messages
        self.ser.reset_input_buffer()
        # Send an empty line to get a prompt
        self._send_line("")
        self._wait_for_prompt(timeout=5)
        print(colorize(f"Connected to {self.port} at {self.baud} baud", Colors.CYAN))

    def disconnect(self):
        if self.ser:
            self.ser.close()

    def _send_line(self, line):
        """Send a line to the ESP32 REPL."""
        self.ser.write((line + "\r\n").encode('utf-8'))
        self.ser.flush()
        time.sleep(0.02)  # Small delay between lines

    def _read_until_prompt(self, timeout=None):
        """Read serial output until we see 'jim> ' or timeout."""
        if timeout is None:
            timeout = self.timeout
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                buf += chunk
                if self.verbose:
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
                # Check for prompt at end of output
                if re.search(r'jim> \s*$', buf):
                    return buf
            else:
                time.sleep(0.05)
        return buf

    def _wait_for_prompt(self, timeout=5):
        """Wait for the jim> prompt to appear."""
        self._read_until_prompt(timeout=timeout)

    def _send_script(self, script):
        """Send a multi-line script and collect all output."""
        lines = script.strip().split('\n')
        all_output = ""

        for line in lines:
            line = line.strip()
            if not line:
                continue
            self._send_line(line)
            # Wait a bit longer for lines that might take time
            if any(kw in line for kw in ['http ', 'mqtt ', 'wifi ', 'esp32 sleep',
                                          'task create', 'task eval']):
                output = self._read_until_prompt(timeout=self.timeout)
            else:
                output = self._read_until_prompt(timeout=5)
            all_output += output

        return all_output

    def set_constraints(self, constraints):
        """Set test constraints on the device."""
        for name, value in constraints.items():
            self._send_line(f"test constraint {name} {value}")
            self._read_until_prompt(timeout=2)

    def run_test_file(self, filepath):
        """Send a test file to the ESP32 and parse results."""
        filename = os.path.basename(filepath)
        suite_name = os.path.splitext(filename)[0]

        print(colorize(f"\n{'='*60}", Colors.BOLD))
        print(colorize(f"  Running: {filename}", Colors.BOLD))
        print(colorize(f"{'='*60}", Colors.BOLD))

        with open(filepath, 'r') as f:
            script = f.read()

        output = self._send_script(script)

        # Parse results from output
        passes = re.findall(r'PASS:\s+(\S+)', output)
        fails = re.findall(r'FAIL:\s+(\S+)(?:\s+\(.*?\))?\s*-\s*(.*)', output)
        skips = re.findall(r'SKIP:\s+(\S+)', output)

        # Parse the test report line if present
        report_match = re.search(
            r'Total:\s*(\d+)\s+Pass:\s*(\d+)\s+Fail:\s*(\d+)\s+Skip:\s*(\d+)',
            output
        )

        suite_pass = len(passes)
        suite_fail = len(fails)
        suite_skip = len(skips)

        if report_match:
            # Prefer the device's own count
            suite_total = int(report_match.group(1))
            suite_pass = int(report_match.group(2))
            suite_fail = int(report_match.group(3))
            suite_skip = int(report_match.group(4))
        else:
            suite_total = suite_pass + suite_fail + suite_skip

        # Print per-test results if not verbose (verbose already printed them)
        if not self.verbose:
            for test_id in passes:
                print(f"  {colorize('PASS', Colors.GREEN)}: {test_id}")
            for test_id, reason in fails:
                print(f"  {colorize('FAIL', Colors.RED)}: {test_id} - {reason}")
            for test_id in skips:
                print(f"  {colorize('SKIP', Colors.YELLOW)}: {test_id}")

        # Suite summary
        status = colorize("PASS", Colors.GREEN) if suite_fail == 0 else colorize("FAIL", Colors.RED)
        print(f"\n  {suite_name}: {status}  "
              f"(Total: {suite_total}  "
              f"Pass: {colorize(str(suite_pass), Colors.GREEN)}  "
              f"Fail: {colorize(str(suite_fail), Colors.RED) if suite_fail else '0'}  "
              f"Skip: {suite_skip})")

        # Accumulate
        self.total_pass += suite_pass
        self.total_fail += suite_fail
        self.total_skip += suite_skip
        self.failed_tests.extend([(suite_name, tid, reason) for tid, reason in fails])
        self.suite_results.append({
            'name': suite_name,
            'total': suite_total,
            'pass': suite_pass,
            'fail': suite_fail,
            'skip': suite_skip,
        })

    def print_summary(self):
        """Print final summary across all suites."""
        total = self.total_pass + self.total_fail + self.total_skip

        print(colorize(f"\n{'='*60}", Colors.BOLD))
        print(colorize(f"  FINAL RESULTS", Colors.BOLD))
        print(colorize(f"{'='*60}", Colors.BOLD))

        # Per-suite table
        max_name = max(len(s['name']) for s in self.suite_results) if self.suite_results else 10
        print(f"\n  {'Suite':<{max_name}}  Total  Pass  Fail  Skip")
        print(f"  {'-'*max_name}  -----  ----  ----  ----")
        for s in self.suite_results:
            fail_str = colorize(str(s['fail']), Colors.RED) if s['fail'] else '0'
            print(f"  {s['name']:<{max_name}}  {s['total']:>5}  {s['pass']:>4}  {fail_str:>4}  {s['skip']:>4}")

        print(f"\n  {colorize('Total', Colors.BOLD)}: {total}  "
              f"{colorize('Pass', Colors.GREEN)}: {self.total_pass}  "
              f"{colorize('Fail', Colors.RED)}: {self.total_fail}  "
              f"Skip: {self.total_skip}")

        if self.failed_tests:
            print(colorize(f"\n  FAILED TESTS:", Colors.RED))
            for suite, tid, reason in self.failed_tests:
                print(f"    {suite}: {tid} - {reason}")

        if self.total_fail == 0:
            print(colorize(f"\n  ALL TESTS PASSED", Colors.GREEN))
        else:
            print(colorize(f"\n  {self.total_fail} TEST(S) FAILED", Colors.RED))

        print()


def find_test_files(test_dir):
    """Find all .test files in the test directory."""
    pattern = os.path.join(test_dir, "*.test")
    files = sorted(glob.glob(pattern))
    return files


def main():
    parser = argparse.ArgumentParser(description="ESP32 Jim Tcl Test Runner")
    parser.add_argument("port", help="Serial port (e.g., /dev/cu.usbserial-2110)")
    parser.add_argument("tests", nargs="*", help="Specific test files to run (default: all)")
    parser.add_argument("--all", action="store_true", help="Run all test files")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--timeout", type=int, default=30, help="Per-command timeout in seconds")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show all serial output")
    parser.add_argument("--constraint", "-c", action="append", default=[],
                        help="Set constraint: name=value (can repeat)")
    parser.add_argument("--test-dir", default=None,
                        help="Directory containing .test files")

    args = parser.parse_args()

    # Determine test directory
    test_dir = args.test_dir or os.path.dirname(os.path.abspath(__file__))

    # Determine which test files to run
    if args.tests:
        test_files = []
        for t in args.tests:
            if os.path.isfile(t):
                test_files.append(t)
            else:
                # Try relative to test_dir
                path = os.path.join(test_dir, t)
                if os.path.isfile(path):
                    test_files.append(path)
                else:
                    print(f"Warning: test file not found: {t}")
    elif args.all:
        test_files = find_test_files(test_dir)
    else:
        # Default: run hardware-independent tests only
        safe_tests = [
            "core.test", "json.test", "esp32.test", "task.test",
            "cron.test", "timer.test", "fs.test", "nvs.test",
            "adc.test", "pwm.test",
        ]
        test_files = [os.path.join(test_dir, t) for t in safe_tests
                      if os.path.isfile(os.path.join(test_dir, t))]

    if not test_files:
        print("No test files found.")
        sys.exit(1)

    # Parse constraints
    constraints = {}
    for c in args.constraint:
        if '=' in c:
            name, value = c.split('=', 1)
            constraints[name] = value
        else:
            constraints[c] = "1"

    # Run
    runner = ESP32TestRunner(
        port=args.port,
        baud=args.baud,
        timeout=args.timeout,
        verbose=args.verbose,
    )

    try:
        runner.connect()

        if constraints:
            print(f"Setting constraints: {constraints}")
            runner.set_constraints(constraints)

        for filepath in test_files:
            runner.run_test_file(filepath)

        runner.print_summary()

    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        runner.disconnect()

    sys.exit(0 if runner.total_fail == 0 else 1)


if __name__ == "__main__":
    main()
