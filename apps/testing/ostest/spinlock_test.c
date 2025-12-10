// ...existing code...
/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/config.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <nuttx/atomic.h>
#include <nuttx/spinlock.h>
/* rspinlock and seqlock tests removed */
/* #include <nuttx/seqlock.h> */
/****************************************************************************
 * Preprocessor Definitions
 ****************************************************************************/
#ifndef CONFIG_TEST_LOOP_SCALE
#  define CONFIG_TEST_LOOP_SCALE 1
#endif
#define LOOP_TIMES     (CONFIG_TEST_LOOP_SCALE * 100000)
#define MAX_THREAD_NUM (CONFIG_SMP_NCPUS)

/* Only keep spinlock test; rspinlock and seqlock removed */
enum lock_type_e
{
  SPINLOCK
};

aligned_data(64) struct spinlock_pub_args_s
{
  union
    {
      /* rspinlock_t rlock; */ /* removed */
      spinlock_t lock;
      /* seqcount_t slock; */ /* removed */
    };
  volatile uint32_t counter;
  atomic_t barrier;
  uint32_t thread_num;
};
struct spinlock_thread_args_s
{
  uint64_t delta;
  FAR struct spinlock_pub_args_s *pub;
};

/* Helper functions for timespec calculating */
static inline uint64_t calc_diff(FAR struct timespec *start,
                                    FAR struct timespec *end)
{
  uint64_t diff_sec = end->tv_sec - start->tv_sec;
  long diff_nsec = end->tv_nsec - start->tv_nsec;
  if (diff_nsec < 0)
    {
      diff_sec -= 1;
      diff_nsec += 1000000000L;
    }
  return diff_sec * 1000000000ULL + diff_nsec;
}

/* Macro: generate test thread functions.
 * Note: access the spinlock member as 'lock' (seqlock/rspinlock removed).
 */
#define LOCK_TEST_FUNC(lock_type, lock_func, unlock_func) \
FAR static void * lock_type##_test_thread(FAR void *arg) \
{ \
  struct spinlock_thread_args_s *param = \
                                (struct spinlock_thread_args_s *)arg; \
  irqstate_t flags; \
  struct timespec start; \
  struct timespec end; \
  uint32_t i; \
  FAR struct spinlock_pub_args_s *pub = param->pub; \
  atomic_fetch_add(&pub->barrier, 1); \
  while (atomic_read(&pub->barrier) != pub->thread_num) \
    { \
      usleep(1); \
    } \
  clock_gettime(CLOCK_REALTIME, &start); \
  for (i = 0; i < LOOP_TIMES; i++) \
    { \
      flags = lock_func(&pub->lock); \
      pub->counter++; \
      unlock_func(&pub->lock, flags); \
    } \
  clock_gettime(CLOCK_REALTIME, &end); \
  param->delta = calc_diff(&start, &end); \
  return NULL; \
}

/* rspinlock and seqlock test generators removed */
/* LOCK_TEST_FUNC(rlock, rspin_lock_irqsave, rspin_unlock_irqrestore) */
LOCK_TEST_FUNC(lock, spin_lock_irqsave, spin_unlock_irqrestore)
/* LOCK_TEST_FUNC(slock, write_seqlock_irqsave, write_sequnlock_irqrestore) */

static inline void run_test_thread(
  enum lock_type_e lock_type,
  FAR void *(*thread_func)(FAR void *arg),
  uint32_t thread_num
  )
{
  const char *test_type = "Spin lock";
  printf("Test type: %s\n", test_type);
  pthread_t tid[MAX_THREAD_NUM];
  struct spinlock_pub_args_s pub;
  pthread_attr_t attr;
  struct sched_param sparam;
  struct spinlock_thread_args_s param[MAX_THREAD_NUM];
  struct timespec stime;
  struct timespec etime;
  int i;
  int status;
  cpu_set_t cpu_set;
  /* Set affinity to CPU0 */
  cpu_set = 1u;
  UNUSED(cpu_set);
  if (OK != sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpu_set))
    {
      printf("spinlock_test: ERROR: nxsched_set_affinity failed");
      ASSERT(false);
    }
  pub.counter    = 0;
  pub.thread_num = thread_num;
  atomic_set_release(&pub.barrier, 0);

  /* Only initialize spinlock (seqlock/rspinlock removed) */
  spin_lock_init(&pub.lock);

  /* Boost to maximum priority for test threads. */
  status = pthread_attr_init(&attr);
  if (status != 0)
    {
      printf("spinlock_test: ERROR: "
             "pthread_attr_init failed, status=%d\n",  status);
      ASSERT(false);
    }
  sparam.sched_priority = SCHED_PRIORITY_MAX;
  status = pthread_attr_setschedparam(&attr, &sparam);
  if (status != OK)
    {
      printf("spinlock_test: ERROR: "
             "pthread_attr_setschedparam failed, status=%d\n",  status);
      ASSERT(false);
    }
  clock_gettime(CLOCK_REALTIME, &stime);
  for (i = 0; i < thread_num; i++)
    {
      param[i].pub = &pub;
      param[i].delta = 0;
      /* Set affinity */
      cpu_set = 1u << ((i + 1) % CONFIG_SMP_NCPUS);
      status = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpu_set);
      if (status != OK)
        {
          printf("spinlock_test: ERROR: "
                 "pthread_attr_setaffinity_np failed, status=%d\n",  status);
          ASSERT(false);
        }
      pthread_create(&tid[i], &attr, thread_func, &param[i]);
    }
  for (i = 0; i < thread_num; i++)
    {
      pthread_join(tid[i], NULL);
    }
  clock_gettime(CLOCK_REALTIME, &etime);
  uint64_t total_ns = 0;
  for (i = 0; i < thread_num; i++)
    {
      total_ns += param[i].delta;
    }
  printf("%s: Test Results:\n", test_type);
  printf("%s: Final counter: %" PRIu32 "\n", test_type, pub.counter);
  assert(pub.counter == thread_num * LOOP_TIMES);
  printf("%s: Average throughput : %" PRIu64 " ops/s\n", test_type,
         (uint64_t)NSEC_PER_SEC * LOOP_TIMES * thread_num / total_ns);
  printf("%s: Total execution time: %" PRIu64 " ns\n \n",
         test_type, calc_diff(&stime, &etime));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/
/****************************************************************************
 * Name: spinlock_test
 ****************************************************************************/
static void spinlock_test_thread_num(uint32_t thread_num)
{
  printf("Start Lock test:\n");
  printf("Thread num: %d, Loop times: %d\n\n", thread_num, LOOP_TIMES);
  run_test_thread(SPINLOCK, lock_test_thread, thread_num);
  /* rspinlock and seqlock tests removed */
}
void spinlock_test(void)
{
  uint32_t tnr;
  for (tnr = 1; tnr < CONFIG_SMP_NCPUS; tnr++)
    {
      spinlock_test_thread_num(tnr);
    }
}