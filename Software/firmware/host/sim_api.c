#include "sim_api.h"

#include <string.h>

#include "adcs_app.h"
#include "cubesat_icd.h"
#include "icd_port.h"
#include "sim_internal.h"
#include "ttc_app.h"

static adcs_t g_adcs;
static ttc_t g_ttc;
static uint32_t g_now;
static int g_present_adcs = 1, g_present_ttc = 1;

uint32_t sim_time_ms(void) { return g_now; }

/* Single-threaded simulation: "interrupts" never pre-empt the main loop. */
uint32_t icd_port_lock(void) { return 0; }
void icd_port_unlock(uint32_t s) { (void)s; }

SIM_API void sim_init(uint32_t seed)
{
    g_now = 1000; /* t = 0 would make "last event" timestamps ambiguous */
    g_present_adcs = g_present_ttc = 1;
    adcs_sim_reset(seed);
    ttc_sim_reset();
    adcs_init(&g_adcs, g_now);
    ttc_init(&g_ttc, g_now);
}

SIM_API void sim_run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        g_now++;
        adcs_sim_physics_step(1e-3);
        ttc_sim_step();
        adcs_step(&g_adcs, g_now);
        ttc_step(&g_ttc, g_now);
    }
}

SIM_API uint32_t sim_now(void) { return g_now; }

static icd_link_t *link_for(uint8_t addr)
{
    if (addr == ICD_ADDR_ADCS && g_present_adcs) return &g_adcs.node.link;
    if (addr == ICD_ADDR_TTC && g_present_ttc) return &g_ttc.node.link;
    return 0;
}

SIM_API int sim_i2c_write(uint8_t addr, const uint8_t *d, int n)
{
    icd_link_t *l = link_for(addr);
    if (!l) return -1;
    icd_link_isr_write_begin(l);
    for (int i = 0; i < n; i++) icd_link_isr_rx_byte(l, d[i]);
    icd_link_isr_write_end(l);
    return 0;
}

SIM_API int sim_i2c_read(uint8_t addr, uint8_t *d, int n)
{
    icd_link_t *l = link_for(addr);
    if (!l) return -1;
    icd_link_isr_read_begin(l);
    for (int i = 0; i < n; i++) d[i] = icd_link_isr_tx_byte(l);
    icd_link_isr_read_end(l);
    return 0;
}

SIM_API void sim_set_present(uint8_t addr, int present)
{
    if (addr == ICD_ADDR_ADCS) g_present_adcs = present;
    if (addr == ICD_ADDR_TTC) g_present_ttc = present;
}

SIM_API void sim_reset_slave(uint8_t addr)
{
    if (addr == ICD_ADDR_ADCS) adcs_init(&g_adcs, g_now);
    if (addr == ICD_ADDR_TTC) ttc_init(&g_ttc, g_now);
}

SIM_API int sim_adcs_mode(void) { return icd_node_mode(&g_adcs.node); }
SIM_API int sim_ttc_mode(void) { return icd_node_mode(&g_ttc.node); }
