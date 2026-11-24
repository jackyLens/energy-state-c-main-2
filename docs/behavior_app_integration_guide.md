# APP 端行为影响分析接口调用指南

模块：`evobeing_behavior_zhenghuimin.h`

---

| 变化 | APP 要做什么 |
|---|---|
| 不再截 28 天窗口，所有喂入的天都参与；输入上限 366 天、4096 条事件 | 日志滚动保留 1 年（或改用流式状态，见下） |
| 目标量优先用晨起 `d_init`（`vitality_daily_t.d_init`），没有时退回 `wake_score` | 新增 `in.wake_d_init[]`；`wake_score[]` 仍必填（做百分比的分母） |
| 手动行为区分"没记录"和"确认没有" | 新增 `in.logged[]`：用户当天打开 / 使用过记录 = 1，完全没碰 = 0；不想区分传 NULL |
| 可选"作用路径"通道（≤ 4 条，如醒前 HRV、深睡占比） | 新增 `in.channel[k][]`、`in.channel_name[k]`、`in.n_channels`；不用传 0 |
| 多了"第二晚"子项（`alcohol_2nd` 等，`lag_nights == 2`）和 `z`、`prob_dir`、`sd_points` 字段 | 列表展示时按 `lag_nights` 决定文案："饮酒后的第二个早晨" |
| 总览多了 `pred_skill_pct`（一步预测技能）、`sigma`、`target_is_d_init` | 预测技能 ≤ 0 时，"合计解释 X%" 那句话建议不显示 |
| 配置项变化：去掉 `window_days / half_life_days / ridge / control_trend / high_ci_ratio`，新增 `habit_lambda / slow_baseline_days / prior_sd_* / effect_drift_sd / level_drift_sd / z_high / z_mid / fdr_q / block_days / lag2_ids / manual_mask` | 用默认即可（`cfg = NULL`） |
| 流式接口：`evobeing_behavior_state_init_zhenghuimin` / `_update_` / `_report_` | 二选一：每天把全部历史喂给批量函数（最简单），或者只保存状态、每天喂一天 |

下文第 2–8 节里凡是写"28 天 / 56 天"的地方，按上表理解为"全部历史（≤ 366 天）"。

---

## 1. 总体流程

```
┌─────────────────────────────────┐
│  APP 端每天持续运行精力引擎      │
│  evobeing_energy / vitality     │
│  ↓ 每天产出：                    │
│    · wake_score（晨起精力分）    │
│    · 日间事件标志（小睡、运动等）│
│    · 起床时刻、星期几            │
└───────────┬─────────────────────┘
            │ 累积 28 天
            ▼
┌─────────────────────────────────┐
│  APP 端积攒行为日志              │
│  · 自动行为：从引擎标志生成      │
│  · 手动行为：用户在界面上记录    │
└───────────┬─────────────────────┘
            │ 每天早起后
            ▼
┌─────────────────────────────────┐
│  调用 behavior_impact 分析函数   │
│  输入：28 天 wake_score          │
│        + 行为记录列表            │
│  输出：每种行为的影响百分比      │
└─────────────────────────────────┘
```

行为分析模块**不依赖**原始传感器数据（心率、HRV 等），只需要两样东西：每天一个晨起精力分 + 行为时间点。传感器数据由精力引擎消化完毕后，行为分析只使用引擎的输出结果。

---

## 2. 需要准备的三组数据

### 2.1 每日晨起精力 `wake_score[28]`

**来源**：精力引擎每步输出的 `vitality_point_t`。当 `flags` 包含 `VITALITY_FLAG_WAKE` 时，该点的 `score` 就是当天的晨起精力。对应日汇总结构 `vitality_daily_t.wake_score`。

**存储建议**：APP 端维护一个滚动数组，每天晨起后写入一条。没有主睡眠的日子（比如整夜外出）填 `NAN`，模块会自动跳过。

```
// 伪代码：精力引擎主循环里截取 wake_score
if (vp.flags & VITALITY_FLAG_WAKE) {
    today_wake_score = vp.score;        // 0–100
    today_wake_t_h   = sample.t_h;      // 距首样本的小时数
    save_to_daily_log(date, today_wake_score, today_wake_t_h);
}
```

**数组填法**：

```c
double wake_score[28];   // 第 0 天最早（28 天前），第 27 天最近（今天）
// 某天没有晨起 → wake_score[d] = NAN
```

### 2.2 每天的起床时刻与星期 `wake_t_h[28]`、`weekday[28]`

| 字段 | 含义 | 格式 | 缺失时 |
|---|---|---|---|
| `wake_t_h[d]` | 第 d 天起床时刻 | 自窗口第 0 天 00:00 起的**小时数**。如第 3 天 07:15 → `3 × 24 + 7.25 = 79.25` | `NAN` |
| `weekday[d]` | 星期几 | `0=周一, 1=周二, …, 6=周日` | 整个数组传 `NULL`（不做周末控制） |

**wake_t_h 的时间基准**：和 `events` 里的 `t_h` 共用同一个零点——窗口第 0 天 00:00。APP 在维护这 28 天数据时，用当地日历日推算即可：

```
第 d 天的 00:00 对应小时数 = d × 24
如果第 d 天 07:30 起床 → wake_t_h[d] = d × 24 + 7.5
```

### 2.3 行为记录 `events[]`

每条记录包含两个字段：

```c
typedef struct {
    int id;        // 行为 id（见下表）
    double t_h;    // 发生时刻：自窗口第 0 天 00:00 起的小时数
} evobeing_behavior_event_t;
```

**行为 id 表**：

| id | 常量名 | 来源 | 举例 |
|---|---|---|---|
| 0 | `ALCOHOL` | 用户记录 | 饮酒 |
| 1 | `CAFFEINE` | 用户记录 | 咖啡 / 茶 |
| 2 | `MEDITATION` | 用户记录 | 冥想 |
| 3 | `STRESSOR` | 用户记录 | 压力事件 |
| 4 | `NAP` | 自动 | 小睡（精力引擎 `VITALITY_FLAG_NAP`）|
| 5 | `EXERCISE` | 自动 | 运动（`VITALITY_FLAG_EXERCISE`）|
| 6 | `OVERLOAD` | 自动 | 过载日（当天 STRESS 样本累计 ≥ 阈值）|
| 7 | `LATE_BED` | 自动 | 晚睡（入睡比本人常态晚 ≥ 1 小时）|
| 8 | `LATE_MEAL` | 用户记录 | 晚餐很晚 / 睡前进食 |
| 9 | `SCREEN` | 用户记录 | 睡前长时间看屏幕 |
| 16–23 | 自定义 | 用户自定义 | 如"饮酒量大"、"睡前阅读"等 |

#### 2.3.1 自动行为的生成方式

这些行为由 APP 从精力引擎的日汇总（`vitality_daily_t`）或逐点标志自动提取，不需要用户操作：

| 行为 | 判定依据 | t_h 取值 |
|---|---|---|
| NAP | `vitality_daily_t.n_nap > 0`，或逐点检查到 `VITALITY_FLAG_NAP` | 小睡结束时刻 |
| EXERCISE | `n_exercise > 0`，或 `VITALITY_FLAG_EXERCISE` | 运动结束时刻 |
| OVERLOAD | 当天 `n_stress_samples ≥ 12`（STRESS 累计 ≥ 1 小时） | 当天 20:00（`d × 24 + 20`） |
| LATE_BED | 入睡时刻比本人近 14 天中位数晚 ≥ 1 小时 | 实际入睡时刻 |

**LATE_BED 判定的参考做法**：APP 端自行维护一个"近 14 天入睡时刻中位数"。每天精力引擎报告睡眠开始时（`VITALITY_FLAG_SLEEP` 且不在短暂的日间小睡窗口内），记录入睡时刻。若今晚入睡比中位数晚 1 小时以上，插入一条 LATE_BED 记录。

#### 2.3.2 手动行为的记录时机

用户在 APP 界面上标记"刚喝了咖啡"时，做两件事：
1. 调用 `evobeing_event_zhenghuimin(&ctx, VITALITY_EVENT_CAFFEINE, 0.0)` → 让精力引擎在下一步实时反映（短期效果）。
2. 同时把 `{id=CAFFEINE, t_h=当前小时数}` 追加到行为日志 → 供行为分析做 28 天回归（长期归因）。

#### 2.3.3 归属规则

一条行为记录归到它**之后的第一次起床**。逻辑上：晚上 23:00 喝酒和凌晨 01:00 喝酒，都影响接下来那个早晨的 wake_score。最后一次起床之后发生的记录（今天白天的事）还没有对应的早晨，本次分析不计入，但要保留在日志里供明天使用。

**这意味着 APP 不需要自己做归属判定**——只需要如实记录每个行为的绝对时间，把时间转换成"自窗口第 0 天 00:00 起的小时数"传入即可，分析函数内部自动按起床时刻切分。

---

## 3. 完整调用代码

```c
#include "evobeing_behavior_zhenghuimin.h"
#include <math.h>
#include <string.h>

// APP 端每天晨起后调用此函数
int analyze_behavior_impact(
    const double *wake_scores,        // [28] 过去 28 天的晨起精力，NAN 表示当天无数据
    const double *wake_times,         // [28] 过去 28 天的起床时刻（小时数）
    const int    *weekdays,           // [28] 星期几，0=周一..6=周日；可为 NULL
    const evobeing_behavior_event_t *events,
    int n_events,
    evobeing_behavior_output_t *result)
{
    evobeing_behavior_input_t in;
    memset(&in, 0, sizeof(in));

    in.n_days     = 28;
    in.wake_score = wake_scores;
    in.wake_t_h   = wake_times;
    in.weekday    = weekdays;
    in.events     = events;
    in.n_events   = n_events;

    // cfg 传 NULL 使用默认配置：
    //   min_with=4, habit_lambda=0.2, slow_baseline=30 天, prior_sd_effect=8, n_boot=200,
    //   自动控制周末 / 前一天精力 / 慢基线；饮酒、过载、晚睡另有"第二晚"子项
    int rc = evobeing_behavior_impact_zhenghuimin(&in, NULL, result);

    // rc == 0：成功
    // rc == -1：参数错误（n_days 超限、指针为空等）
    // rc == -2：有效天数不足 7 天，result 仍可读但 items 全部 INSUFFICIENT
    return rc;
}
```

---

## 4. 读取结果

### 4.1 总览字段

```c
evobeing_behavior_output_t out;
// ...调用后...

out.n_days_used     // 累计有晨起值的天数
out.target_is_d_init// 1 = 目标量是 D_init；0 = wake_score
out.mean_score      // ȳ：本人晨起精力近期加权均值（百分比的分母）
out.sd_score        // 去慢基线后目标量的近期加权标准差
out.sigma           // 模型当前估计的观测噪声（分）
out.explained_pct   // 所有行为合计解释了多少 %（样本内）
out.unexplained_pct // 其余 %（睡眠本身、身体状况等未记录因素）
out.pred_rmse       // 一步预测误差（分）
out.naive_rmse      // 只用慢基线预测的误差（分）
out.pred_skill_pct  // 100·(1 − pred_mse/naive_mse)；≤ 0 说明行为记录暂时没有预测力
out.n_channels / out.channel_name[k]  // 作用路径通道
```

### 4.2 逐行为字段

```c
for (int i = 0; i < out.n_behaviors; i++) {
    evobeing_behavior_item_t *it = &out.items[i];

    it->name            // 行为名（如 "alcohol"、"caffeine_late"）
    it->id              // 行为 id
    it->is_late_variant // 1 = 晚间变体（如"下午咖啡"而非"咖啡"）
    it->lag_nights      // 1 = 次晨；2 = "第二晚"变体（名字带 _2nd）
    it->n_with          // 累计有该行为的早晨数（缺记录的未知日不计）
    it->n_without       // 累计没有该行为的早晨数
    it->status          // INSUFFICIENT / LOW / MID / HIGH

    // ---- 只在 status != INSUFFICIENT 时有效 ----
    it->effect_pct      // 核心数字：影响百分比。如 −12% 表示"有饮酒的早晨，精力比平时低 12%"
    it->effect_points   // 同上，但单位是分（0–100 刻度）。如 −7.2 分
    it->raw_pct         // 未控制混杂的简单对比（参考）
    it->ci_lo_pct       // 90% 区间下界（后验；n_boot>0 时与块自助法取并）
    it->ci_hi_pct       // 90% 区间上界
    it->sd_points       // 效应标准差（分）
    it->z               // effect / sd
    it->prob_dir        // 方向后验概率（0.5–1）："比平时低"这个方向有多大把握
    it->share_pct       // 解释份额（各项之和 ≤ 100%，仅用于排序）
    it->channel_effect_sd[k] // 第 k 条通道上的效应（通道标准差单位），没给通道时 NAN
    it->channel_z[k]
}
```

## 5. 时间坐标系详解

行为分析里所有时间（`wake_t_h[d]` 和 `events[i].t_h`）使用同一个坐标系。

### 5.1 坐标零点

窗口第 0 天（即 28 天前那天）的 00:00 = 0.0。

### 5.2 转换公式

```
t_h = (calendar_date − window_start_date) × 24 + hour + minute / 60.0
```

| 日期 | 本地时间 | d | t_h |
|---|---|---|---|
| 窗口第 0 天 | 07:30 起床 | 0 | 7.5 |
| 窗口第 0 天 | 21:00 饮酒 | 0 | 21.0 |
| 窗口第 1 天 | 00:30 入睡 | 1 | 24.5 |
| 窗口第 1 天 | 07:00 起床 | 1 | 31.0 |
| 窗口第 27 天 | 07:15 起床 | 27 | 655.25 |


---

## 6. 自定义配置（可选）

大多数场景传 `cfg = NULL`（使用默认配置）即可。如需调整：

```c
evobeing_behavior_config_t cfg;
evobeing_behavior_config_default(&cfg);  // 先填默认值

// ---- 可调参数（默认值） ----
cfg.min_with          = 4;     // 行为至少出现 4 次才给结果
cfg.min_without       = 0;     // 默认不限制无行为天数（天天都有时主效应不可估，晚间子项仍可）
cfg.habit_lambda      = 0.20;  // 精力引擎常态 EWMA 的 λ，用于去习惯吸收；0 = 不做
cfg.slow_baseline_days= 30;    // 慢基线时间常数
cfg.prior_sd_effect   = 8;     // 效应先验标准差（分）
cfg.effect_drift_sd   = 0.08;  // 效应每天允许漂移的标准差（分）：越大越"健忘"
cfg.level_drift_sd    = 0.3;   // 截距（基线残余）每天漂移
cfg.n_boot            = 200;   // 块自助法次数（批量模式），0 = 只用后验区间（更快）
cfg.block_days        = 4;     // 块长
cfg.z_high = 1.96; cfg.z_mid = 1.0; cfg.fdr_q = 0.10;   // 档位门槛与多项比较
cfg.control_weekend   = 1;     // 控制周末效应
cfg.control_prev      = 1;     // 控制前一天精力的自相关
cfg.manual_mask       = EVOBEING_BEHAVIOR_MANUAL_MASK_DEFAULT;  // 哪些 id 算"手动记录"（缺记录处理）
cfg.lag2_ids[0..2]    = 饮酒、过载、晚睡;  cfg.n_lag2 = 3;     // 需要"第二晚"子项的行为

// ---- 时间点细分：在某时刻之后发生的行为，单独算一个"晚间"变体 ----
// 默认已配好：咖啡 14:00、运动 20:00、饮酒 21:00
// 如需关闭某项：
cfg.late[i].late_hour = NAN;  // 不细分
// 如需新增自定义行为的细分：
cfg.late[cfg.n_late].id = 16;
cfg.late[cfg.n_late].late_hour = 22.0;  // 自定义行为 16 在 22:00 后另算
cfg.n_late++;

// ---- 自定义行为名 ----
cfg.custom_name[0] = "heavy_drink";   // id=16 的名字（否则显示 "custom_16"）
cfg.custom_name[1] = "reading";       // id=17 的名字

evobeing_behavior_impact_zhenghuimin(&in, &cfg, &out);
```

---

## 7. 调用时序图

```
 用户戴着戒指                APP 后台                        行为模块
    │                         │                               │
    │   每 5 分钟传感器数据   │                               │
    │ ──────────────────────► │                               │
    │                         │  evobeing_energy/vitality()   │
    │                         │  ──► 能量 / 精力分数           │
    │                         │                               │
    │                         │  if VITALITY_FLAG_WAKE:        │
    │                         │    记录 wake_score + 起床时刻  │
    │                         │                               │
    │                         │  if VITALITY_FLAG_NAP:         │
    │                         │    自动写入 NAP 事件            │
    │                         │                               │
    │   用户点"记录饮酒"     │                               │
    │ ──────────────────────► │                               │
    │                         │  1) evobeing_event() → 实时   │
    │                         │  2) 存入行为日志   → 归因     │
    │                         │                               │
    │   === 次日早起 ===      │                               │
    │                         │  收集 28 天数据                │
    │                         │  构建 input                    │
    │                         │ ──────────────────────────────►│
    │                         │                  分析（~1ms）  │
    │                         │ ◄──────────────────────────────│
    │                         │  output: items[]               │
    │                         │                               │
    │   展示行为影响报告       │                               │
    │ ◄─────────────────────  │                               │
```

---

## 10. 常见问题

### Q1: 刚开始用，不到 28 天怎么办？

传实际天数（比如 `n_days = 10`）即可。有效天不足 7 天时函数返回 `-2`，APP 应提示"再使用几天就能看到分析结果"。

### Q2: 行为出现次数不够怎么办？

默认要求一种行为至少出现 4 次（`min_with = 4`）才给出数字。不够的行为在结果里 `status == INSUFFICIENT`，APP 显示"再记录 N 次就能看到"。

### Q3: 一天可以记录多次同一行为吗？

可以。同一天多次咖啡会生成多条 event，但在回归里同一天的指示变量仍然是 0/1（有 vs 无）。多次记录的价值在于时间点细分：上午一杯和下午一杯会分别计入"咖啡"和"下午咖啡"。

### Q4: "晚间变体"是怎么回事？

默认配置下，咖啡 14:00 后、运动 20:00 后、饮酒 21:00 后的行为会额外生成一个带 `_late` 后缀的子项。比如"下午咖啡 −8%"和"咖啡 −2%"可以同时出现，前者说的是午后咖啡的额外负面效果，后者是不论时间的总体效果。如果所有咖啡都在下午喝，主列和晚间列完全重合，只会出现一个。

### Q5: 百分比之和不是 100%？

正确。这不是"精力的组成"，而是"每种行为和精力的关联强度"。各项之和通常远小于 100%，因为晨起精力还受睡眠本身（时长、深度、碎片化）等没有作为"行为"记录的因素影响。`unexplained_pct` 告诉你还有多少是未记录因素贡献的。


