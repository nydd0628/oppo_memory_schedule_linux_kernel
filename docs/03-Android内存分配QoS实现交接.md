# Android 内存分配 QoS 实现交接

**V2 · 前台专池与后台分配许可 · Pixel 7（pantah）· 分支 `memory-QoS`**

内核基线是 `6.1.124-android14-11-g8769cc47188c`。旧实验提交 `6243d7eb29ff` 的前后台共用一个池，双方都在原生快路径失败后排队。当前实现提交 `9222cb89417c` 改为前台专池、后台许可和 `kswapd` 回收补池。原设计和用例可查 01、02，涉及后台入口和授权定义的地方以本文为准。[原详细设计](01-Android内存分配QoS详细设计.md) · [原测试计划](02-Android内存分配QoS测试计划.md)

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
2. **优先接收回收页。** `mm/vmscan.c` 的 `shrink_folio_list()` 仅对全局 `kswapd` 回收的 order-0 释放批次，完成 memcg 解账和 TLB flush 后进入 QoS 释放链。普通直接回收、限定 memcg 回收和其他释放入口沿用原生流程。
3. **在普通链之前接收。** `mm/page_alloc.c` 运行原有释放准备，再在进 PCP／buddy 前择取兼容页，直到池达到目标或这批候选用完。跳过硬件坏页、已隔离页块、CMA 和紧急储备类型。接收时用 `prep_new_page()` 恢复分配状态，处理零填充、KASAN、架构检查和页引用。池准备之外的剩余页继续走普通释放路径。
4. **普通内存的保护线。** 普通空闲内存过不了 Zone MIN 检查时暂停补池，过了 LOW 检查再恢复。有余量时才收回收页。普通补页工作作为备用来源，只在预计扣掉本批后仍过得了 HIGH 检查时从原生分配器补。备用源本身不发起回收。达到普通补页连续失败上限时会暂停，成功接收回收页或执行 `recover` 可复位失败状态。水位约束仍独立生效。
5. **回收进度对账。** 一批实际转入前台池的页从这次 `nr_reclaimed` 中扣除。池账独立记录每种来源。`kswapd` 按普通分配器实际得到的页计算回收进度，进度不足时沿用原有的回收判断。

`F` 为空闲池页，`G` 为前台已授权未领的页，`R` 为补页预留槽，`retiring` 为准备还给普通分配器的页。合计受目标和最大容量约束。静止时 `obtained = F + G + delivered + returned`，有待退页时右侧加 `retiring`。停机检查 F/G/R/retiring 和 `live` 清零及 `balanced=1`。FGS 许可另有 `permits_pending` 和 `permits_used`，不占池容量，停机后 pending 也清零。

Apple 公开的 XNU 将 *secluded memory* 用于改善内存紧张时的相机启动。此次借鉴了按用途隔离的少量库存，以及用不同水位暂停和恢复的思路。TOP/FGS 身份、W3 两类轮转、后台许可和回退规则属于本实验。XNU 按进程优先级处理内存压力的 Jetsam、应用冻结和压缩属于另一个控制层，留作独立对照。[XNU 专池与 Jetsam](https://github.com/apple-oss-distributions/xnu/blob/main/doc/vm/memorystatus.md#memory-monitoring) · [XNU 压力阈值](https://github.com/apple-oss-distributions/xnu/blob/main/doc/vm/memorystatus_notify.md#pressure-thresholds)

## 4　控制和采集

GKI 启用 `CONFIG_MEM_ALLOC_QOS`，开机状态 OFF。挂载 debugfs 后，由具备 `CAP_SYS_ADMIN` 的控制进程访问 `/sys/kernel/debug/mem_alloc_qos/`。配置命令格式与 V1 相同，模式的实际行为采用第 1 节。

```text
config MODE DOMAIN TOP_WEIGHT FGS_WEIGHT TARGET MAX LOW REFILL_BATCH INIT_BATCH BACKOFF_US FAILURE_LIMIT DEPTH BUDGET_US GRANT_BATCH TRACE RUN_ID TOP_TGID FGS_TGID
start
stop
recover
cancel SESSION REQUEST_ID
```

| 参数 | 规则 |
|---|---|
| MODE，DOMAIN | MODE 为 0=N、1=P、2=F、3=W。DOMAIN 为 0=driver、1=anon。N 的池和队列字段为零，P 的队列字段为零。 |
| 权重 | 仅 W 非零，TOP 1–4、FGS 1。F 比较两个可服务队首的入队序号。 |
| 池 | `0 < low < target ≤ max ≤ 128`。启动和补页批量 1–16，退避 500–8000 μs，普通补页失败上限 1–8 批。 |
| 排队 | 每类深度 1–64，授予批量 1–8。匿名域预算 100–1000 μs，驱动域最长 1,000,000 μs 供诊断。 |
| 采集与身份 | TRACE 为 0 核心计数、1 加路径计数、2 加请求事件。后台许可实际成功及旁路结果在所有档位计数。RUN_ID 为非零 64 位整数，两个 TGID 需存活且不同。 |

示例配置使用匿名 W3、32 页、250 μs 和计数档。

```sh
q=/sys/kernel/debug/mem_alloc_qos
printf 'config 3 1 3 1 32 32 16 4 4 1000 3 64 250 1 1 36001 %s %s\n' \
    "$top_tgid" "$fgs_tgid" > "$q/control"
printf 'start\n' > "$q/control"
cat "$q/status"
# 在负载完成后保存最后一次状态。
printf 'stop\n' > "$q/control"
cat "$q/status"
```

`status` 的 `refill_sources` 区分 `reclaim`、`ordinary` 和水位暂停，启动预填数量单列为 `initialized`。后台的 `grants` 是授权次数，`permits_used` 是领取次数，`permitted_ok/fail` 是持许可进入原生分配器的结果。`fgs_bypass` 原因行与 `fgs_bypass_ok/fail` 结果分开保存。上面的 `claimed` 只计实体池页，后台应为零。

`driver` 域可向 `alloc` 写 `auto COOKIE HOLD_US` 或 `force COOKIE HOLD_US`。驱动会检查整页清零，写入内容，持有指定时间再释放。force 可以让前台跳过首次快路径用于诊断，后台在 F/W 使用 auto 就会进许可队列。诊断命令 `test manual 1`、`test step`、`test hold ROLE`、`test fail_after N` 用于控制授权、领取和普通补页。新增 `test reclaim N` 以 N 张写过数据的驱动页走同一条释放准备、接收和重新初始化路径。这些人工页也计入 `reclaim`，正式追回收来源的运行使用自然压力并关闭人工注入。

### 事件

在 tracefs 设置 `trace_clock=mono` 并启用 `events/mem_alloc_qos/enable`。仍以 `session + id` 关联一次请求，以 `session + page` 关联池页的生命周期。

| phase | 读法 |
|---|---|
| `enqueue`、`grant`、`claim`、`revoke` | 前台实体页的 grant/claim/revoke 共享同一页编号及授权序号。入队 ordinal 是全局入队序号。 |
| `grant_permit`、`claim_permit`、`revoke_permit` | 后台许可字段 `page=0`。通过会话、请求号和授权序号关联。 |
| 两种 `grant` | `detail=1` 是两类可同时服务时的轮转；`detail=0` 属于单类可服务。 |
| `enter`、`return` | 量受控分配的端到端时延。return 的 `detail=2×path+success`，路径 0–4 为快、慢、池交付、准备失败、取消，5 为后台有许可的原生分配，6 为后台旁路后的原生分配。 |
| `fallback`、`cancel`、两种 `revoke` | detail 为旁路／撤销原因。0–11 与旧接口相同，12 为 P 模式后台不能用池。 |
| 池事件 | 新增 `reclaim_batch`、`pressure_pause` 与 `pressure_resume`，普通补页继续用 `refill_batch`。 |

E2 的匿名域父请求号仍待压力包联动，当前事件 `parent=0`。覆核时先确认一轮有自然前台慢路径、后台许可和池供给，然后再比较尾延迟、实际交付、旁路率与普通回收的变化。FGS 获得许可后有可能进入原生慢路径或直接回收，需要和许可排队时延分别观察。

## 5　构建和验证

从 Pixel Kernel 工作区根目录构建，使用本地 GKI 源码。

```sh
tools/bazel run --config=use_source_tree_aosp --config=stamp --config=pantah \
    //private/devices/google/pantah:gs201_pantah_dist \
    -- --dist_dir=out/pantah/memory-QoS-v2
```

Pixel 7 源码构建完成。`out/pantah/memory-QoS-v2/` 包含 `boot.img`、`vendor_kernel_boot.img`、`dtbo.img`、`system_dlkm.img` 和 `vendor_dlkm.img`。解包并解压 `boot.img` 的内核后，内容与通过 QEMU 自测的 `Image` 一致。`boot.img` 的 SHA-256 为 `ba8b1ef3c3b369ef646cf60909f01bad94002f598e10c52ae363625169b35b33`。构建配置启用 QoS、4 KiB 基础页、内存热移除和 ZONE_DEVICE。

`tools/testing/selftests/vm/mem_alloc_qos.sh` 与配套 worker 使用独立 trace 实例并在结束时停机对账。ARM64 lockdep/debug-list 内核连续通过三轮。覆盖前台专池、后台自动进队、FIFO、WRR 1–4∶1、池空时后台许可、回收后轮转续接、脏数据页重新清零、队满与超时旁路、未领许可与实体页撤销、清理及匿名入口。

另在 224 MiB 的 ARM64 QEMU 上实测 `kswapd`。用 P 模式启动 8 页前台池，使用 `test fail_after 0` 注入普通补页失败。先让前台领空池子，再从只读 virtio/ext2 盘读取 384 MiB 文件形成文件缓存压力。三轮均靠真实回收触发，未调用 `test reclaim`。每轮 `reclaim=8`、`ordinary=0`，`pgsteal_kswapd` 从 0 增至 53,139–54,195，均记录一次水位暂停后恢复。前台再次领取八页时通过了整页清零检查，停机对账成立。

源码构建的 GKI `6.1.124-android14-11-g9222cb89417c` 在启用 MTE 和硬件标签 KASAN 的 QEMU 上也通过完整自测。用 GKI 配置关闭 QoS 后，也编译验证了页分配器、匿名缺页和回收对象。

本机无连接的 Pixel 7，E2/E3 真机收益与后台代价待实测。真实负载需额外记录前台池页命中、背景原生分配失败、`pgsteal_kswapd`、直接回收、Zone 水位及应用成功率。用 N、P、F、W1、W3 及旧版共用池提交做对照时，分别登记内核、策略和计数口径。原计划中的独立锁耗时与 E2 父子关联工具仍待采集侧补齐。
