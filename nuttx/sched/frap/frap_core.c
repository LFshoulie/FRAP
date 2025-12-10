/* sched/frap/frap_core.c */

#include <nuttx/config.h>

#ifdef CONFIG_FRAP

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <debug.h>

#include "sched/sched.h"
#include <nuttx/list.h>
#include <nuttx/spinlock.h>
#include <nuttx/frap.h>
#include "frap_internal.h"

/****************************************************************************
 * Name: frap_res_init
 *
 * Initialize a FRAP resource descriptor.
 ****************************************************************************/

int frap_res_init(FAR struct frap_res *r, uint32_t id, bool is_global)
{
  if (r == NULL)
    {
      return -EINVAL;
    }

  r->sl        = SP_UNLOCKED;
  r->owner     = NULL;
  list_initialize(&r->fifo);
  r->id        = id;
  r->is_global = is_global;
  r->ceiling   = 0;

  return OK;
}

#endif /* CONFIG_FRAP */
