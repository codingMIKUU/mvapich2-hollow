# OSU / MVAPICH2 用户进程内存测量

`measure_osu_memory.py` 包装已有的 `run_osu_collective.sh`，在各节点启动一个
Python 采样器（本机同账号直接启动，远端通过 SSH），再执行原来的 MPI 实验。只统计本次作业的 OSU rank，包含应用、
MPI 库和映射到进程中的通信资源；不统计 mpiexec、Hydra、采样器、内核线程或模块。

## 直接运行你的实验

在启动实验的机器进入源码目录，运行：

```bash
cd /home/lingbo11/zxm/mvapich2-2.3.7

HOSTS=192.168.1.5,192.168.1.1 \
HCA_MAP=192.168.1.5=mlx5_1,192.168.1.1=mlx5_3 \
USER_MAP=192.168.1.5=lingbo11,192.168.1.1=lingbo12 \
NP=256 PPN=128 \
MV2_IBA_EAGER_THRESHOLD=131072 \
MV2_SHOW_ENV_INFO=1 \
MV2_RNDV_PROTOCOL=RPUT \
MV2_INTER_ALLREDUCE_TUNING=9 \
MV2_INTER_ALLREDUCE_TUNING_TWO_LEVEL=1 \
MEM_INTERVAL=0.2 \
MEM_OUT=memory-results/hollow-allreduce-256-r1 \
python3 contrib/hollow-rc/measure_osu_memory.py \
  hollow allreduce -m 262144:262144 -i 1000 -x 500
```

所有 OSU 参数原样传递给原启动脚本。该例每台预期 128 个 rank，合计 256 个。
不需要重新编译 MPI，也不需要修改或加载内核模块。仅启动节点需要新增文件及
`run_osu_collective.sh` 的修改；远程采样器源码通过 SSH 传送到 Python 内存中执行，
不需要复制脚本到第二台机器、不写远程临时文件。

前提：启动节点和两台计算节点有 Python 3.6+；可以使用 `USER_MAP` 指定的账号
非交互 SSH 登录远端，并读取该账号的 rank 的 `/proc` 文件。
以普通登录用户运行本脚本，不要加 `sudo`：它会切换 SSH 身份和 known_hosts，
并创建 root 所有的输出目录。脚本会在创建输出目录前拒绝这种 sudo 启动方式。
本机 IP/主机名且账号与当前用户一致时，采样器直接本地启动，不需要 SSH 到自己；
原 MPI 启动器本身的 SSH 要求仍由 Hydra 决定。
采样 SSH 使用 BatchMode，不会停在密码提示。连接错误见结果目录的 collector 日志。
连接失败会在终端显示日志中的原因，并明确提示实验尚未启动；此时增加迭代次数无效。
当前脚本支持均匀放置，要求 `NP = PPN × HOSTS 中的主机数`；主机不能重复。

`MEM_OUT` 必须是尚不存在的目录，防止覆盖实验结果。重复实验改为 `-r2`、`-r3`；
不设置 `MEM_OUT` 时，自动生成带时间和随机标识的 `memory-results/...` 目录。
对比 ordinary 或 xrc 时只需替换命令末尾的模式，并使用新的结果目录。

## RSS 的 max 不能消除共享页重复计数

每轮、每节点先计算：

```text
node_rss(t) = sum(该节点本次作业各 rank 的 VmRSS(t))
node_peak_rss = max_t(node_rss(t))
```

不能使用 `sum(各 rank 的 VmHWM)`：各进程峰值可能发生在不同时间。
也不能把两台机器各自的峰值相加称为“集群同时峰值”。本脚本先按同一采样轮次
加总两台机器，再求集群峰值。

RSS 求和本身会对共享驻留页重复计数，随后取 max 不会改变这一点。例如两个进程
各自有 50 MiB 私有页，共享同一份 100 MiB 驻留页，RSS 合计 300 MiB，而这批页面
实际只有 200 MiB。可以把 RSS 峰值用于复现论文的 RSS 口径，但应标为
“每节点 MPI rank 的 RSS 合计峰值”，不能称为去重后的物理内存峰值。
如果不同通信方案的共享比例不同，重复计数的误差也不一定相互抵消。

可选：在上述命令的环境变量部分再加入：

```bash
MEM_PSS=1 MEM_PSS_INTERVAL=1 \
```

这会在 RSS 采样之外，每约 1 秒读取一次 PSS，优先使用 `smaps_rollup`，若不存在则
逐段汇总 `smaps`。PSS 按共享者分摊页面，适合辅助比较共享内存方案。
未采 PSS 的轮次或读取失败的值为空，不会冒充 0，也不会回退为 RSS；
`pss_read_errors` 记录 PSS 失败数。即便 PSS 失败，能读取的 RSS 仍保留。

PSS 扫描可能增加运行时间。RSS 与 PSS 扫描由同一个节点采样器串行完成；
启用 PSS 后，RSS 的实际间隔可能变长。峰值始终是“采样观测峰值”，可能漏掉
两个采样点之间的短峰。可用较短间隔做一次敏感性检查，并记录实际扫描耗时。
正式性能对比建议另跑不带采样的实验，避免把采样影响混入延迟结果。

## 输出与统计口径

| 文件 | 内容 |
| --- | --- |
| `benchmark.log` | 原实验 stdout/stderr，同时显示在终端 |
| `samples.csv` | 每节点每轮样本，包括 rank/PID 清单、RSS、可选 PSS、HugeTLB、swap |
| `cluster_samples.csv` | 每轮跨节点求和，包含整轮耗时 |
| `summary.json` | 每节点和集群的峰值、样本均值、有效样本数、错误和警告 |
| `metadata.json` | 实验命令、所设置的相关环境变量、节点和采样配置、唯一作业标识 |
| `collector-0.log`、`collector-1.log` | 按 HOSTS 顺序排列的 SSH/远程 Python 错误日志 |

测量从启动 MPI 前开始，持续到启动命令退出，包括 `MPI_Init`、OSU 的 `-x 500`
预热、`-i 1000` 正式迭代及结束阶段。**脚本不自动区分这些阶段**，因此这里的
均值不能称为“正式迭代阶段均值”。如需严格的分阶段测量，需要在 benchmark
中额外输出阶段标记或提供独立测量窗口。

`samples.csv` 字段：

- `complete`：本轮采到的有效 rank 数等于 PPN、rank 不重复，且无 rank 数据读取错误。
  集群的 `complete` 还要求全部全局 rank 恰好为 `0..NP-1`。
- `rss_kib`：本轮成功读取的 rank 的 RSS 合计；`complete=False` 时是部分结果。
- `pss_kib`：本轮 PSS 合计；未采样或有 PSS 读取失败时为空。
- `hugetlb_kib`、`swap_kib`：各 rank 的 `HugetlbPages` 和 `VmSwap` 合计。
  不计入 RSS/PSS；字段不可用时为空。
- `scan_seconds`：该节点从查找进程到读完内存的耗时。
- `wall_start`：节点本地 Unix 时间；`elapsed_seconds`：控制端发起该轮时相对于
  启动实验的单调时钟时间。跨节点按 `round` 对齐，不依赖墙上时钟同步。
- `read_errors`：已识别为本次 rank，但随后读取失败的数目。进程权限导致无法识别时，
  也可能只表现为 rank 数不足。检查 `n_ranks`，不要只检查错误计数。

例如查看第一台机器的主要 RSS 结果：

```text
summary.json
  nodes
    192.168.1.5
      rss_kib
        peak_complete_gib
        complete_samples
        peak_round
```

各指标的汇总字段：

- `peak_complete_kib` / `peak_complete_gib`：仅使用 rank 齐全轮次的峰值，终端默认显示这个值。
- `peak_observed_kib`：包括初始化和退出时 rank 尚未齐全/已退出的全部非空样本峰值。
  若此值更大，应检查对应原始数据，说明差异，不能悄悄忽略启动阶段峰值。
- `sample_mean_complete_kib`：完整轮次的样本算术平均；不是按时间加权的均值。
- `complete_samples`：该指标完整有效的样本数；PSS 的有效样本通常少于 RSS。

终端 N/A 和 JSON null 表示没有有效数据，不表示内存为零。
KiB = 1024 B，GiB = 1024³ B。RSS/PSS 数值从 proc 的 `kB` 字段读取，按 KiB 处理。

每个节点顺序读取其 rank，不暂停 MPI，因而并非严格同时快照。控制端尽可能同时
给各节点发起请求，`cluster_samples.csv` 的 `round_seconds` 记录整个轮次跨度。
集群结果应称为“同一采样轮次的集群内存合计峰值”，不能宣称原子快照。

## HugeTLB 及测量边界

本仓库启动代码提到 HugeTLB 池。显式 hugetlbfs 页面与透明大页 THP 不同，
RSS/PSS 不覆盖它的全部占用；因此脚本额外保存 `HugetlbPages`。
检测到非零值时会给出警告。它仍可能对共享页重复计数，且不是严格的去重驻留量，
所以不能直接加到 PSS 后当成准确的物理内存总量。

如果需要跨配置公平比较，应保持大页、共享内存、预分配策略等设置一致；若策略本身
就是比较对象，则应另行分析大页占用，并在图表中明确 RSS/PSS 的统计范围。
本脚本也不能覆盖内核侧 RDMA 对象、内核调度器、网卡片上内存等。

Linux 字段定义：<https://docs.kernel.org/filesystems/proc.html>

## 错误处理与参数

先建立两台采样器连接，再启动 MPI。SSH 不通或 Python 不可用会在启动实验前报错。
采样期间连接失败会终止本地实验进程组并尝试让 Hydra 清理作业，保存已有样本；
远程采样器在控制输入关闭时退出。网络中断时远程 MPI rank 的清理仍取决于 Hydra，
必要时检查远端进程。脚本不会通过进程名批量杀死 OSU 任务。

- 正常实验且采到完整样本：退出 0。
- MPI 启动脚本失败：保留其退出码，已收集数据仍可查看。
- MPI 成功但没有完整集群样本，或开启 PSS 却没有完整集群 PSS 样本：退出 2。
- 采样器错误：退出 1；Ctrl-C：退出 130。

| 环境变量 | 默认值 | 作用 |
| --- | --- | --- |
| `MEM_OUT` | 自动生成目录 | 本地结果目录，不能预先存在 |
| `MEM_INTERVAL` | `0.2` | RSS 目标采样间隔，秒 |
| `MEM_PSS` | `0` | 设为 `1` 开启 PSS |
| `MEM_PSS_INTERVAL` | `1` | PSS 目标间隔，秒，实际不会快于 RSS 轮次 |
| `MEM_TIMEOUT` | `30` | 采样器准备/每轮返回的超时，秒 |
| `MEM_SSH_BIN` | `MV2_SSH_BIN` 或 `ssh` | SSH 可执行文件路径，额外配置可用 ssh config |
| `MEM_REMOTE_PYTHON` | `python3` | 各远端的 Python 可执行文件 |

## 本地验证

```bash
python3 contrib/hollow-rc/test_measure_osu_memory.py -v
bash -n contrib/hollow-rc/run_osu_collective.sh
```

测试使用临时 proc 数据与本地模拟 SSH/OSU 进程，覆盖作业隔离、rank 缺失/重复、
PID 重用、PSS 缺失及 smaps 回退、峰值聚合、日志写入和原启动脚本的作业标记传递。
这些测试不运行双机 MPI/RDMA 实验。
