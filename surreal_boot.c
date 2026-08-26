#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "pico/time.h"

#include "bus.h"
#include "usb.h"
#include "log.h"
#include "led.h"
#include "surreal_boot.h"
#include "bootfiles_data.h"

#define DFU_DNLOAD        1
#define DFU_ABORT         4
#define CUSTOM_BOOT       8

/*
 * Transfer size strategy for PWNED DFU on Apple A10 (CPID:8020).
 *
 * The RP2350 PIO USB host stack cannot reliably complete OUT control
 * transfers with a DATA phase larger than ~0x80 bytes against the
 * Apple SecureROM DFU implementation post-exploit.
 *
 * Observed behaviour:
 *   0x400  → rc=-2 (timeout)
 *   0x200  → rc=-1 (pipe stall, especially after a bus reset)
 *   0x80   → OK  (confirmed stable on this device)
 *   0x40   → OK  (confirmed on earlier test)
 *
 * INITIAL is set to 0x200 so that if the fixed retry logic (which now
 * handles rc=-1 and adds a post-reset delay) makes larger chunks work,
 * we get a 4x speed gain for free.  If 0x200 still fails the adaptive
 * fallback will settle at the proven 0x80 automatically.
 *
 * 0x200 was tested and fails (PIO USB can handle at most 2×64-byte
 * packets = 0x80 in the DATA phase of an OUT control transfer).
 * Set to 0x80 directly to skip the wasted timeout+reset probe cycles.
 */
#define INITIAL_TRANSFER_SIZE 0x80
#define MIN_TRANSFER_SIZE     0x40

/*
 * Print a progress line every this many bytes sent.
 * Keeps the hot path silent (logging was the #1 slowdown:
 * 16 K chunks × 3 printf ≈ 48 K USB-CDC packets ≈ 48–96 s overhead).
 */
#define PROGRESS_INTERVAL_BYTES (256u * 1024u)

#define CTRL_TIMEOUT_MS   500   /* 0x80 completes in <20ms; 500ms = fast failure detection */

#define MAX_BLOCK         (64 * 1024)

static uint8_t boot_scratch[MAX_BLOCK] __attribute__((aligned(4)));

static uint32_t working_transfer_size = INITIAL_TRANSFER_SIZE;

/*
 * ============================================================
 * LZ4
 * ============================================================
 */

static int read_len(
    const uint8_t *src,
    uint32_t size,
    uint32_t *pos,
    uint32_t *len
) {
    uint32_t value = *len;

    if (value != 15u) {
        *len = value;
        return 0;
    }

    while (true) {
        if (*pos >= size) {
            return -1;
        }

        uint8_t x = src[(*pos)++];

        value += x;

        if (x != 255u) {
            break;
        }
    }

    *len = value;

    return 0;
}

static int lz4_decompress_block(
    const uint8_t *src,
    uint32_t src_size,
    uint8_t *dst,
    uint32_t dst_capacity,
    uint32_t expected_size
) {
    uint32_t sp = 0;
    uint32_t dp = 0;

    while (sp < src_size) {

        uint8_t token = src[sp++];

        uint32_t literals = token >> 4;

        if (read_len(
                src,
                src_size,
                &sp,
                &literals
            ) != 0) {
            return -1;
        }

        if (literals > src_size - sp) {
            return -1;
        }

        if (literals > dst_capacity - dp) {
            return -1;
        }

        for (uint32_t i = 0; i < literals; ++i) {
            dst[dp++] = src[sp++];
        }

        if (sp == src_size) {
            break;
        }

        if (sp + 2 > src_size) {
            return -1;
        }

        uint32_t offset =
            (uint32_t)src[sp] |
            ((uint32_t)src[sp + 1] << 8);

        sp += 2;

        if (offset == 0 || offset > dp) {
            return -1;
        }

        uint32_t match = (token & 0x0F) + 4u;

        if ((token & 0x0F) == 15u) {

            uint32_t extra = 15u;

            if (read_len(
                    src,
                    src_size,
                    &sp,
                    &extra
                ) != 0) {
                return -1;
            }

            match = extra + 4u;
        }

        if (match > dst_capacity - dp) {
            return -1;
        }

        uint32_t from = dp - offset;

        for (uint32_t i = 0; i < match; ++i) {
            dst[dp++] = dst[from + i];
        }
    }

    return dp == expected_size ? 0 : -1;
}

/*
 * ============================================================
 * DFU
 * ============================================================
 */

static int dfu_download_chunk(
    bus_t *b,
    const uint8_t *buf,
    uint16_t len,
    uint32_t offset,
    uint32_t ordinal
) {
    struct usb_setup_req_header {
        uint8_t  bmRequestType;
        uint8_t  bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } __attribute__((packed));

    struct usb_setup_req_header req = {
        .bmRequestType = 0x21,
        .bRequest      = DFU_DNLOAD,
        .wValue        = (uint16_t)(ordinal - 1),
        .wIndex        = 0,
        .wLength       = len,
    };

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        (uint8_t *)buf,
        len,
        false,
        CTRL_TIMEOUT_MS
    );

    if (rc != 0) {
        INFO("[DFU] FAILED rc=%d offset=0x%08lx len=0x%04x",
             rc, (unsigned long)offset, (unsigned)len);
    }

    return rc;
}

/*
 * Try current size.
 *
 * On timeout:
 *
 *   reset USB bus
 *   reopen EP0
 *   reduce transfer size
 *   retry the SAME payload portion
 *
 * This allows us to find the largest reliable DFU transfer
 * automatically.
 */
static int dfu_download_adaptive(
    bus_t *b,
    const uint8_t *buf,
    uint32_t len,
    uint32_t offset,
    uint32_t ordinal
) {
    while (true) {

        uint16_t send_len =
            len > working_transfer_size
            ? (uint16_t)working_transfer_size
            : (uint16_t)len;

        int rc = dfu_download_chunk(
            b,
            buf,
            send_len,
            offset,
            ordinal
        );

        if (rc == 0) {
            return send_len;
        }

        /*
         * Retry on both known-transient error codes:
         *
         *   rc=-2  timeout: device did not ACK within CTRL_TIMEOUT_MS.
         *          Typical cause: chunk too large for the device's
         *          post-exploit DFU buffer or the PIO USB host's DATA
         *          phase handling.
         *
         *   rc=-1  pipe stall: device returned STALL or the host saw a
         *          framing error.  Commonly happens on the first transfer
         *          after a USB bus reset because the device has not fully
         *          re-enumerated EP0.  Reducing the chunk size and waiting
         *          after the reset resolves this.
         *
         * Any other error code is fatal (bad USB state we cannot recover
         * from by changing transfer size).
         */
        if (rc != -2 && rc != -1) {
            INFO("[DFU] unrecoverable error rc=%d", rc);
            return rc;
        }

        if (rc == -2) {
            INFO(
                "[DFU] timeout at chunk size 0x%lx",
                (unsigned long)working_transfer_size
            );
        } else {
            INFO(
                "[DFU] pipe stall at chunk size 0x%lx",
                (unsigned long)working_transfer_size
            );
        }

        if (working_transfer_size <= MIN_TRANSFER_SIZE) {
            INFO(
                "[DFU] minimum transfer size 0x%lx also failed (rc=%d)",
                (unsigned long)working_transfer_size,
                rc
            );

            return rc;
        }

        /*
         * Reset the DFU endpoint before retrying.
         *
         * After the reset, wait 150 ms before the next transfer.
         * Apple SecureROM takes time to re-assert EP0 after a bus
         * reset; without the delay the very first control transfer
         * returns rc=-1 regardless of size.
         */
        INFO("[DFU] resetting USB bus before retry");

        usb_bus_reset_open_ep0();

        sleep_ms(150);

        working_transfer_size >>= 1;

        if (working_transfer_size < MIN_TRANSFER_SIZE) {
            working_transfer_size = MIN_TRANSFER_SIZE;
        }

        INFO(
            "[DFU] fallback transfer size = 0x%lx",
            (unsigned long)working_transfer_size
        );
    }
}

static int dfu_download_finish(bus_t *b)
{
    struct usb_setup_req_header {
        uint8_t  bmRequestType;
        uint8_t  bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } __attribute__((packed));

    struct usb_setup_req_header req = {
        .bmRequestType = 0x21,
        .bRequest      = DFU_DNLOAD,
        .wValue        = 0,
        .wIndex        = 0,
        .wLength       = 0,
    };

    INFO("[DFU] sending zero-length termination");

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        NULL,
        0,
        false,
        CTRL_TIMEOUT_MS
    );

    INFO(
        "[DFU] termination rc=%d",
        rc
    );

    return rc;
}

static int custom_request(
    bus_t *b,
    uint8_t request
) {
    struct usb_setup_req_header {
        uint8_t  bmRequestType;
        uint8_t  bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } __attribute__((packed));

    struct usb_setup_req_header req = {
        .bmRequestType = 0x21,
        .bRequest      = request,
        .wValue        = 0,
        .wIndex        = 0,
        .wLength       = 0,
    };

    INFO(
        "[DFU] control request %u",
        (unsigned)request
    );

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        NULL,
        0,
        false,
        CTRL_TIMEOUT_MS
    );

    INFO(
        "[DFU] request %u rc=%d",
        (unsigned)request,
        rc
    );

    return rc;
}

/*
 * ============================================================
 * Boot payload
 * ============================================================
 */

static int boot_internal(
    bus_t *b,
    void *ctx
) {
    (void)ctx;

#if BOOTFILE_COUNT == 0

    INFO("[BOOT] no embedded bootfiles");
    return -1;

#else

    const struct bootfile_desc *boot = &bootfiles[0];

    working_transfer_size = INITIAL_TRANSFER_SIZE;

    uint64_t t_start      = time_us_64();
    uint32_t last_log_pos = 0;

    INFO("");
    INFO("[BOOT] ========================================");
    INFO("[BOOT] Embedded boot payload");
    INFO("[BOOT] ========================================");
    INFO("[BOOT] name: %s", boot->name);
    INFO(
        "[BOOT] uncompressed size: %lu bytes",
        (unsigned long)boot->uncompressed_size
    );
    INFO(
        "[BOOT] compressed blocks: %u",
        (unsigned)boot->chunk_count
    );
    INFO(
        "[BOOT] initial DFU chunk size: 0x%lx",
        (unsigned long)working_transfer_size
    );
    INFO("[BOOT] ========================================");


    size_t sent = 0;
    uint32_t ordinal = 0;

    for (
        uint16_t block_index = 0;
        block_index < boot->chunk_count;
        ++block_index
    ) {

        const struct bootfile_chunk *chunk =
            &boot->chunks[block_index];

        uint32_t packed_size =
            (uint32_t)(chunk->end - chunk->start);

        INFO(
            "[BOOT] block %u/%u compressed=%lu uncompressed=%lu",
            (unsigned)(block_index + 1),
            (unsigned)boot->chunk_count,
            (unsigned long)packed_size,
            (unsigned long)chunk->uncompressed_size
        );

        if (
            packed_size != chunk->compressed_size ||
            chunk->uncompressed_size > MAX_BLOCK
        ) {
            INFO("[BOOT] invalid block metadata");
            return -1;
        }

        INFO(
            "[LZ4] decoding block %u",
            (unsigned)block_index
        );

        int rc = lz4_decompress_block(
            chunk->start,
            chunk->compressed_size,
            boot_scratch,
            sizeof(boot_scratch),
            chunk->uncompressed_size
        );

        if (rc != 0) {
            INFO(
                "[LZ4] decode FAILED block=%u rc=%d",
                (unsigned)block_index,
                rc
            );

            return -1;
        }

        INFO(
            "[LZ4] decode OK block=%u -> %lu bytes",
            (unsigned)block_index,
            (unsigned long)chunk->uncompressed_size
        );

        uint8_t *ptr = boot_scratch;
        uint32_t left = chunk->uncompressed_size;

        while (left > 0) {

            uint32_t chunk_size =
                left > working_transfer_size
                ? working_transfer_size
                : left;

            ++ordinal;

            int sent_now = dfu_download_adaptive(
                b,
                ptr,
                chunk_size,
                (uint32_t)sent,
                ordinal
            );

            if (sent_now < 0) {
                INFO(
                    "[BOOT] DFU transfer failed offset=0x%08lx rc=%d",
                    (unsigned long)sent,
                    sent_now
                );

                return sent_now;
            }

            ptr += sent_now;
            left -= (uint32_t)sent_now;
            sent += (uint32_t)sent_now;

            if (sent - last_log_pos >= PROGRESS_INTERVAL_BYTES) {
                uint64_t elapsed_us = time_us_64() - t_start;
                uint32_t kbps = elapsed_us > 0
                    ? (uint32_t)((uint64_t)sent * 1000u / (elapsed_us / 1000u))
                    : 0u;
                INFO(
                    "[BOOT] %lu / %lu bytes  (%u KBps  sz=0x%lx)",
                    (unsigned long)sent,
                    (unsigned long)boot->uncompressed_size,
                    (unsigned)kbps,
                    (unsigned long)working_transfer_size
                );
                last_log_pos = sent;
            }
        }

        INFO(
            "[BOOT] block %u complete; sent=%lu/%lu",
            (unsigned)(block_index + 1),
            (unsigned long)sent,
            (unsigned long)boot->uncompressed_size
        );
    }

    if (sent != boot->uncompressed_size) {
        INFO(
            "[BOOT] size mismatch sent=%lu expected=%lu",
            (unsigned long)sent,
            (unsigned long)boot->uncompressed_size
        );

        return -1;
    }

    {
        uint64_t total_us  = time_us_64() - t_start;
        uint32_t elapsed_ms = (uint32_t)(total_us / 1000u);
        uint32_t kbps = total_us > 0
            ? (uint32_t)((uint64_t)sent * 1000u / (total_us / 1000u))
            : 0u;
        INFO(
            "[BOOT] ALL PAYLOAD BYTES SENT: %lu bytes in %lu ms  (%u KBps)",
            (unsigned long)sent,
            (unsigned long)elapsed_ms,
            (unsigned)kbps
        );
    }

    INFO(
        "[BOOT] final transfer size: 0x%lx",
        (unsigned long)working_transfer_size
    );


    /*
     * All payload bytes are already transferred at this point.
     * The device may start leaving DFU immediately after the
     * final control requests.
     */
    int finish_rc = dfu_download_finish(b);

    INFO(
        "[BOOT] DFU termination rc=%d",
        finish_rc
    );

    int boot_rc = custom_request(
        b,
        CUSTOM_BOOT
    );

    INFO(
        "[BOOT] CUSTOM_BOOT rc=%d",
        boot_rc
    );

    int abort_rc = custom_request(
        b,
        DFU_ABORT
    );

    INFO(
        "[BOOT] DFU_ABORT rc=%d",
        abort_rc
    );

    uint64_t total_us = time_us_64() - t_start;
    uint32_t elapsed_sec = (uint32_t)(total_us / 1000000u);

    INFO("");
    INFO("[SUCCESS] ========================================");
    INFO("[SUCCESS] PAYLOAD DATA TRANSFER COMPLETE");
    INFO("[SUCCESS] ALL BYTES DELIVERED SUCCESSFULLY");
    INFO("[SUCCESS] FINAL TRANSFER SIZE: 0x%lx",
        (unsigned long)working_transfer_size);
    INFO("[SUCCESS] ========================================");
    INFO("[SUCCESS] Took %lu seconds to transfer",
        (unsigned long)elapsed_sec);
    INFO("[SUCCESS] Device should boot now!");

    return 0;

#endif
}

int surreal_boot_run(void)
{
#if BOOTFILE_COUNT == 0

    INFO("[BOOT] no payload");
    return -1;

#else

    /*
     * LED is handled by main.c:
     *
     *   green blinking while this function runs
     *   green steady after return 0
     *   red only on real error
     */
    INFO("[BOOT] entering USB-host payload transfer stage");

    return usb_bus_execute(
        boot_internal,
        NULL,
        0
    );

#endif
}
