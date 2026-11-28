# Android 内存分配 QoS 实现交接

**V3 · 同步回收补池与在线调容 · Pixel 7（pantah）· 分支 `memory-QoS`**

内核基线是 `6.1.124-android14-11-g8769cc47188c`。V1 提交 `6243d7eb29ff` 使用前后台共用池。V2 提交 `9222cb89417c` 引入前台专池、后台许可和 `kswapd` 回收补池。V3 提交 `f3abdb17adf6` 进一步接入符合条件的直接回收和 memcg 回收，并支持在线手动、自动调容。原设计和用例可查 01、02，当前行为以本文为准。[原详细设计](01-Android内存分配QoS详细设计.md) · [原测试计划](02-Android内存分配QoS测试计划.md)

## 1　申请从哪里进入

| 对象 | 开始分配时 | 进入队列后 | 退出 |
|---|---|---|---|
| TOP 前台，F/W | 先尝试一次原生快路径。成功就直接交付。相容的失败请求才排队。 | 获选时绑定一张前台专池的实体页，恢复运行后领取。 | 池页直接返回分配调用方。超时、队满或停机等使用本次请求原有的原生慢路径。 |
| FGS 后台，F/W | 在调用普通页分配器之前核对条件。相容请求直接排队。 | 获选时得到可消费一次的许可。领取许可后，由原请求线程调用普通页分配器，走原有快慢路径。 | 许可与实际分配结果分别统计。队满、超时、信号或停机等可旁路到原生分配，分别记原因及成功、失败结果。 |
| N 原生 | 两边走原生快慢路径。 | 可配置会话观测路径。 | 池和仲裁都关闭。 |
| P 仅池 | TOP 先尝试快路径，失败后尝试立即取一张池页。FGS 走原生快慢路径。 | 两边都无需排队。 | 这个模式用于单独测前台备页的代价和收益。 |

许可最多触发一次原生分配调用。这个调用仍可能直接回收、失败或比前台晚完成。FGS 旁路也只调用一次原生分配器。驱动显式取消返回 `ECANCELED`。匿名缺页的显式取消按原生语义继续分配。

### 接入位置及检查

- `mm/page_alloc.c` 的 `mem_qos_alloc_pages()` 在真正进入 `__alloc_pages()` 前处理 FGS 许可。TOP 的接入仍位于第一次 `get_page_from_freelist()` 失败后、`__alloc_pages_slowpath()` 之前。普通分配的入口使用相同的原生逻辑。
- `mm/memory.c` 的 `do_anonymous_page()` 只标记本进程用户态私有匿名缺页的数据页。辅助页表页、COW、换入和高阶页走各自原生路径。参与会话的两个进程由有权限的控制端显式登记，`struct pid` 引用隔离数字 PID 的复用。
- 会话选择 `driver` 或 `anon` 域。池只接收 ARM64 单节点 `ZONE_NORMAL` 的兼容 order-0 页，检查 GFP、cpuset、睡眠、紧急储备等边界。匿名 MTE 页需要不同的标签初始化，走原生路径。池页不依赖 CMA。
- 匿名页交付后仍由原缺页函数执行 memcg 计费、swaprate throttle、uptodate、PTE 锁内复查、rmap/LRU 和页表安装。上层失败沿用其原有释放路径。池页交付、FGS 原生取得页和应用触页完成分别统计。

## 2　3∶1 在哪里生效

`mm/mem_alloc_qos.c` 用同一个自旋锁保护两个 FIFO、请求状态、槽位游标、绑定页和待领许可。F 从**当下可服务**的队首中按全局入队序号挑最早者。W 在两个队首都可服务时按权重轮转，W3 是三个 TOP 实体页授权加一个 FGS 分配许可。授权时游标向前一步，撤销不会倒拨。

TOP 可服务需要队首未过期且池里有兼容页。FGS 可服务需要队首未过期。若前台池暂时空了，FGS 可以单独获得许可，双队轮转位置冻结。池补上后双方继续原来的位置。举例来说，分完两个 TOP 槽后池空，FGS 可获单独许可。等双方恢复可服务，先完成第三个 TOP 槽，再轮到 FGS。只有一个队列可服务时，同样冻结游标。3∶1 的统计分母是双方持续可服务期间的授权机会。实际成功比例、回退和后台完成率单报。

前台实体页或后台许可提交后才在锁外定向通知。每次领取和撤销由同一把锁裁决。前台池页在撤销后归池，后台撤销仅消除许可。停机路径撤下入口、同步补页工作、排空排队与未领授权，再等待在途 SRCU 调用退出后释放存量页。调用者已领取的页或正在执行的原生分配由调用者负责。

## 3　刚回收的页怎么补进池

1. **启动。** 复用原生无直接回收分配，分批预填到目标。部分失败时全量归还，服务保持 OFF。
2. **优先接收回收页。** `mm/vmscan.c` 的 `shrink_folio_list()` 在 memcg 解账和 TLB flush 后，把这三个来源的合格单页批次送入 QoS 释放链。来源为全局 `kswapd`、普通 order-0 且可以从 `ZONE_NORMAL` 获取页的分配直接回收，以及 memcg LRU 回收。高阶分配触发的直接回收、特殊储备和休眠回收沿用原生路径。同步回收每次候选链至少保留一张可供普通分配的相容页，只有一张时全量交回原生路径。
3. **在普通链之前接收。** `mm/page_alloc.c` 运行原有释放准备，再在进 PCP／buddy 前择取兼容页，直到池达到目标或这批候选用完。跳过硬件坏页、已隔离页块、CMA 和紧急储备类型。接收时用 `prep_new_page()` 恢复分配状态，处理零填充、KASAN、架构检查和页引用。池准备之外的剩余页继续走普通释放路径。
4. **普通内存的保护线。** 普通空闲内存过不了 Zone MIN 检查时暂停补池，过了 LOW 检查再恢复。有余量时才收回收页。普通补页工作作为备用来源，只在预计扣掉本批后仍过得了 HIGH 检查时从原生分配器补。备用源本身不发起回收。达到普通补页连续失败上限时会暂停，成功接收回收页或执行 `recover` 可复位失败状态。水位约束仍独立生效。
5. **回收进度对账。** 全局 `kswapd` 和分配触发的直接回收会从 `nr_reclaimed` 扣除转入前台池的页。普通分配器按实际得到的页计算进度，进度不足时沿用原有回收判断。memcg 回收按原有解账计进度，因为页已从目标组的用量中移除。池的物理页账和 memcg 解账分别核对。

`F` 为空闲池页，`G` 为前台已授权未领的页，`R` 为补页预留槽，`retiring` 为准备还给普通分配器的页。补页以四者的合计作为占用。目标调低时优先分批退还空闲页，已授权页可正常领取，在途补页会按新目标重新核对。缩容收敛前占用可以暂时高于目标，会话最大容量继续约束所有预留和持有。静止时 `obtained = F + G + delivered + returned`，有待退页时右侧加 `retiring`。停机检查 F/G/R/retiring 和 `live` 清零及 `balanced=1`。FGS 许可另有 `permits_pending` 和 `permits_used`，不占池容量，停机后 pending 也清零。

### 在线容量和水位

- 起始 `TARGET` 与预设 `MAX` 来自启动配置。`MAX` 上限为 1024 页，在 4 KiB 页上是 4 MiB。自动调容默认关闭，`target N` 在线修改目标，范围为 1 到会话 `MAX`。增大目标会请求补页并继续遵守供给保护线。缩小时先还空闲页，之后在途页退出时继续收敛。普通补页触发的池内低线取原配置 `LOW` 与当前目标的较小值。
- `auto MIN MAX` 在活动的 P/F/W 会话开启自动调容，要求 `1 ≤ MIN ≤ 当前目标 ≤ MAX ≤ 会话最大容量`。自动每约 250 ms 查看前台合格请求和空池情况。空池出现前台需求、水位通过 HIGH 且扣除本次增量后仍满足 HIGH 时，目标最多翻倍，受自动上限约束。约 1 秒都没有合格前台需求且水位通过 HIGH 时，目标最多减半，受自动下限约束。
- 水位过不了调容检查时，`auto frozen=1`，当前目标和已有库存保留。前台仍可领取库存，补页继续按 Zone MIN/LOW 防护线判断。水位回升后重新采样决定调整。`auto off` 停止自动工作并保留当时的目标，随后可用 `target N`。关闭会话后重新 `start` 会恢复启动配置，自动调容需重新开启。

Apple 公开的 XNU 将 *secluded memory* 用于改善内存紧张时的相机启动。此次借鉴了按用途隔离的少量库存，以及用不同水位暂停和恢复的思路。TOP/FGS 身份、W3 两类轮转、后台许可和回退规则属于本实验。XNU 按进程优先级处理内存压力的 Jetsam、应用冻结和压缩属于另一个控制层，留作独立对照。[XNU 专池与 Jetsam](https://github.com/apple-oss-distributions/xnu/blob/main/doc/vm/memorystatus.md#memory-monitoring) · [XNU 压力阈值](https://github.com/apple-oss-distributions/xnu/blob/main/doc/vm/memorystatus_notify.md#pressure-thresholds)

## 4　控制和采集

GKI 启用 `CONFIG_MEM_ALLOC_QOS`，开机状态 OFF。挂载 debugfs 后，由具备 `CAP_SYS_ADMIN` 的控制进程访问 `/sys/kernel/debug/mem_alloc_qos/`。配置命令格式与 V1 相同，模式的实际行为采用第 1 节。

```text
config MODE DOMAIN TOP_WEIGHT FGS_WEIGHT TARGET MAX LOW REFILL_BATCH INIT_BATCH BACKOFF_US FAILURE_LIMIT DEPTH BUDGET_US GRANT_BATCH TRACE RUN_ID TOP_TGID FGS_TGID
start
stop
recover
cancel SESSION REQUEST_ID
target PAGES
auto MIN MAX
auto off
```

| 参数 | 规则 |
|---|---|
| MODE，DOMAIN | MODE 为 0=N、1=P、2=F、3=W。DOMAIN 为 0=driver、1=anon。N 的池和队列字段为零，P 的队列字段为零。 |
| 权重 | 仅 W 非零，TOP 1–4、FGS 1。F 比较两个可服务队首的入队序号。 |
| 池 | 启动时 `0 < low < target ≤ max ≤ 1024`。启动和补页批量 1–16，退避 500–8000 μs，普通补页失败上限 1–8 批。 |
| 排队 | 每类深度 1–64，授予批量 1–8。匿名域预算 100–1000 μs，驱动域最长 1,000,000 μs 供诊断。 |
| 采集与身份 | TRACE 为 0 核心计数、1 加路径计数、2 加请求事件。后台许可实际成功及旁路结果在所有档位计数。RUN_ID 为非零 64 位整数，两个 TGID 需存活且不同。 |

示例配置使用匿名 W3、32 页、250 μs 和计数档。

```sh
q=/sys/kernel/debug/mem_alloc_qos
printf 'config 3 1 3 1 32 1024 16 4 4 1000 3 64 250 1 1 36001 %s %s\n' \
    "$top_tgid" "$fgs_tgid" > "$q/control"
printf 'start\n' > "$q/control"
printf 'auto 16 256\n' > "$q/control"
cat "$q/status"
# 需要固定目标做对照时关闭自动调容。
printf 'auto off\n' > "$q/control"
printf 'target 128\n' > "$q/control"
cat "$q/status"
# 在负载完成后保存最后一次状态。
printf 'stop\n' > "$q/control"
cat "$q/status"
```

`status` 的 `pool_target` 是当前在线目标。`auto` 回执行给出启停、冻结、容量区间、扩缩次数、前台池尝试和空池次数。`refill_sources` 的 `reclaim` 是 `kswapd`、`direct`、`memcg` 的和，`ordinary` 是备用补页，启动预填数量单列为 `initialized`。后台的 `grants` 是授权次数，`permits_used` 是领取次数，`permitted_ok/fail` 是持许可进入原生分配器的结果。`fgs_bypass` 原因行与 `fgs_bypass_ok/fail` 结果分开保存。上面的 `claimed` 只计实体池页，后台应为零。

`driver` 域可向 `alloc` 写 `auto COOKIE HOLD_US` 或 `force COOKIE HOLD_US`。驱动会检查整页清零，写入内容，持有指定时间再释放。force 可以让前台跳过首次快路径用于诊断，后台在 F/W 使用 auto 就会进许可队列。`test manual 1`、`test step`、`test hold ROLE`、`test fail_after N` 用于控制授权、领取和普通补页。`test reclaim N`、`test reclaim_direct N`、`test reclaim_memcg N` 以写过数据的驱动页跑对应来源的释放准备、接收和重新初始化。这些人工页计入来源统计，正式压力数据需关闭人工注入。

### 事件

在 tracefs 设置 `trace_clock=mono` 并启用 `events/mem_alloc_qos/enable`。仍以 `session + id` 关联一次请求，以 `session + page` 关联池页的生命周期。

| phase | 读法 |
|---|---|
| `enqueue`、`grant`、`claim`、`revoke` | 前台实体页的 grant/claim/revoke 共享同一页编号及授权序号。入队 ordinal 是全局入队序号。 |
| `grant_permit`、`claim_permit`、`revoke_permit` | 后台许可字段 `page=0`。通过会话、请求号和授权序号关联。 |
| 两种 `grant` | `detail=1` 是两类可同时服务时的轮转。`detail=0` 属于单类可服务。 |
| `enter`、`return` | 量受控分配的端到端时延。return 的 `detail=2×path+success`，路径 0–4 为快、慢、池交付、准备失败、取消，5 为后台有许可的原生分配，6 为后台旁路后的原生分配。 |
| `fallback`、`cancel`、两种 `revoke` | detail 为旁路／撤销原因。0–11 与旧接口相同，12 为 P 模式后台不能用池。 |
| 池事件 | `reclaim_batch`、`direct_batch`、`memcg_batch` 区分回收页来源。`pressure_pause` 与 `pressure_resume` 对应补页水位。`auto_freeze`、`auto_resume`、`auto_grow`、`auto_shrink` 对应自动调容。`target` 记录手动目标，`trim` 记录归还批次。 |

E2 的匿名域父请求号仍待压力包联动，当前事件 `parent=0`。覆核时先确认一轮有自然前台慢路径、后台许可和池供给，然后再比较尾延迟、实际交付、旁路率与普通回收的变化。FGS 获得许可后有可能进入原生慢路径或直接回收，需要和许可排队时延分别观察。

## 5　构建和验证

从 Pixel Kernel 工作区根目录构建，使用本地 GKI 源码。

```sh
tools/bazel run --config=use_source_tree_aosp --config=stamp --config=pantah \
    //private/devices/google/pantah:gs201_pantah_dist \
    -- --dist_dir=out/pantah/memory-QoS-v3
```

现有 `out/pantah/memory-QoS-v2/` 镜像对应 V2。V3 编译验证了 ARM64 GKI 的 `mm/page_alloc.o`、`mm/vmscan.o`、`mm/memory.o` 和 `mm/mem_alloc_qos.o`，另构建并启动了 QEMU 测试内核。关闭 QoS 编译选项时，也验证了页分配器、匿名缺页和回收对象。

`tools/testing/selftests/vm/mem_alloc_qos.sh` 在 ARM64 lockdep/debug-list 测试内核连续通过三轮。新增用例覆盖目标 8→256→2、自动扩缩、开启自动时拒绝手动目标、关闭后切回手动、缩容期间已有授权的领取及撤销、同步回收只有一张相容页时留给原生分配器，另外重跑了旧版两类授权、清零和停机对账用例。

在 224 MiB 的 ARM64 QEMU 调整真实 Zone 水位后，观测到自动调容冻结，8 页目标与空闲库存保留，恢复水位后解除冻结。普通补页测试源关闭时，用 `memory.reclaim` 从 memcg 回收的真实页补回 8 页，再用文件缓存压力触发的 `kswapd` 补回另 8 页，普通补页计数为零。

直接回收另用隔离的 QEMU 测试内核受控调用原生 `try_to_free_pages()`。读取 64 MiB 文件建立可回收缓存后，真实全局直接回收从空池补回 8 页，`direct=8`，`pgsteal_direct` 从 0 增到 52，清零和停机对账都通过。编入临时测试内核的入口只用于触发回收。自然压力轮的 `direct=0`，仍需在实际分配失败负载中测量命中和代价。

本机无连接的 Pixel 7，E2/E3 真机收益与后台代价待实测。真实负载需额外记录前台池页命中、背景原生分配失败、`pgsteal_kswapd`、直接回收、Zone 水位及应用成功率。用 N、P、F、W1、W3 及旧版共用池提交做对照时，分别登记内核、策略和计数口径。原计划中的独立锁耗时与 E2 父子关联工具仍待采集侧补齐。
