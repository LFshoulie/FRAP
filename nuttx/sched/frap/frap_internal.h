/****************************************************************************
 * Name: frap_internal.h
 *
 * Description:
 *   FRAP 私有内部接口，仅在 sched/frap 目录下的实现文件中使用。
 *
 ****************************************************************************/

#pragma once

#include <nuttx/config.h>

#ifdef CONFIG_FRAP

#include "sched/sched.h"
#include <nuttx/list.h>
#include <nuttx/spinlock.h>
#include <nuttx/sched.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/frap.h>
#include "frap_compat.h"


/* 所有 FRAP 相关的 per-task 状态现在直接嵌入在 struct tcb_s 中，
 * 参见 sched/sched.h 中的 CONFIG_FRAP 字段：
 *
 *   FAR struct frap_res *frap_waiting_res;
 *   struct list_node     frap_waiter_node;
 *   uint8_t              frap_base_prio;
 *   uint8_t              frap_spin_prio;
 *   uint8_t              frap_saved_prio;
 *   bool                 frap_in_cs;
 *   bool                 frap_enqueued;
 *   bool                 frap_cancelled;
 */

/* queue helpers: 实现对 r->fifo 的 FIFO 操作。
 * 约定：调用者必须在进入这些函数前持有 r->sl。
 */

void frap_queue_init(FAR struct frap_res *r);
void frap_queue_enqueue_tail(FAR struct frap_res *r, FAR struct tcb_s *tcb);
void frap_queue_enqueue_head_if_needed(FAR struct frap_res *r,
                                       FAR struct tcb_s *tcb);
void frap_queue_remove(FAR struct frap_res *r, FAR struct tcb_s *tcb);
FAR struct tcb_s *frap_queue_peek_head(FAR struct frap_res *r);

#endif /* CONFIG_FRAP */
