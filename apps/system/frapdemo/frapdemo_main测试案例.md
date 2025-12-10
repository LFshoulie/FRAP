##  frapdemo_main测试案例

### 1.整体设定

- **资源（FRAP 全局资源）**：
  - `g_res1`：R1
  - `g_res2`：R2
  - `g_res3`：R3
- **线程总数：12 条**，分成 3 种角色：
  1. **HOT（高优先级）**：4 条
  2. **MID（中优先级）**：4 条
  3. **REMOTE（远程 / 低优先级）**：4 条
- **CPU 绑定**：
  - HOT 全部绑在 **CPU0**
  - MID 也全部绑在 **CPU0**
  - REMOTE 绑在 **非 0 号 CPU**（比如 CPU1），用来制造跨核竞争
- **同步起跑**：
   所有线程在一个 barrier 上等，等 12 条线程都 ready 之后，一起被唤醒开始跑，这样一开始就进入高并发现象。

------

### 2. 三种角色各自的行为

#### 2.1 HOT 线程（高优先级，本地“霸王”）

- **优先级**：最高（`base_prio = maxprio`）
- **CPU**：CPU0
- **每一轮循环：访问两个资源**
  1. `frap_lock(R1, 高自旋优先级)` → 进临界区 → `g_counter_r1++`
  2. `frap_lock(R2, 高自旋优先级)` → 进临界区 → `g_counter_r2++`
- **特点**：
  - 自旋优先级也给到很高（接近“非抢占”），所以在还没进 CS 前就已经几乎不被本核其它线程抢占。
  - 一旦进入临界区，FRAP 内部会 `sched_lock()`，变成非抢占临界区，更不可能被抢。

**作用**：
 在 CPU0 上构造一个：

> “高优先级线程频繁、强势地访问 R1/R2，压着其他本地线程跑”
>  的场景，同时对 R2 和远程线程 REMOTE 形成 arrival blocking 和 spin delay。

------

#### 2.2 MID 线程（中优先级，本地“被欺负”的那一拨）

- **优先级**：中等（比 HOT 低、比 REMOTE 高）
- **CPU**：CPU0（跟 HOT 在同一个核）
- **每一轮循环：访问两个资源**
  1. `frap_lock(R1, spin_prio = base_prio)` → `g_counter_r1++`
  2. `frap_lock(R3, spin_prio = base_prio)` → `g_counter_r3++`
- **关键点**：
  - **自旋优先级 = base 优先级**，不提升，类似 PWLP 风格：
    - 在自旋阶段，很容易被 HOT 抢占。
  - 当 MID 正在自旋时：
    - HOT 到来、优先级更高 → 调度器切走 MID →
    - FRAP 的 `frap_on_preempt()` 看到“被抢的是在自旋、还没进 CS 的任务”，就：
      1. 把 MID 从资源的 FIFO 队列里摘掉；
      2. 把它的优先级恢复到 base；
      3. 打一个 `cancelled` 标记；
    - MID 以后被重新调度到 CPU0 运行时，会在 `frap_lock` 的循环里看到 `cancelled`，然后 **重新排到 FIFO 队尾** 再等一次。

**作用**：
 在 CPU0 上制造大量 **“自旋中被高优先级任务抢占 → 取消 → 尾插重排”** 的 R3 场景，同时验证：

- FIFO 队列在多次 cancel/requeue 下是否仍然正确；
- FRAP 的 additional blocking（Wi）行为是否符合预期；
- 不会因为 cancel 逻辑导致死锁或计数丢失。

------

#### 2.3 REMOTE 线程（远程 CPU、低优先级）

- **优先级**：相对较低（base_prio = low）
- **CPU**：绑在 CPU1（或者其它非 0 核）
- **每一轮循环：访问一个资源**
  - 只访问 `R2`：`frap_lock(R2, 中等 spin_prio)` → `g_counter_r2++`
- **特点**：
  - 它们跟 HOT/MID 不在同一颗 CPU 上，但都在抢 **同一个资源 R2**。
  - 因为 REMOTE 优先级低，所以经常被 HOT/MID 堵着，形成 remote blocking（Ei）。

**作用**：
 制造 **跨核对同一个 FRAP 资源的竞争**，触发论文里面讲的：

- remote spin delay（Ei）
- arrival blocking + additional blocking 混合情况