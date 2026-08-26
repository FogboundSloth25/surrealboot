#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEPS="$ROOT/.deps"
BUILD="$ROOT/build"
DIST="$ROOT/dist"

USBLITER8_REPO="${USBLITER8_REPO:-https://github.com/FogboundSloth25/usbliter8bootsurreal.git}"
USBLITER8_REF="${USBLITER8_REF:-afe8b5c8998fce63e76c0b2a88c606c61e2950c7}"

PICO_SDK_VERSION="${PICO_SDK_VERSION:-2.2.0}"

PICO_SDK="$DEPS/pico-sdk-$PICO_SDK_VERSION"
UPSTREAM_CACHE="$DEPS/usbliter8bootsurreal.git"
SRC="$DEPS/build-source"

BOARDS=(
    waveshare_rp2350_usb_a
    waveshare_rp2350_zero
    pimoroni_tiny2350
    pico2
    adafruit_feather_rp2040
    pico
)

die() {
    echo
    echo "ERROR: $*" >&2
    exit 1
}

info() {
    echo
    echo "============================================================"
    echo "$*"
    echo "============================================================"
}

trap 'echo; echo "ERROR: build failed at line $LINENO"; exit 1' ERR

# ============================================================
# Fedora dependencies
# ============================================================

info "Checking Fedora build dependencies"

if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    DNF=(dnf)
else
    command -v sudo >/dev/null 2>&1 || die "sudo is required"
    DNF=(sudo dnf)
fi

command -v dnf >/dev/null 2>&1 || die "dnf was not found"

"${DNF[@]}" install -y \
    cmake \
    ninja-build \
    make \
    gcc \
    gcc-c++ \
    git \
    curl \
    wget \
    tar \
    xz \
    python3 \
    libusb1-devel \
    arm-none-eabi-gcc-cs \
    arm-none-eabi-binutils-cs \
    arm-none-eabi-newlib

mkdir -p "$DEPS" "$DIST"

# ============================================================
# Board selection
# ============================================================

info "Selecting target board"

if [[ -n "${PICO_BOARD:-}" ]]; then
    printf '%s\n' "${BOARDS[@]}" | grep -Fxq "$PICO_BOARD" \
        || die "Unsupported PICO_BOARD: $PICO_BOARD"
else
    echo "Choose target board:"
    select BOARD in "${BOARDS[@]}"; do
        if [[ -n "${BOARD:-}" ]]; then
            PICO_BOARD="$BOARD"
            break
        fi
    done
fi

echo "Selected board: $PICO_BOARD"

# ============================================================
# ARM toolchain
#
# IMPORTANT:
# We deliberately use Fedora's arm-none-eabi toolchain.
# No developer.arm.com download is needed.
# ============================================================

info "Locating ARM GNU toolchain"

ARM_GCC="$(command -v arm-none-eabi-gcc || true)"
ARM_GXX="$(command -v arm-none-eabi-g++ || true)"
ARM_OBJCOPY="$(command -v arm-none-eabi-objcopy || true)"
ARM_SIZE="$(command -v arm-none-eabi-size || true)"

[[ -n "$ARM_GCC" ]] || die "arm-none-eabi-gcc not found"
[[ -n "$ARM_GXX" ]] || die "arm-none-eabi-g++ not found"
[[ -n "$ARM_OBJCOPY" ]] || die "arm-none-eabi-objcopy not found"

ARM_BIN_DIR="$(dirname "$(realpath "$ARM_GCC")")"

echo "ARM GCC:"
"$ARM_GCC" --version | head -n 1

echo "ARM toolchain directory:"
echo "  $ARM_BIN_DIR"

export PICO_TOOLCHAIN_PATH="$ARM_BIN_DIR"

# ============================================================
# Pico SDK
#
# MUST include submodules.
# This fixes TinyUSB missing.
# ============================================================

info "Preparing Pico SDK $PICO_SDK_VERSION"

if [[ ! -d "$PICO_SDK/.git" ]]; then
    echo "Pico SDK not found. Cloning with recursive submodules..."

    rm -rf "$PICO_SDK"

    git clone \
        --depth 1 \
        --branch "$PICO_SDK_VERSION" \
        --recurse-submodules \
        https://github.com/raspberrypi/pico-sdk.git \
        "$PICO_SDK"
else
    echo "Using cached Pico SDK:"
    echo "  $PICO_SDK"

    pushd "$PICO_SDK" >/dev/null

    popd >/dev/null
fi

# Verify TinyUSB really exists.
TINYUSB_CMAKE="$PICO_SDK/src/rp2_common/tinyusb/CMakeLists.txt"
TINYUSB_SRC="$PICO_SDK/lib/tinyusb/src/tusb.c"

if [[ ! -f "$TINYUSB_CMAKE" ]]; then
    die "Pico SDK TinyUSB CMake integration is missing"
fi

if [[ ! -f "$TINYUSB_SRC" ]]; then
    echo
    echo "TinyUSB source is missing. Repairing submodules..."

    pushd "$PICO_SDK" >/dev/null
    git submodule sync --recursive
    git submodule update --init --recursive
    popd >/dev/null
fi

[[ -f "$TINYUSB_SRC" ]] \
    || die "TinyUSB is still missing after submodule repair"

export PICO_SDK_PATH="$PICO_SDK"

echo
echo "PICO_SDK_PATH=$PICO_SDK"
echo "PICO_TOOLCHAIN_PATH=$PICO_TOOLCHAIN_PATH"

# ============================================================
# USBLiter8 upstream
# ============================================================

info "Preparing USBLiter8 upstream source"

if [[ ! -d "$UPSTREAM_CACHE/.git" ]]; then
    echo "Cloning upstream source from:"
    echo "  $USBLITER8_REPO"

    git clone \
        --filter=blob:none \
        --no-checkout \
        "$USBLITER8_REPO" \
        "$UPSTREAM_CACHE"
else
    echo "Using cached upstream source:"
    echo "  $UPSTREAM_CACHE"
fi

pushd "$UPSTREAM_CACHE" >/dev/null

if ! git cat-file -e "$USBLITER8_REF^{commit}" 2>/dev/null; then
    echo "Fetching pinned commit..."
    git fetch origin "$USBLITER8_REF"
fi

git checkout --force --detach "$USBLITER8_REF"

popd >/dev/null

echo
echo "USBLiter8 HEAD:"
git -C "$UPSTREAM_CACHE" rev-parse HEAD

[[ "$(git -C "$UPSTREAM_CACHE" rev-parse HEAD)" == "$USBLITER8_REF" ]] \
    || die "USBLiter8 commit mismatch"

# ============================================================
# Prepare source tree
# ============================================================

info "Preparing build source tree"

rm -rf "$SRC"
mkdir -p "$SRC"

cp -a "$UPSTREAM_CACHE/." "$SRC/"

# Directly continue to payload when device is already PWNED.
python3 - "$SRC/exploit.c" <<'PYEXPLOIT'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()

old = """    if (pwnd) {
        INFO("already PWNED!");
        return -2;
    }
"""

new = """    if (pwnd) {
        INFO("already PWNED! Skipping exploit and proceeding directly to payload.");
        return 1;
    }
"""

if old not in s:
    raise SystemExit("Could not find PWNED block in exploit.c")

p.write_text(s.replace(old, new, 1))
PYEXPLOIT


# Replace/add our custom files.
cp "$ROOT/CMakeLists.txt" "$SRC/CMakeLists.txt"
cp "$ROOT/surreal_boot.c" "$SRC/surreal_boot.c"
cp "$ROOT/surreal_boot.h" "$SRC/surreal_boot.h"

# ============================================================
# Patch LED states
#
# Existing upstream LED states:
#   BOOTING
#   IDLE
#   RUNNING
#   SUCCESS
#   ERROR
#
# Add:
#   BOOT_PAYLOAD
#   BOOT_SUCCESS
# ============================================================

info "Patching LED states"

python3 - "$SRC/led.h" "$SRC/led.c" <<'PYLED'
from pathlib import Path
import sys

header = Path(sys.argv[1])
source = Path(sys.argv[2])

h = header.read_text()

if "LED_STATE_BOOT_PAYLOAD" not in h:
    old = """    LED_STATE_SUCCESS,
    LED_STATE_ERROR
"""
    new = """    LED_STATE_SUCCESS,
    LED_STATE_ERROR,
    LED_STATE_BOOT_PAYLOAD,
    LED_STATE_BOOT_SUCCESS
"""
    if old not in h:
        raise SystemExit("Could not find LED state enum")
    h = h.replace(old, new, 1)

header.write_text(h)

c = source.read_text()

if "case LED_STATE_BOOT_PAYLOAD" not in c:
    old = """        case LED_STATE_ERROR: {
            led_set_color(RED);
            led_set_blinking(0);
            break;
        }
"""

    new = """        case LED_STATE_ERROR: {
            led_set_color(RED);
            led_set_blinking(0);
            break;
        }

        case LED_STATE_BOOT_PAYLOAD: {
            led_set_color(GREEN);
            led_set_blinking(250);
            break;
        }

        case LED_STATE_BOOT_SUCCESS: {
            led_set_color(GREEN);
            led_set_blinking(0);
            break;
        }
"""

    if old not in c:
        raise SystemExit("Could not find LED_STATE_ERROR block")

    c = c.replace(old, new, 1)

source.write_text(c)
PYLED

# ============================================================
# Patch upstream main.c
#
# After exploit success:
#   LED green
#   send embedded bootfile
#   blink green while sending
#   steady green after success
# ============================================================

info "Patching USBLiter8 main runtime"

python3 - "$SRC/main.c" <<'PYMAIN'
from pathlib import Path
import sys

path = Path(sys.argv[1])
s = path.read_text()

if '#include "surreal_boot.h"' not in s:
    marker = '#include "log.h"'
    if marker not in s:
        raise SystemExit('Could not find log.h include')
    s = s.replace(
        marker,
        marker + '\n#include "surreal_boot.h"',
        1
    )

old = """        /* it all went well then */
        break;
"""

new = """        /* exploit succeeded; now send embedded boot payload */
        led_set_state(LED_STATE_SUCCESS);

        printf("\\n");
        printf("[BOOT] exploit succeeded; starting embedded boot payload transfer\\n");

        led_set_state(LED_STATE_BOOT_PAYLOAD);

        int boot_rc = surreal_boot_run();

        if (boot_rc != 0) {
            printf("[BOOT] payload transfer FAILED rc=%d\\n", boot_rc);
            led_set_state(LED_STATE_ERROR);
            fatal_failure();
        }

        led_set_state(LED_STATE_BOOT_SUCCESS);

        printf("[BOOT] payload transfer SUCCESS\\n");
        printf("[BOOT] device should now transition away from DFU\\n");

        break;
"""

if old not in s:
    raise SystemExit(
        "Could not find exploit success point in main.c"
    )

s = s.replace(old, new, 1)

path.write_text(s)
PYMAIN

# ============================================================
# Validate bootfiles
# ============================================================

info "Checking embedded bootfiles"

BOOTFILES_DIR="$ROOT/bootfiles"

[[ -d "$BOOTFILES_DIR" ]] \
    || die "Missing ./bootfiles directory"

mapfile -t BOOTFILES < <(
    find "$BOOTFILES_DIR" \
        -type f \
        -name "*.boot" \
        -print \
        | sort
)

if [[ "${#BOOTFILES[@]}" -eq 0 ]]; then
    die "No .boot files found in ./bootfiles"
fi

TOTAL_BYTES=0

echo "Found ${#BOOTFILES[@]} boot payload(s):"

for file in "${BOOTFILES[@]}"; do
    size="$(stat -c '%s' "$file")"
    TOTAL_BYTES=$((TOTAL_BYTES + size))

    echo "  ${file#"$ROOT/bootfiles"/} $size bytes"
done

echo
echo "Total boot payload bytes: $TOTAL_BYTES"

# ============================================================
# Generate embedded payloads
#
# IMPORTANT:
# embed_bootfiles.py requires THREE arguments:
#
#   bootfiles directory
#   output .c
#   output .h
# ============================================================

info "Generating embedded boot payload"

GEN_DIR="$SRC/generated"

rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"

EMBED_SCRIPT="$ROOT/tools/embed_bootfiles.py"

[[ -f "$EMBED_SCRIPT" ]] \
    || die "Missing tools/embed_bootfiles.py"

python3 "$EMBED_SCRIPT" \
    "$ROOT/bootfiles" \
    "$GEN_DIR/bootfiles_data.c" \
    "$GEN_DIR/bootfiles_data.h"

[[ -s "$GEN_DIR/bootfiles_data.c" ]] \
    || die "Generated bootfiles_data.c is empty"

[[ -s "$GEN_DIR/bootfiles_data.h" ]] \
    || die "Generated bootfiles_data.h is empty"

echo
echo "Generated:"
echo "  $GEN_DIR/bootfiles_data.c"
echo "  $GEN_DIR/bootfiles_data.h"

# ============================================================
# Build
# ============================================================

info "Configuring CMake"

rm -rf "$BUILD"

cmake \
    -S "$SRC" \
    -B "$BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DPICO_BOARD="$PICO_BOARD" \
    -DPICO_SDK_PATH="$PICO_SDK_PATH" \
    -DPICO_TOOLCHAIN_PATH="$PICO_TOOLCHAIN_PATH"

info "Building"

cmake --build "$BUILD" --parallel "$(nproc)"

# ============================================================
# Verify result
# ============================================================

UF2="$BUILD/usbliter8.uf2"
ELF="$BUILD/usbliter8.elf"

[[ -f "$UF2" ]] \
    || die "Build finished but usbliter8.uf2 was not produced"

[[ -f "$ELF" ]] \
    || die "Build finished but usbliter8.elf was not produced"

mkdir -p "$DIST"

OUTPUT="$DIST/usbliter8-bootsurreal-$PICO_BOARD.uf2"

cp -f "$UF2" "$OUTPUT"

# ============================================================
# Size report
# ============================================================

echo

if [[ -n "$ARM_SIZE" ]]; then
    "$ARM_SIZE" "$ELF"
fi

echo
echo "UF2:"
ls -lh "$OUTPUT"

echo
echo "============================================================"
echo "BUILD SUCCESS"
echo "============================================================"
echo
echo "Board:"
echo "  $PICO_BOARD"
echo
echo "UF2:"
echo "  $OUTPUT"
echo
echo "Embedded payloads:"
for file in "${BOOTFILES[@]}"; do
    echo "  ${file#"$ROOT/bootfiles"/}"
done
echo
echo "DONE: $OUTPUT"
