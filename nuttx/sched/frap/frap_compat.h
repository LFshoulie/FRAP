/* sched/frap/frap_compat.h
 * 小兼容层：
 * - 提供 frap_set_prio()：封装不同树上的 nxsched_setpriority / nxsched_set_priority
 * - 同时引入 sched/sched.h，给 this_task()/struct tcb_s 等私有符号
 */

#pragma once
#include <nuttx/config.h>
#include <nuttx/sched.h>     /* 公共调度接口 */
#include "sched/sched.h"     /* 私有：this_task(), struct tcb_s 等 */
#include <errno.h>

/* 两个变体都声明成弱符号；链接器若无该符号，则值为 NULL */
int nxsched_setpriority(FAR struct tcb_s *tcb, int prio) __attribute__((weak));
int nxsched_set_priority(FAR struct tcb_s *tcb, int prio) __attribute__((weak));

static inline int frap_set_prio(FAR struct tcb_s *tcb, uint8_t prio)
{
  if (nxsched_setpriority) {
    return nxsched_setpriority(tcb, (int)prio);
  } else if (nxsched_set_priority) {
    return nxsched_set_priority(tcb, (int)prio);
  }
  return -ENOSYS;  /* 你的树不应出现这种情况，仅作保护 */
}
