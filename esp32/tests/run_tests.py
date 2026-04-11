#!/usr/bin/env -S python3 -u
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
import functools
import glob
import os
import re
import serial
import sys
import time

print = functools.partial(print, flush=True)

PROMPT_RE = re.compile(r'jim>\s*$')
PASS_RE = re.compile(r'^PASS:\s+(\S+)', re.MULTILINE)
FAIL_RE = re.compile(r'^FAIL:\s+(\S+)\s+\(([^)]*)\)\s*$|^FAIL:\s+(\S+)\s+\(([^)]*)\)\s*-\s*(.*)', re.MULTILINE)
SKIP_RE = re.compile(r'^SKIP:\s+(\S+)', re.MULTILINE)
# Match the dict result from "test report": total N pass N fail N skip N
REPORT_DICT_RE = re.compile(r'total\s+(\d+)\s+pass\s+(\d+)\s+fail\s+(\d+)\s+skip\s+(\d+)')


class Colors:
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    CYAN = '\033[96m'
    BOLD = '\033[1m'
    DIM = '\033[2m'
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
        self.total_pass = 0
        self.total_fail = 0
        self.total_skip = 0
        self.failed_tests = []
        self.suite_results = []

    def connect(self):
        """Open serial, reset device, wait for jim> prompt."""
        self.ser = serial.Serial(self.port, self.baud, timeout=1)
        print(colorize(f"Connecting to {self.port}...", Colors.CYAN))

        # Reset ESP32 via RTS
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False

        # Wait for jim> prompt (up to 15s)
        print("Waiting for boot...", end=" ")
        buf = self._read_until(r'jim>', timeout=15)
        if 'jim>' not in buf:
            print(colorize("TIMEOUT", Colors.RED))
            sys.exit(2)
        print(colorize("OK", Colors.GREEN))

        # Drain remaining boot output
        self._drain(0.5)
        print(colorize(f"Connected at {self.baud} baud\n", Colors.CYAN))

    def disconnect(self):
        if self.ser:
            self.ser.close()

    def _drain(self, wait=0.3):
        """Drain serial buffer."""
        time.sleep(wait)
        while self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
            time.sleep(0.05)

    def _read_until(self, pattern, timeout=10):
        """Read serial until pattern found or timeout."""
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                buf += chunk
                if self.verbose:
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
                if re.search(pattern, buf):
                    return buf
            else:
                time.sleep(0.02)
        return buf

    def _send_and_wait(self, cmd, timeout=10):
        """Send a command and wait for jim> prompt. Returns output."""
        self.ser.write((cmd + "\r\n").encode('utf-8'))
        self.ser.flush()
        time.sleep(0.03)

        buf = ""
        deadline = time.time() + timeout
        # Allow a brief settling period after sending
        while time.time() < deadline:
            if self.ser.in_waiting:
                chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                buf += chunk
                if self.verbose:
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
                if PROMPT_RE.search(buf):
                    # Wait a tiny bit more in case more data follows
                    time.sleep(0.05)
                    if self.ser.in_waiting:
                        extra = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='replace')
                        buf += extra
                        if self.verbose:
                            sys.stdout.write(extra)
                            sys.stdout.flush()
                    return buf
            else:
                time.sleep(0.02)
        return buf

    def _coalesce_commands(self, script):
        """Group script lines into complete Tcl commands by tracking brace depth."""
        commands = []
        current = ""
        depth = 0

        for line in script.strip().split('\n'):
            stripped = line.strip()

            if not stripped and depth == 0:
                continue
            if stripped.startswith('#') and depth == 0:
                commands.append(stripped)
                continue

            for ch in stripped:
                if ch == '{':
                    depth += 1
                elif ch == '}':
                    depth -= 1

            if current:
                current += "; " + stripped if depth > 0 else " " + stripped
            else:
                current = stripped

            if depth <= 0:
                depth = 0
                commands.append(current)
                current = ""

        if current:
            commands.append(current)

        return commands

    def set_constraints(self, constraints):
        """Set test constraints on the device."""
        for name, value in constraints.items():
            self._send_and_wait(f"test constraint {name} {value}", timeout=3)

    def run_test_file(self, filepath):
        """Send a test file to the ESP32 and parse results."""
        filename = os.path.basename(filepath)
        suite_name = os.path.splitext(filename)[0]

        print(colorize(f"\n{'='*60}", Colors.BOLD))
        print(colorize(f"  Running: {filename}", Colors.BOLD))
        print(colorize(f"{'='*60}", Colors.BOLD))

        with open(filepath, 'r') as f:
            script = f.read()

        commands = self._coalesce_commands(script)
        all_output = ""

        for cmd in commands:
            # Skip comments
            if cmd.startswith('#'):
                if self.verbose:
                    print(colorize(f"  # {cmd[2:]}", Colors.DIM))
                continue

            # Determine timeout
            if any(kw in cmd for kw in ['http ', 'mqtt ', 'wifi ', 'esp32 sleep',
                                         'task create', 'task eval', 'ota ',
                                         'cron add', 'timer create']):
                cmd_timeout = self.timeout
            else:
                cmd_timeout = 10

            output = self._send_and_wait(cmd, timeout=cmd_timeout)
            all_output += output

        # Parse results from accumulated output
        passes = PASS_RE.findall(all_output)
        skips = SKIP_RE.findall(all_output)

        # Parse FAIL lines — capture test ID and reason
        fail_details = []
        for m in re.finditer(r'FAIL:\s+(\S+)\s+\(([^)]*)\)\s*(?:-\s*(.*))?', all_output):
            tid = m.group(1)
            desc = m.group(2)
            reason = m.group(3) or desc
            fail_details.append((tid, reason.strip()))

        # Parse the device's report dict: "total N pass N fail N skip N"
        report_match = REPORT_DICT_RE.search(all_output)

        if report_match:
            suite_total = int(report_match.group(1))
            suite_pass = int(report_match.group(2))
            suite_fail = int(report_match.group(3))
            suite_skip = int(report_match.group(4))
        else:
            suite_pass = len(passes)
            suite_fail = len(fail_details)
            suite_skip = len(skips)
            suite_total = suite_pass + suite_fail + suite_skip

        # Print per-test results (non-verbose only)
        if not self.verbose:
            for test_id in passes:
                print(f"  {colorize('PASS', Colors.GREEN)}: {test_id}")
            for test_id, reason in fail_details:
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

        self.total_pass += suite_pass
        self.total_fail += suite_fail
        self.total_skip += suite_skip
        self.failed_tests.extend([(suite_name, tid, reason) for tid, reason in fail_details])
        self.suite_results.append({
            'name': suite_name,
            'total': suite_total,
            'pass': suite_pass,
            'fail': suite_fail,
            'skip': suite_skip,
        })

        # Drain between suites to prevent stale data
        self._drain(0.2)

    def print_summary(self):
        """Print final summary across all suites."""
        total = self.total_pass + self.total_fail + self.total_skip

        print(colorize(f"\n{'='*60}", Colors.BOLD))
        print(colorize(f"  FINAL RESULTS", Colors.BOLD))
        print(colorize(f"{'='*60}", Colors.BOLD))

        max_name = max(len(s['name']) for s in self.suite_results) if self.suite_results else 10
        print(f"\n  {'Suite':<{max_name}}  Total  Pass  Fail  Skip  Status")
        print(f"  {'-'*max_name}  -----  ----  ----  ----  ------")
        for s in self.suite_results:
            fail_str = colorize(str(s['fail']), Colors.RED) if s['fail'] else '   0'
            st = colorize("PASS", Colors.GREEN) if s['fail'] == 0 else colorize("FAIL", Colors.RED)
            print(f"  {s['name']:<{max_name}}  {s['total']:>5}  {s['pass']:>4}  {fail_str}  {s['skip']:>4}  {st}")

        print(f"\n  {colorize('Total', Colors.BOLD)}: {total}  "
              f"{colorize('Pass', Colors.GREEN)}: {self.total_pass}  "
              f"{colorize('Fail', Colors.RED)}: {self.total_fail}  "
              f"Skip: {self.total_skip}")

        if self.failed_tests:
            print(colorize(f"\n  FAILED TESTS:", Colors.RED))
            for suite, tid, reason in self.failed_tests:
                print(f"    {suite}: {tid} - {reason}")

        if self.total_fail == 0:
            print(colorize(f"\n  ALL TESTS PASSED ✓", Colors.GREEN))
        else:
            print(colorize(f"\n  {self.total_fail} TEST(S) FAILED ✗", Colors.RED))

        print()
        return self.total_fail == 0


def main():
    parser = argparse.ArgumentParser(description="ESP32 Jim Tcl Test Runner")
    parser.add_argument("port", help="Serial port (e.g., /dev/cu.usbserial-2110)")
    parser.add_argument("tests", nargs="*", help="Specific test files to run")
    parser.add_argument("--all", action="store_true", help="Run all test files")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--timeout", type=int, default=30, help="Per-command timeout (s)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show serial output")
    parser.add_argument("--constraint", "-c", action="append", default=[],
                        help="Set constraint: name=value")
    parser.add_argument("--test-dir", default=None, help="Test file directory")

    args = parser.parse_intermixed_args()
    test_dir = args.test_dir or os.path.dirname(os.path.abspath(__file__))

    if args.tests:
        test_files = []
        for t in args.tests:
            path = t if os.path.isfile(t) else os.path.join(test_dir, t)
            if os.path.isfile(path):
                test_files.append(path)
            else:
                print(f"Warning: not found: {t}")
    elif args.all:
        test_files = sorted(glob.glob(os.path.join(test_dir, "*.test")))
    else:
        safe = ["core.test", "json.test", "esp32.test", "task.test", "cron.test",
                "timer.test", "fs.test", "nvs.test", "adc.test", "pwm.test"]
        test_files = [os.path.join(test_dir, t) for t in safe
                      if os.path.isfile(os.path.join(test_dir, t))]

    if not test_files:
        print("No test files found.")
        sys.exit(1)

    constraints = {}
    for c in args.constraint:
        name, _, value = c.partition('=')
        constraints[name] = value or "1"

    runner = ESP32TestRunner(args.port, args.baud, args.timeout, args.verbose)

    try:
        runner.connect()
        if constraints:
            print(f"Setting constraints: {constraints}")
            runner.set_constraints(constraints)
        for f in test_files:
            runner.run_test_file(f)
        success = runner.print_summary()
    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(130)
    finally:
        runner.disconnect()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
