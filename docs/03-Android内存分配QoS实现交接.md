# Android 内存分配 QoS 实现交接

**Pixel 7（pantah）· 2026-11-28 · 分支 `memory-QoS`**

基线是 `6.1.124-android14-11-g8769cc47188c` 对应的源码提交 `8769cc47188c3bdca2a34d39ba0f39a945b85465`。实现和自测在其直接子提交 `6243d7eb29ff`。原 V8 实验的末端 `c9f2088ef73d` 仍可从 `fg-page-pool` 分支查阅。设计和测试口径见原输入稿。[详细设计](01-Android内存分配QoS详细设计.md) · [测试计划](02-Android内存分配QoS测试计划.md)

## 1　入口及支持范围

| 位置 | 实现 | 核查点 |
|---|---|---|
| `mm/page_alloc.c` | `alloc_pages_internal()` 复用原生分配逻辑。受控调用通过 `mem_qos_alloc_pages()` 进入，在首轮 `get_page_from_freelist()` 失败后、`__alloc_pages_slowpath()` 之前尝试 QoS | 快速成功直接返回。旁路及等待结束后的回退沿用该次调用原有 GFP 和分配上下文，直接执行原生慢路径一次 |
| `mm/memory.c` | `do_anonymous_page()` 中，静态分支启用后只标记本进程用户态私有匿名缺页的数据页分配 | 接入点在 `anon_vma_prepare()` 后。页表页、共享零页读取、COW、换入和高阶页均走各自原生入口 |
| `mm/mem_alloc_qos.c` | 同一会话选择 `driver` 或 `anon` 域。两个明确登记的 TGID 对应 TOP/FGS，分别持有 `struct pid` 引用 | 使用进程对象匹配，整数 PID 复用时新进程不会继承权限。全局 `__alloc_pages()` 的普通调用从未标记的入口执行 |
| `mm/Kconfig`、`arch/arm64/configs/gki_defconfig` | `CONFIG_MEM_ALLOC_QOS` 选中 ARM64、非 NUMA、支持高精度定时器和 tracing 的实验配置。GKI 构建启用代码，开机状态为 OFF | ARM64 GKI 仍使用 4 KiB 页。开机未创建页池、未登记角色，匿名入口的静态分支关闭 |

`driver` 域使用 `GFP_KERNEL | __GFP_ZERO`。`anon` 域复用该基线 ARM64 匿名分配的 `GFP_HIGHUSER_MOVABLE | __GFP_ZERO | __GFP_CMA | __GFP_COMP`。匿名请求的 `__GFP_CMA` 表示**允许**使用 CMA，QoS 只提供其中合格的非 CMA 页，原生快路径和回退仍可按原规则使用 CMA。MTE 请求带 `__GFP_ZEROTAGS`，因此从池入口旁路，继续由原生分配处理标签。在单节点上仅接收 `ZONE_NORMAL` 的普通页块，核对 cpuset、GFP 作用域、睡眠和紧急储备相关上下文。其他调用点、NUMA 策略和高阶分配目前没有 QoS 接入。

池页由普通分配器完成零填充、架构分配、页引用初始化、KASAN 和页检查处理，交付时直接转移已经准备好的页引用。原生 `mm_page_alloc` 与 page owner 记录的是物理页从分配器出来的时刻和堆栈。QoS 领取时使用自己的 `claim`／`take` 事件。匿名页领取后仍执行 `mem_cgroup_charge()`、swaprate throttle、uptodate、PTE 锁内重检、rmap/LRU 和 `set_pte_at()`。计费或安装失败由原匿名缺页路径释放。QoS 层的 `return pool_ok` 与最终用户态触页成功应分别统计。

## 2　请求怎样取得页

1. **开始。** 会话在登记进程的受控入口通过 SRCU 固定生命周期。追踪档位 2 在分配前生成请求编号并记录 `enter`，档位 0/1 仅在需要仲裁的慢请求生成编号。原生快路径无需 QoS 描述符或仲裁锁。
2. **入队。** F/W 模式先核对条件、每类队长及描述符余额。描述符在启动时预分配，数量为 `2 × depth + max`。入队锁内存下绝对截止时间和全局入队序号。P 模式尝试直接领取一页，池空走慢路径。
3. **授权。** 一个锁保护两条 FIFO、池和绑定。FIFO 比较两条队首的入队序号。WRR 在两个有效队首同时存在时按 TOP 槽再到 FGS 槽分配。每次成功绑定才推进游标。池空、单队服务以及撤销都不改变游标。授权批量限制了一次持锁内处理的请求／过期项／不可用页数。
4. **交付。** 池页与请求一对一绑定后才发通知。调用者用状态谓词睡眠，锁内转移页引用。截止前已授权的请求在恢复时仍可领取。仍在排队的过期请求、信号中断和正常停机走原生慢路径。显式取消的驱动请求返回 `ECANCELED`，匿名缺页继续原生分配。已授权未领取的页可被撤销，已交付的页归调用者。
5. **回收描述符。** 调用者持有一个引用。排队到授权未领期间由链表持有一个引用。锁外唤醒有独立引用，或者接手已摘除的链表引用。最后一个引用退出才归还描述符。停机可以枚举尚未领取的授权。

W3 在双方一直积压、有相容页且无撤销时序列为 TOP、TOP、TOP、FGS。这个保证只约束仲裁服务机会。已经通过原生路径完成的请求，以及授权后线程何时恢复运行，均在该顺序之外。

## 3　页池、补页及清理

- `F` 是空闲页，`G` 是已授权未领页，`R` 是工作项仍持有的补页预留槽。实现另记录 `retiring`，覆盖已移出池但正在锁外归还的页。预留时用 `F + G + R + retiring` 扣除已占容量，容量小于等于 `target`，同时受 `max` 约束。
- STARTING 按 `init_batch` 分批备足目标量。部分失败时全部归还，`status` 保存实得页数、耗时和错误。启动完成后才开放入口。
- 空闲页低于 `low` 且还缺页时，同一个工作项完成分批补页。补页 GFP 保留域内零填充与迁移类型属性，去掉 CMA、直接回收和 kswapd 回收，使用 `__GFP_NOMEMALLOC | __GFP_NOWARN`。普通分配器仍可按自己的水位逻辑返回失败。连续整批没有合格页时按高精度定时器退避，达到失败上限后暂停。用 `recover` 复位失败计数，低水位仍满足时恢复补页。
- 停机先进入 DRAINING、撤下入口，再同步定时器及工作项，撤销排队和未领取的授权，等待 SRCU 区间退出，最后释放剩余池页及描述符。停机等待正在回退的分配调用返回。调用者已领取的页由调用者按原规则释放。
- 静止核对式为 `obtained = F + G + delivered + returned`。运行时有待释放页时右侧另加 `retiring`。OFF 核对 `F=G=R=retiring=0`、`live=0`、`balanced=1`。两类队列的 `grants`、`claimed`、`revoked` 独立记录。P 的 `take` 增加 `claimed`，没有 `grant`。

## 4　实验接口

设备端通过有权限的控制进程访问 `/sys/kernel/debug/mem_alloc_qos/`。`control` 和 `alloc` 是只写入口，`status` 是只读回执。需要挂载 debugfs，写入者需要 `CAP_SYS_ADMIN`。每条命令一次写入。

```text
config MODE DOMAIN TOP_WEIGHT FGS_WEIGHT TARGET MAX LOW REFILL_BATCH INIT_BATCH BACKOFF_US FAILURE_LIMIT DEPTH BUDGET_US GRANT_BATCH TRACE RUN_ID TOP_TGID FGS_TGID
start
stop
recover
cancel SESSION REQUEST_ID
```

| 字段 | 取值与说明 |
|---|---|
| `MODE` | 0=N，1=P，2=F，3=W。N 的池和队列字段均为零，P 的队列相关字段为零。非 W 的两个权重都填零。 |
| `DOMAIN` | 0=driver，1=anon。两类身份填同一控制端 PID 命名空间下的存活 TGID，回执显示全局 TGID。 |
| 池 | 启用时 `0 < low < target ≤ max ≤ 128`，两种补页批量均为 1–16，退避 500–8000 μs，失败上限 1–8 批。 |
| 队列 | 每类深度 1–64，授权批量 1–8。匿名域预算 100–1000 μs。驱动域允许 100–1,000,000 μs，长预算用于可重复的人工步进。W 权重 TOP 为 1–4，FGS 为 1。 |
| `TRACE` | 0 保留池、队列及对账的核心计数。1 加每类各路径结果计数。2 加定向 tracepoint，仍需由采集端启用 tracefs 事件。 |
| `RUN_ID` | 非零 64 位整数，与负载采集目录关联。会话编号 `session` 由内核独立递增，`config_generation` 在整份配置成功提交后递增。 |

示例为 W3、匿名域、32 页、250 μs、计数档。先让两端停在共同 GO 屏障，再填入两端进程真实 TGID。

```sh
q=/sys/kernel/debug/mem_alloc_qos
printf 'config 3 1 3 1 32 32 16 4 4 1000 3 64 250 1 1 36001 %s %s\n' \
    "$top_tgid" "$fgs_tgid" > "$q/control"
printf 'start\n' > "$q/control"
cat "$q/status"
# 采集结束后停止并保存最终回执。
printf 'stop\n' > "$q/control"
cat "$q/status"
```

`status` 依次给出构建版本与页大小、暂存配置、最近会话配置、池账、补页、队列、角色计数和旁路原因。`config_error` 报最近一次提交的字段、字段组或忙状态，失败不替换暂存配置。`start` 再次核对两个已登记进程仍存活。准备新会话前保存旧会话的回执和轨迹。

仅 `driver` 域支持以下功能诊断。

| 接口 | 用途 |
|---|---|
| 向 `alloc` 写 `auto COOKIE HOLD_US` | 当前写入线程走正常首轮快速分配，返回前检查整个页为零，写入后按指定时间持有，再释放。 |
| 向 `alloc` 写 `force COOKIE HOLD_US` | 已登记且池启用时跳过第一次快路径，用于注入慢请求。COOKIE 作为 trace 的 `parent`，范围为 64 位无符号数。 |
| `test manual 1`、`test step`、`test manual 0` | 暂停自动授权，每步执行一个授权批次，随后恢复自动授权。补页独立工作。 |
| `test hold 0\|1\|-1` | 暂停指定类已获得页的强制请求领取，或恢复领取。用于检验异序领取、撤销和停机。 |
| `test fail_after N` | 允许 N 次池分配尝试后注入失败，`0` 立即失败，`-1` 清除。驱动初始化和补页共用此注入状态。 |

## 5　事件如何读

定义在 `include/trace/events/mem_alloc_qos.h`。诊断轮在 tracefs 使用 `mono` 时钟和 `events/mem_alloc_qos/enable`。保留原始轨迹、两端完整导出、各 CPU ring buffer 丢失计数及会话开始／停止回执。

| 事件 | 关联和字段 |
|---|---|
| `mem_qos_request` | 用 `session + id` 关联一次分配，用 `session + page` 关联一次池内持有期。字段含 `run`、`tid`、`role`（0=TOP，1=FGS）、`parent` 和 `phase`。 |
| `enter`／`return` | 计算覆盖入口的全路径时延。enter 的 `detail=1` 是驱动强制样本。return 的 `detail = 2 × path + success`，path 依次为 0 快速、1 原生慢路径、2 池交付、3 原生准备失败、4 显式取消。 |
| `enqueue`／`grant`／`resume`／`claim` | `ordinal` 在入队是全局入队序号，在授权和领取是同一个授权序号。grant 的 `detail=1` 代表两类同时有有效队首。`top`、`fgs` 是事件提交后的队长。 |
| `fallback`／`cancel`／`revoke` | detail 依次为 0 原生、1 上下文、2 GFP、3 Zone/节点、4 空池、5 队满、6 描述符用尽、7 超时、8 信号、9 停机、10 显式取消、11 页属性变化。撤销记录原页和授权序号。 |
| `mem_qos_pool` | 记录初始化、补页预留、批次结果、恢复、会话启停，字段包括 F/G/R、批量或实得页数、连续失败计数和批次耗时。池失败回执和 STOP 对账以 `status` 为准。 |

会话的时延和路径计数仅覆盖已登记进程的受控调用。匿名域的 `parent=0`，E2 侧父触页 ID 仍需与负载包协同发布。也可以先按同一时钟的 TID 与窗口核查覆盖，无法唯一配对的单列。`resume` 记录等待谓词退出的时刻，可能是立即满足、授权通知、超时或信号触发。复盘调度唤醒延迟时，单列未睡眠和与授权并发的请求，用同一请求的 `grant`／`claim` 判定最终归属。当前内核实现提供授权队长、最大队首请求年龄、补页批次耗时及路径结果。独立的锁等待／持锁时长、空池区间自动汇总和应用侧时延分析仍由后续采集及分析补齐。

## 6　构建与验证

从 Pixel Kernel 工作区根目录构建 pantah。Kleaf 默认可能选下载的 GKI，需使用工作区定义的源码配置。

```sh
tools/bazel run --config=use_source_tree_aosp --config=stamp --config=pantah \
    //private/devices/google/pantah:gs201_pantah_dist \
    -- --dist_dir=out/pantah/memory-QoS
```

本次构建完成，产物位于工作区的 `out/pantah/memory-QoS/`。目录包含 `Image`、`boot.img`、`vendor_kernel_boot.img`、`dtbo.img` 和 `system_dlkm.img`。解包 `boot.img` 后，LZ4 内核解压得到的内容与通过 QEMU 自测的 `Image` 一致。`boot.img` 的 SHA-256 为 `8b9638d2a4271a749515e40927919be3ed7ead2d00368d2a506a3d29f5c75fd5`。构建配置确认启用了 QoS、4 KiB 基础页、内存热移除和 ZONE_DEVICE。

功能自测为 `tools/testing/selftests/vm/mem_alloc_qos.sh` 和同目录的 `mem_alloc_qos_worker.c`。测试内核启用 QoS、debugfs 和 tracefs 后，用目标架构工具链执行 `cc -O2 -pthread mem_alloc_qos_worker.c -o mem_alloc_qos_worker`，脚本和 worker 放同目录，以 root 执行。脚本使用独立的 trace 实例，运行完自动停止会话。

QEMU ARM64 virt 使用 2 vCPU、4 GiB RAM、4 KiB 基础页。4 GiB 确保该虚机有位于 4 GiB 地址线以上的 `ZONE_NORMAL` 内存。带 lockdep 和 `DEBUG_LIST` 的测试内核完整自测连续通过三轮。源码构建的 GKI `6.1.124-android14-11-g6243d7eb29ff` 也在启用 MTE 与硬件标签 KASAN 的虚机上通过了同一自测。关闭 `CONFIG_MEM_ALLOC_QOS` 后也单独编译验证了 `mm/page_alloc.o` 和 `mm/memory.o`。

| 测试计划 | 本次证据 |
|---|---|
| T04 | 检查运行中拒绝提交、非法低水位保留旧配置、部分初始化失败归还所有已取页。 |
| T05 | N 的原生快速分配，P 的驱动池取页，以及已经注册的匿名页真实快速成功。其他约束域仍需设备逐项验证。 |
| T06–T08 | 对照同页的 grant/claim/revoke，确认两个类可异序领取。检查 FIFO、WRR 1∶1 至 4∶1、单队游标冻结，以及池耗尽和恢复后 W3 的 2+2 页服务顺序。 |
| T09–T11 | 补页失败、退避暂停、恢复。覆盖超时、队满、授权后显式取消，以及带在途队列和未领取授权的停机、页账与重启。 |
| T12–T22 | QEMU 匿名测试覆盖入口和零填充。Pixel 7 的自然慢路径覆盖、E2/E3 性能及完整压力指标仍待真机测试。 |

本机 `adb devices` 无设备，工作区和本机搜索均未找到配套的 Pixel7_T0_Gates_v39 脚本。E2/E3 没有在本次修改中运行。驱动的强制请求用于验证归属、顺序与并发，T14 的性能结论必须使用自然发生的请求。拿到压力包后先做 T02/T03 和 T13，确认匿名域的自然慢路径样本量及父子关联质量，再做 N/P/F/W1/W3 成对比较。
