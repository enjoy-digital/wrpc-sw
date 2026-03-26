///////////////////////////////////////////////////////////////////////////////
// SPDX-FileCopyrightText: 2026 CERN (home.cern)
//
// SPDX-License-Identifier: LGPL-2.1-or-later
///////////////////////////////////////////////////////////////////////////////
/*
 * This work is part of the White Rabbit project
 *
 * ZynqUS+ PL sysmon registers
 */
#ifndef __ZYNQUS_PLSYSMON_H__
#define __ZYNQUS_PLSYSMON_H__

#include <stdint.h>

struct plsysmon {
    uint32_t temperature;
    uint32_t supply1;
    uint32_t supply2;
    uint32_t vp_vn;

    uint32_t vrefp;
    uint32_t vrefn;
    uint32_t supply3;
    uint32_t _pad1c[1];

    uint32_t _pad20[4];

    uint32_t _pad30[1];
    uint32_t supply4;
    uint32_t supply5;
    uint32_t supply6;

    uint32_t vaux[16];

    /* 0x0080 */
    uint32_t max_temperature;
    uint32_t max_supply1;
    uint32_t max_supply2;
    uint32_t max_supply3;

    uint32_t min_temperature;
    uint32_t min_supply1;
    uint32_t min_supply2;
    uint32_t min_supply3;

    uint32_t max_supply4;
    uint32_t max_supply5;
    uint32_t max_supply6;
    uint32_t _padac;

    uint32_t min_supply4;
    uint32_t min_supply5;
    uint32_t min_supply6;
    uint32_t _padbc;

    uint32_t _padc0[12];

    uint32_t _padf0[2];
    uint32_t status_flag_1;
    uint32_t status_flag;

    /* 0x0100 */
    uint32_t config_reg0;
    uint32_t config_reg1;
    uint32_t config_reg2;
    uint32_t config_reg3;

    uint32_t config_reg4;
    uint32_t analog_bus;
    uint32_t seq_channel2;
    uint32_t seq_average2;

    uint32_t seq_channel0;
    uint32_t seq_channel1;
    uint32_t seq_average0;
    uint32_t seq_average1;

    uint32_t seq_input_mode0;
    uint32_t seq_input_mode1;
    uint32_t seq_acq0;
    uint32_t seq_acq1;

    /* 0x0140 */
    uint32_t alarm_temperature_upper;
    uint32_t alarm_supply1_upper;
    uint32_t alarm_supply2_upper;
    uint32_t alarm_ot_upper;

    uint32_t alarm_temperature_lower;
    uint32_t alarm_supply1_lower;
    uint32_t alarm_supply2_lower;
    uint32_t alarm_ot_lower;

    uint32_t alarm_supply3_upper;
    uint32_t alarm_supply4_upper;
    uint32_t alarm_supply5_upper;
    uint32_t alarm_supply6_upper;

    uint32_t alarm_supply3_lower;
    uint32_t alarm_supply4_lower;
    uint32_t alarm_supply5_lower;
    uint32_t alarm_supply6_lower;

    /* 0x0180 */
    uint32_t alarm_supply7_upper;
    uint32_t alarm_supply8_upper;
    uint32_t alarm_supply9_upper;
    uint32_t alarm_supply10_upper;

    uint32_t alarm_vccams_upper;
    uint32_t _pad194[3];

    uint32_t alarm_supply7_lower;
    uint32_t alarm_supply8_lower;
    uint32_t alarm_supply9_lower;
    uint32_t alarm_supply10_lower;

    uint32_t alarm_vccams_lower;
    uint32_t _pad1b4[3];

    uint32_t _pad1c0[4];

    uint32_t _pad1d0[4];

    uint32_t seq_input_mode2;
    uint32_t seq_acq2;
    uint32_t seq_low_rate_channel0;
    uint32_t seq_low_rate_channel1;

    uint32_t seq_low_rate_channel2;
    uint32_t _pad1f4[3];

    /* 0x0200 */
    uint32_t supply7;
    uint32_t supply8;
    uint32_t supply9;
    uint32_t supply10;

    uint32_t vccams;
    uint32_t _pad214[3];

    uint32_t _pad220[4];
    uint32_t _pad230[4];
    uint32_t _pad240[4];
    uint32_t _pad250[4];
    uint32_t _pad260[4];
    uint32_t _pad270[4];

    /* 0x0280 */
    uint32_t max_supply7;
    uint32_t max_supply8;
    uint32_t max_supply9;
    uint32_t max_supply10;

    uint32_t max_vccams;
};

#define ZYNQUS_PLSYSMON_REGS  ((volatile struct plsysmon *) 0xffa50c00)

#endif /* __ZYNQUS_PLSYSMON_H__ */
