# 行为对晨起精力的影响：设计方案

模块：`include/evobeing_behavior_zhenghuimin.h` / `src/evobeing_behavior_zhenghuimin.c`
测试：`tests/test_behavior.c`　示例：`examples/behavior_demo.c`

## 1. 目标

每天早上起床，精力引擎给出一个初始值。用户想知道：我做过的哪些事（饮酒、咖啡、小睡、运动、晚睡……）跟第二天早上的精力好坏有关联，各有多大。


约束：

- 只跟自己比，不跟别人比。
- 每种行为输出一个百分比，各项之和不必是 100%，晨起精力还受没记录的因素影响。
- 行为两类来源：系统自动识别（小睡、运动、过载、晚睡），用户手动记录（饮酒、咖啡、冥想、压力事件、晚餐很晚、睡前看屏幕，以及自定义）。记录形式统一为"行为 + 时间点"。
- 本模块是独立的离线分析，建议在手机端或云端每天起床后跑一次。
- 不要求"至少 N 天没有该行为"：只要有行为和次晨精力，就参与计算（`min_without` 默认 0）。

## 2. 数据定义

### 2.1 目标量

优先用晨起 **D_init**（`vitality_daily_t.d_init`，相对本人常态的偏离，约 ±0.2），乘 100 作"分"：`y_d = 100·D_init_d`。D_init 是引擎里还没经过百分位映射的量，刻度线性、跨固件一致。没给 D_init 时退回 `wake_score`（0–100）。

一天只有一个值；当天没有主睡眠（`d_init` / `wake_score` 为 NAN，或 `wake_t_h` 为 NAN）的日子不进模型，但那一晚的行为仍带到下一个早晨的"第二晚"列。

百分比的分母始终是 `wake_score` 的近期加权均值 `ȳ`（半衰期 28 天），这样不论目标量是哪个，"−12%" 都读作"比你平时低约 12%"。

### 2.2 行为记录

每条记录 `(id, t_h)`，`t_h` 是自第 0 天 00:00 起的小时数。

**归属规则**：一条记录归到它之后的第一次起床。最后一次起床之后的记录还没有对应的早晨，不计入。第 0 天起床前 24 小时以外的记录丢弃。

**自动行为**由调用方从引擎每日汇总生成，建议规则：

| id | 来源 | 建议规则 |
|---|---|---|
| NAP | `vitality_daily_t.n_nap > 0` 或 `VITALITY_FLAG_NAP` | 小睡结束时刻 |
| EXERCISE | `n_exercise > 0` 或 `VITALITY_FLAG_EXERCISE` | 运动结束时刻 |
| OVERLOAD | `n_stress_samples ≥ 12`（STRESS 累计 ≥ 1 小时） | 当天 20:00 |
| LATE_BED | 入睡时刻比本人近 14 天中位数晚 ≥ 1 h | 入睡时刻 |

**手动行为**：ALCOHOL、CAFFEINE、MEDITATION、STRESSOR、LATE_MEAL、SCREEN，自定义 id 16–23。哪些 id 算"手动"由 `manual_mask` 决定（默认就是这些），缺记录处理只对手动行为生效。

### 2.3 记录日

`logged[d]`：这个早晨之前的那一天，用户有没有打开 / 使用过行为记录。1 = 用过（没记的手动行为视为"没发生"），0 = 完全没用（没记的手动行为视为"未知"），NULL / −1 = 不区分（v1 行为）。

### 2.4 作用路径通道

最多 4 条每日序列，如醒前 HRV、深睡占比、ΔE、血氧偏差，NAN 为缺。模块对每条通道用同一组行为列再跑一个滤波器，给出每种行为在该通道上的标准化效应，用来回答"饮酒是通过把夜间 HRV 压低起作用，还是通过别的路"。

### 2.5 采集频率

按《健康指标日间夜间采集频率》表：心率日间每 5 秒一条、夜间每 5 秒或 1 分钟一条；HRV 每 5 分钟一条；睡眠按阶段保存；步数 / 锻炼时长每 15 分钟一个汇总点。本模块只用每天一个目标值、行为时刻和（可选）每天一个通道值，不碰原始采样。多固件混合不影响：D_init 与 `wake_score` 出自同一个精力引擎，刻度一致；某天夜间 HRV 稀疏到引擎没做晨起初始化，那天为 NAN，自动跳过。

## 3. 算法

### 3.1 特征列

列按首次出现惰性分配，最多 44 列：

- 控制列：截距（吸收慢基线残余）、周末（周六 / 周日早晨 = 1，需给 `weekday`）、前一天偏差（`ỹ_{d−1} / σ`，自相关）、记录日（`logged == 1`）。
- 每种行为一列 0/1：这个早晨之前有没有发生。
- 晚间子列：有 `late_hour` 的行为（默认咖啡 14:00、运动 20:00、饮酒 21:00），行为在阈值之后发生时为 1。
- 第二晚子列：`lag2_ids` 里的行为（默认饮酒、过载、晚睡），前一晚有无发生。

### 3.2 去习惯吸收与慢基线

引擎的常态 `H` 是 λ = 0.20 的 EWMA，`D_init_d = y_d^raw − H_{d−1}`（相对量）。若一个行为天天做，它会被吸进 `H`，D_init 看不出来。但常态的轨迹能从 D_init 序列反推：

    H_d − H_{d−1} = λ·(y_d^raw − H_{d−1}) = λ·D_init_d
    ⇒ H_{d} − H_0 = λ·Σ_{k≤d} D_init_k

于是 `r_d = y_d + λ·Σ_{k<d} y_k`（`y = 100·D_init`）就是相对固定参考 `H_0` 的量。`r_d` 会随季节、生活方式慢慢漂，再减一条慢基线（指数均线，时间常数 `slow_baseline_days = 30`，只用之前的天）得到 `ỹ_d`。`habit_lambda = 0` 可关闭；目标量是 `wake_score` 时不做这步。

测试 [8]（行为成段出现：10 天有 / 10 天无，真值 −8 分）：去吸收后均值 −7.86，不去吸收 −9.02；20 次里 14 次更接近真值。改善不大但方向对，代价是零。

### 3.3 递推贝叶斯

模型：`ỹ_d = x_dᵀ b_d + ε_d`，`ε ~ N(0, σ²)`，`b_d = b_{d−1} + η_d`。

- 先验：行为列 `b ~ N(0, 8²)`，控制列 `N(0, 5²)`。这就是收缩，代替 v1 的岭回归。
- 漂移：效应列每天 `η ~ N(0, 0.08²)`（一年累积约 1.5 分），截距每天 `N(0, 0.3²)`。漂移决定遗忘速度：有数据时方差被压下去，没数据时慢慢回到先验，但**封顶在先验方差**，所以很久没出现的行为不会方差爆炸。
- 更新：标准卡尔曼一步（预测加 Q，更新用新息 `innov = ỹ − xᵀm`），协方差对称化、对角下限。
- 观测噪声 `σ²` 在线估计：`E[innov²] = xᵀPx + σ²`，以 0.05 的步长跟踪，下限 1。初值 `sigma_init = 6`。
- 一天没有目标值：不更新模型，只把那一晚的行为带到下一个早晨的第二晚列。

状态 `evobeing_behavior_state_t`（约 160 KB）可整块落盘，每天喂一天即可；批量函数内部就是按天顺序跑这个更新。测试 [9] 断言流式与批量结果一致。

效应的含义：`b_j` 是"控制其他列后，有行为 j 的早晨比没有时高 / 低多少分"。晚间子项报告 `b_j + b_j^late`（晚间发生时的总效应）；某行为全部都在晚间时，主项也报告两者之和。

### 3.4 滞后与缺记录

**第二晚**：`x^lag_j[d] = x_j[d−1]`。只有 `lag2_ids` 里的行为分配这列；前一晚未知时用该行为的已知出现率插补。报告为 `alcohol_2nd` 等，`lag_nights = 2`。

**缺记录**：对手动行为，`logged[d] == 0` 且没有该行为记录的日子视为"未知"：

- 设计向量用该行为到目前为止的已知出现率 `rate_j` 代替 0/1（期望插补）。
- 不计入 `n_with` / `n_without`。
- 加一列"记录日"，吸收"用户愿意记录的日子本身精力就不一样"这类差异。

### 3.5 不确定性与档位

- 后验：`sd = √(P_jj)`（子项含协方差项），90% 区间 `m ± 1.645·sd`，`z = m / sd`，`prob_dir = Φ(|z|)` 是效应与点估计同号的后验概率。
- 块自助法（批量模式，`n_boot = 200`，`block_days = 4`）：把天按 4 天一块随机重排，重跑整个滤波，取最终效应的分布。`sd` 取后验与自助法之大者，区间取两者并集。块重采样保留了自相关，比 v1 的逐日重采样更诚实。流式报告只有后验区间。
- 档位：
  - HIGH：`|z| ≥ 1.96`，且通过 BH（q = 0.10，在所有有数字的项之间），且 `n_with ≥ 6`，且区间不含 0。
  - MID：`|z| ≥ 1.0`。
  - LOW：其他（方向还不确定）。
  - INSUFFICIENT：`n_with < min_with`（4），或 `n_without = 0`（天天都有），或晚间 / 第二晚子项的次数不够。

多项比较：9–20 个条目同时看，按单项 p = 0.05 会平均冒出一个假 HIGH。BH 把假 HIGH 的比例控制在 10% 以内。测试 [12] 的无效应世界：HIGH 条数 0。

### 3.6 输出口径

- **口径 A（主显示）**：`effect_pct = 100·b_j / ȳ`，`effect_points = b_j`。
- `raw_pct`：未控制混杂的简单近期加权对比 `100·(mean_with − mean_without) / ȳ`，批量模式才有，流式为 NAN。
- **口径 B（内部）**：`share_pct_j = 100·(R²_full − R²_{−j})`，用近期加权二阶矩（`Sxx`、`Sxy`、`Syy`，半衰期 28 天）算"把该列系数置 0 后 R² 的下降"；总和封顶在 `100·(R²_full − R²_controls)`，`unexplained_pct = 100 − Σ share`。只用于排序辅助和"合计解释了 X%"那句话。
- **通道**：`channel_effect_sd[k]` 是行为在通道 k 上的效应，单位是该通道的标准差（通道在线标准化到"10 分 = 1 SD"，所以噪声下限、先验、漂移都能沿用主滤波器的刻度）；`channel_z[k]` 是对应的 z。通道至少 7 个有效天才报告。

### 3.7 一步预测技能

每天更新前先预测，`innov` 就是一步预测误差。朴素预测是"只用慢基线"，误差就是 `ỹ` 本身。

    pred_skill_pct = 100·(1 − Σρ^k innov² / Σρ^k ỹ²)

可为负（模型比朴素还差，说明行为列在拟合噪声）。这是区分"样本内解释份额"与"真实预测力"的指标：示例里样本内解释 99%，预测技能 41%；测试 [12] 的无效应世界预测技能 −19%。前端不必显示，但判断"这个用户的行为记录到底有没有信息"看它。

### 3.8 排序

有数字的按 |effect_pct| 降序，INSUFFICIENT 排最后。

## 4. 函数的输入与输出

### 4.1 批量 `evobeing_behavior_impact_zhenghuimin`

```c
int evobeing_behavior_impact_zhenghuimin(const evobeing_behavior_input_t *in,
                                         const evobeing_behavior_config_t *cfg,   /* NULL = 默认 */
                                         evobeing_behavior_output_t *out);
```

返回 `0` 成功；`-1` 参数错误（指针为空、`n_days` 不在 1–366、事件数超 4096）；`-2` 有效天数不足 7 天（`out->n_days_used` 已填，各项全部 INSUFFICIENT，`unexplained_pct = 100`）。

**输入 `evobeing_behavior_input_t`**

| 字段 | 类型 | 含义 |
|---|---|---|
| `n_days` | int | 天数，1–366，第 0 天最早；全部参与累积 |
| `wake_score` | const double[n_days] | 每天晨起精力 0–100（百分比分母；没给 D_init 时也是目标量）；无则 NAN |
| `wake_t_h` | const double[n_days] | 每天起床时刻（小时数）；NULL 按每天 7:00；NAN = 当天没起床记录 |
| `weekday` | const int[n_days] | 0 = 周一 … 6 = 周日；NULL 则不做周末控制 |
| `events`, `n_events` | | `{id, t_h}`，≤ 4096 |
| `wake_d_init` | const double[n_days] | v2：晨起 D_init；给了就作目标量；NULL 退回 wake_score |
| `logged` | const int[n_days] | v2：记录日 1 / 0；NULL 不区分 |
| `channel[4]`, `channel_name[4]`, `n_channels` | | v2：作用路径通道每日值与名字 |

**配置 `evobeing_behavior_config_t`**（`evobeing_behavior_config_default()` 取默认）

| 字段 | 默认 | 含义 |
|---|---|---|
| `min_with` | 4 | 行为至少出现次数 |
| `min_without` | 0 | 不限制；>0 时主效应需至少 N 天没出现 |
| `habit_lambda` | 0.20 | 引擎常态 EWMA 系数，用于反推常态轨迹；0 = 不做 |
| `slow_baseline_days` | 30 | 慢基线时间常数 |
| `prior_sd_effect` / `prior_sd_control` | 8 / 5 | 行为列 / 控制列先验标准差（分） |
| `effect_drift_sd` / `level_drift_sd` | 0.08 / 0.3 | 效应 / 截距每天漂移标准差（分） |
| `sigma_init` | 6 | 观测噪声标准差初值 |
| `control_weekend` / `control_prev` | 1 / 1 | 控制列开关 |
| `manual_mask` | 默认手动集合 | 缺记录处理生效的行为位掩码 |
| `late[]`, `n_late` | 咖啡 14:00、运动 20:00、饮酒 21:00 | 晚间子列规则 |
| `lag2_ids[]`, `n_lag2` | 饮酒、过载、晚睡 | 第二晚子列 |
| `n_boot` / `block_days` / `seed` | 200 / 4 / 12345 | 块自助法（批量模式），`n_boot = 0` 只用后验区间 |
| `z_high` / `z_mid` / `fdr_q` | 1.96 / 1.0 / 0.10 | 档位门槛与 BH 的 q |
| `report_half_life_days` | 28 | 报告里均值、份额、预测技能的近期加权半衰期 |
| `custom_name[8]` | NULL | 自定义 id 16–23 的名字 |
**输出 `evobeing_behavior_output_t`**

| 字段 | 含义 |
|---|---|
| `n_days_used` | 累计有晨起值的天数 |
| `target_is_d_init` | 1 = 目标量是 D_init |
| `mean_score`, `sd_score` | ȳ（wake_score 近期加权均值）、去基线目标量的近期加权标准差 |
| `sigma` | 模型当前观测噪声标准差（分） |
| `explained_pct` / `unexplained_pct` | 口径 B 合计 / 其余 |
| `pred_rmse`, `naive_rmse`, `pred_skill_pct` | 一步预测误差、朴素误差、预测技能 |
| `n_behaviors`, `items[]`, `n_insufficient` | 条目（含晚间、第二晚子项），已排序 |
| `n_channels`, `channel_name[]` | 通道数与名字 |

**每条 `evobeing_behavior_item_t`**

| 字段 | 含义 |
|---|---|
| `id`, `is_late_variant`, `lag_nights`, `name` | 行为 id；晚间子项；1 次晨 / 2 第二晚；名字如 `alcohol` / `caffeine_late` / `alcohol_2nd` |
| `n_with`, `n_without` | 有 / 无该行为的早晨数（未知日不计） |
| `effect_pct`, `effect_points` | 口径 A，% 与分；INSUFFICIENT 时 NAN |
| `raw_pct` | 简单对比（批量模式） |
| `ci_lo_pct`, `ci_hi_pct` | 90% 区间 |
| `sd_points`, `z`, `prob_dir` | 标准差、z、方向概率 |
| `share_pct` | 口径 B |
| `channel_effect_sd[4]`, `channel_z[4]` | 各通道上的效应（通道 SD 单位）与 z，无则 NAN |
| `status` | 0 INSUFFICIENT / 1 LOW / 2 MID / 3 HIGH |

### 4.2 流式

```c
void evobeing_behavior_state_init_zhenghuimin(evobeing_behavior_state_t *st, const evobeing_behavior_config_t *cfg);
int  evobeing_behavior_update_zhenghuimin(evobeing_behavior_state_t *st, const evobeing_behavior_day_t *day);
int  evobeing_behavior_report_zhenghuimin(const evobeing_behavior_state_t *st, evobeing_behavior_output_t *out);
int  evobeing_behavior_fold_events_zhenghuimin(const evobeing_behavior_event_t *ev, int n_ev, const double *wake_t_h,
                                               int n_days, const evobeing_behavior_config_t *cfg, evobeing_behavior_day_t *days);
```

`update` 返回 0 已更新、1 该天无晨起值跳过、−1 参数错。要用通道，在 `init` 之后、第一次 `update` 之前设 `st->n_channels` 与 `st->channel_name`。`fold_events` 把事件列表折成每天的 `occ[]` / `late[]`，归属规则同 §2.2。状态有 `magic` / `version`，版本不符时重新初始化并用历史重放。

### 4.3 调用示例

```c
evobeing_behavior_input_t in = {0};
in.n_days = 90; in.wake_score = y; in.wake_d_init = dinit; in.wake_t_h = wake; in.weekday = wd;
in.logged = logged; in.events = ev; in.n_events = n_ev;
in.channel[0] = hrv; in.channel_name[0] = "hrv_wake"; in.n_channels = 1;
evobeing_behavior_output_t out;
if (evobeing_behavior_impact_zhenghuimin(&in, NULL, &out) == 0)
    for (int i = 0; i < out.n_behaviors; ++i)
        if (out.items[i].status != EVOBEING_BEHAVIOR_INSUFFICIENT)
            printf("%s %+.0f%%\n", out.items[i].name, out.items[i].effect_pct);
```

`examples/behavior_demo.c` 的输出（90 天合成数据，真实效应：饮酒 −8 分、第二晚 −3、晚睡 −5、晚间咖啡 −4、小睡 +3、运动 +3；饮酒同时压低夜间 HRV）：

```
累计 90 天，晨起精力均值 60，目标量：D_init（相对本人常态的偏离），模型噪声 3.8 分
行为         次数   影响       90% 区间      z  份额 HRV 通道  可信度
alcohol         24/66      -14% [  -17%,   -11%]  -7.1     41%   -1.09 SD  高
late_bed        25/65      -11% [  -14%,    -8%]  -5.9     36%   +0.13 SD  高
alcohol_2nd     24/66       -7% [  -12%,    +3%]  -2.2     10%   +0.17 SD  中
nap             27/63       +5% [   +2%,    +9%]  +2.6      4%   -0.08 SD  高
exercise        37/53       +4% [   +1%,    +8%]  +2.2      4%   -0.13 SD  高
caffeine_late   27/63       -2% [   -7%,    +2%]  -0.8      3%   +0.14 SD  低
meditation      13/77       -1% [   -5%,    +2%]  -0.6      1%   -0.27 SD  低
caffeine        66/24       +1% [   -2%,    +5%]  +0.5      0%   +0.04 SD  低
late_bed_2nd    25/65       +0% [   -3%,    +6%]  +0.1      0%   -0.24 SD  低
记录的行为合计解释 99%（样本内），其余 1% 来自睡眠本身、身体状况等未记录因素
一步预测：模型 RMSE 4.77 分，只用慢基线 6.19 分，预测技能 41%
```
