/****************************************************************************
 * apps/system/frapdemo/frapdemo_main.c
 *
 * Lock micro-benchmark (controller-thread version):
 *  - No usleep in worker critical loop (avoid masking lock differences)
 *  - A high-priority controller thread runs warmup and measured phases
 *  - Compare FRAP vs NuttX spin_lock_irqsave throughput (ops/s)
 *
 * Key fix for "stuck after warmup":
 *  - Controller runs at higher priority than any worker/spin_prio, so it
 *    won't be starved by busy-loop workers.
 *  - Workers do a VERY low-frequency sched_yield() to avoid FIFO starvation
 *    among same-priority workers on the same CPU (if no RR timeslice).
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
#include <errno.h>

#include "frap_table_generated.h"

/* -------------------- knobs -------------------- */
#define WORKER_NUM    12
#define RESOURCE_NUM  1

#define WARMUP_SECS   1
#define RUN_SECS      2

/* Controller priority: MUST be > any worker priority and > any FRAP spin_prio.
 * NuttX typical prio range is 0..255. Use 250 by default.
 */
#ifndef CTRL_PRIO
#  define CTRL_PRIO   250
#endif

/* Worker base priority (should be < CTRL_PRIO) */
#ifndef WORKER_BASE_PRIO
#  define WORKER_BASE_PRIO 200
#endif

/* Extremely low-frequency yield to avoid FIFO starvation among same-priority
 * workers on the same CPU when RR timeslice is disabled.
 * Increase this (e.g., 262144) to reduce interference if your system has RR.
 */
#ifndef YIELD_EVERY_OPS
#  define YIELD_EVERY_OPS 65536u
#endif

/* -------------------- phases -------------------- */
#define PH_IDLE   0
#define PH_FRAP   1
#define PH_SPIN   2
#define PH_EXIT   3

/* -------------------- shared state -------------------- */
static struct frap_res g_res[RESOURCE_NUM];
static spinlock_t      g_spin[RESOURCE_NUM] = { SP_UNLOCKED };

static volatile uint64_t g_shared[RESOURCE_NUM];

struct worker_stat
{
  uint64_t ops;
  uint64_t pad[7]; /* reduce false sharing */
};
static struct worker_stat g_stat[WORKER_NUM];

/* per-worker spin priority for R0 (resid=0) */
static int g_spin_prio_r0[WORKER_NUM];

/* CPU mapping: 12 workers spread across CPU0/1/2 (4 each) */
static const int g_cpu_of_worker[WORKER_NUM] =
{
  0,1,2, 0,1,2, 0,1,2, 0,1,2
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_phase_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_done_cond  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_ready_cond = PTHREAD_COND_INITIALIZER;

static volatile int  g_phase = PH_IDLE;
static volatile bool g_stop  = false;
static int           g_done  = 0;
static int           g_ready = 0;

/* -------------------- helpers -------------------- */
static inline double ts_diff_s(const struct timespec *a, const struct timespec *b)
{
  return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

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

static void set_thread_prio(int prio)
{
  struct sched_param sp;
  sp.sched_priority = prio;
  if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
    {
      printf("[LOCKBENCH] WARN: sched_setscheduler(prio=%d) failed errno=%d\n", prio, errno);
      fflush(stdout);
    }
}

/* -------------------- worker -------------------- */
struct worker_arg
{
  int idx;
  int cpu;
  int spin_prio_r0;
};

static void *worker_main(void *p)
{
  struct worker_arg *arg = (struct worker_arg *)p;
  const int idx = arg->idx;
  int last_phase = PH_IDLE;

  pin_to_cpu(arg->cpu);

  /* Make workers lower than controller */
  set_thread_prio(WORKER_BASE_PRIO);

  pthread_mutex_lock(&g_lock);
  g_ready++;
  pthread_cond_signal(&g_ready_cond);
  pthread_mutex_unlock(&g_lock);

  for (;;)
    {
      pthread_mutex_lock(&g_lock);
      while (g_phase == last_phase)
        {
          pthread_cond_wait(&g_phase_cond, &g_lock);
        }
      int phase = g_phase;
      pthread_mutex_unlock(&g_lock);

      if (phase == PH_EXIT)
        {
          break;
        }

      /* Phase entry */
      uint64_t local_ops = 0;

      if (phase == PH_FRAP)
        {
          /* Set per-thread spin priority once per phase (not per-iteration) */
          frap_set_spin_prio((int8_t)arg->spin_prio_r0);
        }

      while (!g_stop)
        {
          if (phase == PH_FRAP)
            {
              int ret = frap_lock(&g_res[0]);
              if (ret == OK)
                {
                  g_shared[0]++;
                  frap_unlock(&g_res[0]);
                }
              else
                {
                  /* Avoid tight error loop */
                  sched_yield();
                }
            }
          else /* PH_SPIN */
            {
              irqstate_t f = spin_lock_irqsave(&g_spin[0]);
              g_shared[0]++;
              spin_unlock_irqrestore(&g_spin[0], f);
            }

          local_ops++;

          /* Prevent FIFO starvation among same-priority workers on same CPU */
          if ((local_ops % YIELD_EVERY_OPS) == 0)
            {
              sched_yield();
            }
        }

      g_stat[idx].ops = local_ops;

      pthread_mutex_lock(&g_lock);
      g_done++;
      if (g_done >= WORKER_NUM)
        {
          pthread_cond_signal(&g_done_cond);
        }
      last_phase = phase;
      pthread_mutex_unlock(&g_lock);
    }

  free(arg);
  return NULL;
}

/* -------------------- controller -------------------- */
static void reset_stats(void)
{
  g_shared[0] = 0;
  for (int i = 0; i < WORKER_NUM; i++)
    {
      g_stat[i].ops = 0;
    }
}

static void run_phase(int phase, int seconds, const char *tag, bool print_result)
{
  reset_stats();

  pthread_mutex_lock(&g_lock);
  g_done = 0;
  g_stop = false;
  g_phase = phase;
  pthread_cond_broadcast(&g_phase_cond);
  pthread_mutex_unlock(&g_lock);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  sleep(seconds);

  g_stop = true;

  pthread_mutex_lock(&g_lock);
  while (g_done < WORKER_NUM)
    {
      pthread_cond_wait(&g_done_cond, &g_lock);
    }
  pthread_mutex_unlock(&g_lock);

  clock_gettime(CLOCK_MONOTONIC, &t1);

  if (print_result)
    {
      double elapsed = ts_diff_s(&t0, &t1);

      uint64_t total_ops = 0;
      for (int i = 0; i < WORKER_NUM; i++)
        {
          total_ops += g_stat[i].ops;
        }

      printf("\n[LOCKBENCH] %s: elapsed=%.3f s  total_ops=%llu  ops/s=%.2f\n",
             tag, elapsed,
             (unsigned long long)total_ops,
             (double)total_ops / elapsed);

      printf("[LOCKBENCH] %s: shared_counter=%llu  (%s)\n",
             tag,
             (unsigned long long)g_shared[0],
             (g_shared[0] == total_ops) ? "OK" : "MISMATCH");

      fflush(stdout);
    }

  /* go idle briefly for clean separation */
  pthread_mutex_lock(&g_lock);
  g_phase = PH_IDLE;
  pthread_cond_broadcast(&g_phase_cond);
  pthread_mutex_unlock(&g_lock);

  sleep(1);
}

static void *controller_main(void *arg)
{
  (void)arg;

  /* Ensure controller is highest priority so it cannot be starved */
  set_thread_prio(CTRL_PRIO);

  printf("[LOCKBENCH] controller running @ prio=%d\n", CTRL_PRIO);
  fflush(stdout);

  /* Warmup: run short phases without printing */
  printf("[LOCKBENCH] warmup %d s ...\n", WARMUP_SECS);
  fflush(stdout);

  run_phase(PH_FRAP, WARMUP_SECS, "FRAP(WARMUP)", false);
  run_phase(PH_SPIN, WARMUP_SECS, "SPIN(WARMUP)", false);

  /* Measured */
  run_phase(PH_FRAP, RUN_SECS, "FRAP", true);
  run_phase(PH_SPIN, RUN_SECS, "SPINLOCK", true);

  /* Exit all workers */
  pthread_mutex_lock(&g_lock);
  g_phase = PH_EXIT;
  pthread_cond_broadcast(&g_phase_cond);
  pthread_mutex_unlock(&g_lock);

  return NULL;
}

/* -------------------- main -------------------- */
int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("[LOCKBENCH] start: WORKER_NUM=%d RESOURCE_NUM=%d\n", WORKER_NUM, RESOURCE_NUM);
  fflush(stdout);

  /* init FRAP resource */
  frap_res_init(&g_res[0], 0, true);

  /* default spin prio to worker base (fallback) */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      g_spin_prio_r0[i] = WORKER_BASE_PRIO;
    }

  /* load spin prios for resid=0 from generated table */
  for (int e = 0; e < frap_generated_table_len; e++)
    {
      const struct frap_cfg_entry *ent = &frap_generated_table[e];
      if (ent->resid != 0)
        {
          continue;
        }

      if (ent->pid_hint >= 0 && ent->pid_hint < WORKER_NUM)
        {
          g_spin_prio_r0[ent->pid_hint] = ent->spin_prio;
        }
    }

  /* Sanity: controller must be higher than any spin_prio */
  int max_spin = 0;
  for (int i = 0; i < WORKER_NUM; i++)
    {
      if (g_spin_prio_r0[i] > max_spin) max_spin = g_spin_prio_r0[i];
    }

  if (CTRL_PRIO <= max_spin)
    {
      printf("[LOCKBENCH] WARN: CTRL_PRIO(%d) <= max_spin_prio(%d). "
             "Increase CTRL_PRIO or reduce spin_prio table.\n",
             CTRL_PRIO, max_spin);
      fflush(stdout);
    }

  /* create worker threads */
  static pthread_t workers[WORKER_NUM];

  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_t attr;
      pthread_attr_init(&attr);

      /* We explicitly set scheduling. Even if policy is ignored, we still
       * set priorities inside thread via set_thread_prio().
       */
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

      struct sched_param sp;
      sp.sched_priority = WORKER_BASE_PRIO;
      pthread_attr_setschedparam(&attr, &sp);

      struct worker_arg *a = (struct worker_arg *)malloc(sizeof(*a));
      if (!a)
        {
          printf("[LOCKBENCH] malloc failed\n");
          fflush(stdout);
          return 1;
        }

      a->idx = i;
      a->cpu = g_cpu_of_worker[i];
      a->spin_prio_r0 = g_spin_prio_r0[i];

      int ret = pthread_create(&workers[i], &attr, worker_main, a);
      pthread_attr_destroy(&attr);

      if (ret != 0)
        {
          printf("[LOCKBENCH] pthread_create(%d) failed: %d\n", i, ret);
          fflush(stdout);
          free(a);
          return 1;
        }

      usleep(20000);
    }

  /* wait all workers ready (blocked on cond, not busy) */
  pthread_mutex_lock(&g_lock);
  while (g_ready < WORKER_NUM)
    {
      pthread_cond_wait(&g_ready_cond, &g_lock);
    }
  pthread_mutex_unlock(&g_lock);

  /* create controller thread (highest priority) */
  pthread_t controller;
  pthread_attr_t cattr;
  pthread_attr_init(&cattr);
  pthread_attr_setinheritsched(&cattr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&cattr, SCHED_FIFO);

  struct sched_param csp;
  csp.sched_priority = CTRL_PRIO;
  pthread_attr_setschedparam(&cattr, &csp);

  int cret = pthread_create(&controller, &cattr, controller_main, NULL);
  pthread_attr_destroy(&cattr);

  if (cret != 0)
    {
      printf("[LOCKBENCH] controller pthread_create failed: %d\n", cret);
      fflush(stdout);
      return 1;
    }

  /* wait controller finish (it will set PH_EXIT) */
  pthread_join(controller, NULL);

  /* join workers */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_join(workers[i], NULL);
    }

  printf("\n[LOCKBENCH] done.\n");
  fflush(stdout);
  return 0;
}
