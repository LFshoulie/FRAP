/* sched/frap/frap_schedhook.c */

#include <nuttx/config.h>

#ifdef CONFIG_FRAP

#include <stdbool.h>
#include <stdint.h>

#include <debug.h>

#include "sched/sched.h"
#include <nuttx/spinlock.h>
#include <nuttx/frap.h>

#include "frap_internal.h"

/****************************************************************************
 * Name: frap_on_preempt
 *
 * 由调度器在发生抢占切换时调用。
 *
 * 语义：
 *   如果 oldtcb 正在某个 FRAP 资源的自旋队列中等待（尚未进入
 *   非抢占临界段），而 newtcb 的优先级更高，则将 oldtcb 从队列中
 *   移除，恢复其基准优先级，并打标 frap_cancelled，表示这次自旋
 *   被“中断”，下次被调度时需要重新进入队列。
 ****************************************************************************/

void frap_on_preempt(FAR struct tcb_s *oldtcb, FAR struct tcb_s *newtcb)
{
  FAR struct frap_res *r;
  irqstate_t           flags;

  if (oldtcb == NULL || newtcb == NULL)
    {
      return;
    }

  /* 只有更高优先级任务抢占才会影响 FRAP 自旋 */

  if (newtcb->sched_priority <= oldtcb->sched_priority)
    {
      return;
    }

  /* 如果 oldtcb 不在 FRAP 自旋队列中，或者已经在临界段内，则无需处理 */

  if (!oldtcb->frap_enqueued || oldtcb->frap_in_cs)
    {
      return;
    }

  r = oldtcb->frap_waiting_res;
  if (r == NULL)
    {
      return;
    }

  /* 在资源自旋锁保护下安全地将其从 FIFO 中移除 */

  flags = spin_lock_irqsave(&r->sl);

  if (oldtcb->frap_enqueued)
    {
      frap_queue_remove(r, oldtcb);
      oldtcb->frap_cancelled = true;
    }

  spin_unlock_irqrestore(&r->sl, flags);

  /* 恢复基准优先级 P_i */

  frap_set_prio(oldtcb, oldtcb->frap_base_prio);

  sinfo("FRAP preempt: old=%d (spin=%u->base=%u) by new=%d, resid=%u\n",
        oldtcb->pid,
        (unsigned)oldtcb->frap_spin_prio,
        (unsigned)oldtcb->frap_base_prio,
        newtcb->pid,
        r ? (unsigned)r->id : 0);
}

#endif /* CONFIG_FRAP */
