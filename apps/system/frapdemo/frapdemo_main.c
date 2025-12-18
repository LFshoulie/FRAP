/****************************************************************************
 * apps/system/frapdemo/frapdemo_main.c
 *
 * FRAP demo (offline spin-priority table) + throughput comparison against
 * NuttX built-in spinlock (spin_lock_irqsave/spin_unlock_irqrestore).
 *
 * How to use:
 *   1) Generate spin-priority table:
 *        python3 tools/frap_table_generator.py frap_demo_config.json frap_table_generated.h
 *   2) Build & run frapdemo. It runs TWO rounds automatically:
 *        - Round A: FRAP (frap_lock/frap_unlock)
 *        - Round B: SPIN (NuttX spin_lock_irqsave baseline)
 *
 * Notes:
 *  - Keep the original demo features:
 *      * pid_hint mapping via frap_table_generated.h
 *      * CPU pinning (SMP affinity) when available
 *      * start barrier (make contention deterministic)
 *      * correctness check (expected == counted)
 *  - The “interruptor” thread does not lock resources; it creates frequent
 *    high-priority preemptions on CPU0 to stress FRAP's preempt-aware waiting.
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/frap.h>
#include <nuttx/spinlock.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "frap_table_generated.h"

/* ------------------------ Test scale (complex sample) ------------------------ */
#define WORKER_NUM    13
#define RESOURCE_NUM  8
#define MAX_OPS        4

/* per-resource “critical section” compute work (iterations) */
static const int cs_work_iters[RESOURCE_NUM] =
{
  2000, /* R0 */
  3500, /* R1 */
  2800, /* R2 */
  9000, /* R3 */
  2600, /* R4 */
  5200, /* R5 */
  4200, /* R6 */
  8000  /* R7 */
};

enum bench_mode_e
{
  BENCH_FRAP = 0,
  BENCH_SPIN = 1,
};

/* ------------------------ Shared state per round ------------------------ */

/* FRAP resources (R0..R7) */
static struct frap_res g_frap_res[RESOURCE_NUM];

/* Spinlock baseline resources */
static spinlock_t g_spin_res[RESOURCE_NUM];

/* resource counters (incremented inside the resource critical section) */
static uint64_t g_counter[RESOURCE_NUM];

/* interruptor stats */
static uint64_t g_interruptor_runs;

/* created worker threads */
static pthread_t g_workers[WORKER_NUM];

/* (pid_hint -> per-resource spin priority) filled from frap_table_generated.h */
static int g_worker_prios[WORKER_NUM][RESOURCE_NUM];

/* start barrier (shared for both rounds; reset start_flag per round) */
static pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_start_cond = PTHREAD_COND_INITIALIZER;
static int             g_start_flag = 0;

/* ------------------------ Worker specifications ------------------------ */

struct worker_spec
{
  const char *name;
  int cpu;
  int base_prio;
  int loops;
  int nops;                       /* number of resource lock operations per loop */
  uint8_t resid[MAX_OPS];         /* resource IDs, duplicates allowed (e.g., R1 twice) */
  uint32_t sleep_us;              /* per-loop sleep to shape interference */
  bool is_interruptor;
};

/* IMPORTANT: pid_hint must match this order (0..WORKER_NUM-1) */
static const struct worker_spec g_spec[WORKER_NUM] =
{
  /* name         cpu  baseP loops  nops  ops                 sleep   is_int */
  { "hot0",         0,  240,  1800,  3,  {0, 1, 4, 0},         900,   false },
  { "hot1",         0,  238,  1700,  3,  {0, 1, 5, 0},        1100,   false },
  { "hot2",         0,  236,  1600,  3,  {0, 2, 4, 0},        1000,   false },
  { "mid0",         0,  200,  1200,  3,  {2, 3, 6, 0},        1600,   false },
  { "mid1",         0,  190,  1000,  2,  {3, 7, 0, 0},        2200,   false },

  { "remoteA0",     1,  140,   900,  3,  {1, 3, 5, 0},        4200,   false },
  { "remoteA1",     1,  130,   900,  3,  {1, 1, 4, 0},        4500,   false }, /* R1 twice */
  { "remoteA2",     1,  125,   800,  2,  {2, 6, 0, 0},        4800,   false },

  { "remoteB0",     2,  120,   900,  2,  {1, 7, 0, 0},        3400,   false },
  { "remoteB1",     2,  110,   800,  2,  {0, 3, 0, 0},        3600,   false },

  { "background0",  2,   70,   500,  1,  {7, 0, 0, 0},        8000,   false },
  { "background1",  2,   65,   600,  2,  {5, 6, 0, 0},        7000,   false },

  { "interruptor",  0,  250,  4000,  0,  {0, 0, 0, 0},         400,    true  },
};

struct worker_ctx
{
  int idx;              /* pid_hint / worker index */
  int mode;             /* enum bench_mode_e */
  int prios[RESOURCE_NUM];
};

/* ------------------------ Helpers ------------------------ */

static void pin_to_cpu(int cpu)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPUAFFINITY)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)sched_setaffinity(0, sizeof(set), &set);
#else
  (void)cpu;
#endif
}

static void busy_work(int iters)
{
  volatile unsigned long long s = 0;
  while (iters--) s += (unsigned long long)iters;
}

static void wait_for_start(void)
{
  pthread_mutex_lock(&g_start_lock);
  while (!g_start_flag)
    {
      pthread_cond_wait(&g_start_cond, &g_start_lock);
    }
  pthread_mutex_unlock(&g_start_lock);
}

/* FRAP path */
static inline void frap_res_lock(int resid, const int prios[RESOURCE_NUM])
{
  frap_set_spin_prio(prios[resid]);
  frap_lock(&g_frap_res[resid]);
}

static inline void frap_res_unlock(int resid)
{
  frap_unlock(&g_frap_res[resid]);
}

/* Spinlock baseline path */
static inline irqstate_t spin_res_lock(int resid)
{
  return spin_lock_irqsave(&g_spin_res[resid]);
}

static inline void spin_res_unlock(int resid, irqstate_t flags)
{
  spin_unlock_irqrestore(&g_spin_res[resid], flags);
}

/* ------------------------ Worker entry ------------------------ */

static void *worker_entry(void *arg)
{
  struct worker_ctx *ctx = (struct worker_ctx *)arg;
  int idx = ctx->idx;
  const struct worker_spec *sp = &g_spec[idx];

  pin_to_cpu(sp->cpu);

  /* One-time banner (avoid printing in loops; console output hurts throughput) */
  printf("[%s] start (cpu=%d baseP=%d mode=%s)\n",
         sp->name, sp->cpu, sp->base_prio,
         (ctx->mode == BENCH_FRAP) ? "FRAP" : "SPIN");

  wait_for_start();

  if (sp->is_interruptor)
    {
      for (int i = 0; i < sp->loops; i++)
        {
          busy_work(15000);
          g_interruptor_runs++;
          if (sp->sleep_us) usleep(sp->sleep_us);
        }
      free(ctx);
      return NULL;
    }

  for (int i = 0; i < sp->loops; i++)
    {
      for (int k = 0; k < sp->nops; k++)
        {
          int r = (int)sp->resid[k];

          if (ctx->mode == BENCH_FRAP)
            {
              frap_res_lock(r, ctx->prios);
              busy_work(cs_work_iters[r]);
              g_counter[r]++;
              frap_res_unlock(r);
            }
          else
            {
              irqstate_t flags = spin_res_lock(r);
              busy_work(cs_work_iters[r]);
              g_counter[r]++;
              spin_res_unlock(r, flags);
            }
        }

      if (sp->sleep_us) usleep(sp->sleep_us);
    }

  free(ctx);
  return NULL;
}

/* ------------------------ Benchmark runner ------------------------ */

struct bench_result
{
  double elapsed;
  uint64_t counted[RESOURCE_NUM];
  uint64_t expected[RESOURCE_NUM];
  double throughput[RESOURCE_NUM];
  uint64_t total_counted;
  uint64_t total_expected;
  double total_throughput;
};

static void init_resources_for_mode(int mode)
{
  for (int r = 0; r < RESOURCE_NUM; r++)
    {
      g_counter[r] = 0;

      /* init both; cheap and keeps code simple */
      frap_res_init(&g_frap_res[r], r, true);
      g_spin_res[r] = SP_UNLOCKED;
    }

  g_interruptor_runs = 0;

  (void)mode;
}

static void compute_expected(uint64_t expected[RESOURCE_NUM])
{
  for (int r = 0; r < RESOURCE_NUM; r++) expected[r] = 0;

  for (int i = 0; i < WORKER_NUM; i++)
    {
      const struct worker_spec *sp = &g_spec[i];
      if (sp->is_interruptor) continue;

      for (int k = 0; k < sp->nops; k++)
        {
          int r = (int)sp->resid[k];
          expected[r] += (uint64_t)sp->loops;
        }
    }
}

static void fill_worker_prios_from_generated_table(void)
{
  /* default 0 => filled if table entry exists; otherwise fallback to base prio */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      for (int r = 0; r < RESOURCE_NUM; r++)
        {
          g_worker_prios[i][r] = 0;
        }
    }

  for (int e = 0; e < frap_generated_table_len; e++)
    {
      const struct frap_cfg_entry *ent = &frap_generated_table[e];
      int idx = ent->pid_hint;
      if (idx < 0 || idx >= WORKER_NUM) continue;
      if (ent->resid < 0 || ent->resid >= RESOURCE_NUM) continue;
      g_worker_prios[idx][ent->resid] = ent->spin_prio;
    }

  /* fallback: any missing (0) => base prio */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      int baseP = g_spec[i].base_prio;
      for (int r = 0; r < RESOURCE_NUM; r++)
        {
          if (g_worker_prios[i][r] == 0)
            {
              g_worker_prios[i][r] = baseP;
            }
        }
    }
}

static struct bench_result run_bench(int mode)
{
  struct bench_result res;
  memset(&res, 0, sizeof(res));

  init_resources_for_mode(mode);
  compute_expected(res.expected);

  /* reset start barrier */
  pthread_mutex_lock(&g_start_lock);
  g_start_flag = 0;
  pthread_mutex_unlock(&g_start_lock);

  /* create threads with SCHED_FIFO + base priority */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_t attr;
      struct sched_param param;
      pthread_attr_init(&attr);

      param.sched_priority = g_spec[i].base_prio;
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      pthread_attr_setschedparam(&attr, &param);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

      struct worker_ctx *ctx = (struct worker_ctx *)malloc(sizeof(struct worker_ctx));
      if (!ctx)
        {
          printf("Failed to allocate ctx for worker %d\n", i);
          continue;
        }

      ctx->idx = i;
      ctx->mode = mode;
      memcpy(ctx->prios, g_worker_prios[i], sizeof(ctx->prios));

      int ret = pthread_create(&g_workers[i], &attr, worker_entry, (void *)ctx);
      if (ret != 0)
        {
          printf("Create worker %d failed: %d\n", i, ret);
          free(ctx);
        }

      pthread_attr_destroy(&attr);

      /* small stagger: reduce create-time contention but keep runtime contention */
      usleep(15000);
    }

  /* make sure all threads are waiting on the barrier */
  usleep(200000);

  /* start timing + broadcast */
  struct timespec tstart, tend;
  clock_gettime(CLOCK_MONOTONIC, &tstart);

  pthread_mutex_lock(&g_start_lock);
  g_start_flag = 1;
  pthread_cond_broadcast(&g_start_cond);
  pthread_mutex_unlock(&g_start_lock);

  /* join */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_join(g_workers[i], NULL);
    }

  clock_gettime(CLOCK_MONOTONIC, &tend);
  res.elapsed = (tend.tv_sec - tstart.tv_sec) + (tend.tv_nsec - tstart.tv_nsec) / 1e9;

  /* gather results */
  for (int r = 0; r < RESOURCE_NUM; r++)
    {
      res.counted[r] = g_counter[r];
      res.throughput[r] = (res.elapsed > 0.0) ? ((double)g_counter[r] / res.elapsed) : 0.0;
      res.total_counted += g_counter[r];
      res.total_expected += res.expected[r];
    }
  res.total_throughput = (res.elapsed > 0.0) ? ((double)res.total_counted / res.elapsed) : 0.0;

  return res;
}

static void print_round_report(const char *tag, const struct bench_result *r)
{
  printf("\n[%s] Results (elapsed %.3f s)\n", tag, r->elapsed);

  for (int i = 0; i < RESOURCE_NUM; i++)
    {
      printf("  R%d: counted=%llu expected=%llu  thr=%.2f ops/s  %s\n",
             i,
             (unsigned long long)r->counted[i],
             (unsigned long long)r->expected[i],
             r->throughput[i],
             (r->counted[i] == r->expected[i]) ? "[OK]" : "[MISMATCH]");
    }

  printf("  Total: counted=%llu expected=%llu  thr=%.2f ops/s\n",
         (unsigned long long)r->total_counted,
         (unsigned long long)r->total_expected,
         r->total_throughput);

  printf("  Interruptor runs=%llu\n", (unsigned long long)g_interruptor_runs);
}

/* ------------------------ main ------------------------ */

int main(int argc, char *argv[])
{
  (void)argc; (void)argv;

  printf("[FRAPDEMO] complex workload + FRAP vs NuttX spinlock throughput compare\n");
  printf("[FRAPDEMO] workers=%d resources=%d\n", WORKER_NUM, RESOURCE_NUM);

  /* Build spin-priority map from generated table */
  fill_worker_prios_from_generated_table();

  /* Round A: FRAP */
  struct bench_result frap_res = run_bench(BENCH_FRAP);
  print_round_report("FRAP", &frap_res);

  /* Round B: NuttX spinlock baseline */
  struct bench_result spin_res = run_bench(BENCH_SPIN);
  print_round_report("SPIN", &spin_res);

  /* Summary */
  printf("\n[SUMMARY]\n");
  printf("  Total throughput: FRAP=%.2f  SPIN=%.2f  (FRAP/SPIN=%.3f)\n",
         frap_res.total_throughput,
         spin_res.total_throughput,
         (spin_res.total_throughput > 0.0) ? (frap_res.total_throughput / spin_res.total_throughput) : 0.0);

  for (int r = 0; r < RESOURCE_NUM; r++)
    {
      double ratio = (spin_res.throughput[r] > 0.0) ? (frap_res.throughput[r] / spin_res.throughput[r]) : 0.0;
      printf("  R%d thr: FRAP=%.2f  SPIN=%.2f  ratio=%.3f\n",
             r, frap_res.throughput[r], spin_res.throughput[r], ratio);
    }

  printf("[FRAPDEMO] done\n");
  return 0;
}
