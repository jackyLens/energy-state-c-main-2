/*
 * evobeing_zhenghuimin.h —— evobeing 公式的 C 语言主接口（作者 zhenghuimin）
 *
 * 两个计算函数：
 *   evobeing_energy_zhenghuimin()    输入一个样本（名义 15 分钟），返回当前能量值 0–100
 *   evobeing_vitality_zhenghuimin()  在同一样本上返回当前精力值 0–100
 *
 * 能量：身体储备水平（小时–天级），由睡眠充电、日间消耗与心率 / HRV 校正得到。
 * 精力：当前状态好坏（分钟–小时级），叠在能量之上，由日间心率 / HRV 相对本人常态的偏离、
 *       运动 / 小睡等事件以及昨夜睡眠质量（含夜间血氧）决定。
 *       返回的 0–100 是相对本人近 28 天日间基线的展示刻度（中位数 = 50，一个稳健标准差 = 25 分），
 *       四档（准备 / 心流 / 放松 / 低能量）在 ctx->vitality_point.level，基线积累满 3 个日间后给出；
 *       内部的绝对刻度仍在 ctx->vitality_point.vitality。
 * 两个值都只跟本人的历史比，不做人与人之间的比较。
 *
 * 调用顺序：每来一个样本（日间与夜间统一每 15 分钟一个，间隔可变），先调能量函数，再调精力函数
 * （精力需要本样本的能量结果）。只需要能量时可以不调精力函数。
 *
 * 恢复分：已算好的昨夜恢复分（0–100，相对本人基线）在晨起后拿到时，填进当时那个样本的
 * recovery 字段传一次即可（其余样本为 NAN）。库把能量向恢复分的映射值软融合、精力的晨起初始化值
 * 向恢复分融合，使晨起能量 / 精力与恢复分大致成正比（细节见 energy.h 的 energy_recovery_cfg_t
 * 与 vitality.h 的 vitality_morning_t.w_recovery）。
 *
 * 示例见文件末尾。
 */
#ifndef EVOBEING_ZHENGHUIMIN_H
#define EVOBEING_ZHENGHUIMIN_H

#include "energy.h"
#include "vitality.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVOBEING_VERSION "3.1.0"
#define EVOBEING_AUTHOR "zhenghuimin"

/* 返回码：0 成功；负数为错误（见 energy.h 的 energy_status_t） */
#define EVOBEING_OK ENERGY_OK

/* ------------------------------------------------------------------ */
/* 输入：一个样本（名义 15 分钟，间隔可变）                              */
/* ------------------------------------------------------------------ */
/*
 * 所有字段都要填。没有测到的观测值填 NAN（不是 0）。
 * 用 evobeing_sample_init_zhenghuimin() 初始化后再逐项赋值。
 */
typedef struct {
    /* 时间：距首个样本的小时数，严格递增。15 分钟一格时相邻样本相差 0.25。
     * 一个样本描述"以其时间戳结束、到上一个样本为止"的区间；数据中断后继续喂样本即可。 */
    double t_h;

    /* 当地钟点，0–24，含小数：13:30 → 13.5。 */
    double tod;

    /* 睡眠分期概率，顺序固定为 [清醒, 浅睡, 深睡, REM]，每个值 0–1，四项之和 ≤ 1。
     * 夜间按分期算法输出填写（只有一个分期时该项填 1，其余 0）；
     * 日间无分期记录时四项全部填 0。 */
    double stage[4];

    /* 心率，单位 bpm，本样本时长内的均值；未测到填 NAN。 */
    double hr;

    /* HRV = ln(RMSSD / ms)，由本样本内的逐拍 RR 间期算出 RMSSD（毫秒）后取自然对数。
     * 日间只有心率、没有 RR 时填 NAN。设备已给出 RMSSD 毫秒值时用 energy_hrv_log_from_ms()。 */
    double hrv_log;

    /* 本样本时长内的步数。 */
    double steps;

    /* 本样本时长内的活动热量，单位 kcal。 */
    double kcal;

    /* 心率观测质量，0–1。1 = 正常；0 = 本样本心率不参与计算（伪迹 / 未佩戴）。 */
    double q_hr;

    /* HRV 观测质量，0–1，含义同 q_hr。 */
    double q_hrv;

    /* 血氧饱和度 SpO₂，单位 %，本样本时长内的均值；未测到填 NAN。
     * 只有夜间血氧参与计算（作为睡眠呼吸平稳度进入精力的晨起初始化）。 */
    double spo2;

    /* 本样本时长内的血氧最低值，单位 %；未测到填 NAN。 */
    double spo2_min;

    /* 恢复分 0–100：App 已算好的昨夜恢复分，晨起后在第一个拿到它的样本上填一次，其余样本填 NAN。
     * 同一次醒来重复传入会被忽略。 */
    double recovery;
} evobeing_sample_t;

/* 把样本置为初始状态：hr / hrv_log / spo2 / spo2_min / recovery = NAN，q_hr / q_hrv = 1，其余 0。 */
void evobeing_sample_init_zhenghuimin(evobeing_sample_t *s);

/* ------------------------------------------------------------------ */
/* 个人画像：跨次运行保存的学习结果                                      */
/* ------------------------------------------------------------------ */
/*
 * 库在运行中学习这个人的常态（夜间 / 日间心率与 HRV 基线、常规睡眠窗、典型夜间回充量、
 * 睡眠结构、夜间血氧、日间精力分布）。程序退出前取出保存，下次启动时传回，可以避免重新学习
 * （约 3 晚 / 3 个日间）。全部字段为 0 表示还没有画像（首次使用直接传全 0 结构或 NULL）。
 * 传回前可把 baseline.age_days 填成“距保存已过去的天数”，库会按半衰期先衰减；不知道就保持 0。
 */
typedef struct {
    energy_baseline_t energy;   /* 能量层：个人观测基线与睡眠窗 */
    vitality_habit_t vitality;  /* 精力层：个人夜间常态 */
    vitality_baseline_t baseline; /* 精力层：日间精力分布（相对刻度与四档的基线，约 1.6 KB） */
} evobeing_profile_t;

/* ------------------------------------------------------------------ */
/* 上下文：每个用户一个，由调用方持有                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    energy_ctx_t energy;
    vitality_ctx_t vitality;
    energy_sample_t last_sample;      /* 最近一次喂入的样本（精力函数使用） */
    energy_point_t energy_point;      /* 最近一次能量计算的完整输出 */
    vitality_point_t vitality_point;  /* 最近一次精力计算的完整输出 */
    int have_energy_point;            /* 本样本是否已算过能量（精力函数依赖它） */
    int config_error;
} evobeing_ctx_t;

/*
 * 初始化上下文。
 *   profile  上次保存的个人画像；没有传 NULL。
 * 调用后即可开始喂样本。重新调用等于从头开始（状态重置、画像按参数恢复）。
 */
void evobeing_init_zhenghuimin(evobeing_ctx_t *ctx, const evobeing_profile_t *profile);

/*
 * 计算能量。
 *   sample   当前样本
 *   energy   输出：当前能量值 0–100
 * 返回 EVOBEING_OK；时间未递增返回 ENERGY_EDOMAIN；参数非法返回 ENERGY_EINVAL。
 * 出错时 *energy 不变。
 */
int evobeing_energy_zhenghuimin(evobeing_ctx_t *ctx, const evobeing_sample_t *sample, double *energy);

/*
 * 计算精力。必须在同一样本上先调用过 evobeing_energy_zhenghuimin()。
 *   vitality 输出：当前精力值 0–100（相对本人基线的展示刻度；前几小时以均衡线为中心过渡）
 * 返回 EVOBEING_OK；未先算能量返回 ENERGY_EINVAL。出错时 *vitality 不变。
 *
 * 四档状态：ctx->vitality_point.level（VITALITY_LEVEL_READY 准备 / FLOW 心流 / RELAX 放松 / LOW 低能量；
 *   基线不足 3 个日间时为 VITALITY_LEVEL_NONE，界面可显示“基线建立中”）。
 *   ctx->vitality_point.flags 含 VITALITY_FLAG_LEVEL_CHANGED 表示本样本换了档。
 * 需要更多信息（绝对刻度 vitality、置信带、四象限、标志位、晨起初始化值等）时读 ctx->vitality_point，
 * 字段含义见 vitality.h 的 vitality_point_t；能量的详细输出在 ctx->energy_point。
 */
int evobeing_vitality_zhenghuimin(evobeing_ctx_t *ctx, double *vitality);

/* 取出当前学到的个人画像（建议每天晨起后保存一次）。 */
void evobeing_get_profile_zhenghuimin(const evobeing_ctx_t *ctx, evobeing_profile_t *out);

/*
 * 用户手动标记事件（咖啡 / 冥想 / 小睡 / 压力事件 / 饮酒），在下一次精力计算时生效。
 *   type       VITALITY_EVENT_CAFFEINE 等，见 vitality.h
 *   amplitude  幅度，0 表示用默认值
 */
int evobeing_event_zhenghuimin(evobeing_ctx_t *ctx, vitality_event_type_t type, double amplitude);

/*
 * 调用示例
 * --------
 *
 *   #include "evobeing_zhenghuimin.h"
 *
 *   evobeing_ctx_t ctx;
 *   evobeing_init_zhenghuimin(&ctx, NULL);           // 首次使用；有画像时传 &profile
 *
 *   for (每个样本，日间与夜间统一每 15 分钟一个) {
 *       evobeing_sample_t s;
 *       evobeing_sample_init_zhenghuimin(&s);
 *       s.t_h      = t_hours;                       // 距首样本的小时数
 *       s.tod      = 13.5;                          // 当地钟点 13:30
 *       s.stage[0] = 0; s.stage[1] = 0; s.stage[2] = 0; s.stage[3] = 0;   // 日间无分期
 *       s.hr       = 72.0;                          // bpm
 *       s.hrv_log  = NAN;                           // 日间无 RR 间期
 *       s.steps    = 360;                           // 本 15 分钟内的步数合计
 *       s.kcal     = 18.0;                          // 本 15 分钟内的活动热量合计
 *       s.q_hr     = 1.0;
 *       s.q_hrv    = 1.0;
 *       s.spo2     = NAN;
 *       s.spo2_min = NAN;
 *       s.recovery = NAN;                           // 晨起拿到恢复分的那一个样本填 0–100
 *
 *       double energy = 0, vitality = 0;
 *       if (evobeing_energy_zhenghuimin(&ctx, &s, &energy) == EVOBEING_OK &&
 *           evobeing_vitality_zhenghuimin(&ctx, &vitality) == EVOBEING_OK) {
 *           static const char *name[4] = {"低能量", "放松", "心流", "准备"};
 *           vitality_level_t lv = ctx.vitality_point.level;
 *           printf("能量 %.0f  精力 %.0f  %s\n", energy, vitality,
 *                  lv == VITALITY_LEVEL_NONE ? "基线建立中" : name[lv]);
 *       }
 *   }
 *
 *   evobeing_profile_t profile;
 *   evobeing_get_profile_zhenghuimin(&ctx, &profile);   // 保存到用户画像，下次 init 时传回
 *
 * 夜间样本：stage 填分期概率，hr 与 hrv_log 来自逐拍 RR 间期，spo2 / spo2_min 填血氧。
 */

#ifdef __cplusplus
}
#endif

#endif /* EVOBEING_ZHENGHUIMIN_H */
