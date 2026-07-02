/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 Missing Link Electronics (www.missinglinkelectronics.com)
 * Author: Oskar Szakinnis <oskar.szakinnis@missinglinkelectronics.com>
 *
 * Released according to the GNU GPL, version 2 or any later version,
 * OR according to the GNU LGPL, version 3.0 or any later version
 */
/* Basic NMEA parsing required for synchronizing White Rabbit to GNSS */

#ifndef __NMEA_H
#define __NMEA_H

#include <stdint.h>
#include <stdbool.h>


/* -------------------------------------------------------------------------- */
/*                           Types, defines, globals                          */
/* -------------------------------------------------------------------------- */

#define NMEA_SENTENCE_MAXLEN 82u

// NMEA Sentence delimiters
#define NMEA_DELIM_START '$'
#define NMEA_DELIM_STOP_1 '\r'
#define NMEA_DELIM_STOP_2 '\n'
#define NMEA_DELIM_CHECKSUM '*'
#define NMEA_DELIM_FIELD ','


// NMEA field properties and indices 
// TODO: Careful, indices occasionally seem to vary between modules
#define NMEA_FIELD_LEN_TYPE 5
#define NMEA_FIELD_IDX_TYPE 0
#define NMEA_FIELD_IDX_GGA_FIXTYPE 6
#define NMEA_FIELD_IDX_RMC_DATE 9
#define NMEA_FIELD_IDX_RMC_TIME 1

// NMEA sentence type enum
typedef enum nmea_sentence_type {
	NMEA_RMC,
	NMEA_GGA,
	NMEA_NUM_TYPES,
	NMEA_ANY
} nmea_sentence_type_t;


/* -------------------------------------------------------------------------- */
/*                       NMEA general sentence handling                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Validate sentence by calculating checksum and comparing against 
 * checksum contained in sentence.
 * 
 * @param sentence the sentence to be validated
 * @return true if checksum matches or if no checksum is present 
 * @return false if checksum does not match 
 */
bool nmea_sentence_is_valid(const char *sentence);

/**
 * @brief Check if type field of sentence matches type argument.
 * 
 * @param sentence the sentence to check; does not have to be a full nmea sentence
 * (can be used while sentence is still being read), but must at least consist 
 * of the full type field
 * @param type type to check against
 * @return int 1 if type matches, 0 otherwise, -1 on error 
 */
int nmea_sentence_type_is(const char *sentence, nmea_sentence_type_t type);

/**
 * @brief Retrieve field by index from NMEA sentence.
 * 
 * @param sentence a valid NMEA sentence
 * @param field_idx index of the field
 * @return char* a static buffer containing the requested field; this 
 * buffer is reused and overwritten by subsequent calls of this function! 
 * Returns NULL if field index is larger than number of fields in sentence. 
 */
char *nmea_sentence_get_field(const char *sentence, unsigned field_idx);


/* -------------------------------------------------------------------------- */
/*                       NMEA sentence-specific parsing                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Parse fix information from valid GGA sentence.
 * 
 * @param sentence valid GGA sentence
 * @return int 0 if fix type is "0" (invalid/none), 1 otherwise, -1 on error 
 */
int nmea_gga_parse_hasfix(const char *sentence);

/**
 * @brief Retrieve UTC timestamp
 * 
 * @param sentence valid RMC sentence
 * @return int64_t timestamp on success, -1 on error
 */
int64_t nmea_rmc_parse_utc(const char *sentence);

#endif
