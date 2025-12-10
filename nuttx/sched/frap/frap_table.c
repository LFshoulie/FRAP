/* sched/frap/frap_table.c */
#include <nuttx/config.h>
#ifdef CONFIG_FRAP

#include <string.h>
#include <errno.h>
#include "sched/sched.h"
#include <nuttx/frap.h>

int frap_set_spin_prio(int8_t spin_prio)
{
  this_task()->frap_spin_prio = (uint8_t)spin_prio;
  return OK;
}

int frap_get_spin_prio(void)
{
    return (int)this_task()->frap_spin_prio;
}

#endif /* CONFIG_FRAP */
