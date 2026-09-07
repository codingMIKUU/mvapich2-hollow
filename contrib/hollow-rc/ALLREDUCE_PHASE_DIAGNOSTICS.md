# Two-level Allreduce 阶段计时

用途：将当前 RC/Hollow 的 Allreduce 时延差异定位到节点内归约、节点代表之间的通信、节点内广播。只增加观测，不修改集合通信算法、进程绑定、协议、SRQ 或驱动配置。

## 开销与范围

- 默认关闭。关闭时不读取时钟、不累计计数、不打印；保留少量可预测分支。不能声称与未插桩二进制完全相同。
- 开启后默认每 64 次调用采样一次。每个被采样的调用，节点代表最多读取 4 次时钟，其他 rank 读取 3 次时钟；未采样调用只做过滤和计数。
- 使用 MPI 内部计时器和进程内固定大小汇总数组，不额外调用 MPI 集合通信，不加 barrier，不逐 WQE/CQE 计时。
- 只在正常 `MPI_Finalize` 时向 stderr 输出每个 rank 的汇总。基准运行过程中不打印采样日志；异常退出或 Ctrl+C 可能没有汇总。
- 只覆盖 `MPI_COMM_WORLD` 进入 `MPIR_Allreduce_two_level_MV2` 的调用体。其他 communicator、平坦算法或其他拓扑专用 helper 不计入；没有输出不代表没有通信。
- 最多保留 64 个消息字节数/数据类型/归约操作/实际跨节点实现组合。超出部分不计时，退出时报告 `dropped_calls`。

计时本身不可能完全没有开销。请对同一配置交替运行关闭/开启的实验，确认 OSU 的完整时延没有超出原本波动；如果有影响，增大 `EVERY`、增加原有测试轮数以获取足够样本。不要通过增加同步来对齐计时。

## 编译、安装

此修改只需要更新 MVAPICH2 的 `libmpi`；不需要重新编译或安装 rdma-core、内核驱动。两台机器都需要拿到这些源码修改并更新对应的 MPI 安装。

已有本项目构建环境时，在每台机器自己的源码目录执行现有脚本：

```bash
JOBS=4 contrib/hollow-rc/build_mvapich2.sh ordinary
JOBS=4 contrib/hollow-rc/build_mvapich2.sh hollow
```

这两个脚本会重新 configure、make、install 对应的 MVAPICH2，并把**已有** rdma-core 构建产物复制到 MPI 安装目录；不会构建 rdma-core 或重载内核模块。沿用原来的两个安装前缀，不另建安装环境。不要在 MPI 作业正在使用该安装目录时安装。

对于已有且配置正确的构建目录，也可以只增量编译、安装 MPI 库：

```bash
make -C ../mvapich2-build-ordinary -j4 lib/libmpi.la
make -C ../mvapich2-build-ordinary install-libLTLIBRARIES
make -C ../mvapich2-build-hollow -j4 lib/libmpi.la
make -C ../mvapich2-build-hollow install-libLTLIBRARIES
```

此简短方式使用构建目录中保存的安装前缀，请先确认它确实对应运行脚本选择的安装目录。动态链接该 `libmpi` 的 OSU 程序不需要重新编译。两种构建方式选一种即可。

如果需要编译期完全移除诊断，给现有构建脚本增加 `CPPFLAGS=-DMV2_ENABLE_ALLREDUCE_PHASE_DIAG=0`；此时运行时开关也无法开启计时。

## 新增运行参数

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `MV2_ALLREDUCE_PHASE_STATS` | `0` | `1` 开启，`0` 关闭 |
| `MV2_ALLREDUCE_PHASE_EVERY` | `64` | 采样间隔，必须为正的 2 的幂 |
| `MV2_ALLREDUCE_PHASE_SKIP` | `0` | 每个尺寸/类型/操作/实现组合先跳过多少次调用；设为 OSU 预热次数 |
| `MV2_ALLREDUCE_PHASE_BYTES` | `0` | `0` 统计所有尺寸；其他值只统计该字节数 |

`SKIP=500`、`EVERY=64` 时，第 501、565、629……次调用采样；若随后恰好执行 1000 次测量，每个 rank 获得 16 个样本。每个尺寸分别跳过 500 次，不是整个作业只跳过一次。

开关随现有 Hydra 环境继承传给两端；不需要修改运行脚本。不要在外层另行设置禁止环境继承的 Hydra 选项。如果两端运行的 MPI 库或采样参数不一致，汇总可能缺 rank 或采样计数不同。

## 直接运行当前 256 KiB 对照

保持原来的 NP/PPN、Eager/RPUT、two-level 和对齐后的 SRQ 配置，只增加采样与日志重定向：

```bash
HOSTS=192.168.1.5,192.168.1.1 \
HCA_MAP=192.168.1.5=mlx5_1,192.168.1.1=mlx5_3 \
USER_MAP=192.168.1.5=lingbo11,192.168.1.1=lingbo12 \
NP=256 PPN=128 \
MV2_IBA_EAGER_THRESHOLD=131072 MV2_SHOW_ENV_INFO=1 \
MV2_RNDV_PROTOCOL=RPUT \
MV2_INTER_ALLREDUCE_TUNING=9 MV2_INTER_ALLREDUCE_TUNING_TWO_LEVEL=1 \
MV2_MEMORY_OPTIMIZATION=1 MV2_SRQ_SIZE=256 MV2_SRQ_LIMIT=64 MV2_SRQ_MAX_SIZE=8192 \
MV2_ALLREDUCE_PHASE_STATS=1 MV2_ALLREDUCE_PHASE_SKIP=500 \
MV2_ALLREDUCE_PHASE_BYTES=262144 \
contrib/hollow-rc/run_osu_collective.sh ordinary allreduce \
    -m 262144:262144 -i 1000 -x 500 -f 2>phase-ordinary.log
```

Hollow 对照：其余不变，将 `ordinary allreduce` 换成 `hollow allreduce`，日志改成 `2>phase-hollow.log`。计时关闭对照只将 `MV2_ALLREDUCE_PHASE_STATS=1` 改成 `0`。标准输出的 OSU 时延仍正常显示；其他错误也会被收进 stderr 文件，异常时先检查该文件。

如需复查每节点一个进程的实验，只改 `NP=2 PPN=1`，并使用不同日志名。不要将不同作业日志合并为一个输入文件。

```bash
python3 contrib/hollow-rc/summarize_allreduce_phases.py phase-ordinary.log
python3 contrib/hollow-rc/summarize_allreduce_phases.py phase-hollow.log
```

该工具仅离线读取日志，不启动 MPI、不连接远端。

## 如何解释阶段

| 字段 | 实际测量范围 | 解释限制 |
| --- | --- | --- |
| `reduce_avg_us` | 节点内归约，包含代表 rank 的初始输入复制 | 包含等待本地其他 rank 到达的时间 |
| `inter_avg_us` | 节点代表调用跨节点 Allreduce 的耗时 | 只统计 `inter_samples>0` 的代表；包含对端就绪、协议握手、progress 等等待，不是纯网卡传输时间 |
| `bcast_avg_us` | 节点内广播调用耗时 | 非代表 rank 通常提前进入广播，**包含等待代表完成跨节点通信的时间** |
| `total_avg_us` | 三段合计 | 同一 rank、同一样本内可相加；不包含 helper 之前的算法选择/其他外层工作以及之后的函数退出开销 |

`*_max_us` 是单 rank 在采样调用中观测到的最大值，不是全部 1000 次调用的最大值。

离线汇总展示：

- `sample-weighted-avg-us`：有效样本加权平均；跨节点阶段仅平均节点代表。
- `max-rank-avg-us`：所有相关 rank 中最大的阶段平均耗时。
- `max-sampled-call-us`：所有已采样调用中的最大阶段耗时。
- `leader rank=...`：逐个节点代表的三段耗时，优先对比这两行。

不能把不同 rank 的三个最大值相加；它们可能来自不同轮次，非代表广播等待也与代表的跨节点阶段重叠。OSU 输出是它自己的测量区间及 rank 汇总，而这里默认只有每 rank 16 个样本，数值不要求完全相等。

当前源码的 two-level helper 中，指定 `TUNING=9` 的 Ring 函数指针进入 `MPIR_Allreduce_pt2pt_rs_MV2` 分支，因此会显示 `inter_algo=rs`，不是插桩改变了算法。`rd`、`rsa_collectives` 表示其他实际分支；`none` 表示没有跨节点阶段。

排查时先看：两个版本的差距主要出现在代表的 `reduce`、`inter` 还是 `bcast`。如果主要是 `inter`，还需要区分对端代表迟到与传输路径本身慢；本次低开销阶段计时不进一步插桩每条 WQE 或 progress 循环。

## 本地测试

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
    -s contrib/hollow-rc/tests -p 'test_*.py' -v
```

使用真实诊断头文件的模拟时钟测试覆盖默认关闭、编译期关闭、采样/预热、尺寸过滤、代表/非代表、失败路径和离线汇总。它们不访问 RDMA 设备，不能代替双机性能开关对照。
