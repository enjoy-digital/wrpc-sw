///////////////////////////////////////////////////////////////////////////////
// SPDX-FileCopyrightText: 2026 CERN (home.cern)
//
// SPDX-License-Identifier: LGPL-2.1-or-later
///////////////////////////////////////////////////////////////////////////////
/*
 * This work is part of the White Rabbit project
 *
 * ZynqUS+ PS sysmon registers
 */
#ifndef __ZYNQUS_PSSYSMON_H__
#define __ZYNQUS_PSSYSMON_H__

#include <stdint.h>

struct pssysmon {
	uint32_t temp_lpd;
	uint32_t vcc_psintlp;
	uint32_t vcc_psintfp;
	uint32_t vp_vn;

	uint32_t _pad_10;
	uint32_t _pad_14;
	uint32_t vcc_psaux;
	uint32_t _pad_1c;

	uint32_t _pad_20[4];

	uint32_t _pad_30;
	uint32_t vcco_psddr;
	uint32_t vcco_psio3;
	uint32_t vcco_psio0;

	uint32_t _pad_40[4];
	uint32_t _pad_50[4];
	uint32_t _pad_60[4];
	uint32_t _pad_70[4];

	uint32_t max_temp_lpd;
	uint32_t max_vcc_psintlp;
	uint32_t max_vcc_psintfp;
	uint32_t max_vcc_psaux;

	uint32_t min_temp_lpd;
	uint32_t min_vcc_psintlp;
	uint32_t min_vcc_psintfp;
	uint32_t min_vcc_psaux;

	uint32_t max_vcco_psddr;
	uint32_t max_vcco_psio3;
	uint32_t max_vcco_psio0;
	uint32_t _pad_ac;

	uint32_t min_vcco_psddr;
	uint32_t min_vcco_psio3;
	uint32_t min_vcco_psio0;
	uint32_t _pad_bc;

	uint32_t _pad_c0[4];
	uint32_t _pad_d0[4];
	uint32_t _pad_e0[4];

	uint32_t _pad_f0;
	uint32_t _pad_f4;
	uint32_t status_flag_1;
	uint32_t status_flag;

	/* 0x100 */
	uint32_t config_reg0;
	uint32_t config_reg1;
	uint32_t config_reg2;
	uint32_t config_reg3;

	uint32_t config_reg4;
	uint32_t _pad_114;
	uint32_t seq_channel2;
	uint32_t seq_average2;

	uint32_t seq_channel0;
	uint32_t _pad_124;
	uint32_t seq_average0;
	uint32_t _pad_12c;

	uint32_t _pad_130;
	uint32_t _pad_134;
	uint32_t _pad_138;
	uint32_t seq_acq1;

	/* 0x140 */
	uint32_t alarm_temp_lpd_upper;
	uint32_t alarm_vcc_psintlp_upper;
	uint32_t alarm_vcc_psintfp_upper;
	uint32_t alarm_ot_upper;

	uint32_t alarm_temp_lpd_lower;
	uint32_t alarm_lower_vcc_psintlp;
	uint32_t alarm_vcc_psintfp_lower;
	uint32_t alarm_ot_lower;

	uint32_t alarm_vcc_psaux_upper;
	uint32_t alarm_vcco_psddr_upper;
	uint32_t alarm_vcco_psio3_upper;
	uint32_t alarm_vcco_psio0_upper;

	uint32_t alarm_vcc_psaux_lower;
	uint32_t alarm_vcco_psddr_lower;
	uint32_t alarm_vcco_psio3_lower;
	uint32_t alarm_vcco_psio0_lower;

	/* 0x180 */
	uint32_t alarm_vcco_psio1_upper;
	uint32_t alarm_vcco_psio2_upper;
	uint32_t alarm_mgtravcc_upper;
	uint32_t alarm_mgtravtt_upper;

	uint32_t alarm_vcc_psadc_upper;
	uint32_t alarm_temp_fpd_upper;
	uint32_t _pad_198;
	uint32_t _pad_19c;

	uint32_t alarm_vcco_psio1_lower;
	uint32_t alarm_vcco_psio2_lower;
	uint32_t alarm_mgtravcc_lower;
	uint32_t alarm_mgtravtt_lower;

	uint32_t alarm_vcc_psadc_lower;
	uint32_t alarm_temp_fpd_lower;
	uint32_t _pad_1b8;
	uint32_t _pad_1bc;

	uint32_t _pad_1c0[4];
	uint32_t _pad_1d0[4];

	uint32_t _pad_1e0;
	uint32_t _pad_1e4;
	uint32_t seq_low_rate_channel0;
	uint32_t _pad_1ec;

	uint32_t seq_low_rate_channel2;
	uint32_t _pad_1f4;
	uint32_t _pad_1f8;
	uint32_t _pad_1fc;

	/* 0x200 */
	uint32_t vcco_psio1;
	uint32_t vcco_psio2;
	uint32_t vcc_psgtr;
	uint32_t vtt_psgtr;

	uint32_t vcc_psadc;
	uint32_t temp_fpd;
	uint32_t _pad_218;
	uint32_t _pad_21c;

	uint32_t _pad_220[4];
	uint32_t _pad_230[4];
	uint32_t _pad_240[4];
	uint32_t _pad_250[4];
	uint32_t _pad_260[4];
	uint32_t _pad_270[4];

	/* 0x280 */
	uint32_t max_vcco_psio1;
	uint32_t max_vcco_psio2;
	uint32_t max_vcc_psgtr;
	uint32_t max_vtt_psgtr;

	uint32_t max_psadc;
	uint32_t max_temp_fpd;
	uint32_t _pad_298;
	uint32_t _pad_29c;

	uint32_t min_vcco_psio1;
	uint32_t min_vcco_psio2;
	uint32_t min_vcc_psgtr;
	uint32_t min_vtt_psgtr;

	uint32_t min_vcc_psadc;
	uint32_t min_temp_fpd;
};

#define ZYNQUS_PSSYSMON_REGS  ((volatile struct pssysmon *) 0xffa50800)

#endif /* __ZYNQUS_PSSYSMON_H__ */
