# ESP32 Test Runner
#
# Usage on ESP32 REPL (two options):
#
# Option A: Using the C-level [test] extension (preferred)
#   The [test] command is available if CONFIG_JIM_EXT_TEST=y.
#   Just paste or eval test files directly -- no framework loading needed.
#
#     test reset
#     test suite "Core Tests"
#     eval [fs read /data/tests/core.test]
#     test suite "JSON Tests"
#     eval [fs read /data/tests/json.test]
#     test report
#
# Option B: Using the pure-Tcl testlib.tcl
#   Load the framework first, then eval test files:
#
#     eval [fs read /data/tests/testlib.tcl]
#     testReset
#     testSuite "Core Tests"
#     eval [fs read /data/tests/core.test]
#     testSuite "JSON Tests"
#     eval [fs read /data/tests/json.test]
#     testReport
#
# Option C: Paste everything into the REPL
#   Each .test file is self-contained enough to paste directly.
#   Just load testlib.tcl first (or have the C extension).
#
# Uploading test files to SPIFFS:
#   fs mount
#   fs mkdir /data/tests
#   fs write /data/tests/testlib.tcl {... paste content ...}
#   fs write /data/tests/core.test  {... paste content ...}

# --- Runner script (assumes fs is mounted and test files are on SPIFFS) ---

# Detect which framework to use
if {[info commands test] ne "" && ![catch {test report}]} {
    # C extension available
    set _use_c_ext 1
} else {
    # Fall back to pure Tcl
    set _use_c_ext 0
    eval [fs read /data/tests/testlib.tcl]
}

proc _run_suite {name file} {
    if {$::_use_c_ext} {
        test suite $name
    } else {
        testSuite $name
    }
    if {![catch {fs read /data/tests/$file} contents]} {
        uplevel #0 $contents
    } else {
        puts "  (skipped -- $file not found)"
    }
}

# Reset counters
if {$_use_c_ext} {
    test reset
} else {
    testReset
}

# Run suites -- add new .test files here
_run_suite "Core Tests"       core.test
_run_suite "JSON Tests"       json.test
_run_suite "GPIO Tests"       gpio.test
_run_suite "NVS Tests"        nvs.test
_run_suite "WiFi Tests"       wifi.test
_run_suite "Timer Tests"      timer.test
_run_suite "Filesystem Tests" fs.test

# Print summary
if {$_use_c_ext} {
    test report
} else {
    testReport
}

# Cleanup
rename _run_suite {}
unset _use_c_ext
