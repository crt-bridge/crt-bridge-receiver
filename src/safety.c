/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * safety.c -- JVC DT-V1710CG modeline envelope validator + structured rejection logger.
 *
 * Three duties: check every mode against the fixed envelope of safety.h, log
 * every rejection in a structured line (no silent fallback), and always reject
 * the dead band, which is hardware-loss-grade.
 *
 * MUST be called by every modeset code path BEFORE sr_switch_to_mode() or
 * drmModeSetCrtc(). The dead band [16001, 26999] Hz will not sync on a JVC
 * deflection circuit and risks hardware damage under sustained signal.
 *
 * Pure function: no allocation, no I/O on the validate path. Logger writes one
 * line to stderr (always) and to syslog (when SAFETY_USE_SYSLOG is defined for
 * the daemon binary). Hermetic in tests.
 */

/* POSIX feature gate for clock_gettime / CLOCK_MONOTONIC under -std=c11 strict. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "safety.h"
#include <stdio.h>
#include <time.h>

#ifdef SAFETY_USE_SYSLOG
#include <syslog.h>
#endif

modeline_result_t validate_modeline(unsigned h_freq_hz, unsigned v_freq_hz)
{
    if (h_freq_hz < H_BAND_LOW_MIN_HZ)
        return MODELINE_REJECT_H_LOW;
    if (h_freq_hz <= H_BAND_LOW_MAX_HZ)
        goto check_v;
    if (h_freq_hz < H_BAND_HIGH_MIN_HZ)
        return MODELINE_REJECT_H_DEAD_BAND;
    if (h_freq_hz <= H_BAND_HIGH_MAX_HZ)
        goto check_v;
    return MODELINE_REJECT_H_HIGH;
check_v:
    if (v_freq_hz < V_MIN_HZ || v_freq_hz > V_MAX_HZ)
        return MODELINE_REJECT_V;
    return MODELINE_ACCEPT;
}

const char *safety_reason_str(modeline_result_t r)
{
    switch (r) {
    case MODELINE_ACCEPT:             return "ACCEPT";
    case MODELINE_REJECT_H_LOW:       return "H_LOW (below 15.625 kHz)";
    case MODELINE_REJECT_H_DEAD_BAND: return "H_DEAD_BAND (hardware-loss-grade)";
    case MODELINE_REJECT_H_HIGH:      return "H_HIGH (above 45 kHz)";
    case MODELINE_REJECT_V:           return "V_OUT_OF_RANGE (50-80 Hz envelope)";
    case MODELINE_REJECT_BAND:        return "BAND (no usable band of the monitor file)";
    }
    return "UNKNOWN";
}

void safety_log_rejection(const char *call_site, unsigned h_freq_hz, unsigned v_freq_hz, modeline_result_t reason)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long t_ns =
        (unsigned long long)ts.tv_sec * 1000000000ULL +
        (unsigned long long)ts.tv_nsec;
    const char *r = safety_reason_str(reason);
    const char *site = call_site ? call_site : "?";

    fprintf(stderr,
            "{\"event\":\"modeline_rejected\",\"site\":\"%s\","
            "\"h_hz\":%u,\"v_hz\":%u,\"reason\":\"%s\","
            "\"t_mono_ns\":%llu}\n",
            site, h_freq_hz, v_freq_hz, r, t_ns);

#ifdef SAFETY_USE_SYSLOG
    syslog(LOG_DAEMON | LOG_ERR,
           "modeline_rejected site=%s h_hz=%u v_hz=%u reason=%s t_mono_ns=%llu",
           site, h_freq_hz, v_freq_hz, r, t_ns);
#endif
}

void safety_log_rejection_hz(const char *call_site, double h_hz, double v_hz, modeline_result_t reason)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long t_ns =
        (unsigned long long)ts.tv_sec * 1000000000ULL +
        (unsigned long long)ts.tv_nsec;
    const char *r = safety_reason_str(reason);
    const char *site = call_site ? call_site : "?";

    fprintf(stderr,
            "{\"event\":\"modeline_rejected\",\"site\":\"%s\","
            "\"h_hz\":%.2f,\"v_hz\":%.2f,\"reason\":\"%s\","
            "\"t_mono_ns\":%llu}\n",
            site, h_hz, v_hz, r, t_ns);

#ifdef SAFETY_USE_SYSLOG
    syslog(LOG_DAEMON | LOG_ERR,
           "modeline_rejected site=%s h_hz=%.2f v_hz=%.2f reason=%s t_mono_ns=%llu",
           site, h_hz, v_hz, r, t_ns);
#endif
}
