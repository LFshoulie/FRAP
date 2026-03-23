/****************************************************************************
 * apps/system/frapdemo/frapdemo_workload.c
 *
 * FRAP Demo with Validation (Actual vs Expected Check)
 * Fixed: Unused function error & Macro collision
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <errno.h>

#include <nuttx/frap.h>
#include "tlsf.h"
#include "rbtree.h"
#include "frap_table_generated.h"

/* ====================== Debug Helpers ====================== */

/* [修复] 添加 __attribute__((unused)) 防止编译器报错 "defined but not used" */
static int get_cpu_id(void) __attribute__((unused));
static int get_cpu_id(void)
{
#ifdef CONFIG_SMP
  return sched_getcpu();
#else
  return 0;
#endif
}

#define DBG_LOG(fmt, ...) \
  do { \
    printf("[CPU%d] " fmt, get_cpu_id(), ##__VA_ARGS__); \
    fflush(stdout); \
  } while (0)

/* ====================== Configuration ====================== */

#define WORKER_NUM 8
#define RESOURCE_NUM 6

#define ID_R0     0
#define ID_R1     1
#define ID_R2     2
#define ID_R3     3
#define ID_TLSF   4
#define ID_RBTREE 5

/* 循环次数 */
#define LOOP_COUNT 2000 
#define TLSF_POOL_SIZE (1024 * 1024)

/* ====================== Statistics Structures ====================== */

/* 记录每个 Worker 对每个资源的实际访问次数 */
struct worker_stats_s {
  unsigned long long total_ops;
  unsigned long long res_access[RESOURCE_NUM];
};

static struct worker_stats_s g_stats[WORKER_NUM];

/* ====================== Global Resources ====================== */

/* 显式声明，不依赖静态初始化 */
static pthread_mutex_t start_lock;
static pthread_cond_t start_cond;
static bool start_flag = false;

static struct frap_res g_res[RESOURCE_NUM];
static tlsf_t g_tlsf;
static void *g_tlsf_pool;
static struct rb_root g_rbroot;
static int arrs[WORKER_NUM][RESOURCE_NUM];
static pthread_t workers[WORKER_NUM];

/* ====================== Helpers ====================== */

static void pin_to_cpu(int cpu)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPUAFFINITY)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);
#endif
}

static void busy_work(unsigned loops)
{
  volatile unsigned long long s = 0;
  while (loops--) s += loops;
}

static void wait_for_start(const char *name)
{
  // DBG_LOG("[%s] Ready.\n", name);
  pthread_mutex_lock(&start_lock);
  while (!start_flag)
    {
      pthread_cond_wait(&start_cond, &start_lock);
    }
  pthread_mutex_unlock(&start_lock);
}

/* ====================== Workload Logic ====================== */

static void logic_tlsf(int spin_prio, int worker_id)
{
  if (spin_prio <= 0) return;

  frap_set_spin_prio(spin_prio);
  frap_lock(&g_res[ID_TLSF]);

  void *p = tlsf_malloc(g_tlsf, 64);
  if (p)
    {
      *(volatile int *)p = 0x12345678;
      tlsf_free(g_tlsf, p);
    }

  frap_unlock(&g_res[ID_TLSF]);
  
  /* 统计 */
  g_stats[worker_id].res_access[ID_TLSF]++;
}

static void logic_rbtree(int spin_prio, int worker_id)
{
  if (spin_prio <= 0) return;

  unsigned long key = (unsigned long)((worker_id * 10000) + (rand() % 9999));
  
  struct mynode *node = (struct mynode *)malloc(sizeof(struct mynode));
  if (!node) return;
  node->key = key;

  frap_set_spin_prio(spin_prio);
  frap_lock(&g_res[ID_RBTREE]);

  if (my_insert(&g_rbroot, node) == 1)
    {
      rb_erase(&node->node, &g_rbroot);
    }
  
  frap_unlock(&g_res[ID_RBTREE]);
  free(node);

  /* 统计 */
  g_stats[worker_id].res_access[ID_RBTREE]++;
}

static void logic_generic(int res_id, int spin_prio, int cost, int worker_id)
{
  if (spin_prio <= 0) return;

  frap_set_spin_prio(spin_prio);
  
  frap_lock(&g_res[res_id]);
  busy_work(cost * 100);
  frap_unlock(&g_res[res_id]);

  /* 统计 */
  g_stats[worker_id].res_access[res_id]++;
}

/* ====================== Worker Functions ====================== */

/* 查找 Worker ID 的辅助宏 */
#define GET_WID(prio_ptr) ({ \
  int _id = -1; \
  for(int k=0; k<WORKER_NUM; k++) if(arrs[k] == prio_ptr) _id = k; \
  _id; \
})

#define DEFINE_WORKER(func_name, cpu_id, wname, body) \
static void *func_name(void *arg) { \
  int *prio = (int *)arg; \
  int id = GET_WID(prio); \
  pin_to_cpu(cpu_id); \
  wait_for_start(wname); \
  unsigned long long cnt = 0; \
  for (int i = 0; i < LOOP_COUNT; i++) { \
    body \
    usleep(100); \
    cnt++; \
  } \
  if(id >= 0) g_stats[id].total_ops = cnt; \
  return NULL; \
}

/* 根据 JSON 定义的任务逻辑 (同时增加 ID 参数用于统计) */

/* hot0: R0, R1, TLSF, RBTREE */
DEFINE_WORKER(worker_hot0, 0, "hot0", {
  logic_generic(ID_R0, prio[ID_R0], 2, id);
  logic_generic(ID_R1, prio[ID_R1], 5, id);
  logic_tlsf(prio[ID_TLSF], id);
  logic_rbtree(prio[ID_RBTREE], id);
})

/* hot1: R0, R1, TLSF */
DEFINE_WORKER(worker_hot1, 0, "hot1", {
  logic_generic(ID_R0, prio[ID_R0], 2, id);
  logic_generic(ID_R1, prio[ID_R1], 5, id);
  logic_tlsf(prio[ID_TLSF], id);
})

/* mid0: R0, R2, RBTREE */
DEFINE_WORKER(worker_mid0, 0, "mid0", {
  logic_generic(ID_R0, prio[ID_R0], 2, id);
  logic_generic(ID_R2, prio[ID_R2], 4, id);
  logic_rbtree(prio[ID_RBTREE], id);
})

/* mid1: R2, R3 */
DEFINE_WORKER(worker_mid1, 0, "mid1", {
  logic_generic(ID_R2, prio[ID_R2], 4, id);
  logic_generic(ID_R3, prio[ID_R3], 8, id);
})

/* remoteA0: R1 */
DEFINE_WORKER(worker_remoteA0, 1, "remoteA0", {
  logic_generic(ID_R1, prio[ID_R1], 5, id);
})

/* remoteA1: R1, R3, RBTREE */
DEFINE_WORKER(worker_remoteA1, 1, "remoteA1", {
  logic_generic(ID_R1, prio[ID_R1], 5, id);
  logic_generic(ID_R3, prio[ID_R3], 8, id);
  logic_rbtree(prio[ID_RBTREE], id);
})

/* remoteB0: R1 (Twice) */
DEFINE_WORKER(worker_remoteB0, 2, "remoteB0", {
  logic_generic(ID_R1, prio[ID_R1], 5, id);
  logic_generic(ID_R1, prio[ID_R1], 5, id); /* 访问两次 */
})

/* background: R3 */
DEFINE_WORKER(worker_background, 2, "background", {
  logic_generic(ID_R3, prio[ID_R3], 8, id);
})

static void *(*worker_table[WORKER_NUM])(void *) = {
  worker_hot0, worker_hot1, worker_mid0, worker_mid1,
  worker_remoteA0, worker_remoteA1, worker_remoteB0, worker_background
};

/* ====================== Verification Logic ====================== */

/* 定义每个 Worker 单次循环中预期访问各资源的次数 (基于 JSON 的 req 字段) */
static const int g_expected_per_loop[WORKER_NUM][RESOURCE_NUM] = {
    /* R0, R1, R2, R3, TLSF, TREE */
    {  1,  1,  0,  0,  1,    1 },   /* hot0 */
    {  1,  1,  0,  0,  1,    0 },   /* hot1 */
    {  1,  0,  1,  0,  0,    1 },   /* mid0 */
    {  0,  0,  1,  1,  0,    0 },   /* mid1 */
    {  0,  1,  0,  0,  0,    0 },   /* remoteA0 */
    {  0,  1,  0,  1,  0,    1 },   /* remoteA1 */
    {  0,  2,  0,  0,  0,    0 },   /* remoteB0 (2x R1) */
    {  0,  0,  0,  1,  0,    0 }    /* background */
};

static void print_validation_report(void)
{
    printf("\n========================================================================\n");
    printf("                  FRAP VALIDATION REPORT (Loop=%d)\n", LOOP_COUNT);
    printf("========================================================================\n");
    printf("%-12s | %-8s | %-12s | %-12s | %-6s\n", 
           "Worker", "Resource", "Actual Ops", "Expected Ops", "Status");
    printf("------------------------------------------------------------------------\n");

    unsigned long long total_actual_all = 0;
    unsigned long long total_expected_all = 0;
    bool all_pass = true;

    const char *wnames[] = {"hot0", "hot1", "mid0", "mid1", 
                            "rmA0", "rmA1", "rmB0", "backg"};
    const char *rnames[] = {"R0", "R1", "R2", "R3", "TLSF", "TREE"};

    /* 逐个检查每个 Worker 的资源访问情况 */
    for (int i = 0; i < WORKER_NUM; i++)
    {
        for (int r = 0; r < RESOURCE_NUM; r++)
        {
            int expected_per_loop = g_expected_per_loop[i][r];
            /* 只有当预期该资源被访问时才打印检查 */
            if (expected_per_loop > 0)
            {
                unsigned long long expected = (unsigned long long)LOOP_COUNT * expected_per_loop;
                unsigned long long actual = g_stats[i].res_access[r];
                
                total_expected_all += expected;
                total_actual_all += actual;

                bool match = (expected == actual);
                if (!match) all_pass = false;

                printf("%-12s | %-8s | %-12llu | %-12llu | %s\n", 
                       wnames[i], rnames[r], actual, expected, 
                       match ? "PASS" : "FAIL");
            }
        }
    }
    printf("------------------------------------------------------------------------\n");

    printf("\n>>> GRAND TOTAL SUMMARY <<<\n");
    printf("Total Resource Accesses (Actual)   : %llu\n", total_actual_all);
    printf("Total Resource Accesses (Expected) : %llu\n", total_expected_all);
    
    if (total_actual_all == total_expected_all && all_pass)
    {
        printf("\n[RESULT] ==> ALL CHECKS PASSED! <==\n");
    }
    else
    {
        printf("\n[RESULT] ==> VALIDATION FAILED! <==\n");
    }
    printf("========================================================================\n");
}

/* ====================== Main ====================== */

int frapdemo_main(int argc, char *argv[])
{
  printf("\n[FRAPDEMO] Starting Workload Config Demo...\n");

  /* 1. Global Reset */
  start_flag = false;
  memset(g_stats, 0, sizeof(g_stats));
  
  pthread_mutex_init(&start_lock, NULL);
  pthread_cond_init(&start_cond, NULL);

  /* 2. Init Resources */
  for (int i = 0; i < RESOURCE_NUM; i++)
    {
      frap_res_init(&g_res[i], i, true);
    }

  /* 3. Init Logic */
  g_tlsf_pool = malloc(TLSF_POOL_SIZE);
  if (!g_tlsf_pool)
    {
      printf("[Error] TLSF OOM\n");
      return -1;
    }
  g_tlsf = tlsf_create_with_pool(g_tlsf_pool, TLSF_POOL_SIZE);
  my_rbinit(&g_rbroot);

  /* 4. Load Table */
  memset(arrs, 0, sizeof(arrs));
  for (int e = 0; e < frap_generated_table_len; e++)
    {
      const struct frap_cfg_entry *ent = &frap_generated_table[e];
      int idx = ent->pid_hint;
      int rid = ent->resid;
      if (idx >= 0 && idx < WORKER_NUM && rid >= 0 && rid < RESOURCE_NUM)
        {
          arrs[idx][rid] = ent->spin_prio;
        }
    }

  /* 5. Create Threads */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      pthread_attr_t attr;
      struct sched_param param;
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
        }

      pthread_attr_init(&attr);
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      param.sched_priority = base_prio;
      pthread_attr_setschedparam(&attr, &param);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

      printf("[FRAPDEMO] Creating worker %d (Prio %d)\n", i, base_prio);
      fflush(stdout);

      int ret = pthread_create(&workers[i], &attr, worker_table[i], (void *)arrs[i]);
      if (ret != 0) printf("[Error] Failed to create worker %d\n", i);
      
      pthread_attr_destroy(&attr);
      usleep(2000);
    }

  /* 6. Broadcast */
  sleep(1);
  printf("[FRAPDEMO] Broadcasting start signal...\n");
  fflush(stdout);

  struct timespec ts_start, ts_end;

  pthread_mutex_lock(&start_lock);
  start_flag = true;
  pthread_cond_broadcast(&start_cond);
  pthread_mutex_unlock(&start_lock);

  clock_gettime(CLOCK_MONOTONIC, &ts_start);

  /* 7. Join */
  for (int i = 0; i < WORKER_NUM; i++)
    {
      if (workers[i] != 0)
        pthread_join(workers[i], NULL);
    }

  clock_gettime(CLOCK_MONOTONIC, &ts_end);

  /* 8. Results */
  double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                   (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

  /* 打印详细校验报告 */
  print_validation_report();

  printf("Elapsed Time: %.6f s\n", elapsed);

  tlsf_destroy(g_tlsf);
  free(g_tlsf_pool);

  struct rb_node *node;
  while ((node = rb_first(&g_rbroot)))
    {
      struct mynode *data = rb_entry(node, struct mynode, node);
      rb_erase(node, &g_rbroot);
      free(data);
    }

  pthread_mutex_destroy(&start_lock);
  pthread_cond_destroy(&start_cond);

  printf("Cleanup done.\n");
  fflush(stdout);
  
  return 0;
}