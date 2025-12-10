/* include/nuttx/frap.h
 *
 * Finite-resource-aware spin protocol (FRAP) public interface.
 */

#pragma once

#include <nuttx/config.h>

#ifdef CONFIG_FRAP

#include <stdint.h>
#include <stdbool.h>
#include <nuttx/list.h>
#include <nuttx/spinlock.h>
#include <nuttx/sched.h>

/* 资源描述符：在系统初始化阶段静态或动态初始化 */
struct frap_res
{
  /* 短临界区：保护 owner/fifo 的自旋锁（每个资源一个） */
  spinlock_t        sl;

  /* 当前占有该资源的任务（临界区内非 NULL） */
  FAR struct tcb_s *owner;

  /* 等待该资源的任务 FIFO（使用 list.h 的循环双向链表） */
  struct list_node  fifo;

  /* 调试/统计用 ID（由上层赋值，唯一即可） */
  uint32_t          id;

  /* 是否为“全局资源”：全局资源使用 FRAP 自旋协议；
   * 否则使用本地 PCP 变体（仅在本核内提升到 ceiling）
   */
  bool              is_global;

  /* 本地 PCP 使用的优先级上限（ceiling） */
  uint8_t           ceiling;
};

/* API：初始化资源。
 *
 *  - r        : 资源对象（由调用方提供存储）
 *  - id       : 调试 ID
 *  - is_global: true  => 使用 FRAP 全局自旋协议
 *               false => 使用本地 PCP 变体
 */
int frap_res_init(FAR struct frap_res *r, uint32_t id, bool is_global);

/* FRAP 全局资源加锁/解锁
 *
 * 调用约定：
 *   frap_lock(r, spin_prio);
 *   // 进入非抢占临界段，直到 frap_unlock 之前不会被同核抢占
 *   ... critical section ...
 *   frap_unlock(r);
 */
int  frap_lock(FAR struct frap_res *r);
void frap_unlock(FAR struct frap_res *r);

/* 本地 PCP 变体（不跨核共享资源时可使用） */
int  frap_local_lock(FAR struct frap_res *r, uint8_t ceiling);
void frap_local_unlock(FAR struct frap_res *r);

/* 由调度器在抢占发生时调用（见 frap_schedhook.c） */
void frap_on_preempt(FAR struct tcb_s *oldtcb, FAR struct tcb_s *newtcb);

/* 根据 tid + resid 绑定/查询自旋优先级 */
int frap_set_spin_prio(int8_t spin_prio);
int frap_get_spin_prio(void);

#endif /* CONFIG_FRAP */
