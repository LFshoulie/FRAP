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
  /* 等待标志：1=继续等待，0=被唤醒/可尝试获取资源 */
  tcb->frap_wait_lock   = 1;

  DEBUGASSERT(!tcb->frap_enqueued);

  /* R1: 将任务优先级提升到自旋优先级 P_i^k */

  frap_set_prio(tcb, spin_prio);

  for (;;)
    {
      /*
       * MCS 风格等待：
       * - 队列/owner 的检查、入队/出队、唤醒后继仅在 r->sl 保护下发生
       * - 等待阶段仅自旋本地标志 frap_wait_lock（可被抢占）
       */

      flags = spin_lock_irqsave(&r->sl);

      /* 若尚未入队：尝试快速获取；否则一次性入队并设置本地等待标志 */

      if (!tcb->frap_enqueued)
        {
          FAR struct tcb_s *head = frap_queue_peek_head(r);

          if (r->owner == NULL && head == NULL)
            {
              /* 无 owner 且队列为空：直接占有资源 */
              r->owner = tcb;

              /* R2: 非抢占执行临界段（同核不可被更高优先级打断） */
              sched_lock();
              tcb->frap_in_cs = true;

              spin_unlock_irqrestore(&r->sl, flags);
              return OK;
            }

          /* 入队：默认等待，后继由前驱/释放者唤醒 */
          tcb->frap_wait_lock = 1;
          frap_queue_enqueue_tail(r, tcb);

          /* 若资源空闲且自己成为队头，则允许立即尝试获取 */
          if (r->owner == NULL && frap_queue_peek_head(r) == tcb)
            {
              tcb->frap_wait_lock = 0;
            }
        }
      else
        {
          /* 兜底：若资源空闲且自己是队头，确保被允许尝试 */
          if (r->owner == NULL && frap_queue_peek_head(r) == tcb)
            {
              tcb->frap_wait_lock = 0;
            }

        }

      spin_unlock_irqrestore(&r->sl, flags);

      /* 等待阶段：仅自旋本地标志，避免所有等待者反复争抢 r->sl */

      while (tcb->frap_wait_lock != 0)
        {
          if (tcb->frap_cancelled)
            {
              break;
            }

          sched_yield();
        }

      if (tcb->frap_cancelled)
        {
          /* 被更高优先级任务抢占：已由 frap_on_preempt 出队并降回 base */
          tcb->frap_cancelled = false;
          tcb->frap_wait_lock = 1;

          /* 重新进入自旋阶段：恢复到自旋优先级 */
          frap_set_prio(tcb, tcb->frap_spin_prio);
          continue;
        }

      /* 被唤醒：尝试获取资源（短临界区） */

      flags = spin_lock_irqsave(&r->sl);

      if (tcb->frap_enqueued && r->owner == NULL &&
          frap_queue_peek_head(r) == tcb)
        {
          frap_queue_remove(r, tcb);
          r->owner = tcb;

          sched_lock();
          tcb->frap_in_cs = true;

          spin_unlock_irqrestore(&r->sl, flags);
          return OK;
        }

      /* 未能进入：回到等待状态（后续由释放者/抢占钩子重新唤醒） */
      tcb->frap_wait_lock = 1;
      spin_unlock_irqrestore(&r->sl, flags);

      sched_yield();
    }
}

/****************************************************************************
 * Name: frap_unlock
 *
 * 对应 frap_lock 的解锁操作。
 ****************************************************************************/

void frap_unlock(struct frap_res *r)
{
  struct tcb_s *tcb = this_task();
  irqstate_t flags;
  FAR struct tcb_s *head;

  DEBUGASSERT(r->owner == tcb && tcb->frap_in_cs);

  /* 先释放资源（仍处于 sched_lock 保护下） */
  flags = spin_lock_irqsave(&r->sl);
  r->owner = NULL;

  /* 唤醒队头：前驱/释放者通知后继（MCS 思想） */
  head = frap_queue_peek_head(r);
  if (head != NULL)
    {
      head->frap_wait_lock = 0;
    }

  spin_unlock_irqrestore(&r->sl, flags);

  /* 再退出非抢占区 */
  tcb->frap_in_cs = false;
  sched_unlock();

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
