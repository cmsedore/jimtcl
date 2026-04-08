#!/usr/bin/env python3
"""
ESP32 Control Plane Test Harness

Tests the mpack/COBS control plane protocol end-to-end over serial.

Usage:
    python test_ctlplane.py /dev/cu.usbserial-2110
    python test_ctlplane.py /dev/cu.usbserial-2110 --verbose
    python test_ctlplane.py /dev/cu.usbserial-2110 --auth-key mykey
    python test_ctlplane.py /dev/cu.usbserial-2110 --timeout 15
"""

import argparse
import functools
import sys
import time
import traceback

from esp32_ctlplane import (
    ESP32ControlPlane,
    ESP32ControlPlaneError,
    ESP32ControlPlaneTimeout,
)

print = functools.partial(print, flush=True)


# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Test result tracking
# ---------------------------------------------------------------------------

class TestResult:
    def __init__(self, name, status, message=""):
        """
        Args:
            name: Test function name.
            status: "pass", "fail", or "skip".
            message: Optional detail message.
        """
        self.name = name
        self.status = status
        self.message = message


# ---------------------------------------------------------------------------
# Individual tests
# ---------------------------------------------------------------------------

def test_sys_info(cp):
    """sys.info returns chip model, cores, heap info, and uptime."""
    resp = cp.sys_info()
    assert resp["status"] == "ok", f"Expected ok, got {resp['status']}"
    assert "cores" in resp, "Missing 'cores' field"
    assert "heap_free" in resp, "Missing 'heap_free' field"
    assert "heap_min" in resp, "Missing 'heap_min' field"
    assert "uptime_us" in resp, "Missing 'uptime_us' field"
    assert "model" in resp, "Missing 'model' field"
    assert resp["cores"] >= 1, f"Expected cores >= 1, got {resp['cores']}"
    assert resp["heap_free"] > 0, f"Expected heap_free > 0, got {resp['heap_free']}"


def test_sys_heap(cp):
    """sys.heap returns free and minimum heap sizes."""
    resp = cp.sys_heap()
    assert resp["status"] == "ok", f"Expected ok, got {resp['status']}"
    assert "free" in resp, "Missing 'free' field"
    assert "minimum" in resp, "Missing 'minimum' field"
    assert resp["free"] > 0, f"Expected free > 0, got {resp['free']}"
    assert resp["minimum"] > 0, f"Expected minimum > 0, got {resp['minimum']}"
    assert resp["free"] >= resp["minimum"], \
        f"free ({resp['free']}) should be >= minimum ({resp['minimum']})"


def test_sys_uptime(cp):
    """sys.uptime returns a positive uptime_us value."""
    resp = cp.sys_uptime()
    assert resp["status"] == "ok", f"Expected ok, got {resp['status']}"
    assert "uptime_us" in resp, "Missing 'uptime_us' field"
    assert resp["uptime_us"] > 0, f"Expected uptime_us > 0, got {resp['uptime_us']}"


def test_sys_wifi(cp):
    """sys.wifi returns connection status."""
    resp = cp.sys_wifi()
    assert resp["status"] == "ok", f"Expected ok, got {resp['status']}"
    assert "connected" in resp, "Missing 'connected' field"
    assert isinstance(resp["connected"], bool), \
        f"'connected' should be bool, got {type(resp['connected'])}"


def test_vm_list_default(cp):
    """vm.list returns a list (may or may not be empty depending on device state)."""
    resp = cp.vm_list()
    assert resp["status"] == "ok", f"Expected ok, got {resp['status']}"
    assert "vms" in resp, "Missing 'vms' field"
    assert isinstance(resp["vms"], list), f"'vms' should be a list, got {type(resp['vms'])}"


def test_vm_create_and_list(cp):
    """Create a VM, verify it appears in vm.list, then clean up."""
    # Clean up in case a previous run left it behind
    cp.vm_delete("_test_vm")
    time.sleep(0.5)

    # Create
    resp = cp.vm_create("_test_vm", script="vwait forever")
    assert resp["status"] == "ok", f"vm.create failed: {resp}"

    # Wait for it to start
    time.sleep(1.0)

    # List and find it
    resp = cp.vm_list()
    assert resp["status"] == "ok", f"vm.list failed: {resp}"
    names = [vm["name"] for vm in resp["vms"]]
    assert "_test_vm" in names, f"_test_vm not in vm list: {names}"

    # Clean up
    cp.vm_delete("_test_vm")
    time.sleep(0.5)


def test_vm_eval(cp):
    """Eval a script in a VM and get the result."""
    # Create a VM
    cp.vm_delete("_test_eval")
    time.sleep(0.3)
    resp = cp.vm_create("_test_eval", script="vwait forever")
    assert resp["status"] == "ok", f"vm.create failed: {resp}"
    time.sleep(1.0)

    # Eval
    resp = cp.vm_eval("_test_eval", "expr {2 + 3}")
    assert resp["status"] == "ok", f"vm.eval failed: {resp}"
    assert resp["retcode"] == 0, f"Expected retcode 0, got {resp['retcode']}"
    assert resp["result"] == "5", f"Expected '5', got '{resp['result']}'"

    # Clean up
    cp.vm_delete("_test_eval")
    time.sleep(0.3)


def test_vm_send(cp):
    """Fire-and-forget a script to a VM succeeds."""
    # Create a VM
    cp.vm_delete("_test_send")
    time.sleep(0.3)
    resp = cp.vm_create("_test_send", script="vwait forever")
    assert resp["status"] == "ok", f"vm.create failed: {resp}"
    time.sleep(1.0)

    # Send (fire-and-forget)
    resp = cp.vm_send("_test_send", "set x 42")
    assert resp["status"] == "ok", f"vm.send failed: {resp}"

    # Clean up
    cp.vm_delete("_test_send")
    time.sleep(0.3)


def test_vm_info(cp):
    """Get detailed VM info for a running VM."""
    # Create a VM
    cp.vm_delete("_test_info")
    time.sleep(0.3)
    resp = cp.vm_create("_test_info", script="vwait forever")
    assert resp["status"] == "ok", f"vm.create failed: {resp}"
    time.sleep(1.0)

    # Get info
    resp = cp.vm_info("_test_info")
    assert resp["status"] == "ok", f"vm.info failed: {resp}"
    assert resp["name"] == "_test_info", f"Expected name '_test_info', got '{resp['name']}'"
    assert resp["state"] == "running", f"Expected state 'running', got '{resp['state']}'"
    assert "stacksize" in resp, "Missing 'stacksize' field"
    assert "priority" in resp, "Missing 'priority' field"
    assert "auto_restart" in resp, "Missing 'auto_restart' field"
    assert "cb_state" in resp, "Missing 'cb_state' field"

    # Clean up
    cp.vm_delete("_test_info")
    time.sleep(0.3)


def test_vm_delete(cp):
    """Delete a VM and verify it no longer appears in vm.list."""
    # Create
    cp.vm_delete("_test_del")
    time.sleep(0.3)
    resp = cp.vm_create("_test_del", script="vwait forever")
    assert resp["status"] == "ok", f"vm.create failed: {resp}"
    time.sleep(1.0)

    # Delete
    resp = cp.vm_delete("_test_del")
    assert resp["status"] == "ok", f"vm.delete failed: {resp}"
    time.sleep(0.5)

    # Verify gone
    resp = cp.vm_list()
    names = [vm["name"] for vm in resp["vms"]]
    assert "_test_del" not in names, f"_test_del still in vm list after delete: {names}"


def test_eval_main(cp):
    """Eval a script on the main interpreter."""
    resp = cp.eval("expr {10 * 7}")
    assert resp["status"] == "ok", f"eval failed: {resp}"
    assert resp["retcode"] == 0, f"Expected retcode 0, got {resp['retcode']}"
    assert resp["result"] == "70", f"Expected '70', got '{resp['result']}'"


def test_eval_error(cp):
    """Eval an invalid script returns error status with retcode != 0."""
    resp = cp.eval("nonexistent_command_xyz")
    assert resp["status"] == "error", f"Expected error status, got '{resp['status']}'"
    assert resp["retcode"] != 0, f"Expected non-zero retcode, got {resp['retcode']}"
    assert len(resp["result"]) > 0, "Expected non-empty error message"


def test_vars_load_and_get(cp):
    """Load variables into main interpreter, read them back."""
    load_resp = cp.vars_load({"_test_a": "hello", "_test_b": "42"})
    assert load_resp["status"] == "ok", f"vars.load failed: {load_resp}"
    assert load_resp["count"] == 2, f"Expected count 2, got {load_resp['count']}"

    get_resp = cp.vars_get(["_test_a", "_test_b"])
    assert get_resp["status"] == "ok", f"vars.get failed: {get_resp}"
    assert get_resp["vars"]["_test_a"] == "hello", \
        f"Expected 'hello', got '{get_resp['vars']['_test_a']}'"
    assert get_resp["vars"]["_test_b"] == "42", \
        f"Expected '42', got '{get_resp['vars']['_test_b']}'"


def test_vars_get_nonexistent(cp):
    """Getting a nonexistent variable returns null."""
    resp = cp.vars_get(["_surely_this_does_not_exist_xyz"])
    assert resp["status"] == "ok", f"vars.get failed: {resp}"
    val = resp["vars"]["_surely_this_does_not_exist_xyz"]
    assert val is None, f"Expected None for nonexistent var, got {val!r}"


def test_round_trip_types(cp):
    """Verify string values survive round-trip via vars.load/vars.get."""
    test_vars = {
        "_rt_str": "hello world",
        "_rt_num": "12345",
        "_rt_neg": "-99",
        "_rt_float": "3.14159",
        "_rt_empty": "",
        "_rt_special": "foo bar\tbaz",
    }
    resp = cp.vars_load(test_vars)
    assert resp["status"] == "ok", f"vars.load failed: {resp}"

    resp = cp.vars_get(list(test_vars.keys()))
    assert resp["status"] == "ok", f"vars.get failed: {resp}"

    for key, expected in test_vars.items():
        actual = resp["vars"].get(key)
        assert actual == expected, \
            f"Round-trip mismatch for {key}: expected {expected!r}, got {actual!r}"


def test_unknown_command(cp):
    """Sending an unknown command returns an error."""
    resp = cp.send({"cmd": "totally.bogus.command"})
    assert resp["status"] == "error", f"Expected error, got {resp['status']}"
    assert "unknown" in resp.get("message", "").lower(), \
        f"Expected 'unknown' in message, got: {resp.get('message', '')}"


def test_missing_cmd_field(cp):
    """Sending a message without 'cmd' returns an error."""
    resp = cp.send({"not_cmd": "oops"})
    assert resp["status"] == "error", f"Expected error, got {resp['status']}"


# ---------------------------------------------------------------------------
# Auth-specific tests (only when --auth-key is provided)
# ---------------------------------------------------------------------------

def test_auth_required(cp):
    """When auth is configured, commands without auth return auth_required."""
    # Create a fresh connection (no auth session)
    fresh = ESP32ControlPlane(cp.port, cp.baud, timeout=cp.timeout, verbose=cp.verbose)
    try:
        resp = fresh.sys_info()
        assert resp["status"] == "auth_required", \
            f"Expected auth_required, got {resp['status']} (auth may not be configured)"
    finally:
        fresh.close()


def test_auth_success(cp, auth_key):
    """Authenticate with the correct key, then issue a command."""
    fresh = ESP32ControlPlane(cp.port, cp.baud, timeout=cp.timeout, verbose=cp.verbose)
    try:
        resp = fresh.auth(auth_key)
        assert resp["status"] == "ok", f"auth failed: {resp}"

        # Now a protected command should work
        resp = fresh.sys_info()
        assert resp["status"] == "ok", f"sys.info after auth failed: {resp}"
    finally:
        fresh.close()


def test_auth_bad_key(cp):
    """Authenticate with an incorrect key fails."""
    fresh = ESP32ControlPlane(cp.port, cp.baud, timeout=cp.timeout, verbose=cp.verbose)
    try:
        resp = fresh.auth("wrong_key_definitely_not_right")
        assert resp["status"] == "error", f"Expected error for bad key, got {resp['status']}"
    finally:
        fresh.close()


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

# Tests that always run (no auth required or test handles both cases)
STANDARD_TESTS = [
    test_sys_info,
    test_sys_heap,
    test_sys_uptime,
    test_sys_wifi,
    test_vm_list_default,
    test_vm_create_and_list,
    test_vm_eval,
    test_vm_send,
    test_vm_info,
    test_vm_delete,
    test_eval_main,
    test_eval_error,
    test_vars_load_and_get,
    test_vars_get_nonexistent,
    test_round_trip_types,
    test_unknown_command,
    test_missing_cmd_field,
]

# Tests that require --auth-key
AUTH_TESTS = [
    test_auth_required,
    test_auth_bad_key,
    # test_auth_success is special-cased because it needs the key argument
]


def detect_ctlplane(cp):
    """Try to detect if the device is in control plane (mpack) mode.

    Sends a sys.info command and checks for a valid mpack response.
    Returns True if the device responds with a valid control plane message.
    """
    try:
        resp = cp.sys_info()
        return resp.get("status") in ("ok", "auth_required")
    except ESP32ControlPlaneTimeout:
        return False
    except Exception:
        return False


def run_tests(cp, auth_key=None):
    """Run all tests and print results.

    Args:
        cp: ESP32ControlPlane instance.
        auth_key: Optional auth key for auth tests.

    Returns:
        True if all tests passed (skips don't count as failures).
    """
    results = []

    # Standard tests
    for test_fn in STANDARD_TESTS:
        name = test_fn.__name__
        try:
            test_fn(cp)
            results.append(TestResult(name, "pass"))
            print(f"  {colorize('PASS', Colors.GREEN)}: {name}")
        except AssertionError as e:
            results.append(TestResult(name, "fail", str(e)))
            print(f"  {colorize('FAIL', Colors.RED)}: {name} - {e}")
        except ESP32ControlPlaneTimeout as e:
            results.append(TestResult(name, "fail", f"Timeout: {e}"))
            print(f"  {colorize('FAIL', Colors.RED)}: {name} - Timeout: {e}")
        except Exception as e:
            results.append(TestResult(name, "fail", f"{type(e).__name__}: {e}"))
            print(f"  {colorize('FAIL', Colors.RED)}: {name} - {type(e).__name__}: {e}")
            if cp.verbose:
                traceback.print_exc()

    # Auth tests
    if auth_key:
        for test_fn in AUTH_TESTS:
            name = test_fn.__name__
            try:
                test_fn(cp)
                results.append(TestResult(name, "pass"))
                print(f"  {colorize('PASS', Colors.GREEN)}: {name}")
            except AssertionError as e:
                results.append(TestResult(name, "fail", str(e)))
                print(f"  {colorize('FAIL', Colors.RED)}: {name} - {e}")
            except ESP32ControlPlaneTimeout as e:
                results.append(TestResult(name, "fail", f"Timeout: {e}"))
                print(f"  {colorize('FAIL', Colors.RED)}: {name} - Timeout: {e}")
            except Exception as e:
                results.append(TestResult(name, "fail", f"{type(e).__name__}: {e}"))
                print(f"  {colorize('FAIL', Colors.RED)}: {name} - {type(e).__name__}: {e}")
                if cp.verbose:
                    traceback.print_exc()

        # Special: test_auth_success needs the key
        name = "test_auth_success"
        try:
            test_auth_success(cp, auth_key)
            results.append(TestResult(name, "pass"))
            print(f"  {colorize('PASS', Colors.GREEN)}: {name}")
        except AssertionError as e:
            results.append(TestResult(name, "fail", str(e)))
            print(f"  {colorize('FAIL', Colors.RED)}: {name} - {e}")
        except ESP32ControlPlaneTimeout as e:
            results.append(TestResult(name, "fail", f"Timeout: {e}"))
            print(f"  {colorize('FAIL', Colors.RED)}: {name} - Timeout: {e}")
        except Exception as e:
            results.append(TestResult(name, "fail", f"{type(e).__name__}: {e}"))
            print(f"  {colorize('FAIL', Colors.RED)}: {name} - {type(e).__name__}: {e}")
    else:
        for test_fn in AUTH_TESTS:
            results.append(TestResult(test_fn.__name__, "skip", "no --auth-key provided"))
            print(f"  {colorize('SKIP', Colors.YELLOW)}: {test_fn.__name__} (no --auth-key)")
        results.append(TestResult("test_auth_success", "skip", "no --auth-key provided"))
        print(f"  {colorize('SKIP', Colors.YELLOW)}: test_auth_success (no --auth-key)")

    # Summary
    passed = sum(1 for r in results if r.status == "pass")
    failed = sum(1 for r in results if r.status == "fail")
    skipped = sum(1 for r in results if r.status == "skip")

    print(colorize(f"\n{'='*60}", Colors.BOLD))
    print(f"  RESULTS: "
          f"{colorize(str(passed), Colors.GREEN)} passed, "
          f"{colorize(str(failed), Colors.RED) if failed else '0'} failed, "
          f"{skipped} skipped")

    if failed:
        print(colorize(f"\n  FAILED:", Colors.RED))
        for r in results:
            if r.status == "fail":
                print(f"    {r.name}: {r.message}")
        print(colorize(f"\n  {failed} TEST(S) FAILED", Colors.RED))
    else:
        print(colorize(f"\n  ALL TESTS PASSED", Colors.GREEN))

    print(colorize(f"{'='*60}\n", Colors.BOLD))

    return failed == 0


def main():
    parser = argparse.ArgumentParser(
        description="ESP32 Control Plane Test Harness",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
The device must be running in mpack control plane mode
(CONFIG_JIM_BOOT_MPACK) or have the ctlplane started on the
target UART. If the device is in REPL mode, the binary protocol
will not be understood and detection will fail.
""")
    parser.add_argument("port", help="Serial port (e.g., /dev/cu.usbserial-2110)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--timeout", type=int, default=10,
                        help="Response timeout in seconds (default: 10)")
    parser.add_argument("--auth-key", default=None,
                        help="Auth key to test authentication commands")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Show send/receive details for every command")

    args = parser.parse_args()

    print(colorize(f"\nESP32 Control Plane Test Harness", Colors.BOLD))
    print(colorize(f"{'='*60}", Colors.BOLD))
    print(f"  Port:    {args.port}")
    print(f"  Baud:    {args.baud}")
    print(f"  Timeout: {args.timeout}s")
    print(f"  Auth:    {'yes' if args.auth_key else 'no'}")
    print(f"  Verbose: {'yes' if args.verbose else 'no'}")
    print(colorize(f"{'='*60}\n", Colors.BOLD))

    try:
        cp = ESP32ControlPlane(args.port, baud=args.baud, timeout=args.timeout,
                               verbose=args.verbose)
    except Exception as e:
        print(f"{colorize('ERROR', Colors.RED)}: Failed to open serial port: {e}")
        sys.exit(2)

    # Reset device and wait for control plane to start
    print("Resetting device and waiting for boot...", end=" ")
    cp.ser.dtr = False
    cp.ser.rts = True
    time.sleep(0.1)
    cp.ser.rts = False
    # Wait for boot to complete (control plane starts after ~2-3s)
    time.sleep(5)
    # Drain all boot log output
    while cp.ser.in_waiting:
        cp.ser.read(cp.ser.in_waiting)
        time.sleep(0.1)
    print(colorize("OK", Colors.GREEN))

    # Detect control plane mode
    print("Detecting control plane mode...", end=" ")
    if detect_ctlplane(cp):
        status = "auth_required" if args.auth_key else "ok"
        print(colorize("OK", Colors.GREEN))
    else:
        print(colorize("FAILED", Colors.RED))
        print()
        print("The device does not appear to be in control plane mode.")
        print("Make sure the device is booted with CONFIG_JIM_BOOT_MPACK=y,")
        print("or start the control plane manually:")
        print("  ctlplane start serial 1 -tx <pin> -rx <pin> -baud 115200")
        cp.close()
        sys.exit(2)

    # If auth key provided, authenticate before running tests
    if args.auth_key:
        print("Authenticating...", end=" ")
        try:
            resp = cp.auth(args.auth_key)
            if resp["status"] == "ok":
                print(colorize("OK", Colors.GREEN))
            else:
                print(colorize(f"FAILED: {resp}", Colors.RED))
                cp.close()
                sys.exit(2)
        except Exception as e:
            print(colorize(f"FAILED: {e}", Colors.RED))
            cp.close()
            sys.exit(2)

    print()

    try:
        success = run_tests(cp, auth_key=args.auth_key)
    except KeyboardInterrupt:
        print("\nInterrupted.")
        cp.close()
        sys.exit(130)
    except Exception as e:
        print(f"\n{colorize('FATAL', Colors.RED)}: {e}")
        if args.verbose:
            traceback.print_exc()
        cp.close()
        sys.exit(2)
    finally:
        cp.close()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
