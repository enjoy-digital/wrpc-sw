/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2024 CERN (www.cern.ch)
 * Author: Harvey Leicester <harvey.leicester@cern.ch>
 *
 * Released according to the GNU GPL, version 2 or any later version.
 */

/* based on wrs_auxclk by Grzegorz Daniluk */

#include <stdlib.h>
#include <string.h>
#include "dev/syscon.h"
#include "wrc.h"
#include "shell.h"
#include "board.h"
#include "hw/timecode_regs.h"
#include "cmds.h"

#define NS_FACTOR 1000000000
#define MAX_FREQ  250000000
#define MIN_FREQ  1
#define CNT_RES   2
#define AUXCLK ((volatile struct auxclk_gen*) (BASE_TIMECODE+TIMECODE_AUXCLK))

static const char * const auxclk_cmds[] =
{
  "set",
  "get",
};

struct params {
  uint32_t freq;
  uint32_t period_ns;
  uint32_t duty;
  uint32_t h_width, l_width;
};

static int calc_settings(struct params *req, struct params *calc)
{
  calc->freq = req->freq;
  if (req->freq > MAX_FREQ)
    calc->freq = MAX_FREQ;
  if (req->freq < MIN_FREQ)
    calc->freq = MIN_FREQ;

  if (!(req->duty > 0 && req->duty < 100))
    req->duty = 50;

  req->period_ns = NS_FACTOR/calc->freq;
  calc->h_width = (req->period_ns/CNT_RES)*(((float)req->duty/(float)100));
  calc->l_width = (req->period_ns/CNT_RES) - calc->h_width;
  /* last step, calculate the actual frequency and period based on the
   * actual h_width and l_width */
  calc->period_ns = (calc->h_width + calc->l_width) * CNT_RES;
  calc->freq = (NS_FACTOR / calc->period_ns);
  calc->duty = (calc->h_width*100)/(calc->period_ns/CNT_RES);

  /* check if what's about to be generated matches the request */
  if (req->period_ns == calc->period_ns && req->duty == calc->duty)
    return 0;
  else{
    return -1;
  }
}

static void get_settings(struct params *calc)
{
  calc->h_width = (AUXCLK->PR);
  calc->l_width = (AUXCLK->DCR);

  calc->period_ns = (calc->h_width + calc->l_width) * CNT_RES;
  calc->freq = NS_FACTOR / calc->period_ns;
  calc->duty = (calc->h_width*100)/(calc->period_ns/CNT_RES);
}

static void apply_settings(struct params *p)
{
  AUXCLK->PR = p->h_width;
  AUXCLK->DCR = p->l_width;
}

static void print_settings(struct params *p)
{
  pp_printf("frequency: %d Hz (%d ns)\n", p->freq, p->period_ns);
  pp_printf("high: %d ns; low: %d ns\n", p->h_width, p->l_width);
  pp_printf("duty: %d%%\n", p->duty);
}

/**
 * @shellcommand auxclk
 * @shellusage auxclk set [<frequency> [<duty>]]
 * Sets the auxiliary clock frequency and optional duty cycle.
 *
 * @shellusage auxclk get
 * Displays the current auxiliary clock settings.
 */
int cmd_auxclk(const char *args[])
{

  struct params req = {10000000, 0, 50, 0, 0};
  struct params calc;
  int ret;

  int icmd = sub_cmd(auxclk_cmds, ARRAY_SIZE(auxclk_cmds), args);

  switch (icmd) {
  case 0:
    if (args[1]) {
      req.freq = atoi(args[1]);
      req.duty = atoi(args[2]);
      ret = calc_settings(&req, &calc);
      if (!(calc.duty > 0 && calc.duty < 100)) {
        pp_printf("Requested duty %d (calculated %d)"
            " outside range (0; 100)\n", req.duty, calc.duty);
        return -1;
      }
      if (ret != 0) {
        pp_printf("Could not generate required signal, here is "
            "the alternative you could use:\n");
        print_settings(&calc);
        return 0;
      }
      apply_settings(&calc);
      pp_printf("applied settings:\n");
      print_settings(&calc);
      break;
    }
  case 1:
    get_settings(&calc);
    print_settings(&calc);
    break;
  }
  return 0;
}
