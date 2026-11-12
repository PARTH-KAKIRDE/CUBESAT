/*
 * icd_node - common command handling shared by every slave (ADCS, TT&C, dummy).
 *
 * Implements PING, GET_VERSION, GET_STATUS, GET_TELEMETRY, SET_MODE, the mode
 * transition rules, the status flags (RESET_OCCURRED, NO_MASTER_CONTACT,
 * FAULT_ACTIVE) and sticky fault bits. Subsystem-specific behaviour comes in
 * through icd_node_app_t callbacks.
 *
 * Fault bits are sticky: a fault seen even briefly stays set in fault_flags
 * until the OBC has read it with GET_STATUS and the condition has gone away.
 * FAULT_ACTIVE means at least one fault is present right now.
 */
#ifndef ICD_NODE_H
#define ICD_NODE_H

#include <stdbool.h>
#include <stdint.h>
#include "icd_link.h"

typedef struct {
    uint8_t fw_major, fw_minor, fw_patch;
    void *ctx;
    /* Faults present right now (ICD fault_flags bits). May be NULL. */
    uint16_t (*live_faults)(void *ctx);
    /* Extra status flags, e.g. ICD_SF_GROUND_PKT_PENDING. May be NULL. */
    uint8_t (*extra_flags)(void *ctx);
    /* Mode change request already checked against the ICD rules.
     * Return ICD_ST_OK to accept, or an error (e.g. HW_FAULT) to refuse.
     * A request for SAFE must always be accepted. May be NULL. */
    uint8_t (*on_set_mode)(void *ctx, uint8_t from, uint8_t to);
    /* Fill one telemetry page. Return ICD_ST_OK or ICD_ST_BAD_PARAM. */
    uint8_t (*get_telemetry)(void *ctx, uint8_t page, uint8_t *out, uint8_t *len);
    /* Subsystem command. Return ICD_ST_UNKNOWN_CMD if not handled. May be NULL. */
    uint8_t (*app_command)(void *ctx, uint8_t cmd, const uint8_t *req, uint8_t req_len,
                           uint8_t *resp, uint8_t *resp_len);
} icd_node_app_t;

typedef struct {
    icd_link_t link;
    const icd_node_app_t *app;
    uint8_t mode;
    uint8_t mode_reason;
    uint16_t sticky_faults;
    bool reset_flag;
    bool no_master_latched;
    bool master_lost;           /* live: no valid transaction for 30 s */
    uint32_t now_ms;
    uint32_t last_contact_ms;
} icd_node_t;

void icd_node_init(icd_node_t *n, const icd_node_app_t *app, uint32_t now_ms);

/* Main loop: process any pending command and update timers. */
void icd_node_poll(icd_node_t *n, uint32_t now_ms);

/* Autonomous mode change by the slave itself (e.g. drop to SAFE on a fault). */
void icd_node_set_mode(icd_node_t *n, uint8_t mode, uint8_t reason);
static inline uint8_t icd_node_mode(const icd_node_t *n) { return n->mode; }

/* Record a fault (sticky until reported). */
void icd_node_raise_fault(icd_node_t *n, uint16_t bits);

/* Fill an 8-byte GET_STATUS payload without clearing anything (for beacons). */
void icd_node_peek_status(icd_node_t *n, uint8_t *out);

/* True if no valid transaction for ICD_NO_MASTER_TIMEOUT_MS. */
static inline bool icd_node_master_lost(const icd_node_t *n) { return n->master_lost; }

#endif
