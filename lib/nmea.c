/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *
 * Released according to the GNU GPL, version 2 or any later version,
 * OR according to the GNU LGPL, version 3.0 or any later version
 */

#include <string.h>
#include <stdint.h>

#include "lib/nmea.h"

#include "wrpc.h"
#include "util.h"
#include "pp-printf.h"


/* -------------------------------------------------------------------------- */
/*                           Types, defines, globals                          */
/* -------------------------------------------------------------------------- */

static const char * const nmea_sentence_type_strs[] = {
	[NMEA_RMC] = "RMC",
	[NMEA_GGA] = "GGA"
};

static const char field_delims_start[] = {NMEA_DELIM_FIELD, NMEA_DELIM_CHECKSUM, '\0'};
static const char field_delims_stop[] = {NMEA_DELIM_FIELD, NMEA_DELIM_CHECKSUM, NMEA_DELIM_STOP_1, '\0'};


/* -------------------------------------------------------------------------- */
/*                       NMEA general sentence handling                       */
/* -------------------------------------------------------------------------- */

bool nmea_sentence_is_valid(const char *sentence)
{
	if (strnlen(sentence, NMEA_SENTENCE_MAXLEN) > NMEA_SENTENCE_MAXLEN) {
		pp_error("%s(): Sentence exceeds maximum NMEA sentence length\n", __func__);
		return false;
	}

	// retrieve checksum value contained in sentence
	const char *checksum_delim = strchr(sentence, NMEA_DELIM_CHECKSUM);
	if (checksum_delim == NULL) {
		pp_info("%s(): Sentence \"%s\" does not contain checksum and therefore is assumed to be valid\n", __func__, sentence);
		return true;
	}
	const char sentence_checksum[3] = {checksum_delim[1], checksum_delim[2], '\0'};
	int checksum_expected;
	fromhex(sentence_checksum, &checksum_expected);

	// calculate real sentence checksum
	int checksum_real = (int)sentence[1];
	sentence = &sentence[2];
	while (sentence != checksum_delim) {
		checksum_real ^= *(sentence++);
	}

	return (checksum_expected == checksum_real);
}

int nmea_sentence_type_is(const char *sentence, nmea_sentence_type_t type)
{
	if (strnlen(sentence, NMEA_FIELD_LEN_TYPE + 1) < NMEA_FIELD_LEN_TYPE + 1) {
		pp_error("%s(): Provided string too short to contain type field\n", __func__);
		return -1;
	}

	const char *type_str = &sentence[3];

	if (strncmp(nmea_sentence_type_strs[type], type_str, 3) == 0) {
		return 1;
	} else {
		return 0;
	}
}

char *nmea_sentence_get_field(const char *sentence, unsigned field_idx)
{
	// using sentence length here ensures that the buffer is large enough
	static char field_buf[NMEA_SENTENCE_MAXLEN];

	// determine start of field
	const char *field_start = sentence;
	for (int i = 0; i < field_idx; i++) {
		field_start = strpbrk(field_start, field_delims_start);
		if (field_start == NULL){
			pp_error("%s(): Field index larger than number of fields in sentence\n", __func__);
			return NULL;
		}
		// skip field start delimiter (it's not part of the field itself)
		field_start = field_start + 1;
	}

	// determine length of field
	int field_len = strcspn(field_start, field_delims_stop);

	// copy and return field
	memcpy(field_buf, field_start, field_len);
	field_buf[field_len] = '\0';
	return field_buf;
}

int nmea_gga_parse_hasfix(const char *sentence)
{
	if (!nmea_sentence_type_is(sentence, NMEA_GGA)) {
		pp_error("%s(): Provided sentence is not of type GGA\n", __func__);
		return -1;
	}

	const char *fixtype_field = nmea_sentence_get_field(sentence, NMEA_FIELD_IDX_GGA_FIXTYPE);
	if (fixtype_field == NULL) {
		pp_error("%s(): Could not retrieve fixtype field from provided sentence\n", __func__);
		return -1;
	}

	return (fixtype_field[0] != '0' ? 1 : 0);
}

int64_t nmea_rmc_parse_utc(const char *sentence)
{
	struct time_m datetime;

	if (!nmea_sentence_type_is(sentence, NMEA_RMC)) {
		pp_error("%s(): Provided sentence is not of type RMC\n", __func__);
		return -1;
	}

	const char *date_field = nmea_sentence_get_field(sentence, NMEA_FIELD_IDX_RMC_DATE);
	if (date_field == NULL) {
		pp_error("%s(): Could not retrieve date field from provided sentence\n", __func__);
		return -1;
	}
	// date_field is formatted DDMMYY
	const char str_day[3] = {date_field[0], date_field[1], '\0'};
	const char str_month[3] = {date_field[2], date_field[3], '\0'};
	const char str_year[3] = {date_field[4], date_field[5], '\0'};

	datetime.tm_mday = atoi(str_day);
	// datetime.tm_wday can be left out, isn't required for timestamp calculation
	datetime.tm_mon = atoi(str_month);
	datetime.tm_year = 2000 + atoi(str_year);

	const char *time_field = nmea_sentence_get_field(sentence, NMEA_FIELD_IDX_RMC_TIME);
	if (time_field == NULL) {
		pp_error("%s: Could not retrieve time field from provided sentence\n", __func__);
		return -1;
	}
	// time_field is formatted HHMMSS[.ss[s]]
	const char str_hour[3] = {time_field[0], time_field[1], '\0'};
	const char str_minute[3] = {time_field[2], time_field[3], '\0'};
	const char str_second[3] = {time_field[4], time_field[5], '\0'};

	datetime.tm_hour = atoi(str_hour);
	datetime.tm_min = atoi(str_minute);
	datetime.tm_sec = atoi(str_second);

	return utc_datetime_to_seconds(datetime);
}
