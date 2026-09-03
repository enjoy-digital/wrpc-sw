/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 CERN (www.cern.ch)
 * Author: Harvey Leicester <harvey.leicester@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

#include <string.h>
#include "wrc.h"
#include "dev/syscon.h"
#include "shell.h"
#include "board.h"
#include "dev/timecode.h"
#include "cmds.h"

#define TIMECODE ((volatile struct timecode*) (BASE_TIMECODE))

static const char * const auxtmg_cmds[] =
{
  "status",
  "sel"
};

static struct wr_timecode_fields tc;

static void auxtmg_print_tc(struct wr_timecode_fields *tc){
  pp_printf("%02d:%02d:%02d %02d:%02d:%02d diy:%d sbs:%d utc_valid:%d ls_val:%d ls_flag59:%d ls_flag61:%d ls_valid:%d\n", \
    tc->year, tc->month, tc->day, tc->hour, tc->min, tc->sec, tc->diy, tc->sbs, tc->utc_valid, tc->ls_val, tc->ls_flag59, tc->ls_flag61, tc->ls_valid);
}

static void auxtmg_status(struct wr_timecode_fields *tc){

  timecode_get_tc(tc);
  auxtmg_print_tc(tc);
  pp_printf("sel: ");
  int sel = timecode_get_sel();
  if(sel == TIMECODE_SEL_CLK){
    pp_printf("clk\n");
  }
  if(sel == TIMECODE_SEL_NMEA){
    pp_printf("nmea\n");
  }
  if(sel == TIMECODE_SEL_IRIG){
    pp_printf("irig\n");
  }
  pp_printf("enabled: ");
  if(timecode_is_enabled(TIMECODE_SEL_CLK))
    pp_printf("clk ");
  if(timecode_is_enabled(TIMECODE_SEL_NMEA))
    pp_printf("nmea ");
  if(timecode_is_enabled(TIMECODE_SEL_IRIG))
    pp_printf("irig");
  pp_printf("\n");
}

static int auxtmg_sel(const char *ip)
{

  if(!strcmp(ip, "clk")){
    return timecode_sel(TIMECODE_SEL_CLK);
  }
  if(!strcmp(ip, "nmea")){
    return timecode_sel(TIMECODE_SEL_NMEA);
  }
  if(!strcmp(ip, "irig")){
    return timecode_sel(TIMECODE_SEL_IRIG);
  }
  return 0;
}

/**
 * @shellcommand auxtmg
 * @shellusage auxtmg status
 * Displays the auxiliary timing input status.
 *
 * @shellusage auxtmg sel <clk|nmea|irig>
 * Selects the auxiliary timing source.
 */
int cmd_auxtmg(const char *args[])
{
  int icmd = sub_cmd(auxtmg_cmds, ARRAY_SIZE(auxtmg_cmds), args);

  switch (icmd) {
  case 0:
    auxtmg_status(&tc);
    break;
  case 1:
    if (args[1]) {
      if(!auxtmg_sel(args[1])){
        pp_printf("unable to select %s. is it in bitstream?\n", args[1]);
        return 0;
      }
      break;
    }else{
      pp_printf("usage: auxtmg %s <clk/nmea/irig>\n", auxtmg_cmds[icmd]);
    }
    break;
  default:
    break;
  }
  return 0;
}
