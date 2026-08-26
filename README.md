# USBLiter8 BootSurreal

## Overview

**USBLiter8 BootSurreal** is a standalone extension for `usbliter8` that allows an RP2350 board to automatically send a boot payload after successfully exploiting an Apple device into **PWNED DFU mode**.

Unlike the original workflow, this project does not require `usbliter8ctl` or a computer after the process starts.

The RP2350 acts as a complete USB host:

1. Runs the USBLiter8 exploit.
2. Waits until the Apple device enters PWNED DFU mode.
3. Sends a pre-embedded `.boot` payload.
4. Executes the payload automatically.

The payload is stored directly inside the RP2350 firmware and embedded into the XIP flash using GNU assembler `.incbin`.

---

# How it works

```
Apple Device (DFU Mode)
          |
          v
RP2350 USB Host
          |
          v
USBLiter8 Exploit
          |
          v
PWNED DFU Mode
          |
          v
DFU_DNLOAD payload transfer
          |
          v
Zero-length DFU_DNLOAD
          |
          v
CUSTOM_BOOT request
          |
          v
DFU_ABORT
          |
          v
Payload execution
```

---

# Technical explanation

## 1. DFU mode

The Apple device starts in normal DFU mode.

At this point:

* SecureROM is running.
* No arbitrary code execution is available.
* Only Apple's DFU protocol is active.

The RP2350 communicates with the device through USB.

---

## 2. Exploitation

The RP2350 runs the USBLiter8 exploit.

The exploit targets Apple's SecureROM DFU implementation and allows execution control.

After a successful exploit, the device enters:

```
PWNED DFU
```

PWNED DFU allows custom DFU commands and arbitrary payload loading.

---

## 3. Payload transfer

After entering PWNED DFU, BootSurreal sends the embedded payload.

The transfer uses:

```
DFU_DNLOAD
```

DFU commands:

```
DFU_DNLOAD  = 1
DFU_ABORT   = 4
CUSTOM_BOOT = 8
```

The payload is split into chunks and transmitted over USB control transfers.

Example:

```
Payload chunk 0
        |
        v
   DFU_DNLOAD

Payload chunk 1
        |
        v
   DFU_DNLOAD

Payload chunk 2
        |
        v
   DFU_DNLOAD
```

After all bytes are transferred:

```
DFU_DNLOAD(length=0)
```

is sent to finish the upload.

---

## 4. Executing the payload

After the upload is complete:

```
CUSTOM_BOOT
```

is sent.

This tells the device to execute the uploaded payload.

Then:

```
DFU_ABORT
```

is sent to clean up the DFU state.

The device leaves DFU mode and starts executing the payload.

---

# Boot payload storage

BootSurreal embeds `.boot` files directly into the firmware.

Example structure:

```
bootfiles/
└── iPhone11,2/
    ├── iBSS.patch
    ├── 16.5.1/
    │   └── iBSS.boot
    └── marker.txt
```

Only `.boot` files are embedded into the firmware.

Patch files and marker files are kept only as references.

---

# Building uf2

Supported system:

```
Fedora
Arch (CachyOS, and etc supported too)
MacOS (both intel and Apple Silicon)
Debian, Ubuntu
```

Build:

Make sure boot files are in the folder named "bootfiles"

```bash
chmod +x build.sh
./build.sh
```


The build script automatically:

* Installs dependencies.
* Downloads Pico SDK.
* Downloads Arm GNU Toolchain.
* Downloads required sources.
* Builds the final UF2 firmware.

---

# Supported boards

```
waveshare_rp2350_usb_a
waveshare_rp2350_zero
pimoroni_tiny2350
pico2
adafruit_feather_rp2040
pico
```

RP2350 is recommended because it provides:

* Better USB host performance.
* Higher reliability.
* Better compatibility with newer devices.

---

# Current limitations

## Multiple payloads

If multiple `.boot` files exist:

```
bootfiles/
 ├── a.boot
 ├── b.boot
 └── c.boot
```

The firmware currently uses the first file alphabetically.

Automatic matching:

```
Device model
      +
iOS version
      |
      v
Correct payload
```

is not implemented yet.

---

## Patch handling

`iBSS.patch` files are not automatically applied.

The firmware expects:

```
*.boot
```

to already be the final payload.

---

# Simple explanation

Normally the workflow requires a computer:

```
Computer
    |
    |-- USBLiter8
    |
    |-- Exploit
    |
    |-- Send payload
    |
    v
 Apple Device
```

BootSurreal removes the computer:

```
RP2350
    |
    |-- USBLiter8
    |
    |-- Exploit
    |
    |-- Send payload
    |
    v
 Apple Device
```

The RP2350 contains everything required:

* USB host stack
* exploit code
* payload storage
* DFU sender
* boot logic

The entire process can run from a small standalone board.

---

# Architecture

```
                 RP2350 Firmware

+--------------------------------+
|                                |
|       USB Host Stack           |
|                                |
|       USBLiter8 Exploit        |
|                                |
|       PWNED DFU Handler        |
|                                |
|       Payload Sender            |
|                                |
|       Embedded boot payload     |
|                                |
+--------------------------------+

              |
              |
             USB

              |
              v

        Apple SecureROM

              |
              v

          PWNED DFU

              |
              v

        Payload Execution
```
