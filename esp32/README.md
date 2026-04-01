# Jim Tcl for ESP32

Run Tcl scripts on the ESP32 microcontroller. This port builds the Jim Tcl
interpreter as an ESP-IDF component and provides native extensions for ESP32
hardware peripherals.

## Features

- **Full Jim Tcl interpreter** on ESP32 (~50-100 KB flash, ~20-50 KB RAM per VM)
- **Multiple Tcl VMs** - run independent interpreters on separate FreeRTOS tasks
- **Native ESP32 extensions**: GPIO, WiFi, I2C, IEEE 802.15.4, NVS (non-volatile storage)
- **Interactive REPL** over UART for live development
- **Boot script** support via NVS for headless operation

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) v5.0+
- ESP32, ESP32-S2, ESP32-S3, ESP32-C3, or ESP32-C6 target

## Building

```bash
# Set up ESP-IDF environment
. $IDF_PATH/export.sh

# Build from the esp32 directory
cd esp32
idf.py set-target esp32    # or esp32s3, esp32c3, etc.
idf.py build
idf.py flash monitor
```

## Quick Start

Once flashed, connect via serial monitor (115200 baud). You'll see a `jim>` prompt:

```tcl
jim> puts "Hello from ESP32!"
Hello from ESP32!

jim> esp32 info
cores 2 revision 1 heap_free 280000

jim> esp32 heap
free 280000 minimum 270000
```

## Extension Reference

### GPIO

```tcl
gpio mode 2 output          ;# Configure GPIO2 as output
gpio write 2 1              ;# Set GPIO2 high
gpio write 2 0              ;# Set GPIO2 low
gpio mode 4 input           ;# Configure GPIO4 as input
gpio pullup 4 1             ;# Enable internal pull-up
set level [gpio read 4]     ;# Read GPIO4 level
```

### WiFi

```tcl
wifi connect "MySSID" "MyPassword"  ;# Connect to WiFi (blocks until connected)
wifi status                          ;# Returns: connected/disconnected
wifi ip                              ;# Returns IP address (e.g., 192.168.1.42)
wifi scan                            ;# Returns list of nearby networks
wifi disconnect
```

### I2C

```tcl
i2c init 0 -sda 21 -scl 22 -freq 400000   ;# Init I2C port 0
i2c detect 0                                 ;# Scan for devices
i2c write 0 0x48 {0x01 0x60}               ;# Write bytes to device
set data [i2c read 0 0x48 2]                ;# Read 2 bytes
i2c deinit 0
```

### NVS (Non-Volatile Storage)

```tcl
set h [nvs open "myapp"]        ;# Open namespace, get handle
nvs set $h "name" "ESP32 Node"  ;# Store string
nvs set $h "count" 42 int       ;# Store integer
nvs get $h "name"                ;# Returns: "ESP32 Node"
nvs get $h "count" int           ;# Returns: 42
nvs delete $h "count"
nvs close $h
```

### IEEE 802.15.4 (Zigbee/Thread/Matter radio)

Requires ESP32-C6, ESP32-H2, or other chip with 802.15.4 radio.

```tcl
ieee802154 init -channel 15 -panid 0xABCD -txpower 10  ;# Initialize radio

ieee802154 config                                 ;# Show current config
ieee802154 config -promiscuous 1                  ;# Enable promiscuous mode
ieee802154 config -shortaddr 0x0001               ;# Set short address

ieee802154 send {0x41 0x88 0x01 0xCD 0xAB 0xFF 0xFF 0x01 0x00 0x48 0x65 0x6C 0x6C 0x6F}
                                                  ;# Transmit raw frame

set frame [ieee802154 receive 10000]              ;# Receive (10s timeout)
puts "RSSI: [dict get $frame rssi]"
puts "Data: [dict get $frame data]"

ieee802154 energydetect 5000                      ;# Energy detect (5ms)
ieee802154 status                                 ;# Radio state

ieee802154 deinit                                 ;# Disable radio
```

### Task VMs (Multiple Interpreters)

```tcl
# Create a background Tcl VM
set t [task create -name "blink" -stacksize 8192 {
    gpio mode 2 output
    while {1} {
        gpio write 2 1
        esp32 sleep 500
        gpio write 2 0
        esp32 sleep 500
    }
}]

# Evaluate code in a running task
task eval $t {gpio read 2}

# Fire-and-forget
task send $t {gpio write 2 1}

# List all task VMs
task list

# Destroy a task
task delete $t
```

### ESP32 System

```tcl
esp32 info      ;# Chip info (cores, revision, heap)
esp32 heap      ;# Heap memory stats
esp32 sleep 100 ;# Cooperative sleep (ms) - yields to FreeRTOS
esp32 restart   ;# Reboot the ESP32
```

## Boot Script

Store a Tcl script in NVS to run automatically at boot:

```tcl
set h [nvs open "jimtcl"]
nvs set $h "boot" {
    puts "Booting..."
    wifi connect "MySSID" "MyPassword"
    gpio mode 2 output
    gpio write 2 1
    puts "Ready!"
}
nvs close $h
```

On next reboot, this script runs before the interactive prompt.

## Architecture

```
esp32/
  CMakeLists.txt              # ESP-IDF project file
  sdkconfig.defaults          # Default SDK config
  partitions.csv              # Flash partition table
  main/
    main.c                    # App entry point: init + REPL
  components/jimtcl/
    CMakeLists.txt             # Component build (pulls in ../../../jim.c etc.)
    jim-config.h               # ESP32-specific config (replaces autoconf output)
    jim-esp32.h                # Platform API header
    jim-esp32.c                # Platform shim: allocator, time, console I/O
    jim-esp32-loadext.c        # Static extension loader
    extensions/
      jim-gpio.c               # GPIO pin control
      jim-wifi.c               # WiFi STA management
      jim-i2c.c                # I2C master bus
      jim-nvs.c                # Non-volatile storage
      jim-esp-task.c           # Multi-VM FreeRTOS task management
      jim-ieee802154.c         # IEEE 802.15.4 radio (Zigbee/Thread)
```

The core Jim Tcl sources (`jim.c`, `jim-subcmd.c`, etc.) are compiled directly
from the parent repository - no files are copied or forked.

## Memory Considerations

- Each interpreter uses ~20-50 KB RAM depending on loaded scripts
- `JIM_MAX_CALLFRAME_DEPTH` is set to 200 (vs. 1000 default) to fit FreeRTOS stacks
- `JIM_MAX_EVAL_DEPTH` is set to 400 (vs. 2000 default)
- Task VMs default to 8 KB stack; increase via `-stacksize` for deep recursion
- Monitor heap usage with `esp32 heap` during development

## Adding New Extensions

1. Create `extensions/jim-myext.c` following the pattern in `jim-gpio.c`
2. Implement `Jim_myextInit(Jim_Interp *interp)` 
3. Add the source to `CMakeLists.txt`
4. Call `Jim_myextInit()` from `jim-esp32-loadext.c`
