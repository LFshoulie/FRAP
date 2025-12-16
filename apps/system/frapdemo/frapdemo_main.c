/****************************************************************************
 * apps/system/frapdemo/frapdemo_main.c
 *
 * FRAP demo + throughput & correctness verification.
 * Modified: ensure each thread's scheduling priority is initialized to its
 *           base priority (P_i) at thread creation time.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/frap.h>

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

/* ---------- parameters (match the JSON above) ---------- */
#define WORKER_NUM 8
#define RESOURCE_NUM 4

/* loop counts (per-thread) - moderate values for a reasonable run time */
static const int loops_for_worker[WORKER_NUM] = {
  800, /* hot0 */
  700, /* hot1 */
  500, /* mid0 */
  450, /* mid1 */
  250, /* remoteA0 */
  250, /* remoteA1 */
  300, /* remoteB0 */
  200  /* background */
};

/* resource counters (protected by non-preemptible CS when incremented) */
static uint64_t g_counter[RESOURCE_NUM];

/* FRAP resources global */
static struct frap_res g_res[RESOURCE_NUM];

/* created worker threads */
static pthread_t workers[WORKER_NUM];
/* per-worker spin-priority array (resource indexed) */
static int worker_prios[WORKER_NUM][RESOURCE_NUM];

/* start barrier */
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  start_cond = PTHREAD_COND_INITIALIZER;
static int             start_flag = 0;

/* Base priorities (P_i) for each worker (must match the JSON tasks' P fields) */
static const int base_prio_of_worker[WORKER_NUM] = {
  240, /* hot0 */
  238, /* hot1 */
  200, /* mid0 */
  190, /* mid1 */
  120, /* remoteA0 */
  110, /* remoteA1 */
  115, /* remoteB0 */
  60   /* background */
};

/* helper: pin to cpu if available */
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

/* busy work inside critical section (short loop) */
static void busy_work(int iters)
{
  volatile unsigned long long s = 0;
  while (iters--) s += iters;
}

/* wait until main releases workers */
static void wait_for_start(void)
{
  pthread_mutex_lock(&start_lock);
  while (!start_flag)
    {
      pthread_cond_wait(&start_cond, &start_lock);
    }
  pthread_mutex_unlock(&start_lock);
}

/* generic worker function dispatcher */
typedef void *(*worker_fn_t)(void *);
static void *worker_hot0(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(0);
  printf("[worker_hot0] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[0];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[0]); /* R0 */
      frap_lock(&g_res[0]);
      busy_work(2000);
      g_counter[0]++;
      frap_unlock(&g_res[0]);

      frap_set_spin_prio(arr[1]); /* R1 */
      frap_lock(&g_res[1]);
      busy_work(3500);
      g_counter[1]++;
      frap_unlock(&g_res[1]);

      usleep(1000);
    }
  free(arg);
  return NULL;
}

static void *worker_hot1(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(0);
  printf("[worker_hot1] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[1];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[0]);
      frap_lock(&g_res[0]);
      busy_work(1800);
      g_counter[0]++;
      frap_unlock(&g_res[0]);

      frap_set_spin_prio(arr[1]);
      frap_lock(&g_res[1]);
      busy_work(3200);
      g_counter[1]++;
      frap_unlock(&g_res[1]);

      usleep(1200);
    }
  free(arg);
  return NULL;
}

static void *worker_mid0(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(0);
  printf("[worker_mid0] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[2];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[0]);
      frap_lock(&g_res[0]);
      busy_work(3000);
      g_counter[0]++;
      frap_unlock(&g_res[0]);

      frap_set_spin_prio(arr[2]);
      frap_lock(&g_res[2]);
      busy_work(2500);
      g_counter[2]++;
      frap_unlock(&g_res[2]);

      usleep(1500);
    }
  free(arg);
  return NULL;
}

static void *worker_mid1(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(0);
  printf("[worker_mid1] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[3];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[2]);
      frap_lock(&g_res[2]);
      busy_work(2400);
      g_counter[2]++;
      frap_unlock(&g_res[2]);

      frap_set_spin_prio(arr[3]);
      frap_lock(&g_res[3]);
      busy_work(8000);
      g_counter[3]++;
      frap_unlock(&g_res[3]);

      usleep(2000);
    }
  free(arg);
  return NULL;
}

static void *worker_remoteA0(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(1);
  printf("[worker_remoteA0] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[4];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[1]);
      frap_lock(&g_res[1]);
      busy_work(3000);
      g_counter[1]++;
      frap_unlock(&g_res[1]);

      usleep(4000);
    }
  free(arg);
  return NULL;
}

static void *worker_remoteA1(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(1);
  printf("[worker_remoteA1] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[5];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[1]);
      frap_lock(&g_res[1]);
      busy_work(3200);
      g_counter[1]++;
      frap_unlock(&g_res[1]);

      frap_set_spin_prio(arr[3]);
      frap_lock(&g_res[3]);
      busy_work(6000);
      g_counter[3]++;
      frap_unlock(&g_res[3]);

      usleep(5000);
    }
  free(arg);
  return NULL;
}

static void *worker_remoteB0(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(2);
  printf("[worker_remoteB0] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[6];
  for (int i = 0; i < loops; i++)
    {
      /* remoteB0 requests R1 twice per loop to increase remote contention */
      frap_set_spin_prio(arr[1]);
      frap_lock(&g_res[1]);
      busy_work(2200);
      g_counter[1]++;
      frap_unlock(&g_res[1]);

      frap_set_spin_prio(arr[1]);
      frap_lock(&g_res[1]);
      busy_work(2200);
      g_counter[1]++;
      frap_unlock(&g_res[1]);

      usleep(3000);
    }
  free(arg);
  return NULL;
}

static void *worker_background(void *arg)
{
  int *arr = (int *)arg;
  pin_to_cpu(2);
  printf("[worker_background] start; prios R0..R3 = %d %d %d %d\n",
         arr[0], arr[1], arr[2], arr[3]);
  wait_for_start();

  int loops = loops_for_worker[7];
  for (int i = 0; i < loops; i++)
    {
      frap_set_spin_prio(arr[3]);
      frap_lock(&g_res[3]);
      busy_work(2000);
      g_counter[3]++;
      frap_unlock(&g_res[3]);
      usleep(7000);
    }
  free(arg);
  return NULL;
}

/* worker table (index by pid_hint) */
static worker_fn_t worker_table[WORKER_NUM] = {
  worker_hot0,
  worker_hot1,
  worker_mid0,
  worker_mid1,
  worker_remoteA0,
  worker_remoteA1,
  worker_remoteB0,
  worker_background
};

int main(int argc, char *argv[])
{
  (void)argc; (void)argv;
  printf("[FRAPTEST] starting demo (throughput + correctness check)\n");

  /* init resources */
  for (int i = 0; i < RESOURCE_NUM; i++)
    {
      frap_res_init(&g_res[i], i, true);
      g_counter[i] = 0;
    }

  /* fill worker_prios with defaults (base) */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      for (int r = 0; r < RESOURCE_NUM; r++)
        worker_prios[i][r] = 0; /* default 0 = will be overwritten */
    }

  /* apply the generated table into per-worker arrays */
  for (int e = 0; e < frap_generated_table_len; e++)
    {
      const struct frap_cfg_entry *ent = &frap_generated_table[e];
      int idx = ent->pid_hint;
      if (idx < 0 || idx >= WORKER_NUM)
        continue;
      if (ent->resid >= 0 && ent->resid < RESOURCE_NUM)
        worker_prios[idx][ent->resid] = ent->spin_prio;
    }

  /* For any missing entries, fill with base-priority defaults (simple fallback) */
  // for (int i = 0; i < WORKER_NUM; i++)
  //   {
  //     int base_prio = base_prio_of_worker[i];
  //     for (int r = 0; r < RESOURCE_NUM; r++)
  //       {
  //         if (worker_prios[i][r] == 0)
  //           worker_prios[i][r] = base_prio;
  //       }
  //   }

  /* create threads (pass pointer to their prio array) */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_t attr;
      struct sched_param param;
      pthread_attr_init(&attr);

      /* IMPORTANT: initialize thread scheduling priority to the task's base_prio */
      param.sched_priority = base_prio_of_worker[i];
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      pthread_attr_setschedparam(&attr, &param);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

      int *arg = malloc(sizeof(int) * RESOURCE_NUM);
      if (arg == NULL)
        {
          printf("Failed to allocate arg for worker %d\n", i);
          continue;
        }
      memcpy(arg, worker_prios[i], sizeof(int) * RESOURCE_NUM);

      int ret = pthread_create(&workers[i], &attr, worker_table[i], (void *)arg);
      if (ret != 0)
        {
          printf("Create worker %d failed: %d\n", i, ret);
          free(arg);
        }
      pthread_attr_destroy(&attr);
      usleep(20000);
    }

  /* 确保所有线程在同一时间开始请求资源 */
  sleep(1);
  pthread_mutex_lock(&start_lock);
  start_flag = 1;
  pthread_cond_broadcast(&start_cond);
  pthread_mutex_unlock(&start_lock);

  /* measure start time */
  struct timespec tstart, tend;
  clock_gettime(CLOCK_MONOTONIC, &tstart);

  /* join threads */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_join(workers[i], NULL);
    }

  clock_gettime(CLOCK_MONOTONIC, &tend);
  double elapsed = (tend.tv_sec - tstart.tv_sec) + (tend.tv_nsec - tstart.tv_nsec) / 1e9;

  /* compute theoretical totals from loops_for_worker & req pattern (matching JSON) */
  uint64_t expected[RESOURCE_NUM] = {0};
  /* hot0: idx0 requests R0 & R1 each loop */
  expected[0] += (uint64_t)loops_for_worker[0]; expected[1] += (uint64_t)loops_for_worker[0];
  /* hot1: idx1 requests R0 & R1 */
  expected[0] += (uint64_t)loops_for_worker[1]; expected[1] += (uint64_t)loops_for_worker[1];
  /* mid0: idx2 requests R0 & R2 */
  expected[0] += (uint64_t)loops_for_worker[2]; expected[2] += (uint64_t)loops_for_worker[2];
  /* mid1: idx3 requests R2 & R3 */
  expected[2] += (uint64_t)loops_for_worker[3]; expected[3] += (uint64_t)loops_for_worker[3];
  /* remoteA0: idx4 requests R1 */
  expected[1] += (uint64_t)loops_for_worker[4];
  /* remoteA1: idx5 requests R1 & R3 */
  expected[1] += (uint64_t)loops_for_worker[5]; expected[3] += (uint64_t)loops_for_worker[5];
  /* remoteB0: idx6 requests R1 twice per loop */
  expected[1] += (uint64_t)loops_for_worker[6] * 2ULL;
  /* background: idx7 requests R3 */
  expected[3] += (uint64_t)loops_for_worker[7];

  printf("\n[FRAPTEST] Results (elapsed %.3f s):\n", elapsed);
  for (int r = 0; r < RESOURCE_NUM; r++)
    {
      // 所有任务访问资源的总次数 / 所有任务跑完的时间
      double throughput = (double)g_counter[r] / elapsed;
      printf(" Resource R%d: counted=%llu expected=%llu throughput=%.2f entries/s %s\n",
             r,
             (unsigned long long)g_counter[r],
             (unsigned long long)expected[r],
             throughput,
             (g_counter[r] == expected[r]) ? "[OK]" : "[MISMATCH]");
    }

  /* sanity: total expected vs counted */
  uint64_t total_expected = 0, total_counted = 0;
  for (int r = 0; r < RESOURCE_NUM; r++) { total_expected += expected[r]; total_counted += g_counter[r]; }
  printf("[FRAPTEST] Total counted=%llu expected=%llu\n",
         (unsigned long long)total_counted, (unsigned long long)total_expected);
  printf("[FRAPTEST] Total throughput=%.2f\n",total_counted / elapsed);
  printf("[FRAPTEST] demo finished\n");
  return 0;
}
