# USBLiter8 BootSurreal

## Overview

**USBLiter8 BootSurreal** is a standalone extension for `usbliter8` that allows an RP2350 board to automatically send a iBSS payload after successfully exploiting an Apple device into **PWNED DFU mode**.

Unlike the original workflow, this project does not require `usbliter8ctl` or a computer after the process starts.

The RP2350 acts as a complete USB host:

1. Runs the USBLiter8 exploit.
2. Waits until the Apple device enters PWNED DFU mode.
3. Sends a pre-embedded `iBSS.boot` payload.
4. Executes the payload automatically.
5. Boots your surrealra1n downgraded device.

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
LZ4 decompress embedded .boot
          |
          v
DFU_DNLOAD payload transfer (0x80)
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

After entering PWNED DFU, BootSurreal sends the first embedded `*.boot` file from `ibss/`.

The file is stored in firmware as LZ4-compressed 64 KiB blocks. Each block is decompressed on the RP2350, then sent over USB.

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

Decompressed payload bytes are split into USB control transfers of `0x80` bytes (falls back to `0x40` if a transfer fails).

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

This requests that the device execute the uploaded payload.

Then:

```
DFU_ABORT
```

is sent to clean up the DFU state.

The firmware does not wait for a boot confirmation. If the transfer finished, the device is expected to leave DFU and run the payload.

---

# Boot payload storage

BootSurreal embeds `*.boot` files from `ibss/` into the firmware at build time (`tools/embed_bootfiles.py`, LZ4 blocks). At runtime only the first file is sent.

Example structure:

```
ibss/
└── iBSS.boot
```

Supported boards share one QSPI flash between firmware and the embedded payload (2 MB on Waveshare RP2350-USB-A, 4 MB on Pico 2). The `.boot` file is compiled into the UF2, not stored on a separate chip, so only one iBSS fits.

The `.uf2` file is larger than the image on flash (UF2 stores 256 payload bytes per 512-byte block). Check used flash size from the build output, not from the UF2 file size.
---

# Building uf2:

Supported systems:

```
Fedora
Arch (CachyOS, and etc supported too)
MacOS (both intel and Apple Silicon)
Debian, Ubuntu
```

Build:


* Make sure your device is already downgraded and your iBSS.boot from surrealra1n/boot folder in the folder named "ibss"

```bash
./build.sh
```
After compiling, uf2 file will be in "dist" folder.

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
waveshare_rp2350_zero (untested)
pimoroni_tiny2350 (untested)
pico2 (untested)
```

# Current limitations

* Can send only 0x80 per packet, so full payload transfer will take like 1 minute (or more, depends on your iBSS file size.).
* Supported boards have 2–4 MB of flash (2 MB on Waveshare RP2350-USB-A).
  One embedded `iBSS.boot` already fills most of that, so only a single
  payload is supported. Put exactly one `*.boot` file in `ibss/`.
