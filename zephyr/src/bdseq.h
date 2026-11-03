/*
 * Persistent Sparkplug birth/death sequence number (SPB-3).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_BDSEQ_H_
#define GW_BDSEQ_H_

#include <stdint.h>

/** Load the stored value from the settings subsystem (0 on first boot). */
int bdseq_init(void);

/**
 * bdSeq for a new connection attempt: returns the stored value and persists
 * its successor (mod 256) before the attempt is made, so a reset during the
 * attempt can never reuse a value already announced in a will message.
 */
uint8_t bdseq_next(void);

#endif /* GW_BDSEQ_H_ */
