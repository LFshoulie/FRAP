/****************************************************************************
 * apps/system/frapdemo/frapdemo_main.c
 *
 * FRAP demo: rbtree + TLSF + matmul, 16 tasks, 3 resources
 *
 * 任务集与自旋优先级表：
 *   - 任务集严格对齐 JSON:
 *       rb_0..rb_3, tlsf_0..tlsf_3, mat_0..mat_3, god_0..god_3
 *   - 自旋优先级表由脚本生成：frap_table_generated.h
 *
 * 资源含义：
 *   RES_RBTREE (0): 红黑树插入+查找
 *   RES_TLSF   (1): TLSF malloc/free 批量分配与释放
 *   RES_MATMUL (2): 8x8 矩阵乘法
 *
 * 新增指标：
 *   - Per-thread throughput: ops/s, iter/s, run time
 *   - Global throughput: ops/s, iter/s
 *   - Wait/Hold/Total latency: avg/max (ms), per-thread + per-resource
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_FRAP

#include <nuttx/frap.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "rbtree.h"
#include "tlsf.h"
#include "frap_table_generated.h"

/* 来自 frap_lock.c 的辅助统计函数（你现有工程里已经有） */
extern uint32_t frap_get_queue_preempt_count(void);

/* -------------------------------------------------------------------------- */
/* 常量配置                                                                   */
/* -------------------------------------------------------------------------- */

#define WORKER_NUM            16
#define RESOURCE_NUM          3

#define RES_RBTREE            0
#define RES_TLSF              1
#define RES_MATMUL            2

#define TARGET_ITERATIONS     10000
#define DEADLOCK_THRESHOLD_MS 300000

#define WORKER_STACKSIZE      (8 * 1024)

/* workload 规模：控制临界区时间大约 CS_TARGET_MS 左右 */
#define RB_NODES              32
#define MAT_SIZE              8
#define TLSF_POOL_SIZE        (64 * 1024)
#define TLSF_BATCH_ALLOCS     32
#define CS_TARGET_MS          100   /* 想 50ms 就写 50 */

/* -------------------------------------------------------------------------- */
/* 自旋优先级表（由脚本生成，见 frap_table_generated.h）                        */
/* -------------------------------------------------------------------------- */

/* 每个 worker 在每个资源上的自旋优先级（从表加载，只读使用） */
static int g_loaded_spin_prio[WORKER_NUM][RESOURCE_NUM];

/* -------------------------------------------------------------------------- */
/* FRAP 资源 / TLSF / RBTREE / 矩阵数据                                       */
/* -------------------------------------------------------------------------- */

/* FRAP 资源句柄 */
static struct frap_res g_res[RESOURCE_NUM];

/* RBTREE：每个线程一棵独立的树 */
struct rb_data_node
{
  struct rb_node node;
  int            key;
  int            val;
};

static struct rb_root      g_rb_root[WORKER_NUM];
static struct rb_data_node g_rb_nodes[WORKER_NUM][RB_NODES];

/* TLSF 全局堆（由 RES_TLSF 保护） */
static uint8_t g_tlsf_pool[TLSF_POOL_SIZE];
static tlsf_t  g_tlsf;

/* 矩阵数据：每个线程独立 A/B/C */
static int g_mat_a[WORKER_NUM][MAT_SIZE][MAT_SIZE];
static int g_mat_b[WORKER_NUM][MAT_SIZE][MAT_SIZE];
static int g_mat_c[WORKER_NUM][MAT_SIZE][MAT_SIZE];

/* -------------------------------------------------------------------------- */
/* 统计与监控                                                                 */
/* -------------------------------------------------------------------------- */

struct worker_stat_s
{
  /* 基本计数 */
  uint64_t iters;          /* 外层循环次数（TARGET_ITERATIONS） */
  uint64_t ops;            /* 锁/临界区次数（god_* 每轮 3 次） */
  uint64_t ops_res[RESOURCE_NUM];

  /* FRAP 行为统计 */
  uint64_t preempt_count;  /* cancel-on-preempt 次数（由全局计数器差分获得） */

  /* 正确性/异常 */
  uint64_t data_errors;    /* 校验/一致性错误 */
  uint64_t deadlock_count; /* watchdog 认为的“卡死/未完成”次数 */

  /* 时延（毫秒） */
  uint64_t wait_sum_ms[RESOURCE_NUM]; /* frap_lock() 等待/自旋时间 */
  uint64_t wait_max_ms[RESOURCE_NUM];

  uint64_t hold_sum_ms[RESOURCE_NUM]; /* 持锁执行时间（不含等待） */
  uint64_t hold_max_ms[RESOURCE_NUM];

  uint64_t total_sum_ms[RESOURCE_NUM];/* wait+hold+unlock 的总耗时 */
  uint64_t total_max_ms[RESOURCE_NUM];

  /* 兼容旧字段：打印用 */
  uint64_t max_time_ms;    /* 所有资源里单次 total_ms 最大值 */
  uint64_t baseline_ms;    /* 校准后的 busy-wait 基线 */

  /* 运行环境 */
  int      cpu_id;
};

static volatile uint64_t g_thread_start_ms[WORKER_NUM];
static volatile uint64_t g_thread_end_ms[WORKER_NUM];

static volatile uint64_t g_last_seen_ms[WORKER_NUM];
static volatile int      g_progress[WORKER_NUM];
static volatile bool     g_worker_done[WORKER_NUM];

static struct worker_stat_s g_stats[WORKER_NUM];

/* watchdog 用的标记 */
static bool     g_deadlock_reported[WORKER_NUM];
static uint64_t g_deadlock_first_seen_ms[WORKER_NUM];

static bool     g_start_gun = false;
static int      g_num_cpus  = 1;

/* 校准后用于“约 CS_TARGET_MS” busy-wait 的循环次数 */
static int      g_spin_loops_20ms = 500;
static uint64_t g_baseline_ms     = 1;

/* -------------------------------------------------------------------------- */
/* 小工具函数                                                                 */
/* -------------------------------------------------------------------------- */

static uint64_t time_ms(FAR struct timespec *ts)
{
  return (uint64_t)ts->tv_sec * 1000ull + (uint64_t)ts->tv_nsec / 1000000ull;
}

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return time_ms(&ts);
}

static void heartbeat(int id)
{
  g_last_seen_ms[id] = now_ms();
}

/* 绑核：按任务集 JSON 里的 cpu 字段 0/1 */
static void pin_to_cpu(int cpu)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPUAFFINITY)
  cpu_set_t set;

  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);
#else
  (void)cpu;
#endif
}

/* 简单忙等：loops 越大耗时越长 */
static void spin_loops(int loops)
{
  volatile int k = 0;

  for (int i = 0; i < loops * 10000; i++)
    {
      k++;
    }

  (void)k;
}

/* 约 CS_TARGET_MS 的 busy-wait，由 g_spin_loops_20ms 控制 */
static void busy_wait_approx_20ms(void)
{
  spin_loops(g_spin_loops_20ms);
}

/* 从自旋表加载到二维数组 [pid_hint][resid] */
static void load_frap_table(void)
{
  memset((void *)g_loaded_spin_prio, 0, sizeof(g_loaded_spin_prio));

  for (int i = 0; i < frap_generated_table_len; i++)
    {
      const struct frap_cfg_entry *e = &frap_generated_table[i];

      if (e->pid_hint >= 0 && e->pid_hint < WORKER_NUM &&
          e->resid    >= 0 && e->resid    < RESOURCE_NUM)
        {
          g_loaded_spin_prio[e->pid_hint][e->resid] = e->spin_prio;
        }
    }
}

static void update_stats(int id, int resid,
                         uint64_t wait_ms, uint64_t hold_ms, uint64_t total_ms)
{
  struct worker_stat_s *st = &g_stats[id];

  st->ops++;
  if (resid >= 0 && resid < RESOURCE_NUM)
    {
      st->ops_res[resid]++;

      st->wait_sum_ms[resid] += wait_ms;
      if (wait_ms > st->wait_max_ms[resid])
        {
          st->wait_max_ms[resid] = wait_ms;
        }

      st->hold_sum_ms[resid] += hold_ms;
      if (hold_ms > st->hold_max_ms[resid])
        {
          st->hold_max_ms[resid] = hold_ms;
        }

      st->total_sum_ms[resid] += total_ms;
      if (total_ms > st->total_max_ms[resid])
        {
          st->total_max_ms[resid] = total_ms;
        }
    }

  if (total_ms > st->max_time_ms)
    {
      st->max_time_ms = total_ms;
    }

  heartbeat(id);
}

static void calibrate_baseline(void)
{
  struct timespec ts1;
  struct timespec ts2;

  /* 第一步：粗略测量一次 spin_loops(10) 的时间，得到单位成本 dt_unit */
  clock_gettime(CLOCK_MONOTONIC, &ts1);
  spin_loops(10);
  clock_gettime(CLOCK_MONOTONIC, &ts2);

  uint64_t dt_unit = time_ms(&ts2) - time_ms(&ts1);
  if (dt_unit == 0)
    {
      dt_unit = 1;
    }

  /* 根据 dt_unit 粗略估算初始 loops_for_target */
  int loops_for_target = (int)((int64_t)10 * CS_TARGET_MS / (int64_t)dt_unit);
  if (loops_for_target <= 0)
    {
      loops_for_target = 1;
    }

  g_spin_loops_20ms = loops_for_target;

  /* 第二步：用这个初始值实际跑一次，测实测时间 dt_cs */
  clock_gettime(CLOCK_MONOTONIC, &ts1);
  busy_wait_approx_20ms();
  clock_gettime(CLOCK_MONOTONIC, &ts2);

  uint64_t dt_cs = time_ms(&ts2) - time_ms(&ts1);
  if (dt_cs == 0)
    {
      dt_cs = 1;
    }

  /* 第三步：按实测时间再缩放一次 loops，使得 time ~= CS_TARGET_MS */
  g_spin_loops_20ms = (int)((int64_t)g_spin_loops_20ms * CS_TARGET_MS / (int64_t)dt_cs);
  if (g_spin_loops_20ms <= 0)
    {
      g_spin_loops_20ms = 1;
    }

  /* 第四步：用调整后的 loops 再测一次，作为最终 baseline */
  clock_gettime(CLOCK_MONOTONIC, &ts1);
  busy_wait_approx_20ms();
  clock_gettime(CLOCK_MONOTONIC, &ts2);

  uint64_t dt_final = time_ms(&ts2) - time_ms(&ts1);
  if (dt_final == 0)
    {
      dt_final = 1;
    }

  g_baseline_ms = dt_final;

  for (int i = 0; i < WORKER_NUM; i++)
    {
      g_stats[i].baseline_ms = g_baseline_ms;
    }

  printf("[FRAP] Calibration: spin_loops(10) ~%llu ms, "
         "target_cs=%d ms, loops_for_target=%d, final_cs~%llu ms\n",
         (unsigned long long)dt_unit,
         CS_TARGET_MS,
         g_spin_loops_20ms,
         (unsigned long long)g_baseline_ms);
}

/* -------------------------------------------------------------------------- */
/* RBTREE 临界区（资源 0）                                                    */
/* -------------------------------------------------------------------------- */

static void logic_rbtree_cs(int id)
{
  struct timespec t0;
  struct timespec t1;
  struct timespec t2;
  struct timespec t3;
  uint32_t        cnt_before;
  uint32_t        cnt_after;

  heartbeat(id);

  /* 设置自旋优先级（只读使用表，不改数值） */
  if (g_loaded_spin_prio[id][RES_RBTREE] > 0)
    {
      frap_set_spin_prio((int8_t)g_loaded_spin_prio[id][RES_RBTREE]);
    }

  cnt_before = frap_get_queue_preempt_count();

  clock_gettime(CLOCK_MONOTONIC, &t0);
  frap_lock(&g_res[RES_RBTREE]);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  cnt_after = frap_get_queue_preempt_count();
  if (cnt_after > cnt_before)
    {
      g_stats[id].preempt_count += (cnt_after - cnt_before);
    }

  /* -------------------- 临界区：RBTREE 插入/查找 -------------------- */

  struct rb_root *root = &g_rb_root[id];
  root->rb_node = NULL;

  for (int i = 0; i < RB_NODES; i++)
    {
      struct rb_data_node *n = &g_rb_nodes[id][i];
      n->key = id * 1000 + i;
      n->val = id;

      struct rb_node **newn   = &root->rb_node;
      struct rb_node  *parent = NULL;

      while (*newn)
        {
          struct rb_data_node *this = rb_entry(*newn, struct rb_data_node, node);

          parent = *newn;
          if (n->key < this->key)
            {
              newn = &((*newn)->rb_child[RB_LEFT]);
            }
          else if (n->key > this->key)
            {
              newn = &((*newn)->rb_child[RB_RIGHT]);
            }
          else
            {
              /* 覆盖 */
              this->val = n->val;
              parent = NULL;
              break;
            }
        }

      if (parent != NULL || *newn == NULL)
        {
          rb_link_node(&n->node, parent, newn);
          rb_insert_color(&n->node, root);
        }
    }

  /* 做几次查找并验证 val 是否为当前线程 id */
  for (int i = 0; i < RB_NODES; i += RB_NODES / 4)
    {
      int target = id * 1000 + i;
      struct rb_node      *node  = root->rb_node;
      struct rb_data_node *found = NULL;

      while (node)
        {
          struct rb_data_node *d = rb_entry(node, struct rb_data_node, node);

          if (target < d->key)
            {
              node = d->node.rb_child[RB_LEFT];
            }
          else if (target > d->key)
            {
              node = d->node.rb_child[RB_RIGHT];
            }
          else
            {
              found = d;
              break;
            }
        }

      if (!found || found->val != id)
        {
          g_stats[id].data_errors++;
        }
    }

  /* 补一点 busy-wait，把持锁时间拉到 CS_TARGET_MS 左右 */
  busy_wait_approx_20ms();

  clock_gettime(CLOCK_MONOTONIC, &t2);
  frap_unlock(&g_res[RES_RBTREE]);
  clock_gettime(CLOCK_MONOTONIC, &t3);

  uint64_t wait_ms  = time_ms(&t1) - time_ms(&t0);
  uint64_t hold_ms  = time_ms(&t2) - time_ms(&t1);
  uint64_t total_ms = time_ms(&t3) - time_ms(&t0);

  update_stats(id, RES_RBTREE, wait_ms, hold_ms, total_ms);
}

/* -------------------------------------------------------------------------- */
/* TLSF 临界区（资源 1）                                                      */
/* -------------------------------------------------------------------------- */

static void logic_tlsf_cs(int id)
{
  struct timespec t0;
  struct timespec t1;
  struct timespec t2;
  struct timespec t3;
  uint32_t        cnt_before;
  uint32_t        cnt_after;

  heartbeat(id);

  if (g_loaded_spin_prio[id][RES_TLSF] > 0)
    {
      frap_set_spin_prio((int8_t)g_loaded_spin_prio[id][RES_TLSF]);
    }

  cnt_before = frap_get_queue_preempt_count();

  clock_gettime(CLOCK_MONOTONIC, &t0);
  frap_lock(&g_res[RES_TLSF]);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  cnt_after = frap_get_queue_preempt_count();
  if (cnt_after > cnt_before)
    {
      g_stats[id].preempt_count += (cnt_after - cnt_before);
    }

  /* -------------------- 临界区：TLSF malloc/free -------------------- */

  void *ptrs[TLSF_BATCH_ALLOCS];

  for (int i = 0; i < TLSF_BATCH_ALLOCS; i++)
    {
      size_t sz = 32 + ((id + i * 17) & 0xff); /* 32..287 字节 */
      void  *p  = tlsf_malloc(g_tlsf, sz);
      ptrs[i]   = p;

      if (!p)
        {
          g_stats[id].data_errors++;
          break;
        }

      memset(p, (unsigned char)(id + i), sz > 16 ? 16 : sz);
    }

  /* 读一些数据，避免被优化掉 */
  for (int i = 0; i < TLSF_BATCH_ALLOCS; i += 8)
    {
      if (ptrs[i])
        {
          volatile unsigned char v = *((unsigned char *)ptrs[i]);
          (void)v;
        }
    }

  for (int i = 0; i < TLSF_BATCH_ALLOCS; i++)
    {
      if (ptrs[i])
        {
          tlsf_free(g_tlsf, ptrs[i]);
        }
    }

  busy_wait_approx_20ms();

  clock_gettime(CLOCK_MONOTONIC, &t2);
  frap_unlock(&g_res[RES_TLSF]);
  clock_gettime(CLOCK_MONOTONIC, &t3);

  uint64_t wait_ms  = time_ms(&t1) - time_ms(&t0);
  uint64_t hold_ms  = time_ms(&t2) - time_ms(&t1);
  uint64_t total_ms = time_ms(&t3) - time_ms(&t0);

  update_stats(id, RES_TLSF, wait_ms, hold_ms, total_ms);
}

/* -------------------------------------------------------------------------- */
/* 矩阵乘法临界区（资源 2）                                                   */
/* -------------------------------------------------------------------------- */

static void logic_matmul_cs(int id)
{
  struct timespec t0;
  struct timespec t1;
  struct timespec t2;
  struct timespec t3;
  uint32_t        cnt_before;
  uint32_t        cnt_after;

  heartbeat(id);

  if (g_loaded_spin_prio[id][RES_MATMUL] > 0)
    {
      frap_set_spin_prio((int8_t)g_loaded_spin_prio[id][RES_MATMUL]);
    }

  cnt_before = frap_get_queue_preempt_count();

  clock_gettime(CLOCK_MONOTONIC, &t0);
  frap_lock(&g_res[RES_MATMUL]);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  cnt_after = frap_get_queue_preempt_count();
  if (cnt_after > cnt_before)
    {
      g_stats[id].preempt_count += (cnt_after - cnt_before);
    }

  /* -------------------- 临界区：8x8 矩阵乘法 -------------------- */

  /* 初始化矩阵 */
  for (int i = 0; i < MAT_SIZE; i++)
    {
      for (int j = 0; j < MAT_SIZE; j++)
        {
          g_mat_a[id][i][j] = (i + j + id) & 7;
          g_mat_b[id][i][j] = (i * 2 + j + 3) & 7;
          g_mat_c[id][i][j] = 0;
        }
    }

  for (int i = 0; i < MAT_SIZE; i++)
    {
      for (int j = 0; j < MAT_SIZE; j++)
        {
          int sum = 0;

          for (int k = 0; k < MAT_SIZE; k++)
            {
              sum += g_mat_a[id][i][k] * g_mat_b[id][k][j];
            }

          g_mat_c[id][i][j] = sum;
        }
    }

  if (g_mat_c[id][0][0] == 0)
    {
      g_stats[id].data_errors++;
    }

  busy_wait_approx_20ms();

  clock_gettime(CLOCK_MONOTONIC, &t2);
  frap_unlock(&g_res[RES_MATMUL]);
  clock_gettime(CLOCK_MONOTONIC, &t3);

  uint64_t wait_ms  = time_ms(&t1) - time_ms(&t0);
  uint64_t hold_ms  = time_ms(&t2) - time_ms(&t1);
  uint64_t total_ms = time_ms(&t3) - time_ms(&t0);

  update_stats(id, RES_MATMUL, wait_ms, hold_ms, total_ms);
}

/* -------------------------------------------------------------------------- */
/* worker 行为                                                                 */
/* -------------------------------------------------------------------------- */

/* 与 JSON 中 cpu 字段保持一致：0/1 交错 */
static const int g_worker_cpu[WORKER_NUM] =
{
  0, 1, 0, 1,  /* rb_0..rb_3   */
  0, 1, 0, 1,  /* tlsf_0..3    */
  0, 1, 0, 1,  /* mat_0..3     */
  0, 1, 0, 1   /* god_0..3     */
};

/* 对应 JSON 中 P：60 / 90 / 120 / 180 */
static const int g_worker_base_prio[WORKER_NUM] =
{
  60, 60, 60, 60,     /* rb_*   P=60  */
  90, 90, 90, 90,     /* tlsf_* P=90  */
  120, 120, 120, 120, /* mat_* P=120  */
  180,180,180,180     /* god_* P=180  */
};

static void logic_rb_only(int id)   { logic_rbtree_cs(id); }
static void logic_tlsf_only(int id) { logic_tlsf_cs(id); }
static void logic_mat_only(int id)  { logic_matmul_cs(id); }

/* god_*：顺序访问 0->1->2，但不嵌套锁，避免真正死锁环 */
static void logic_god(int id)
{
  logic_rbtree_cs(id);
  logic_tlsf_cs(id);
  logic_matmul_cs(id);
}

static void *worker_entry(void *arg)
{
  int id  = (int)(intptr_t)arg;
  int cpu = g_worker_cpu[id];

  pin_to_cpu(cpu);
  g_stats[id].cpu_id = cpu;

  while (!g_start_gun)
    {
      usleep(1000);
    }

  /* 每个线程自己的起始时间（用于吞吐量/用时统计） */
  g_thread_start_ms[id] = now_ms();
  heartbeat(id);

  for (int i = 0; i < TARGET_ITERATIONS; i++)
    {
      g_stats[id].iters++;

      if (id >= 0 && id <= 3)
        {
          /* rb_0..rb_3 -> RES_RBTREE */
          logic_rb_only(id);
        }
      else if (id >= 4 && id <= 7)
        {
          /* tlsf_0..3 -> RES_TLSF */
          logic_tlsf_only(id);
        }
      else if (id >= 8 && id <= 11)
        {
          /* mat_0..3 -> RES_MATMUL */
          logic_mat_only(id);
        }
      else
        {
          /* god_0..3 -> RBTREE -> TLSF -> MATMUL */
          logic_god(id);
        }

      g_progress[id] = i + 1;
      heartbeat(id);
    }

  g_thread_end_ms[id] = now_ms();
  g_worker_done[id]   = true;
  return NULL;
}

/* -------------------------------------------------------------------------- */
/* 主函数：创建线程、watchdog、打印报表                                       */
/* -------------------------------------------------------------------------- */

int frapdemo_main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

#ifdef CONFIG_SMP_NCPUS
  g_num_cpus = CONFIG_SMP_NCPUS;
#else
  g_num_cpus = 1;
#endif

  printf("[FRAP] Scenario: frap_rbtree_tlsf_mat_16_tasks\n");
  printf("CPUs (system): %d, Iterations per worker: %d\n",
         g_num_cpus, TARGET_ITERATIONS);

  memset((void *)g_thread_start_ms,       0, sizeof(g_thread_start_ms));
  memset((void *)g_thread_end_ms,         0, sizeof(g_thread_end_ms));
  memset((void *)g_last_seen_ms,          0, sizeof(g_last_seen_ms));
  memset((void *)g_progress,              0, sizeof(g_progress));
  memset((void *)g_worker_done,           0, sizeof(g_worker_done));
  memset((void *)g_deadlock_reported,     0, sizeof(g_deadlock_reported));
  memset((void *)g_deadlock_first_seen_ms,0, sizeof(g_deadlock_first_seen_ms));
  memset((void *)g_stats,                 0, sizeof(g_stats));

  /* 初始化 RBTREE root */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      g_rb_root[i].rb_node = NULL;
    }

  /* 初始化 TLSF 堆 */
  g_tlsf = tlsf_create_with_pool(g_tlsf_pool, TLSF_POOL_SIZE);
  if (!g_tlsf)
    {
      printf("[FATAL] tlsf_create_with_pool failed.\n");
      return -1;
    }

  /* 初始化 FRAP 资源（0/1/2） */
  for (int i = 0; i < RESOURCE_NUM; i++)
    {
      frap_res_init(&g_res[i], i, true);
    }

  load_frap_table();
  calibrate_baseline();

  /* 简短名称用于打印进度 */
  const char *short_names[WORKER_NUM] =
  {
    "rb0", "rb1", "rb2", "rb3",
    "t0",  "t1",  "t2",  "t3",
    "m0",  "m1",  "m2",  "m3",
    "g0",  "g1",  "g2",  "g3"
  };

  const char *full_names[WORKER_NUM] =
  {
    "rb_0",   "rb_1",   "rb_2",   "rb_3",
    "tlsf_0", "tlsf_1", "tlsf_2", "tlsf_3",
    "mat_0",  "mat_1",  "mat_2",  "mat_3",
    "god_0",  "god_1",  "god_2",  "god_3"
  };

  /* 提高 main 的优先级，避免被 worker 饿死 */
  struct sched_param main_param;
  main_param.sched_priority = 200;
  sched_setscheduler(0, SCHED_FIFO, &main_param);

  pthread_t        threads[WORKER_NUM];
  pthread_attr_t   attr;
  struct sched_param param;

  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, WORKER_STACKSIZE);
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

      param.sched_priority = g_worker_base_prio[i];
      pthread_attr_setschedparam(&attr, &param);

      pthread_create(&threads[i], &attr, worker_entry,
                     (void *)(intptr_t)i);
      pthread_attr_destroy(&attr);
    }

  printf("!!! STARTING !!!\n");
  g_start_gun = true;

  uint64_t start_ms = now_ms();
  bool     all_done = false;

  while (!all_done)
    {
      uint64_t cur = now_ms();
      all_done = true;

      printf("\rProgress: ");
      for (int i = 0; i < WORKER_NUM; i++)
        {
          printf("%s:%d ", short_names[i], g_progress[i]);

          if (!g_worker_done[i])
            {
              all_done = false;

              uint64_t last = g_last_seen_ms[i];
              if (last != 0)
                {
                  uint64_t diff = (cur > last) ? (cur - last) : 0;

                  if (diff > DEADLOCK_THRESHOLD_MS &&
                      !g_deadlock_reported[i])
                    {
                      g_deadlock_reported[i]      = true;
                      g_deadlock_first_seen_ms[i] = cur;
                      g_stats[i].deadlock_count++;

                      printf("\n[FATAL] %s STUCK for %llu ms "
                             "(first seen at +%llu ms)\n",
                             full_names[i],
                             (unsigned long long)diff,
                             (unsigned long long)
                             (g_deadlock_first_seen_ms[i] - start_ms));
                    }
                }
            }
        }

      /* 运行中观察：当前全局吞吐量（ops/s） */
      uint64_t elapsed_ms = (cur > start_ms) ? (cur - start_ms) : 1;
      if (elapsed_ms == 0) elapsed_ms = 1;
      uint64_t live_ops = 0;
      for (int k = 0; k < WORKER_NUM; k++)
        {
          live_ops += g_stats[k].ops;
        }
      uint64_t live_thr_x1000 = (live_ops * 1000000ull) / elapsed_ms;
      printf("| Ops:%llu Thr:%lu.%03lu ops/s",
             (unsigned long long)live_ops,
             (unsigned long)(live_thr_x1000 / 1000ull),
             (unsigned long)(live_thr_x1000 % 1000ull));

      fflush(stdout);

      if (all_done)
        {
          break;
        }

      usleep(1000 * 1000);
    }

  printf("\nAll workers done. Joining...\n");

  int real_deadlocks = 0;

  for (int i = 0; i < WORKER_NUM; i++)
    {
      if (!g_worker_done[i] || g_progress[i] < TARGET_ITERATIONS)
        {
          /* 真正没跑完的，再记一次 deadlock 事件 */
          g_stats[i].deadlock_count++;
          real_deadlocks++;
        }
    }

  if (real_deadlocks > 0)
    {
      printf("[SUMMARY] REAL DEADLOCKS: %d threads did not finish.\n",
             real_deadlocks);
    }
  else
    {
      printf("[SUMMARY] No real deadlocks: all threads completed.\n");
    }

  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_join(threads[i], NULL);
    }

/* 报表：增加吞吐量、等待/持锁时延等指标 */

  uint64_t end_ms = start_ms;
  for (int i = 0; i < WORKER_NUM; i++)
    {
      if (g_thread_end_ms[i] > end_ms)
        {
          end_ms = g_thread_end_ms[i];
        }
    }

  if (end_ms <= start_ms)
    {
      end_ms = now_ms();
    }

  uint64_t wall_ms = end_ms - start_ms;
  if (wall_ms == 0)
    {
      wall_ms = 1;
    }

  printf("\nFRAP demo report (RBTREE + TLSF + MATMUL, 16 tasks)\n");
  printf("===============================================================================================\n");

  /* 汇总 */
  uint64_t total_iters    = 0;
  uint64_t total_ops      = 0;
  uint64_t total_preempt  = 0;
  uint64_t total_dead     = 0;
  uint64_t total_errors   = 0;

  uint64_t total_ops_res[RESOURCE_NUM] = {0};
  uint64_t total_wait_res[RESOURCE_NUM] = {0};
  uint64_t total_hold_res[RESOURCE_NUM] = {0};
  uint64_t total_total_res[RESOURCE_NUM] = {0};
  uint64_t max_wait_res[RESOURCE_NUM] = {0};
  uint64_t max_hold_res[RESOURCE_NUM] = {0};
  uint64_t max_total_res[RESOURCE_NUM] = {0};

  printf("Wall time: %llu ms\n", (unsigned long long)wall_ms);

  /* 表 1：吞吐量/完成情况 */
  printf("\nID Name CPU  Iter    Ops     Thr(ops/s) Thr(iter/s) Preempt  P/Op    DataErr Deadlk  Run(ms)\n");
  printf("-----------------------------------------------------------------------------------------------\n");

  for (int i = 0; i < WORKER_NUM; i++)
    {
      struct worker_stat_s *st = &g_stats[i];

      uint64_t rt_ms = 0;
      if (g_thread_start_ms[i] != 0)
        {
          uint64_t endi = g_thread_end_ms[i] ? g_thread_end_ms[i] : end_ms;
          rt_ms = (endi > g_thread_start_ms[i]) ? (endi - g_thread_start_ms[i]) : 1;
        }
      else
        {
          rt_ms = wall_ms;
        }

      if (rt_ms == 0)
        {
          rt_ms = 1;
        }

      /* ops/s & iter/s（保留 3 位小数） */
      uint64_t thr_ops_x1000  = (st->ops   * 1000000ull) / rt_ms;
      uint64_t thr_iter_x1000 = (st->iters * 1000000ull) / rt_ms;

      unsigned long thr_ops_i  = (unsigned long)(thr_ops_x1000 / 1000ull);
      unsigned long thr_ops_f  = (unsigned long)(thr_ops_x1000 % 1000ull);
      unsigned long thr_it_i   = (unsigned long)(thr_iter_x1000 / 1000ull);
      unsigned long thr_it_f   = (unsigned long)(thr_iter_x1000 % 1000ull);

      /* P/Op（每次锁平均 cancel 次数） */
      uint64_t pop_x1000 = 0;
      if (st->ops > 0)
        {
          pop_x1000 = (st->preempt_count * 1000ull) / st->ops;
        }

      unsigned long pop_i = (unsigned long)(pop_x1000 / 1000ull);
      unsigned long pop_f = (unsigned long)(pop_x1000 % 1000ull);

      printf("%2d %-4s %3d  %-6llu %-6llu %6lu.%03lu   %6lu.%03lu   %-7llu %2lu.%03lu  %-6llu %-6llu %-7llu\n",
             i, short_names[i], st->cpu_id,
             (unsigned long long)st->iters,
             (unsigned long long)st->ops,
             thr_ops_i, thr_ops_f,
             thr_it_i,  thr_it_f,
             (unsigned long long)st->preempt_count,
             pop_i, pop_f,
             (unsigned long long)st->data_errors,
             (unsigned long long)st->deadlock_count,
             (unsigned long long)rt_ms);

      total_iters   += st->iters;
      total_ops     += st->ops;
      total_preempt += st->preempt_count;
      total_dead    += st->deadlock_count;
      total_errors  += st->data_errors;

      for (int r = 0; r < RESOURCE_NUM; r++)
        {
          total_ops_res[r]   += st->ops_res[r];
          total_wait_res[r]  += st->wait_sum_ms[r];
          total_hold_res[r]  += st->hold_sum_ms[r];
          total_total_res[r] += st->total_sum_ms[r];

          if (st->wait_max_ms[r] > max_wait_res[r])   max_wait_res[r] = st->wait_max_ms[r];
          if (st->hold_max_ms[r] > max_hold_res[r])   max_hold_res[r] = st->hold_max_ms[r];
          if (st->total_max_ms[r] > max_total_res[r]) max_total_res[r] = st->total_max_ms[r];
        }
    }

  /* 全局吞吐量 */
  uint64_t gthr_ops_x1000 = (total_ops * 1000000ull) / wall_ms;
  unsigned long gthr_ops_i = (unsigned long)(gthr_ops_x1000 / 1000ull);
  unsigned long gthr_ops_f = (unsigned long)(gthr_ops_x1000 % 1000ull);

  uint64_t gthr_it_x1000 = (total_iters * 1000000ull) / wall_ms;
  unsigned long gthr_it_i = (unsigned long)(gthr_it_x1000 / 1000ull);
  unsigned long gthr_it_f = (unsigned long)(gthr_it_x1000 % 1000ull);

  printf("-----------------------------------------------------------------------------------------------\n");
  printf("TOTAL Iter=%llu, Ops=%llu, Preempt=%llu, Deadlocks=%llu, DataErr=%llu\n",
         (unsigned long long)total_iters,
         (unsigned long long)total_ops,
         (unsigned long long)total_preempt,
         (unsigned long long)total_dead,
         (unsigned long long)total_errors);

  printf("GLOBAL Throughput: %lu.%03lu ops/s, %lu.%03lu iter/s\n",
         gthr_ops_i, gthr_ops_f, gthr_it_i, gthr_it_f);

  /* 表 2：等待/持锁/总耗时（综合所有资源） */
  printf("\nID Name  AvgWait(ms) MaxWait  AvgHold(ms) MaxHold  AvgTot(ms) MaxTot  Base(ms)\n");
  printf("--------------------------------------------------------------------------------\n");

  for (int i = 0; i < WORKER_NUM; i++)
    {
      struct worker_stat_s *st = &g_stats[i];

      uint64_t wsum = 0, hsum = 0, tsum = 0;
      uint64_t wmax = 0, hmax = 0, tmax = 0;

      for (int r = 0; r < RESOURCE_NUM; r++)
        {
          wsum += st->wait_sum_ms[r];
          hsum += st->hold_sum_ms[r];
          tsum += st->total_sum_ms[r];

          if (st->wait_max_ms[r] > wmax)   wmax = st->wait_max_ms[r];
          if (st->hold_max_ms[r] > hmax)   hmax = st->hold_max_ms[r];
          if (st->total_max_ms[r] > tmax)  tmax = st->total_max_ms[r];
        }

      uint64_t avgw_x1000 = 0, avgh_x1000 = 0, avgt_x1000 = 0;
      if (st->ops > 0)
        {
          avgw_x1000 = (wsum * 1000ull) / st->ops;
          avgh_x1000 = (hsum * 1000ull) / st->ops;
          avgt_x1000 = (tsum * 1000ull) / st->ops;
        }

      unsigned long avgw_i = (unsigned long)(avgw_x1000 / 1000ull);
      unsigned long avgw_f = (unsigned long)(avgw_x1000 % 1000ull);
      unsigned long avgh_i = (unsigned long)(avgh_x1000 / 1000ull);
      unsigned long avgh_f = (unsigned long)(avgh_x1000 % 1000ull);
      unsigned long avgt_i = (unsigned long)(avgt_x1000 / 1000ull);
      unsigned long avgt_f = (unsigned long)(avgt_x1000 % 1000ull);

      printf("%2d %-4s %4lu.%03lu    %-7llu %4lu.%03lu    %-7llu %4lu.%03lu   %-7llu %-7llu\n",
             i, short_names[i],
             avgw_i, avgw_f, (unsigned long long)wmax,
             avgh_i, avgh_f, (unsigned long long)hmax,
             avgt_i, avgt_f, (unsigned long long)tmax,
             (unsigned long long)st->baseline_ms);
    }

  /* 表 3：按资源汇总 */
  const char *res_name[RESOURCE_NUM] = { "RBTREE", "TLSF", "MATMUL" };

  printf("\nPer-resource summary (all threads)\n");
  printf("RES     Ops     Thr(ops/s)  AvgWait(ms) MaxWait  AvgHold(ms) MaxHold  AvgTot(ms) MaxTot\n");
  printf("--------------------------------------------------------------------------------------\n");

  for (int r = 0; r < RESOURCE_NUM; r++)
    {
      uint64_t opsr = total_ops_res[r];

      uint64_t thr_x1000 = (opsr * 1000000ull) / wall_ms;
      unsigned long thr_i = (unsigned long)(thr_x1000 / 1000ull);
      unsigned long thr_f = (unsigned long)(thr_x1000 % 1000ull);

      uint64_t avgw_x1000 = 0, avgh_x1000 = 0, avgt_x1000 = 0;
      if (opsr > 0)
        {
          avgw_x1000 = (total_wait_res[r]  * 1000ull) / opsr;
          avgh_x1000 = (total_hold_res[r]  * 1000ull) / opsr;
          avgt_x1000 = (total_total_res[r] * 1000ull) / opsr;
        }

      printf("%-6s %-7llu %6lu.%03lu    %4lu.%03lu    %-7llu %4lu.%03lu    %-7llu %4lu.%03lu   %-7llu\n",
             res_name[r],
             (unsigned long long)opsr,
             thr_i, thr_f,
             (unsigned long)(avgw_x1000 / 1000ull), (unsigned long)(avgw_x1000 % 1000ull),
             (unsigned long long)max_wait_res[r],
             (unsigned long)(avgh_x1000 / 1000ull), (unsigned long)(avgh_x1000 % 1000ull),
             (unsigned long long)max_hold_res[r],
             (unsigned long)(avgt_x1000 / 1000ull), (unsigned long)(avgt_x1000 % 1000ull),
             (unsigned long long)max_total_res[r]);
    }

  printf("===============================================================================================\n");

  if (total_errors == 0)
    {
      printf("[SUCCESS] workload checks passed.\n");
    }
  else
    {
      printf("[FAILURE] data corruption detected.\n");
    }

  /* 同样写入 frap_report.txt */

  FILE *fp = fopen("frap_report.txt", "w");
  if (fp != NULL)
    {
      fprintf(fp, "[FRAP] Scenario: frap_rbtree_tlsf_mat_16_tasks\n");
      fprintf(fp, "CPUs (system): %d, Iterations per worker: %d\n", g_num_cpus, TARGET_ITERATIONS);
      fprintf(fp, "Wall time: %llu ms\n\n", (unsigned long long)wall_ms);

      fprintf(fp, "ID Name CPU  Iter    Ops     Thr(ops/s) Thr(iter/s) Preempt  P/Op    DataErr Deadlk  Run(ms)\n");
      fprintf(fp, "-----------------------------------------------------------------------------------------------\n");

      for (int i = 0; i < WORKER_NUM; i++)
        {
          struct worker_stat_s *st = &g_stats[i];

          uint64_t rt_ms = 0;
          if (g_thread_start_ms[i] != 0)
            {
              uint64_t endi = g_thread_end_ms[i] ? g_thread_end_ms[i] : end_ms;
              rt_ms = (endi > g_thread_start_ms[i]) ? (endi - g_thread_start_ms[i]) : 1;
            }
          else
            {
              rt_ms = wall_ms;
            }

          if (rt_ms == 0) rt_ms = 1;

          uint64_t thr_ops_x1000  = (st->ops   * 1000000ull) / rt_ms;
          uint64_t thr_iter_x1000 = (st->iters * 1000000ull) / rt_ms;

          unsigned long thr_ops_i  = (unsigned long)(thr_ops_x1000 / 1000ull);
          unsigned long thr_ops_f  = (unsigned long)(thr_ops_x1000 % 1000ull);
          unsigned long thr_it_i   = (unsigned long)(thr_iter_x1000 / 1000ull);
          unsigned long thr_it_f   = (unsigned long)(thr_iter_x1000 % 1000ull);

          uint64_t pop_x1000 = 0;
          if (st->ops > 0) pop_x1000 = (st->preempt_count * 1000ull) / st->ops;
          unsigned long pop_i = (unsigned long)(pop_x1000 / 1000ull);
          unsigned long pop_f = (unsigned long)(pop_x1000 % 1000ull);

          fprintf(fp, "%2d %-4s %3d  %-6llu %-6llu %6lu.%03lu   %6lu.%03lu   %-7llu %2lu.%03lu  %-6llu %-6llu %-7llu\n",
                  i, short_names[i], st->cpu_id,
                  (unsigned long long)st->iters,
                  (unsigned long long)st->ops,
                  thr_ops_i, thr_ops_f,
                  thr_it_i,  thr_it_f,
                  (unsigned long long)st->preempt_count,
                  pop_i, pop_f,
                  (unsigned long long)st->data_errors,
                  (unsigned long long)st->deadlock_count,
                  (unsigned long long)rt_ms);
        }

      fprintf(fp, "-----------------------------------------------------------------------------------------------\n");
      fprintf(fp, "TOTAL Iter=%llu, Ops=%llu, Preempt=%llu, Deadlocks=%llu, DataErr=%llu\n",
              (unsigned long long)total_iters,
              (unsigned long long)total_ops,
              (unsigned long long)total_preempt,
              (unsigned long long)total_dead,
              (unsigned long long)total_errors);

      fprintf(fp, "GLOBAL Throughput: %lu.%03lu ops/s, %lu.%03lu iter/s\n\n",
              gthr_ops_i, gthr_ops_f, gthr_it_i, gthr_it_f);

      fprintf(fp, "ID Name  AvgWait(ms) MaxWait  AvgHold(ms) MaxHold  AvgTot(ms) MaxTot  Base(ms)\n");
      fprintf(fp, "--------------------------------------------------------------------------------\n");

      for (int i = 0; i < WORKER_NUM; i++)
        {
          struct worker_stat_s *st = &g_stats[i];
          uint64_t wsum = 0, hsum = 0, tsum = 0;
          uint64_t wmax = 0, hmax = 0, tmax = 0;

          for (int r = 0; r < RESOURCE_NUM; r++)
            {
              wsum += st->wait_sum_ms[r];
              hsum += st->hold_sum_ms[r];
              tsum += st->total_sum_ms[r];

              if (st->wait_max_ms[r] > wmax)  wmax = st->wait_max_ms[r];
              if (st->hold_max_ms[r] > hmax)  hmax = st->hold_max_ms[r];
              if (st->total_max_ms[r] > tmax) tmax = st->total_max_ms[r];
            }

          uint64_t avgw_x1000 = 0, avgh_x1000 = 0, avgt_x1000 = 0;
          if (st->ops > 0)
            {
              avgw_x1000 = (wsum * 1000ull) / st->ops;
              avgh_x1000 = (hsum * 1000ull) / st->ops;
              avgt_x1000 = (tsum * 1000ull) / st->ops;
            }

          fprintf(fp, "%2d %-4s %4lu.%03lu    %-7llu %4lu.%03lu    %-7llu %4lu.%03lu   %-7llu %-7llu\n",
                  i, short_names[i],
                  (unsigned long)(avgw_x1000 / 1000ull), (unsigned long)(avgw_x1000 % 1000ull),
                  (unsigned long long)wmax,
                  (unsigned long)(avgh_x1000 / 1000ull), (unsigned long)(avgh_x1000 % 1000ull),
                  (unsigned long long)hmax,
                  (unsigned long)(avgt_x1000 / 1000ull), (unsigned long)(avgt_x1000 % 1000ull),
                  (unsigned long long)tmax,
                  (unsigned long long)st->baseline_ms);
        }

      fprintf(fp, "\nPer-resource summary (all threads)\n");
      fprintf(fp, "RES     Ops     Thr(ops/s)  AvgWait(ms) MaxWait  AvgHold(ms) MaxHold  AvgTot(ms) MaxTot\n");
      fprintf(fp, "--------------------------------------------------------------------------------------\n");

      for (int r = 0; r < RESOURCE_NUM; r++)
        {
          uint64_t opsr = total_ops_res[r];

          uint64_t thr_x1000 = (opsr * 1000000ull) / wall_ms;

          uint64_t avgw_x1000 = 0, avgh_x1000 = 0, avgt_x1000 = 0;
          if (opsr > 0)
            {
              avgw_x1000 = (total_wait_res[r]  * 1000ull) / opsr;
              avgh_x1000 = (total_hold_res[r]  * 1000ull) / opsr;
              avgt_x1000 = (total_total_res[r] * 1000ull) / opsr;
            }

          fprintf(fp, "%-6s %-7llu %6lu.%03lu    %4lu.%03lu    %-7llu %4lu.%03lu    %-7llu %4lu.%03lu   %-7llu\n",
                  res_name[r],
                  (unsigned long long)opsr,
                  (unsigned long)(thr_x1000 / 1000ull), (unsigned long)(thr_x1000 % 1000ull),
                  (unsigned long)(avgw_x1000 / 1000ull), (unsigned long)(avgw_x1000 % 1000ull),
                  (unsigned long long)max_wait_res[r],
                  (unsigned long)(avgh_x1000 / 1000ull), (unsigned long)(avgh_x1000 % 1000ull),
                  (unsigned long long)max_hold_res[r],
                  (unsigned long)(avgt_x1000 / 1000ull), (unsigned long)(avgt_x1000 % 1000ull),
                  (unsigned long long)max_total_res[r]);
        }

      if (total_errors == 0)
        {
          fprintf(fp, "\n[SUCCESS] workload checks passed.\n");
        }
      else
        {
          fprintf(fp, "\n[FAILURE] data corruption detected.\n");
        }

      fclose(fp);
      printf("[INFO] Report saved to ./frap_report.txt\n");
    }
  else
    {
      printf("[WARN] Failed to open frap_report.txt for writing.\n");
    }

  return 0;
}

#else  /* !CONFIG_FRAP */

int frapdemo_main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  printf("FRAP demo disabled: CONFIG_FRAP not enabled.\n");
  return 0;
}

#endif /* CONFIG_FRAP */