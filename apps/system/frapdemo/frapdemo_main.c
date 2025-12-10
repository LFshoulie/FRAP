/****************************************************************************
 * apps/system/frapdemo/frapdemo_main.c
 *
 * FRAP demo that uses frap_table_generated.h (produced by frap_table_generator.py)
 *
 * This demo creates 8 threads matching frap_demo_config.json:
 *  - hot0, hot1, mid0, mid1 on CPU0
 *  - remoteA0, remoteA1 on CPU1
 *  - remoteB0, background on CPU2
 *
 * The program applies the generated spin-priority table (frap_set_spin_prio)
 * using pid_hint to map entries to threads created in this program.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <nuttx/frap.h>

#include "frap_table_generated.h"

/* Number of worker threads (must match the JSON pid_hint indices) */
#define WORKER_NUM 8

static pthread_t workers[WORKER_NUM];

/* Declare three global FRAP resources (IDs 0..3) */
static struct frap_res g_r0;
static struct frap_res g_r1;
static struct frap_res g_r2;
static struct frap_res g_r3;

/* Busy-wait helper (microsecond-like scale) */
static void busy_work(unsigned loops)
{
  volatile unsigned long long s = 0;
  while (loops--) s += loops;
}

/* Bind to CPU (if supported) */
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

/* Worker routines - each corresponds to a task in the JSON (index == pid_hint) */

/* hot0 (index 0): CPU0, frequently locks R0 and R1 with high spin-prio */
static void *worker_hot0(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_hot0 started on CPU0, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(0);
  for (int i = 0; i < 800; i++)
    {
      frap_set_spin_prio(arr[0]); /* set spin prio for R0 */
      frap_lock(&g_r0);
      busy_work(2000);
      frap_unlock(&g_r0);

      frap_set_spin_prio(arr[1]); /* set spin prio for R1 */
      frap_lock(&g_r1);
      busy_work(4000);
      frap_unlock(&g_r1);

      usleep(1000);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_hot0 finished\n");
  return NULL;
}

/* hot1 (index 1): CPU0, similar to hot0 slightly different timing */
static void *worker_hot1(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_hot1 started on CPU0, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(0);
  for (int i = 0; i < 700; i++)
    {
      frap_set_spin_prio(arr[0]); /* set spin prio for R0 */
      frap_lock(&g_r0);
      busy_work(1800);
      frap_unlock(&g_r0);

      frap_set_spin_prio(arr[1]); /* set spin prio for R1 */
      frap_lock(&g_r1);
      busy_work(3500);
      frap_unlock(&g_r1);

      usleep(1200);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_hot1 finished\n");
  return NULL;
}

/* mid0 (index 2): CPU0, accesses R0 and R2, spin_prio == base (200) */
static void *worker_mid0(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_mid0 started on CPU0, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(0);
  for (int i = 0; i < 500; i++)
    {
      frap_set_spin_prio(arr[0]); /* set spin prio for R0 */
      frap_lock(&g_r0);  /* P^k = base -> will be preempted by hot */
      busy_work(3000);
      frap_unlock(&g_r0);

      frap_set_spin_prio(arr[2]); /* set spin prio for R2 */
      frap_lock(&g_r2);
      busy_work(2500);
      frap_unlock(&g_r2);

      usleep(1500);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_mid0 finished\n");
  return NULL;
}

/* mid1 (index 3): CPU0, accesses R2 and R3, R3 is long critical section */
static void *worker_mid1(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_mid1 started on CPU0, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(0);
  for (int i = 0; i < 450; i++)
    {
      frap_set_spin_prio(arr[2]); /* set spin prio for R2 */
      frap_lock(&g_r2);
      busy_work(2400);
      frap_unlock(&g_r2);

      frap_set_spin_prio(arr[3]); /* set spin prio for R3 */
      frap_lock(&g_r3);
      busy_work(8000); /* long critical section to emphasize btilde*c */
      frap_unlock(&g_r3);

      usleep(2000);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_mid1 finished\n");
  return NULL;
}

/* remoteA0 (index 4): CPU1, accesses R1 (low freq) */
static void *worker_remoteA0(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_remoteA0 started on CPU1, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(1);
  for (int i = 0; i < 250; i++)
    {
      frap_set_spin_prio(arr[1]); /* set spin prio for R1 */
      frap_lock(&g_r1);
      busy_work(3000);
      frap_unlock(&g_r1);
      usleep(4000);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_remoteA0 finished\n");
  return NULL;
}

/* remoteA1 (index 5): CPU1, accesses R1 and R3 */
static void *worker_remoteA1(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_remoteA1 started on CPU1, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(1);
  for (int i = 0; i < 220; i++)
    {
      frap_set_spin_prio(arr[1]); /* set spin prio for R1 */
      frap_lock(&g_r1);
      busy_work(3200);
      frap_unlock(&g_r1);

      frap_set_spin_prio(arr[3]); /* set spin prio for R3 */
      frap_lock(&g_r3);
      busy_work(6000);
      frap_unlock(&g_r3);

      usleep(5000);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_remoteA1 finished\n");
  return NULL;
}

/* remoteB0 (index 6): CPU2, heavier on R1 (requests twice per period in JSON) */
static void *worker_remoteB0(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_remoteB0 started on CPU2, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(2);
  for (int i = 0; i < 200; i++)
    {
      /* simulate two quick requests per cycle */
      frap_set_spin_prio(arr[1]); /* set spin prio for R1 */
      frap_lock(&g_r1);
      busy_work(2200);
      frap_unlock(&g_r1);

      usleep(1000);

      frap_set_spin_prio(arr[1]); /* set spin prio for R1 */
      frap_lock(&g_r1);
      busy_work(2200);
      frap_unlock(&g_r1);

      usleep(3000);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_remoteB0 finished\n");
  return NULL;
}

/* background (index 7): CPU2, occasional R3 access */
static void *worker_background(void *arg)
{
  int *arr = (int *)arg;

  /* DEBUG */
  printf("[FRAPDEMO] worker_background started on CPU2, spin_prio: "
         "R0=%d R1=%d R2=%d R3=%d\n",
         arr[0], arr[1], arr[2], arr[3]);

  pin_to_cpu(2);
  for (int i = 0; i < 120; i++)
    {
      frap_set_spin_prio(arr[3]); /* set spin prio for R3 */
      frap_lock(&g_r3);
      busy_work(2000);
      frap_unlock(&g_r3);
      usleep(7000);
    }

  /* DEBUG */
  printf("[FRAPDEMO] worker_background finished\n");
  return NULL;
}

/* mapping pid_hint index -> worker function */
static void *(*worker_table[WORKER_NUM])(void *) = {
  worker_hot0,      /* pid_hint 0 */
  worker_hot1,      /* pid_hint 1 */
  worker_mid0,      /* pid_hint 2 */
  worker_mid1,      /* pid_hint 3 */
  worker_remoteA0,  /* pid_hint 4 */
  worker_remoteA1,  /* pid_hint 5 */
  worker_remoteB0,  /* pid_hint 6 */
  worker_background /* pid_hint 7 */
};

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  printf("\n[FRAPDEMO] Starting generated FRAP demo...\n");

  /* init FRAP resources matching config ids */
  frap_res_init(&g_r0, 0, true);
  frap_res_init(&g_r1, 1, true);
  frap_res_init(&g_r2, 2, true);
  frap_res_init(&g_r3, 3, true);

  /* Apply generated FRAP spin-priority table (frap_generated_table) */
  printf("[FRAPDEMO] Applying generated FRAP spin-priority table...\n");
  int arrs[WORKER_NUM][4] = {0};
  for (int e = 0; e < frap_generated_table_len; e++)
    {
      const struct frap_cfg_entry *ent = &frap_generated_table[e];
      int idx = ent->pid_hint;
      if (idx < 0 || idx >= WORKER_NUM)
        {
          printf("  SKIP invalid pid_hint %d\n", idx);
          continue;
        }
      arrs[idx][ent->resid] = ent->spin_prio;
    }

  printf("===== spin_prio arrs table =====\n");
  printf("      ");
  for (int r = 0; r < 4; r++)
    {
      printf(" R%d ", r);
    }
  printf("\n");

  for (int i = 0; i < WORKER_NUM; i++)
    {
      printf("T%02d: ", i);
      for (int r = 0; r < 4; r++)
        {
          printf("%3d ", arrs[i][r]);
        }
      printf("\n");
    }

  printf("=================================\n");

  /* create worker threads in the same order as pid_hint in JSON */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_t attr;
      pthread_attr_init(&attr);

      struct sched_param param;
      /* Set a reasonable scheduling priority mapping for threads:
       * For demonstration we set thread base priority around values used in JSON.
       * Note: actual kernel priority interpretation must match P values in JSON.
       */
      int base_prio = 50;
      switch (i)
        {
          case 0: base_prio = 240; break;
          case 1: base_prio = 238; break;
          case 2: base_prio = 200; break;
          case 3: base_prio = 190; break;
          case 4: base_prio = 120; break;
          case 5: base_prio = 110; break;
          case 6: base_prio = 115; break;
          case 7: base_prio = 60;  break;
          default: base_prio = 50;
        }

      param.sched_priority = base_prio;
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      pthread_attr_setschedparam(&attr, &param);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

      /* DEBUG: show thread config before creation */
      printf("[FRAPDEMO] Creating worker %d: base_prio=%d, spin_prio "
             "R0=%d R1=%d R2=%d R3=%d\n",
             i, base_prio,
             arrs[i][0], arrs[i][1], arrs[i][2], arrs[i][3]);

      int ret = pthread_create(&workers[i], &attr,
                               worker_table[i], (void *)arrs[i]);
      if (ret != 0)
        {
          printf("[FRAPDEMO] Failed to create worker %d (ret=%d)\n", i, ret);
        }

      pthread_attr_destroy(&attr);
      usleep(10000); /* stagger creation slightly */
    }

  /* join all threads (they are finite loops so join will eventually return) */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_join(workers[i], NULL);
      /* DEBUG */
      printf("[FRAPDEMO] worker %d joined\n", i);
    }

  printf("[FRAPDEMO] demo finished\n");
  return 0;
}