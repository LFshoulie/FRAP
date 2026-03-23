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
 * 由调度器在发生切换时调用
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

  /* 调度器不应传入 NULL */
  DEBUGASSERT(oldtcb != NULL);
  DEBUGASSERT(newtcb != NULL);

  /* 如果是曾被抢占取消，必须立即将优先级重新提升回 spin_prio */
  /* if (newtcb->frap_cancelled)
    {
      newtcb->frap_queue_preempt_cnt++;
      newtcb->frap_cancelled = false;
      frap_set_prio(newtcb, newtcb->frap_spin_prio);
    } */
    
  /* 只有更高优先级任务抢占才会影响 FRAP 自旋；仅当被抢占任务在队列自旋等待时才处理 */
  if (newtcb->sched_priority <= oldtcb->sched_priority || !oldtcb->frap_enqueued)
    {
      return;
    }
    
  /* 在当前实现下，进入临界区后不会发生调度 */
  DEBUGASSERT(!oldtcb->frap_in_cs);

  r = oldtcb->frap_waiting_res;
  
  /* enqueued 状态下 waiting_res 必须存在 */
  DEBUGASSERT(r != NULL);

  /* 在资源自旋锁保护下安全地将其从 FIFO 中移除 */
  flags = spin_lock_irqsave(&r->sl);

  DEBUGASSERT(oldtcb->frap_enqueued);

  /* 被抢占的任务出队，并设置cancled为1 */
  frap_queue_remove(r, oldtcb);
  oldtcb->frap_cancelled = true;
  oldtcb->frap_queue_preempt_cnt++;  // 被抢占次数加一
  
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