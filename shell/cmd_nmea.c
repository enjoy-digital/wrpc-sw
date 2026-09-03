/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 CERN (www.cern.ch)
 * Author: Harvey Leicester <harvey.leicester@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include <stdlib.h>
#include <string.h>
#include "dev/syscon.h"
#include "wrc.h"
#include "shell.h"
#include "board.h"
#include "hw/timecode_regs.h"
#include "hw/nmea_master.h"
#include "cmds.h"

#define NMEA_MASTER ((volatile struct nmea_master*) (BASE_TIMECODE+TIMECODE_NMEA))

#define SUART_CALC_BAUD_NMEA(baudrate, clkrate) \
    ( ((( (unsigned long long)baudrate * 8ULL) << (16 - 7)) + \
      (clkrate >> 8)) / (clkrate >> 7) )

static const char * const nmea_cmds[] =
{
  "baud",
  "invert",
  "status"
};

static const uint32_t valid_bauds[5] =
{
  9600,
  19200,
  38400,
  57600,
  115200
};

static uint32_t baud_i = CONFIG_NMEA_OUT_BAUD;

static int set_baud(uint32_t baudrate)
{

  int valid = 0;
  int baud = 0;
  int clk_freq = (NMEA_MASTER->SR & NMEA_MASTER_SR_CLK_FREQ_MASK) >> NMEA_MASTER_SR_CLK_FREQ_SHIFT;
  int cr = NMEA_MASTER->CR;
  int i=0;
  for(i=0; i<ARRAY_SIZE(valid_bauds); i++){
    if(baudrate == valid_bauds[i]){
      valid = 1;
      break;
    }
  }
  
  if(valid){
    baud = SUART_CALC_BAUD_NMEA(baudrate, clk_freq);
    cr &= ~NMEA_MASTER_CR_BAUD_DIV_MASK;
    cr |= (baud << NMEA_MASTER_CR_BAUD_DIV_SHIFT);
    NMEA_MASTER->CR = cr;
    baud_i = baudrate;
    return 0;
  }else{
    pp_printf("invalid baud rate, valid rates: ");
    for(i=0; i<ARRAY_SIZE(valid_bauds); i++){
      pp_printf("%d ", valid_bauds[i]);
    }
    return -1;
  }
}

static int get_baud(void)
{
  return baud_i;
}

static void set_invert(int invert)
{
  int cr = NMEA_MASTER->CR;
  if(invert){
    cr |= NMEA_MASTER_CR_INVERT;
  }else{
    cr &= ~NMEA_MASTER_CR_INVERT;
  }
  NMEA_MASTER->CR = cr;
}

static int get_invert(void)
{
  return (NMEA_MASTER->CR & NMEA_MASTER_CR_INVERT) ? 1 : 0;
}

static void get_status(int *valid, int *tip)
{
  *valid = (NMEA_MASTER->SR & NMEA_MASTER_SR_VALID) ? 1 : 0;
  *tip   = (NMEA_MASTER->SR & NMEA_MASTER_SR_TIP) ? 1 : 0;
}

static void get_tod(int *hour, int *min, int *sec)
{

  int tod_hr = (NMEA_MASTER->TOD & NMEA_MASTER_TOD_HOUR_MASK)   >> NMEA_MASTER_TOD_HOUR_SHIFT;
  int tod_min = (NMEA_MASTER->TOD & NMEA_MASTER_TOD_MINUTE_MASK) >> NMEA_MASTER_TOD_MINUTE_SHIFT;
  int tod_sec = (NMEA_MASTER->TOD & NMEA_MASTER_TOD_SECOND_MASK) >> NMEA_MASTER_TOD_SECOND_SHIFT;

  *hour = (tod_hr);
  *min  = (tod_min);
  *sec  = (tod_sec);
}

static void get_date(int *day, int *month, int *year)
{

  int date_day = (NMEA_MASTER->DATE & NMEA_MASTER_DATE_DAY_MASK)   >> NMEA_MASTER_DATE_DAY_SHIFT; 
  int date_mon = (NMEA_MASTER->DATE & NMEA_MASTER_DATE_MONTH_MASK)  >> NMEA_MASTER_DATE_MONTH_SHIFT;
  int date_yr  = (NMEA_MASTER->DATE & NMEA_MASTER_DATE_YEAR_MASK)  >> NMEA_MASTER_DATE_YEAR_SHIFT;

  *day   = (date_day);
  *month = (date_mon);
  *year  = (date_yr);
}

static void print_status(void)
{

  int day, month, year;
  int hour, min, sec;
  int valid, tip;
  get_date(&day, &month, &year);
  get_tod(&hour, &min, &sec);
  get_status(&valid, &tip);
  pp_printf("baud: %d \n", get_baud());
  pp_printf("date: %04x:%02x:%02x time: %02x:%02x:%02x\n", year, month, day, hour, min, sec);
  pp_printf("valid: %d tip: %d invert: %d\n", valid, tip, get_invert());
}

/**
 * @shellcommand nmea
 * @shellusage nmea baud [<baudrate>]
 * Displays or sets the NMEA baud rate.
 *
 * @shellusage nmea invert [0|1]
 * Displays or sets NMEA signal inversion.
 *
 * @shellusage nmea status
 * Displays NMEA timing status.
 */
int cmd_nmea(const char *args[])
{

  uint32_t baud;
  uint32_t inv;
  int icmd = sub_cmd(nmea_cmds, ARRAY_SIZE(nmea_cmds), args);

  switch (icmd) {
  case 0:
    if (args[1]) {
      baud = atoi(args[1]);
      return set_baud(baud);
    }else{
      pp_printf("baud: %d\n", get_baud());
      return 0;
    }
    break;
  case 1:
    if (args[1]) {
      inv = (uint32_t)(atoi(args[1]));
      set_invert(inv);
    }else{
      pp_printf("invert: %d\n", get_invert());
      return 0;
    }
    break;
  case 2:
    print_status();
    break;
  }
  return 0;
}
