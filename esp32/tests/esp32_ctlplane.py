"""
ESP32 Control Plane Client Library

Communicates with the ESP32 Jim Tcl mpack control plane over serial
using COBS-framed MessagePack.

Dependencies:
    pip install msgpack cobs pyserial

Usage:
    from esp32_ctlplane import ESP32ControlPlane

    cp = ESP32ControlPlane("/dev/cu.usbserial-2110", verbose=True)
    print(cp.sys_info())
    cp.close()
"""

import sys
import time
import msgpack
from cobs import cobs
import serial


class ESP32ControlPlaneError(Exception):
    """Raised when the control plane returns an error status."""
    pass


class ESP32ControlPlaneTimeout(Exception):
    """Raised when no response is received within the timeout."""
    pass


class ESP32ControlPlane:
    """Client for the ESP32 mpack/COBS control plane protocol."""

    def __init__(self, port, baud=115200, timeout=10, verbose=False):
        """Connect to ESP32 control plane.

        Args:
            port: Serial port path (e.g., /dev/cu.usbserial-2110).
            baud: Baud rate (default 115200).
            timeout: Default receive timeout in seconds (default 10).
            verbose: Print send/receive details when True.
        """
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.verbose = verbose
        self.ser = serial.Serial(port, baud, timeout=0.1)
        # Drain any stale data
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def send(self, cmd_dict, timeout=None):
        """Send a command dict, return response dict.

        Encodes cmd_dict as msgpack, COBS-frames it, sends over serial.
        Reads response: accumulates bytes until 0x00 delimiter,
        COBS-decodes, msgpack-decodes, and returns the dict.

        Args:
            cmd_dict: Command dictionary (must contain "cmd" key).
            timeout: Receive timeout in seconds (overrides default).

        Returns:
            Response dictionary from the device.

        Raises:
            ESP32ControlPlaneTimeout: If no complete response within timeout.
        """
        if timeout is None:
            timeout = self.timeout

        # Encode
        packed = msgpack.packb(cmd_dict, use_bin_type=True)
        cobs_frame = cobs.encode(packed)
        wire = cobs_frame + b'\x00'

        if self.verbose:
            self._log_send(cmd_dict, len(packed), len(cobs_frame))

        # Send
        self.ser.write(wire)
        self.ser.flush()

        # Receive: accumulate bytes until 0x00 delimiter
        buf = bytearray()
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if chunk:
                for byte in chunk:
                    if byte == 0x00:
                        # End of frame
                        if len(buf) == 0:
                            # Empty frame, skip
                            continue
                        return self._decode_response(buf)
                    else:
                        buf.append(byte)
            else:
                time.sleep(0.01)

        raise ESP32ControlPlaneTimeout(
            f"No response within {timeout}s (received {len(buf)} bytes so far)"
        )

    def _decode_response(self, cobs_frame):
        """Decode a COBS frame into a response dict."""
        decoded = cobs.decode(bytes(cobs_frame))
        resp = msgpack.unpackb(decoded, raw=False)

        if self.verbose:
            self._log_recv(resp, len(decoded), len(cobs_frame))

        return resp

    def _log_send(self, cmd_dict, packed_len, cobs_len):
        """Print verbose send info."""
        cmd = cmd_dict.get("cmd", "?")
        # Build a compact representation
        parts = [f'"cmd": "{cmd}"']
        for k, v in cmd_dict.items():
            if k == "cmd":
                continue
            parts.append(f'"{k}": {_format_value(v)}')
        compact = "{" + ", ".join(parts) + "}"
        print(f"  -> SEND: {compact} ({packed_len} bytes, COBS: {cobs_len} bytes)",
              flush=True)

    def _log_recv(self, resp, decoded_len, cobs_len):
        """Print verbose receive info."""
        parts = []
        for k, v in resp.items():
            parts.append(f'"{k}": {_format_value(v)}')
        compact = "{" + ", ".join(parts) + "}"
        print(f"  <- RECV: {compact} ({decoded_len} bytes, COBS: {cobs_len} bytes)",
              flush=True)

    def close(self):
        """Close serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    # -----------------------------------------------------------------------
    # Convenience methods
    # -----------------------------------------------------------------------

    def auth(self, key):
        """Authenticate with the control plane.

        Args:
            key: Authentication key string.

        Returns:
            Response dict.
        """
        return self.send({"cmd": "auth", "key": key})

    def vm_list(self):
        """List all active VMs.

        Returns:
            Response dict with "vms" list.
        """
        return self.send({"cmd": "vm.list"})

    def vm_info(self, name):
        """Get detailed info about a VM.

        Args:
            name: VM name.

        Returns:
            Response dict with VM details.
        """
        return self.send({"cmd": "vm.info", "target": name})

    def vm_eval(self, target, script):
        """Evaluate a script in a VM and wait for the result.

        Args:
            target: VM name.
            script: Tcl script to evaluate.

        Returns:
            Response dict with "retcode" and "result".
        """
        return self.send({"cmd": "vm.eval", "target": target, "script": script},
                         timeout=max(self.timeout, 30))

    def vm_send(self, target, script):
        """Fire-and-forget a script to a VM.

        Args:
            target: VM name.
            script: Tcl script to send.

        Returns:
            Response dict (status ok on success).
        """
        return self.send({"cmd": "vm.send", "target": target, "script": script})

    def vm_create(self, name, script=None, autorestart=False):
        """Create a new VM.

        Args:
            name: VM name (max 15 chars).
            script: Tcl script body for the VM (default: empty event loop).
            autorestart: Whether to auto-restart on crash.

        Returns:
            Response dict with "retcode" and "result".
        """
        if script is None:
            script = "vwait forever"
        cmd = {"cmd": "vm.create", "name": name, "script": script}
        return self.send(cmd)

    def vm_delete(self, target):
        """Delete (kill) a VM.

        Args:
            target: VM name.

        Returns:
            Response dict.
        """
        return self.send({"cmd": "vm.delete", "target": target})

    def vm_restart(self, target):
        """Restart a VM.

        Args:
            target: VM name.

        Returns:
            Response dict.
        """
        return self.send({"cmd": "vm.restart", "target": target})

    def sys_info(self):
        """Get system info (chip, cores, heap, uptime).

        Returns:
            Response dict with system details.
        """
        return self.send({"cmd": "sys.info"})

    def sys_heap(self):
        """Get heap memory info.

        Returns:
            Response dict with "free" and "minimum".
        """
        return self.send({"cmd": "sys.heap"})

    def sys_wifi(self):
        """Get WiFi connection info.

        Returns:
            Response dict with connection details.
        """
        return self.send({"cmd": "sys.wifi"})

    def sys_uptime(self):
        """Get system uptime.

        Returns:
            Response dict with "uptime_us".
        """
        return self.send({"cmd": "sys.uptime"})

    def eval(self, script):
        """Evaluate a script on the main interpreter.

        Args:
            script: Tcl script string.

        Returns:
            Response dict with "retcode" and "result".
        """
        return self.send({"cmd": "eval", "script": script})

    def vars_load(self, vars_dict, target=None):
        """Load variables into an interpreter.

        Args:
            vars_dict: Dict of variable name -> value pairs.
            target: Optional VM name (default: main interpreter).

        Returns:
            Response dict with "count".
        """
        cmd = {"cmd": "vars.load", "vars": vars_dict}
        if target is not None:
            cmd["target"] = target
        return self.send(cmd)

    def vars_get(self, names, target=None):
        """Get variable values from an interpreter.

        Args:
            names: List of variable name strings.
            target: Optional VM name (default: main interpreter).

        Returns:
            Response dict with "vars" map.
        """
        cmd = {"cmd": "vars.get", "names": names}
        if target is not None:
            cmd["target"] = target
        return self.send(cmd)


def _format_value(v):
    """Format a value for compact display."""
    if isinstance(v, str):
        if len(v) > 60:
            return f'"{v[:57]}..."'
        return f'"{v}"'
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, list):
        if len(v) == 0:
            return "[]"
        if len(v) <= 3:
            items = ", ".join(_format_value(x) for x in v)
            return f"[{items}]"
        return f"[...{len(v)} items]"
    if isinstance(v, dict):
        if len(v) == 0:
            return "{}"
        return f"{{...{len(v)} keys}}"
    if v is None:
        return "null"
    return str(v)
