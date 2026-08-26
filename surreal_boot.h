#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the embedded boot-payload transfer.
 *
 * Returns:
 *   0  - payload was successfully sent
 *  <0 - error
 */
int surreal_boot_run(void);

#ifdef __cplusplus
}
#endif
