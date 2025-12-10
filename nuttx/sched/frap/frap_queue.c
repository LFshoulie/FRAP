/* sched/frap/frap_queue.c */

#include <nuttx/config.h>
#include "sched/sched.h"
#include <debug.h>

#ifdef CONFIG_FRAP

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <nuttx/list.h>
#include "frap_internal.h"

/* 初始化每个资源的 FIFO 队列与自旋锁。
 * 通常由 frap_res_init() 间接调用。
 */

void frap_queue_init(FAR struct frap_res *r)
{
  DEBUGASSERT(r != NULL);
  r->sl = SP_UNLOCKED;
  list_initialize(&r->fifo);
}

/* 将任务插入等待 FIFO 的尾部（如果尚未在队列中）。 */

void frap_queue_enqueue_tail(FAR struct frap_res *r, FAR struct tcb_s *tcb)
{
  DEBUGASSERT(r != NULL && tcb != NULL);

  if (!tcb->frap_enqueued)
    {
      list_add_tail(&r->fifo, &tcb->frap_waiter_node);
      tcb->frap_enqueued = true;
    }
}

/* 如果该任务尚未在队列中，将其插入队头。
 * 用于“空队列首次请求”和“已经是队头”的场景。
 */

void frap_queue_enqueue_head_if_needed(FAR struct frap_res *r,
                                       FAR struct tcb_s *tcb)
{
  DEBUGASSERT(r != NULL && tcb != NULL);

  if (!tcb->frap_enqueued)
    {
      list_add_head(&r->fifo, &tcb->frap_waiter_node);
      tcb->frap_enqueued = true;
    }
}

/* 从 FIFO 移除指定任务（如果在队列中）。 */

void frap_queue_remove(FAR struct frap_res *r, FAR struct tcb_s *tcb)
{
  DEBUGASSERT(r != NULL && tcb != NULL);

  if (tcb->frap_enqueued)
    {
      list_delete(&tcb->frap_waiter_node);
      tcb->frap_enqueued = false;
    }
}

/* 取得队头任务（如果有）。 */

FAR struct tcb_s *frap_queue_peek_head(FAR struct frap_res *r)
{
  DEBUGASSERT(r != NULL);

  return list_peek_head_type(&r->fifo, struct tcb_s, frap_waiter_node);
}

#endif /* CONFIG_FRAP */
