/* sched/frap/frap_lock.c */

#include <nuttx/config.h>

#ifdef CONFIG_FRAP

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <debug.h>

#include "sched/sched.h"
#include <nuttx/spinlock.h>
#include <nuttx/arch.h>
#include <nuttx/frap.h>
#include "frap_internal.h"

/****************************************************************************
 * Name: frap_lock
 *
 * FRAP 全局自旋协议加锁。
 *
 * - 进入时：当前任务可被抢占。
 * - 返回时：当前任务已获得资源 r，且处于 sched_lock() 保护下，
 *           即同一 CPU 上更高优先级任务不会在临界段中抢占它。
 *
 * 调用者必须在 frap_unlock() 之前保持语义上的“临界段”。
 ****************************************************************************/

int frap_lock(FAR struct frap_res *r)
{
  FAR struct tcb_s *tcb;
  uint8_t           base;
  irqstate_t        flags;

  if (r == NULL || !r->is_global)
    {
      return -EINVAL;
    }

  tcb  = this_task();
  base = (uint8_t)tcb->sched_priority;

  /* 自旋优先级不能低于当前基准优先级，否则违背实时性假设 */
  int spin_prio = frap_get_spin_prio();
  if (spin_prio < base)
    {
      return -EINVAL;
    }

  /* 初始化 per-task FRAP 状态 */

  tcb->frap_waiting_res = r;
  tcb->frap_base_prio   = base;
  tcb->frap_spin_prio   = spin_prio;
  tcb->frap_cancelled   = false;
  tcb->frap_in_cs       = false;

  DEBUGASSERT(!tcb->frap_enqueued);

  /* R1: 将任务优先级提升到自旋优先级 P_i^k */

  frap_set_prio(tcb, spin_prio);

  for (;;)
    {
      bool can_enter = false;

      /* 短临界区：检查资源状态并操作 FIFO */

      flags = spin_lock_irqsave(&r->sl);

      if (r->owner == NULL)
        {
          FAR struct tcb_s *head = frap_queue_peek_head(r);

          if (head == NULL)
            {
              /* 队列为空：自己占队头并立即进入临界段 */
              frap_queue_enqueue_head_if_needed(r, tcb);
              can_enter = true;
            }
          else if (head == tcb)
            {
              /* 已经是队头，直接进入 */
              can_enter = true;
            }
        }

      if (can_enter)
        {
          /* 从队列摘除并占有资源 */
          frap_queue_remove(r, tcb);
          r->owner = tcb;

          /* R2: 非抢占执行临界段（同核不可被更高优先级打断） */
          sched_lock();
          tcb->frap_in_cs = true;

          spin_unlock_irqrestore(&r->sl, flags);


          return OK;
        }

      /* 不能进入：确保在 FIFO 尾部排队，然后自愿让出 CPU */

      frap_queue_enqueue_tail(r, tcb);

      spin_unlock_irqrestore(&r->sl, flags);

      if (tcb->frap_cancelled)
        {
          /* 被更高优先级任务抢占过一次（由 frap_on_preempt 处理） */
          tcb->frap_cancelled = false;
        }

      /* 自愿调度，等待下一次被调度后继续尝试 */
      sched_yield();
    }
}

/****************************************************************************
 * Name: frap_unlock
 *
 * 对应 frap_lock 的解锁操作。
 ****************************************************************************/

void frap_unlock(FAR struct frap_res *r)
{
  FAR struct tcb_s *tcb;
  irqstate_t        flags;

  DEBUGASSERT(r != NULL && r->is_global);

  tcb = this_task();

  DEBUGASSERT(r->owner == tcb);
  DEBUGASSERT(tcb->frap_in_cs);

  /* 先退出非抢占区，再释放资源 */
  tcb->frap_in_cs = false;
  sched_unlock();

  /* 清空 owner，唤醒后续等待者由其自行争抢 */
  flags    = spin_lock_irqsave(&r->sl);
  r->owner = NULL;
  spin_unlock_irqrestore(&r->sl, flags);

  /* 恢复基准优先级 P_i */
  frap_set_prio(tcb, tcb->frap_base_prio);

  tcb->frap_waiting_res = NULL;
}

/****************************************************************************
 * Name: frap_local_lock
 *
 * 本地 PCP 变体的加锁：不使用全局自旋队列，仅在本核内提升到 ceiling。
 ****************************************************************************/

int frap_local_lock(FAR struct frap_res *r, uint8_t ceiling)
{
  FAR struct tcb_s *tcb;
  uint8_t           base;
  uint8_t           eff;
  irqstate_t        flags;

  if (r == NULL || r->is_global)
    {
      return -EINVAL;
    }

  tcb  = this_task();
  base = (uint8_t)tcb->sched_priority;

  /* 记录 ceiling，便于调试和后续策略扩展 */
  r->ceiling = ceiling;

  /* 记录进入 PCP 临界段前的真实优先级，便于解锁恢复 */
  tcb->frap_saved_prio = base;

  /* 有效优先级 = max(P_i, ceiling) */
  eff = base > ceiling ? base : ceiling;

  frap_set_prio(tcb, eff);

  /* 进入非抢占临界段 */
  flags = spin_lock_irqsave(&r->sl);
  r->owner = tcb;
  spin_unlock_irqrestore(&r->sl, flags);

  sched_lock();
  tcb->frap_in_cs = true;

  return OK;
}

/****************************************************************************
 * Name: frap_local_unlock
 ****************************************************************************/

void frap_local_unlock(FAR struct frap_res *r)
{
  FAR struct tcb_s *tcb;
  irqstate_t        flags;
  uint8_t           restore;

  DEBUGASSERT(r != NULL && !r->is_global);

  tcb = this_task();

  DEBUGASSERT(r->owner == tcb);
  DEBUGASSERT(tcb->frap_in_cs);

  tcb->frap_in_cs = false;
  sched_unlock();

  flags    = spin_lock_irqsave(&r->sl);
  r->owner = NULL;
  spin_unlock_irqrestore(&r->sl, flags);

  /* 优先恢复到进入 PCP 前的保存值；若未设置则退化为当前优先级 */
  restore = tcb->frap_saved_prio != 0
              ? tcb->frap_saved_prio
              : (uint8_t)tcb->sched_priority;

  frap_set_prio(tcb, restore);
  tcb->frap_saved_prio = 0;
}

#endif /* CONFIG_FRAP */
