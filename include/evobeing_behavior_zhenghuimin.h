/*
 * evobeing_behavior_zhenghuimin.h — 行为对晨起精力的影响（个体内归因，v2）
 *
 * 独立的离线分析模块：输入每天的晨起精力值和行为记录，输出每种行为与次晨精力的
 * 关联（百分比）。只跟本人比，不跟别人比。
 * 不依赖、也不改动 evobeing_energy_zhenghuimin / evobeing_vitality_zhenghuimin，
 * 不使用 load / activity 之类的输入。建议放在手机端或云端运行（每天起床后跑一次）。
 *
 * v2 相对 v1 的模型变化（docs/behavior_impact_design.md 有详细说明）
 * ----------------------------------------------------------------
 *   1. 目标量：优先用晨起 D_init（相对本人常态的偏离，×100 成"分"），而不是百分位分数。
 *      没给 D_init 时退回 wake_score。
 *   2. 习惯吸收：引擎的常态是 λ=0.2 的 EWMA，常做的行为会被吸进常态。本模块用 D_init
 *      序列把常态轨迹反推出来（h_d − h_0 = λ·Σ_{k<d} y_k），换成慢基线（slow_baseline_days）。
 *   3. 累积方式：不再是"28 天窗口每天重算"，而是递推贝叶斯更新（卡尔曼 / RLS，带随机游走漂移）。
 *      信息一直累积，输出逐日平滑；可以保存 evobeing_behavior_state_t 做流式更新。
 *   4. 滞后：饮酒、过载、晚睡默认多一列"第二晚"（lag 2）。
 *   5. 缺记录：手动行为在"用户当天没打开记录"的日子按未知处理，不当作对照；并加一列
 *      "记录日"控制。
 *   6. 不确定性：后验标准差给区间；批量模式可选块自助法（4 天一块，保留自相关）；
 *      HIGH 档经 Benjamini–Hochberg 多项比较控制。
 *   7. 作用路径：可选 ≤ 4 条"通道"（如夜间 HRV、睡眠结构、ΔE、血氧偏差），每种行为另给
 *      在各通道上的标准化效应。
 *   8. 验证：输出一步预测技能（相对"慢基线"这个朴素预测），区分样本内解释份额与真实预测力。
 *
 * 暴露编码沿用 v1：每夜 0/1，有 late_hour 的行为另有"晚间"子列。不做剂量、距入睡时长。
 *
 * 百分比的含义（口径 A：影响幅度）
 * -------------------------------
 *   effect_pct = 100 · b_j / ȳ
 *   b_j 是"有这个行为的夜晚，次晨精力比没有时高 / 低多少分"（已控制其他行为、周末、
 *   前一天的精力、慢基线之后），ȳ 是本人晨起 wake_score 的近期加权均值（只作百分比的分母）。
 *   例如 −12% 表示"饮酒后的早晨，精力比你平时低约 12%"。各项之和不必为 100%。
 *   share_pct（口径 B）是样本内解释份额，之和 ≤ 100%，只用于排序和"合计解释了 X%"那句话。
 *
 * 可信度
 * ------
 *   行为至少出现 min_with 次才给数字。天天都有（n_without = 0）时主效应不可估，标 INSUFFICIENT，
 *   但晚间子列仍可能有数字。档位：
 *     HIGH：|z| ≥ z_high 且通过 BH（q = fdr_q）且 n_with ≥ 6
 *     MID ：|z| ≥ z_mid
 *     LOW ：其他（方向还不确定）
 *
 * 用法（批量，最简单）
 * -------------------
 *   evobeing_behavior_input_t in = {0};
 *   in.n_days = n;  in.wake_score = y;  in.wake_d_init = dinit;  in.weekday = wd;
 *   in.events = ev;  in.n_events = m;  in.wake_t_h = wake;
 *   evobeing_behavior_output_t out;
 *   evobeing_behavior_impact_zhenghuimin(&in, NULL, &out);
 *
 * 用法（流式，状态自己保存）
 * -------------------------
 *   static evobeing_behavior_state_t st;           // 约 160 KB，可整块落盘
 *   evobeing_behavior_state_init_zhenghuimin(&st, NULL);
 *   每天起床后：evobeing_behavior_day_t day = {...}; evobeing_behavior_update_zhenghuimin(&st, &day);
 *   要显示时：  evobeing_behavior_report_zhenghuimin(&st, &out);
 */
#ifndef EVOBEING_BEHAVIOR_ZHENGHUIMIN_H
#define EVOBEING_BEHAVIOR_ZHENGHUIMIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVOBEING_BEHAVIOR_MAX_DAYS     366  /* 批量输入最多接受的天数 */
#define EVOBEING_BEHAVIOR_MAX_TYPES    24   /* 同时分析的行为种类上限（含自定义） */
#define EVOBEING_BEHAVIOR_MAX_EVENTS   4096
#define EVOBEING_BEHAVIOR_MAX_LATE     8    /* 有晚间子列的行为数上限 */
#define EVOBEING_BEHAVIOR_MAX_LAG      8    /* 有"第二晚"列的行为数上限 */
#define EVOBEING_BEHAVIOR_MAX_CHANNELS 4    /* 作用路径通道数上限 */
#define EVOBEING_BEHAVIOR_MAX_COLS     (4 + EVOBEING_BEHAVIOR_MAX_TYPES + EVOBEING_BEHAVIOR_MAX_LATE + EVOBEING_BEHAVIOR_MAX_LAG)

/* 自带的行为 id。调用方可从 CUSTOM_BASE 起自定义（需在 config.custom_name 里给名字）。 */
enum {
    EVOBEING_BEHAVIOR_ALCOHOL = 0,        /* 用户记录：饮酒 */
    EVOBEING_BEHAVIOR_CAFFEINE = 1,       /* 用户记录：咖啡 / 茶 */
    EVOBEING_BEHAVIOR_MEDITATION = 2,     /* 用户记录：冥想 */
    EVOBEING_BEHAVIOR_STRESSOR = 3,       /* 用户记录：压力事件 */
    EVOBEING_BEHAVIOR_NAP = 4,            /* 自动：小睡 */
    EVOBEING_BEHAVIOR_EXERCISE = 5,       /* 自动：运动 */
    EVOBEING_BEHAVIOR_OVERLOAD = 6,       /* 自动：过载日 */
    EVOBEING_BEHAVIOR_LATE_BED = 7,       /* 自动：入睡比本人常态晚 ≥ 1 小时 */
    EVOBEING_BEHAVIOR_LATE_MEAL = 8,      /* 用户记录：晚餐很晚 / 睡前进食 */
    EVOBEING_BEHAVIOR_SCREEN = 9,         /* 用户记录：睡前长时间看屏幕 */
    EVOBEING_BEHAVIOR_CUSTOM_BASE = 16    /* 16..23 自定义（手动） */
};

/* 默认的"手动记录"行为集合（位掩码）：缺记录处理只对这些 id 生效 */
#define EVOBEING_BEHAVIOR_MANUAL_MASK_DEFAULT \
    ((1u << EVOBEING_BEHAVIOR_ALCOHOL) | (1u << EVOBEING_BEHAVIOR_CAFFEINE) | (1u << EVOBEING_BEHAVIOR_MEDITATION) | \
     (1u << EVOBEING_BEHAVIOR_STRESSOR) | (1u << EVOBEING_BEHAVIOR_LATE_MEAL) | (1u << EVOBEING_BEHAVIOR_SCREEN) | 0xFF0000u)

typedef struct {
    int id;        /* 行为 id */
    double t_h;    /* 发生时刻：自窗口第 0 天 00:00 起的小时数（如第 3 天 21:30 → 3·24+21.5） */
} evobeing_behavior_event_t;

typedef struct {
    int n_days;                   /* 天数 ≤ MAX_DAYS，第 0 天最早 */
    const double *wake_score;     /* [n_days] 每天晨起精力 0–100（百分比的分母；也是没给 D_init 时的目标量）；无则 NAN */
    const double *wake_t_h;       /* [n_days] 每天起床时刻（小时数）；NULL 则按每天 7:00；NAN 表示当天没起床记录 */
    const int *weekday;           /* [n_days] 0=周一 … 6=周日；NULL 则不做周末控制 */
    const evobeing_behavior_event_t *events;
    int n_events;
    /* ---- v2 可选输入（零初始化即为"不提供"） ---- */
    const double *wake_d_init;    /* [n_days] 晨起 D_init（vitality_daily_t.d_init，约 ±0.2）；给了就作为目标量 */
    const int *logged;            /* [n_days] 1 = 用户当天（这个早晨之前的那一天）打开 / 使用过行为记录；0 = 完全没记录；
                                   * NULL = 不区分（没有记录就当没发生，v1 行为） */
    const double *channel[EVOBEING_BEHAVIOR_MAX_CHANNELS]; /* [n_days] 作用路径通道的每日值（如醒前 HRV、深睡占比），NAN 为缺 */
    const char *channel_name[EVOBEING_BEHAVIOR_MAX_CHANNELS];
    int n_channels;
} evobeing_behavior_input_t;

/* 时间点细分：某行为在 late_hour（当地时刻）之后发生，另算一个"晚间 × 行为"子项。 */
typedef struct {
    int id;
    double late_hour;   /* NAN = 不细分。默认：咖啡 14:00、运动 20:00、饮酒 21:00 */
} evobeing_behavior_late_rule_t;

typedef struct {
    /* 样本门槛 */
    int min_with;              /* 行为至少出现次数，4 */
    int min_without;           /* 默认 0（不限制）；>0 时主效应需至少 N 天没出现 */
    /* 目标量与基线 */
    double habit_lambda;       /* 引擎常态 EWMA 系数，0.20；用于从 D_init 反推常态轨迹。0 = 不做 */
    double slow_baseline_days; /* 慢基线时间常数（天），30 */
    /* 递推贝叶斯 */
    double prior_sd_effect;    /* 行为效应的先验标准差（分），8 */
    double prior_sd_control;   /* 控制列先验标准差（分），5 */
    double effect_drift_sd;    /* 效应随机游走：每天漂移标准差（分），0.08（一年约 1.5 分），决定旧信息的遗忘速度 */
    double level_drift_sd;     /* 截距（慢基线残余）每天漂移标准差（分），0.3 */
    double sigma_init;         /* 观测噪声标准差初值（分），6 */
    /* 结构 */
    int control_weekend;       /* 1：加入周六 / 周日早晨指示列 */
    int control_prev;          /* 1：加入前一天晨起偏差（自相关） */
    unsigned manual_mask;      /* 手动记录行为的位掩码（缺记录处理），EVOBEING_BEHAVIOR_MANUAL_MASK_DEFAULT */
    evobeing_behavior_late_rule_t late[EVOBEING_BEHAVIOR_MAX_LATE];
    int n_late;
    int lag2_ids[EVOBEING_BEHAVIOR_MAX_LAG]; /* 需要"第二晚"列的行为；默认 饮酒、过载、晚睡 */
    int n_lag2;
    /* 不确定性 */
    int n_boot;                /* 批量模式块自助法次数，200；0 = 只用后验区间 */
    int block_days;            /* 块长（天），4 */
    unsigned seed;             /* 12345 */
    double z_high, z_mid;      /* 档位门槛，1.96 / 1.0 */
    double fdr_q;              /* BH 的 q，0.10 */
    double report_half_life_days; /* 报告里均值 / 份额 / 预测技能的近期加权半衰期，28 */
    const char *custom_name[8];  /* 自定义 id 16..23 的名字；NULL 用 "custom_16" 等 */
} evobeing_behavior_config_t;

typedef enum {
    EVOBEING_BEHAVIOR_INSUFFICIENT = 0, /* 样本不够，不给数字 */
    EVOBEING_BEHAVIOR_LOW = 1,
    EVOBEING_BEHAVIOR_MID = 2,
    EVOBEING_BEHAVIOR_HIGH = 3
} evobeing_behavior_status_t;

typedef struct {
    int id;
    int is_late_variant;     /* 1：这是"晚间 × 行为"子项 */
    int lag_nights;          /* 1：次晨；2："第二晚"子项（行为之后的第二个早晨） */
    char name[32];           /* 如 alcohol / caffeine_late / alcohol_2nd */
    int n_with, n_without;   /* 累计有效早晨中，有 / 无该行为的数（缺记录的未知日不计） */
    double effect_pct;       /* 口径 A：次晨精力相对本人均值的变化 %；INSUFFICIENT 时 NAN */
    double effect_points;    /* 同上，单位是分（0–100 刻度） */
    double raw_pct;          /* 未控制混杂的简单对比：有 − 无 的加权均值差 / ȳ·100（批量模式；流式为 NAN） */
    double ci_lo_pct, ci_hi_pct; /* 90% 区间（后验，或 n_boot>0 时块自助法） */
    double sd_points;        /* 效应的标准差（分） */
    double z;                /* effect / sd */
    double prob_dir;         /* 方向后验概率 0.5–1：效应与点估计同号的概率 */
    double share_pct;        /* 口径 B：样本内解释份额 %，各项之和 ≤ 100 */
    double channel_effect_sd[EVOBEING_BEHAVIOR_MAX_CHANNELS]; /* 各通道上的效应（通道标准差单位），无则 NAN */
    double channel_z[EVOBEING_BEHAVIOR_MAX_CHANNELS];
    evobeing_behavior_status_t status;
} evobeing_behavior_item_t;

typedef struct {
    int n_days_used;         /* 累计有晨起值的天数 */
    int target_is_d_init;    /* 1：目标量是 D_init；0：wake_score */
    double mean_score;       /* ȳ：wake_score 的近期加权均值（百分比分母） */
    double sd_score;         /* 目标量（去慢基线后）的近期加权标准差 */
    double sigma;            /* 模型当前的观测噪声标准差（分） */
    double explained_pct;    /* 行为列合计的样本内解释份额 %（= Σ share_pct） */
    double unexplained_pct;  /* 100 − explained_pct */
    double pred_rmse;        /* 一步预测误差（分，近期加权） */
    double naive_rmse;       /* 只用慢基线预测的误差 */
    double pred_skill_pct;   /* 100·(1 − pred_mse / naive_mse)，可为负；真实预测力的度量 */
    int n_behaviors;
    evobeing_behavior_item_t items[EVOBEING_BEHAVIOR_MAX_TYPES * 3]; /* 按 |effect_pct| 降序；INSUFFICIENT 排最后 */
    int n_insufficient;
    int n_channels;
    char channel_name[EVOBEING_BEHAVIOR_MAX_CHANNELS][24];
} evobeing_behavior_output_t;

/* ------------------------------------------------------------------ */
/* 流式状态（调用方保存；内容不保证跨版本兼容，version 不符时请重新初始化） */
/* ------------------------------------------------------------------ */
typedef struct {
    int p;
    double m[EVOBEING_BEHAVIOR_MAX_COLS];
    double P[EVOBEING_BEHAVIOR_MAX_COLS][EVOBEING_BEHAVIOR_MAX_COLS];
    double sigma2;
    /* 慢基线 / 目标相关 */
    double base;         /* 慢基线（目标量单位） */
    int n_base;
    double habit_cum;    /* Σ y（D_init 单位 ×100），用于反推常态轨迹 */
    double var_y;        /* 去基线目标量的近期加权方差 */
    double chan_var;     /* 通道：原始去基线值的方差（用于在线标准化） */
    int chan_n;
    double prev_resid;   /* 上一有效早晨的去基线目标（控制列） */
    int prev_valid;
    /* 一步预测统计 */
    double se_model, se_naive, se_w;
    /* 样本内解释份额用的近期加权矩 */
    double sum_xy[EVOBEING_BEHAVIOR_MAX_COLS], sum_x[EVOBEING_BEHAVIOR_MAX_COLS];
    double sum_xx[EVOBEING_BEHAVIOR_MAX_COLS][EVOBEING_BEHAVIOR_MAX_COLS];
    double sum_yy, sum_w;
} evobeing_behavior_kf_t;

typedef struct {
    unsigned magic, version;
    evobeing_behavior_config_t cfg;
    int n_days;                     /* 喂入的天数（含无晨起值的） */
    int n_valid;                    /* 有晨起值的天数 */
    int target_is_d_init;
    /* 列布局（惰性分配） */
    int p;
    int col_level, col_weekend, col_prev, col_logged;
    int col_main[EVOBEING_BEHAVIOR_MAX_TYPES];
    int col_late[EVOBEING_BEHAVIOR_MAX_TYPES];
    int col_lag[EVOBEING_BEHAVIOR_MAX_TYPES];
    int col_kind[EVOBEING_BEHAVIOR_MAX_COLS];   /* 0 控制 1 主 2 晚 3 第二晚 */
    int col_id[EVOBEING_BEHAVIOR_MAX_COLS];
    int has_type[EVOBEING_BEHAVIOR_MAX_TYPES];
    /* 计数 */
    int n_with[EVOBEING_BEHAVIOR_MAX_TYPES], n_without[EVOBEING_BEHAVIOR_MAX_TYPES];
    int n_late_with[EVOBEING_BEHAVIOR_MAX_TYPES], n_lag_with[EVOBEING_BEHAVIOR_MAX_TYPES];
    double rate[EVOBEING_BEHAVIOR_MAX_TYPES];   /* 已知日的出现率（缺记录日的插补值） */
    int n_rate[EVOBEING_BEHAVIOR_MAX_TYPES];
    /* 上一晚（供第二晚列） */
    unsigned char prev_occ[EVOBEING_BEHAVIOR_MAX_TYPES];
    int prev_known[EVOBEING_BEHAVIOR_MAX_TYPES];
    /* 百分比分母 */
    double mean_score, w_score;
    /* 主滤波器 + 通道滤波器 */
    int n_channels;
    char channel_name[EVOBEING_BEHAVIOR_MAX_CHANNELS][24];
    evobeing_behavior_kf_t kf[1 + EVOBEING_BEHAVIOR_MAX_CHANNELS];
} evobeing_behavior_state_t;

/* 一天（一个早晨）的输入，供流式更新 */
typedef struct {
    double wake_score;    /* 0–100；NAN 表示这个早晨没有晨起值（当天跳过，但行为仍记入"上一晚"） */
    double wake_d_init;   /* NAN = 没有 → 用 wake_score 做目标 */
    int weekday;          /* 0=周一 … 6=周日；-1 = 未知 */
    int logged;           /* 1 / 0 / -1 未知（见输入说明） */
    unsigned char occ[EVOBEING_BEHAVIOR_MAX_TYPES];   /* 这个早晨之前有没有该行为 */
    unsigned char late[EVOBEING_BEHAVIOR_MAX_TYPES];  /* 其中是否发生在 late_hour 之后 */
    double channel[EVOBEING_BEHAVIOR_MAX_CHANNELS];   /* 通道值，NAN 为缺 */
} evobeing_behavior_day_t;

/* 默认配置 */
void evobeing_behavior_config_default(evobeing_behavior_config_t *c);

/*
 * 批量计算（内部按天顺序跑一遍递推更新，再出报告；n_boot>0 时另做块自助法）。
 * 返回 0 成功；-1 参数错误；-2 有效天数不足 7 天（out 仍填好 n_days_used，items 全部 INSUFFICIENT）。
 */
int evobeing_behavior_impact_zhenghuimin(const evobeing_behavior_input_t *in,
                                         const evobeing_behavior_config_t *cfg,
                                         evobeing_behavior_output_t *out);

/* 流式：初始化状态（cfg NULL 用默认）、喂入一天、出报告。update 返回 0 已更新，1 该天无晨起值跳过，-1 参数错。
 * 要用作用路径通道时，在 init 之后、第一次 update 之前设置 st->n_channels 与 st->channel_name。 */
void evobeing_behavior_state_init_zhenghuimin(evobeing_behavior_state_t *st, const evobeing_behavior_config_t *cfg);
int  evobeing_behavior_update_zhenghuimin(evobeing_behavior_state_t *st, const evobeing_behavior_day_t *day);
int  evobeing_behavior_report_zhenghuimin(const evobeing_behavior_state_t *st, evobeing_behavior_output_t *out);

/* 把事件列表折成每天的 occ / late（归属规则：一条记录归到它之后的第一次起床）。
 * days[n_days] 的 occ/late 被填写，其余字段不动。返回填入的事件数。 */
int evobeing_behavior_fold_events_zhenghuimin(const evobeing_behavior_event_t *events, int n_events,
                                              const double *wake_t_h, int n_days,
                                              const evobeing_behavior_config_t *cfg,
                                              evobeing_behavior_day_t *days);

/* 自带 id 的名字（自定义 id 返回 "custom"）。 */
const char *evobeing_behavior_name_zhenghuimin(int id);

#ifdef __cplusplus
}
#endif
#endif
