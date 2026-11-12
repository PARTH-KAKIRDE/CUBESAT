/*
 * Host simulation of the two slaves, built as a shared library
 * (build/host/cubesat_sim.dll / .so) and loaded by the OBC tests with ctypes.
 *
 * The REAL firmware logic runs here (icd_link, icd_node, adcs_app, bdot,
 * ttc_app); only the *_hw layer is simulated. Time is simulated too: nothing
 * happens until sim_run_ms() advances the clock, one main-loop pass per ms.
 */
#ifndef SIM_API_H
#define SIM_API_H

#include <stdint.h>

#ifdef _WIN32
#define SIM_API __declspec(dllexport)
#else
#define SIM_API __attribute__((visibility("default")))
#endif

SIM_API void sim_init(uint32_t seed);
SIM_API void sim_run_ms(uint32_t ms);
SIM_API uint32_t sim_now(void);

/* I2C bus. Return 0 on success, -1 if no device ACKs the address. */
SIM_API int sim_i2c_write(uint8_t addr7, const uint8_t *data, int len);
SIM_API int sim_i2c_read(uint8_t addr7, uint8_t *data, int len);
SIM_API void sim_set_present(uint8_t addr7, int present); /* unplug / replug */
/* Reboot one slave (as a watchdog reset would). */
SIM_API void sim_reset_slave(uint8_t addr7);

/* ADCS physics and fault injection */
SIM_API void sim_adcs_set_rate_dps(double wx, double wy, double wz);
SIM_API void sim_adcs_get_rate_dps(double *out3);
#define SIM_ADCS_FAIL_MAG  0x01
#define SIM_ADCS_FAIL_IMU  0x02
#define SIM_ADCS_FAIL_DRVX 0x04
#define SIM_ADCS_FAIL_DRVY 0x08
#define SIM_ADCS_FAIL_DRVZ 0x10
SIM_API void sim_adcs_set_faults(uint32_t mask);
/* Magnetometer samples taken while a coil was driven (must stay 0). */
SIM_API uint32_t sim_adcs_dirty_mag_samples(void);
SIM_API int sim_adcs_mode(void);

/* TT&C radio and pins */
SIM_API void sim_ttc_uplink(const uint8_t *data, int len, int rssi_dbm);
SIM_API int sim_ttc_downlink(uint8_t *buf); /* returns length, 0 if none */
SIM_API void sim_ttc_set_pins(int deploy_sw, int rbf_in);
SIM_API int sim_ttc_burn_on(void);
SIM_API uint32_t sim_ttc_burn_total_ms(void);
SIM_API void sim_ttc_set_radio_ok(int ok);
SIM_API int sim_ttc_mode(void);

#endif
