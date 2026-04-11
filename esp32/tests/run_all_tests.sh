#!/bin/bash
# ESP32 Jim Tcl - Full Validation Test Suite
#
# Automates the complete test cycle:
#   1. Build in REPL mode, flash, run on-device Tcl tests
#   2. Build in mpack mode (with auth), flash, run Python control plane tests
#   3. Restore REPL mode
#
# Usage:
#   ./run_all_tests.sh /dev/cu.usbserial-2110
#   ./run_all_tests.sh /dev/cu.usbserial-2110 --skip-repl    # skip REPL tests
#   ./run_all_tests.sh /dev/cu.usbserial-2110 --skip-mpack   # skip mpack tests
#   ./run_all_tests.sh /dev/cu.usbserial-2110 --verbose

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Defaults
PORT=""
SKIP_REPL=0
SKIP_MPACK=0
VERBOSE=""
AUTH_KEY="test_validation_key_42"
BAUD=115200
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
# Use system Python for test scripts (ESP-IDF venv may lack msgpack/cobs)
SYS_PYTHON="/Library/Developer/CommandLineTools/usr/bin/python3"
if [[ ! -x "$SYS_PYTHON" ]]; then
    SYS_PYTHON=$(which python3 2>/dev/null || echo "python3")
fi

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-repl)  SKIP_REPL=1; shift ;;
        --skip-mpack) SKIP_MPACK=1; shift ;;
        --verbose|-v) VERBOSE="-v"; shift ;;
        --port|-p)    PORT="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 <port> [--skip-repl] [--skip-mpack] [--verbose]"
            echo ""
            echo "Runs the full ESP32 Jim Tcl validation test suite:"
            echo "  Phase 1: REPL mode — on-device Tcl tests (core, json, esp32, mpack, etc)"
            echo "  Phase 2: mpack mode — Python control plane tests (with auth)"
            echo ""
            echo "Options:"
            echo "  --skip-repl    Skip REPL mode tests"
            echo "  --skip-mpack   Skip mpack control plane tests"
            echo "  --verbose, -v  Show detailed test output"
            exit 0
            ;;
        *)
            if [[ -z "$PORT" ]]; then
                PORT="$1"
            fi
            shift
            ;;
    esac
done

if [[ -z "$PORT" ]]; then
    echo -e "${RED}Error: serial port required${NC}"
    echo "Usage: $0 /dev/cu.usbserial-2110 [options]"
    exit 1
fi

# Source ESP-IDF environment
echo -e "${CYAN}Sourcing ESP-IDF environment...${NC}"
. "$IDF_PATH/export.sh" > /dev/null 2>&1

# Check Python dependencies
echo -e "${CYAN}Checking Python dependencies...${NC}"
"$SYS_PYTHON" -c "import msgpack, cobs, serial" 2>/dev/null || {
    echo -e "${YELLOW}Installing missing Python packages...${NC}"
    "$SYS_PYTHON" -m pip install msgpack cobs pyserial --quiet 2>/dev/null || true
}

# Track results
REPL_RESULT=""
MPACK_RESULT=""
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0
START_TIME=$(date +%s)

# Helper: set sdkconfig option (uses Python for reliability across platforms)
set_sdkconfig() {
    local key="$1"
    local value="$2"
    local file="$PROJECT_DIR/sdkconfig"
    "$SYS_PYTHON" -c "
import re, sys
key, value, path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as f:
    content = f.read()
if value == 'y':
    content = re.sub(r'# ' + re.escape(key) + r' is not set', key + '=y', content)
    content = re.sub(re.escape(key) + r'=.*', key + '=y', content)
    if key + '=y' not in content:
        content += key + '=y\n'
elif value == 'n':
    content = re.sub(re.escape(key) + r'=y', '# ' + key + ' is not set', content)
else:
    content = re.sub(re.escape(key) + r'=.*', key + '=\"' + value + '\"', content)
    if key + '=' not in content:
        content += key + '=\"' + value + '\"\n'
with open(path, 'w') as f:
    f.write(content)
" "$key" "$value" "$file"
}

# Helper: build and flash
build_and_flash() {
    local mode="$1"
    echo -e "\n${BOLD}${CYAN}Building ($mode mode)...${NC}"
    cd "$PROJECT_DIR"

    if ! idf.py build > /tmp/esp32_build.log 2>&1; then
        echo -e "${RED}Build failed!${NC}"
        tail -20 /tmp/esp32_build.log
        return 1
    fi
    echo -e "${GREEN}Build OK${NC}"

    echo -e "${CYAN}Flashing...${NC}"
    if ! idf.py -p "$PORT" flash > /tmp/esp32_flash.log 2>&1; then
        echo -e "${RED}Flash failed!${NC}"
        tail -10 /tmp/esp32_flash.log
        return 1
    fi
    echo -e "${GREEN}Flash OK${NC}"
}

# ============================================================
# Phase 1: REPL mode tests
# ============================================================
if [[ $SKIP_REPL -eq 0 ]]; then
    echo -e "\n${BOLD}============================================================${NC}"
    echo -e "${BOLD}  Phase 1: REPL Mode — On-Device Tcl Tests${NC}"
    echo -e "${BOLD}============================================================${NC}"

    # Configure for REPL mode
    set_sdkconfig "CONFIG_JIM_BOOT_REPL" "y"
    set_sdkconfig "CONFIG_JIM_BOOT_MPACK" "n"
    set_sdkconfig "CONFIG_JIM_CTLPLANE_AUTH_KEY" ""

    build_and_flash "REPL" || { REPL_RESULT="BUILD_FAIL"; }

    if [[ -z "$REPL_RESULT" ]]; then
        echo -e "\n${CYAN}Running on-device tests...${NC}"
        cd "$SCRIPT_DIR"

        # Run tests and capture output
        REPL_OUTPUT=$("$SYS_PYTHON" run_tests.py "$PORT" \
            core.test json.test esp32.test mpack.test ctlplane.test \
            $VERBOSE 2>&1) || true

        echo "$REPL_OUTPUT"

        # Parse results (macOS-compatible)
        REPL_PASS=$(echo "$REPL_OUTPUT" | sed -n 's/.*Pass: \([0-9]*\).*/\1/p' | tail -1)
        REPL_FAIL=$(echo "$REPL_OUTPUT" | sed -n 's/.*Fail: \([0-9]*\).*/\1/p' | tail -1)
        REPL_SKIP=$(echo "$REPL_OUTPUT" | sed -n 's/.*Skip: \([0-9]*\).*/\1/p' | tail -1)
        REPL_PASS=${REPL_PASS:-0}
        REPL_FAIL=${REPL_FAIL:-0}
        REPL_SKIP=${REPL_SKIP:-0}

        TOTAL_PASS=$((TOTAL_PASS + REPL_PASS))
        TOTAL_FAIL=$((TOTAL_FAIL + REPL_FAIL))
        TOTAL_SKIP=$((TOTAL_SKIP + REPL_SKIP))

        if [[ "$REPL_FAIL" -eq 0 ]]; then
            REPL_RESULT="PASS"
        else
            REPL_RESULT="FAIL"
        fi
    fi
else
    echo -e "\n${YELLOW}Skipping REPL mode tests${NC}"
    REPL_RESULT="SKIP"
fi

# ============================================================
# Phase 2: mpack control plane tests
# ============================================================
if [[ $SKIP_MPACK -eq 0 ]]; then
    echo -e "\n${BOLD}============================================================${NC}"
    echo -e "${BOLD}  Phase 2: mpack Mode — Python Control Plane Tests${NC}"
    echo -e "${BOLD}============================================================${NC}"

    # Configure for mpack boot mode with auth.
    # Directly modify sdkconfig (choice + string value).
    echo -e "${CYAN}Configuring for mpack mode with auth...${NC}"
    cd "$PROJECT_DIR"

    set_sdkconfig "CONFIG_JIM_BOOT_REPL" "n"
    set_sdkconfig "CONFIG_JIM_BOOT_MPACK" "y"
    set_sdkconfig "CONFIG_JIM_CTLPLANE_AUTH_KEY" "$AUTH_KEY"

    build_and_flash "mpack" || { MPACK_RESULT="BUILD_FAIL"; }

    if [[ -z "$MPACK_RESULT" ]]; then
        echo -e "\n${CYAN}Running control plane tests (with auth)...${NC}"
        cd "$SCRIPT_DIR"

        MPACK_OUTPUT=$("$SYS_PYTHON" test_ctlplane.py "$PORT" \
            --auth-key "$AUTH_KEY" \
            $VERBOSE 2>&1) || true

        echo "$MPACK_OUTPUT"

        # Parse results (macOS-compatible)
        MPACK_PASS=$(echo "$MPACK_OUTPUT" | sed -n 's/.*\([0-9][0-9]*\) passed.*/\1/p' | tail -1)
        MPACK_FAIL=$(echo "$MPACK_OUTPUT" | sed -n 's/.*\([0-9][0-9]*\) failed.*/\1/p' | tail -1)
        MPACK_SKIP=$(echo "$MPACK_OUTPUT" | sed -n 's/.*\([0-9][0-9]*\) skipped.*/\1/p' | tail -1)
        MPACK_PASS=${MPACK_PASS:-0}
        MPACK_FAIL=${MPACK_FAIL:-0}
        MPACK_SKIP=${MPACK_SKIP:-0}

        TOTAL_PASS=$((TOTAL_PASS + MPACK_PASS))
        TOTAL_FAIL=$((TOTAL_FAIL + MPACK_FAIL))
        TOTAL_SKIP=$((TOTAL_SKIP + MPACK_SKIP))

        if [[ "$MPACK_FAIL" -eq 0 ]]; then
            MPACK_RESULT="PASS"
        else
            MPACK_RESULT="FAIL"
        fi
    fi
else
    echo -e "\n${YELLOW}Skipping mpack mode tests${NC}"
    MPACK_RESULT="SKIP"
fi

# ============================================================
# Restore REPL mode
# ============================================================
echo -e "\n${CYAN}Restoring REPL mode...${NC}"
cd "$PROJECT_DIR"
set_sdkconfig "CONFIG_JIM_BOOT_MPACK" "n"
set_sdkconfig "CONFIG_JIM_BOOT_REPL" "y"
set_sdkconfig "CONFIG_JIM_CTLPLANE_AUTH_KEY" ""
idf.py build > /dev/null 2>&1 && idf.py -p "$PORT" flash > /dev/null 2>&1 && \
    echo -e "${GREEN}Device restored to REPL mode${NC}" || \
    echo -e "${YELLOW}Warning: failed to restore REPL mode${NC}"

# ============================================================
# Final Summary
# ============================================================
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo -e "\n${BOLD}============================================================${NC}"
echo -e "${BOLD}  VALIDATION TEST RESULTS${NC}"
echo -e "${BOLD}============================================================${NC}"
echo ""
colorize_result() {
    case $1 in
        PASS)       echo -e "${GREEN}PASS${NC}" ;;
        FAIL)       echo -e "${RED}FAIL${NC}" ;;
        SKIP)       echo -e "${YELLOW}SKIP${NC}" ;;
        BUILD_FAIL) echo -e "${RED}BUILD FAILED${NC}" ;;
        *)          echo "$1" ;;
    esac
}
echo -e "  Phase 1 (REPL on-device):    $(colorize_result $REPL_RESULT)"
echo -e "  Phase 2 (mpack ctl plane):   $(colorize_result $MPACK_RESULT)"
echo ""
TOTAL=$((TOTAL_PASS + TOTAL_FAIL + TOTAL_SKIP))
echo -e "  Total: ${TOTAL}  ${GREEN}Pass: ${TOTAL_PASS}${NC}  ${RED}Fail: ${TOTAL_FAIL}${NC}  Skip: ${TOTAL_SKIP}"
echo -e "  Duration: ${ELAPSED}s"
echo ""

if [[ $TOTAL_FAIL -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}ALL VALIDATION TESTS PASSED ✓${NC}"
    echo ""
    exit 0
else
    echo -e "  ${RED}${BOLD}${TOTAL_FAIL} TEST(S) FAILED ✗${NC}"
    echo ""
    exit 1
fi
