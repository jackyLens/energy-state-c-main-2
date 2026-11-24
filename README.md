# evobeing —— 能量与精力的 C 语言实现

作者：zhenghuimin。

每 **15 分钟**（日间与夜间统一）给出一个样本（睡眠分期、心率、夜间 HRV、步数、热量、血氧），间隔允许变动，但目前都统一按照15分钟进行；
本库据此持续计算两个 0–100 的值：

- **能量**：身体的储备水平，小时到天级变化。夜间睡眠充电，日间消耗，用心率与 HRV 校正。
- **精力**：此刻的状态好坏，分钟到小时级变化。叠在能量之上，由日间心率 / HRV 相对本人常态的偏离、
  运动后回弹、小睡、以及昨夜睡眠质量（含夜间血氧平稳度）决定。

两个值都只跟本人的历史比。库在运行中学习这个人的常态（心率、HRV 基线，睡眠窗，夜间回充量，夜间血氧），


**这里需要注意**：已算好的 **恢复分**（0–100）可在晨起后通过样本的 `recovery` 字段传入一次：
库把能量向恢复分的映射值软融合、把精力的晨起初始化值向恢复分融合，使 **晨起的能量与精力与恢复分大致成正比。

## 文件

```
include/evobeing_zhenghuimin.h   主接口：两个计算函数 + 初始化 + 画像导入导出（调用只需看这个文件）
src/evobeing_zhenghuimin.c       主接口实现（编排）
include/energy.h  src/energy.c   能量引擎（ODE + EKF，个人基线学习，缺口处理）
include/vitality.h src/vitality.c 精力引擎（偏离量 D 的标量卡尔曼滤波，事件，晨起初始化）
include/evobeing_behavior_zhenghuimin.h
src/evobeing_behavior_zhenghuimin.c 行为归因：过去 28 天各行为对晨起精力的影响百分比（独立离线模块）
src/energy_internal.h            仅测试使用
src/main.c                       命令行：CSV 进 → CSV 出
examples/evobeing_demo.c         主接口调用示例（3 天合成数据）
examples/stream_demo.c           能量引擎流式示例
examples/vitality_demo.c         精力引擎示例（事件日、短睡夜、血氧不稳夜）
examples/behavior_demo.c         行为归因示例（28 天合成行为日志）
tests/                           单元测试
data/sample_panel_15min.csv 同一天的 15 min 版本（带 recovery 列）
docs/                            算法与产品说明
```

## 集成

把 `src/energy.c`、`src/vitality.c`、`src/evobeing_zhenghuimin.c` 加入工程，头文件路径加 `include/`

## 调用

```c
#include "evobeing_zhenghuimin.h"

evobeing_ctx_t ctx;
evobeing_init_zhenghuimin(&ctx, NULL);               /* 首次使用；有画像时传 &profile */

for (每个样本：每 15 分钟算一次，间隔可变) {
    evobeing_sample_t s;
    evobeing_sample_init_zhenghuimin(&s);
    s.t_h      = t_hours;                            /* 距首样本的小时数，严格递增；样本描述到上一样本为止的区间 */
    s.tod      = 13.5;                               /* 当地钟点，13:30 */
    s.stage[0] = 0; s.stage[1] = 0; s.stage[2] = 0; s.stage[3] = 0;   /* 日间无分期；夜间填区间内 [清醒,浅睡,深睡,REM] 的占比 */
    s.hr       = 72.0;                               /* 区间内均值，bpm */
    s.hrv_log  = NAN;                                /* ln(RMSSD/ms)，日间无 RR 间期填 NAN */
    s.steps    = 360;                                /* 区间内合计（15 min） */
    s.kcal     = 18.0;                               /* 区间内合计 */
    s.q_hr     = 1.0;                                /* 心率质量 0–1 */
    s.q_hrv    = 1.0;
    s.spo2     = NAN;                                /* 血氧 %，日间可不测 */
    s.spo2_min = NAN;
    s.recovery = NAN;                                /* 晨起拿到 App 恢复分的那一个样本填 0–100，其余 NAN */

    double energy = 0, vitality = 0;
    if (evobeing_energy_zhenghuimin(&ctx, &s, &energy) == EVOBEING_OK &&
        evobeing_vitality_zhenghuimin(&ctx, &vitality) == EVOBEING_OK) {
        /* vitality 为展示分（相对本人基线：中位数 50，1σ = 25 分）；四档见 ctx.vitality_point.level */
        static const char *name[] = {"低能量", "放松", "心流", "准备"};
        vitality_level_t lv = ctx.vitality_point.level;
        printf("能量 %.0f  精力 %.0f  %s\n", energy, vitality,
               lv == VITALITY_LEVEL_NONE ? "基线建立中" : name[lv]);
    }
}

evobeing_profile_t profile;
evobeing_get_profile_zhenghuimin(&ctx, &profile);    /* 保存（含常态与展示基线）；下次 init 传回即可跳过学习期，四档立即可用 */
```

**每个样本先调能量函数，再调精力函数**；只需要能量时不调精力函数。
用户手动标记（咖啡、冥想、小睡、压力事件、饮酒）用 `evobeing_event_zhenghuimin()`。
精力函数返回的是面向用户的展示分 `relative`(**取结构体中的这个变量**，)：本人日间精力的中位数 = 50，高 / 低 1 个本人标准差 = ±25 分，
四档 `level`（准备 ≥ 75 / 心流 / 放松 / 低能量 < 25）
按同一基线划分，积累 3 个日间后开始输出，之前为 `VITALITY_LEVEL_NONE`。内部的绝对曲线仍在 `ctx.vitality_point.vitality`。
更多输出（置信带、四象限、标志位、晨起初始化值、本夜血氧统计）在 `ctx.energy_point` 与 `ctx.vitality_point`，
字段含义见 `energy.h` / `vitality.h`。

### 输入字段

| 字段 | 定义 | 格式 |
|---|---|---|
| `t_h` | 距首样本的小时数 | 严格递增；名义 15 分钟一格（相差 0.25），间隔可变 |
| `tod` | 当地钟点 | 0–24，含小数 |
| `stage[4]` | 区间内各分期占比 [清醒, 浅睡, 深睡, REM] | 每项 0–1，和 ≤ 1；日间全 0 |
| `hr` | 区间内心率均值 | bpm；未测到 NAN |
| `hrv_log` | ln(RMSSD / ms)，由逐拍 RR 间期算出 | 无 RR 填 NAN；毫秒值用 `energy_hrv_log_from_ms()` |
| `steps` | 区间内步数合计 | 数值 |
| `kcal` | 区间内活动热量合计 | kcal |
| `q_hr` / `q_hrv` | 观测质量 | 0–1；0 = 不参与计算 |
| `spo2` / `spo2_min` | 血氧均值 / 最低值 | %；未测到 NAN；只用夜间 |
**| `recovery` | App 算好的昨夜恢复分 | 0–100；晨起后第一个拿到它的样本填一次，其余 NAN（0 也视为无） |**

### 计算间隔

**名义间隔 15 分钟，日间与夜间统一**（v2.1）：与设备的步数 / 热量 15 分钟汇总点对齐，心率、HRV、分期、血氧在区间内聚合后一起传入。
一个样本描述"以其时间戳结束、到上一个样本为止"的区间：分期填占比、心率填均值、步数与热量填合计。库用本样本的分期与负荷推进该区间，
所有按时长计的统计（睡眠时长、夜段 / 日段长度、基线引导 0.5 h / 最短段 1 h、门控连续拒绝 0.5 h、运动与过载的持续判定、
醒前 1 h HRV 窗口、基线直方图权重、展示平滑）全部按时间加权，不再按样本个数；一次观测的权重随区间长度放大
（内部噪声参数仍按 5 分钟一次标定，15 分钟一个区间均值顶 3 次观测）。
间隔超过 `gap.max_hold_h`（默认 1 h）视为流被打断（摘下戒指），本样本只代表名义 15 分钟，之前的时间按缺口策略推进。

间隔仍可变（1 min–1 h 内按实际区间计权）。区间越长，区间内的峰值（一次快走、一段运动）越被均值抹平，负荷 / 事件识别越粗；
20 分钟以内的小睡在 15 分钟粒度下仍能识别，30 分钟粒度下时刻与幅度会变。

### 恢复分

只做两处软融合：

- 能量层（`energy_config_t.recovery`，默认 enable=1）：`E ← E + w·(E_rec − E)`，`E_rec = E_norm + gain·(R − r_mid)/100`，
  其中 `E_norm` 是本人晨起时锚定前 E 的常态（前 3 晨累计平均，之后 EWMA λ=0.2）。默认 w=0.5、gain=0.6、r_mid=70：
  R 每差 15 分，晨起 E 差 0.045（约 4.5 分能量）；R=70 时能量与不传时接近。EKF 方差同步收缩 `P·(1−w)`。
  同一次醒来 12 h 内重复传入忽略；输出带 `ENERGY_FLAG_RECOVERY`。
- 精力层（`vitality_morning_t.w_recovery`，默认 0.5）：`D_rec = gain·clip((R − 70)/60, ±0.5)`，`D ← D + w·(D_rec − D)`。
  恢复分比睡眠段结束先到时暂存，等 D_init 算出后再融合（否则会被 D_init 覆盖）；输出带 `VITALITY_FLAG_RECOVERY`，
  `d_init` 字段与每日汇总的 `d_init / wake_score` 取融合后的值。

### 返回码

`EVOBEING_OK`（0）成功；`ENERGY_EDOMAIN` 时间未递增；`ENERGY_EINVAL` 参数非法或调用顺序错误。
出错时输出变量不变。

## 算法概要

**能量** 用两个隐状态：睡眠压力 S 与能量 E，按解析解推进（支持任意步长）：

```
S(t+h) = S∞ + (S − S∞)·e^(−b·h)
E(t+h) = E∞ + (E − E∞)·e^(−(α+β)·h)
```

睡眠期按分期权重充电，清醒期按负荷、压力、睡眠债消耗。心率与 HRV 作为观测，
用 2×2 扩展卡尔曼滤波校正（Student-t 稳健化、4σ 门控）。观测方程的截距是个人基线，由库在线学习
（首晚引导 → 前 3 晚累计平均 → 之后 EWMA λ=0.1，异常夜降权），所以同一条心率曲线整体平移不改变能量。
输出 0–100 时叠加昼夜节律调制。

**精力** `V = clip(V* + D)`，均衡线 `V* = E·(1 + ρ·c(t))`，偏离量 D 按 `dD/dt = −D/τ(E) + 事件脉冲` 推进，
能量越低回弹越慢，且 `D ≤ 0.30·E`。观测是能量模型没解释掉的日间心率 / HRV 残差：
心率略高于常态是"打起精神"（精力升），持续大幅高于常态是"过载"（精力降），低于常态是"犯困"。
每天醒来时 D 重置为晨起初始化值，由本夜能量回充量、醒前 HRV、睡眠结构、夜间血氧平稳度相对本人常态的偏差给出，
`|D_init| ≤ 0.2`；App 传入恢复分时再向 `gain·(R − 70)/60` 软融合（权重 0.5）。日间血氧不参与计算：健康人静息血氧稳定在 96–99%，变化在传感器噪声之内。

**行为归因**（独立模块，不经过两个主函数）：以每天起床时刻的精力初始值为目标，过去 28 天的行为记录（饮酒、咖啡、小睡、运动、冥想、晚睡……，统一为"行为 + 时间点"，归到之后的第一次起床）生成 0/1 列，加上周末、前一天精力、趋势三个控制列做近期加权的岭回归，系数除以本人均值即"有该行为的早晨比平时高 / 低 X%"；自助法给 90% 区间和高 / 中 / 低可信度；行为出现不足 4 次或对照不足 4 天不给数字。各项之和不必为 100%，另给"记录的行为合计解释 X%"。


## 约定
- 数据中断（摘下、没电）后直接喂下一个样本即可；超过 1 小时的缺口按个人常规睡眠窗 / 清醒静息推进，(这里是改进的一点，需要注意)
  输出带 `ENERGY_FLAG_GAP`。
- 学习期（前 3 晚）输出带 `ENERGY_FLAG_LEARNING` / `VITALITY_FLAG_LEARNING`，可提示"正在了解你的常态"，也就是说前三天不区分4个档，第4天才开始输出4个档。
