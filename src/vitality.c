/*
 * vitality.c —— 精力（状态）层：能量之上的快变量 D(t) 与 V(t)
 *
 * 结构（与设计文档 docs/vitality_design.md 对应）：
 *   1. 默认参数与校验
 *   2. 子函数：τ_D、均衡线、倒 U 唤醒变换、裁剪、象限、相对刻度、四档
 *   3. 每步流程：预测 → 睡眠段跟踪 / 晨起初始化 → 事件检测与脉冲 → 观测更新 → 裁剪 → 基线 / 相对刻度 / 四档 → 输出
 *   4. 批量封装（energy_compute + 逐点 feed + 展示平滑 + 每日汇总）
 *
 * 所有“高 / 低”都相对本人：输入的残差来自个体化的能量观测模型，晨起初始化的三项相对
 * 本人在线学习的常态，分位刻度只看本人历史。
 */
#include "vitality.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V_RATE_UNIT_H (1.0 / 12.0) /* 观测噪声 /
#define V_STEP_H ENERGY_STEP_H     /* 名义计算间隔 15 min（首样本、无间隔信息时的代表时长） */
#define V_MAX_HOURS 1.0e6
#define V_SLEEP_CLOSE_H 0.25 /* 连续清醒 15 min 才算睡眠段结束 */
#define V_EX_COOL_H (10.0 / 60.0) /* 运动后连续"平静"10 min 视为结束 → 回弹 */
#define V_EX_IDLE_RESET_H 0.5 /* 运动后 30 min 仍未"平静"则放弃该运动段（不回弹） */
#define V_WEIGHT_MIN_H (1.0 / 60.0) /* 一个样本至少代表 1 min、至多 1 h 的时间（与能量层一致） */
#define V_WEIGHT_MAX_H 1.0

/* 样本代表的时长（小时）：本样本到上一样本的间隔，裁到 [1 min, 1 h]；首样本按名义 15 min */
static double v_weight_h(double h)
{
    if (!(h > 0.0)) return V_STEP_H;
    return h < V_WEIGHT_MIN_H ? V_WEIGHT_MIN_H : (h > V_WEIGHT_MAX_H ? V_WEIGHT_MAX_H : h);
}

static double vclamp(double v, double lo, double hi)
{
    if (!(v > lo)) return lo; /* NaN -> lo */
    return v > hi ? hi : v;
}

static double finite_or(double v, double fb)
{
    return isfinite(v) ? v : fb;
}

/* ------------------------------------------------------------------ */
/* 1. 默认参数                                                          */
/* ------------------------------------------------------------------ */
const char *vitality_version(void)
{
    return VITALITY_VERSION;
}

void vitality_dyn_default(vitality_dyn_t *c)
{
    if (!c) return;
    c->tau0_h = 0.75;
    c->lambda = 1.5;
    c->d_cap = 0.30;
    c->q_d = 0.02;
    c->p0 = 0.01;
}

void vitality_obs_default(vitality_obs_t *c)
{
    if (!c) return;
    c->sigma_hr = 7.0;
    c->sigma_hrv = 0.40;
    c->a_hr = 3.0;
    c->a_hrv = 2.0;
    c->r_hr = 1.0;
    c->r_hrv = 2.0;
    c->z_c = 2.8;
    c->drowsy_gain = 0.6;
    c->y_min = -2.0;
    c->rest_activity = 0.10;
    c->act_zero = 0.35;
    c->load_zero = 0.60;
    c->gate_sigma = 4.0;
}

void vitality_event_default(vitality_event_t *c)
{
    if (!c) return;
    c->a_exercise = 0.12;
    c->exercise_min_h = 10.0 / 60.0;
    c->exercise_full_h = 0.5;
    c->load_on = 0.5;
    c->load_off = 0.10;
    c->a_nap = 0.15;
    c->nap_min_h = 15.0 / 60.0;
    c->nap_full_h = 20.0 / 60.0;
    c->nap_max_h = 3.0;
    c->nap_z_hr = -0.75;
    c->nap_tod_min = 9.0;
    c->nap_tod_max = 20.0;
    c->a_fatigue = 0.03;
    c->fatigue_z = -1.0;
    c->fatigue_min_h = 0.5;
    c->arousal_z_lo = 0.7;
    c->arousal_z_hi = 2.1;
    c->stress_min_h = 10.0 / 60.0;
    c->a_caffeine = 0.10;
    c->caffeine_tau_h = 20.0 / 60.0;
    c->a_meditation = 0.08;
    c->a_stressor = -0.10;
    c->a_alcohol = -0.08;
}

void vitality_morning_default(vitality_morning_t *c)
{
    if (!c) return;
    c->gain = 0.40;
    c->w_delta_e = 0.4;
    c->w_hrv = 0.3;
    c->w_sleep = 0.3;
    c->w_spo2 = 0.2;
    c->spo2_dip_delta = 3.0;
    c->spo2_mean_scale = 1.5;
    c->spo2_dip_scale = 0.10;
    c->spo2_min_cover = 0.5;
    c->sigma_hrv_night = 0.22;
    c->lambda = 0.20;
    c->cold_nights = 3;
    c->min_sleep_h = 3.0;
    c->min_sleep_prob = 0.5;
    c->w_recovery = 0.5;
    c->rec_mid = 70.0;
    c->rec_scale = 60.0;
    c->rec_min_gap_h = 12.0;
}

void vitality_display_default(vitality_display_t *c)
{
    if (!c) return;
    c->rho = 0.15;
    c->peak_hour = 10.0;
    c->dip_hour = 14.5;
    c->dip_depth = 0.70;
    c->dip_width_h = 1.6;
    c->smooth_minutes = 15.0;
    c->band_z = 1.96;
    c->score_mode = 3;
    c->pct_half_life_days = 14.0;
    c->pct_min_days = 3;
    c->rel_sigma_floor = 0.03;
    c->rel_sigma_cap = 0.25;
    c->rel_prior_sigma = 0.06;
    c->rel_prior_n = 36.0;
    c->rel_fast_tau_h = 1.0;
    c->rel_fast_gain = 6.0;
    c->rel_fast_prior_sigma = 0.010;
    c->rel_fast_floor = 0.003;
    c->rel_fast_clip = 3.0;
    c->level_z_lo = -1.0;
    c->level_z_hi = 1.0;
    c->level_hysteresis = 2.0;
    c->quadrant_e = 0.5;
    c->quadrant_v = 0.5;
}

void vitality_config_default(vitality_config_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    vitality_dyn_default(&c->dyn);
    vitality_obs_default(&c->obs);
    vitality_event_default(&c->event);
    vitality_morning_default(&c->morning);
    vitality_display_default(&c->display);
    c->use_obs = 1;
    c->detect_events = 1;
}

static int vfail(char *buf, size_t len, const char *msg)
{
    if (buf && len) snprintf(buf, len, "%s", msg);
    return ENERGY_EINVAL;
}

static int pos_finite(double v)
{
    return isfinite(v) && v > 0.0;
}

int vitality_config_validate(const vitality_config_t *cfg, char *errbuf, size_t errlen)
{
    if (!cfg) return vfail(errbuf, errlen, "cfg 为空");
    const vitality_dyn_t *d = &cfg->dyn;
    if (!pos_finite(d->tau0_h) || !isfinite(d->lambda) || d->lambda < 0.0)
        return vfail(errbuf, errlen, "dyn: tau0_h 必须 >0，lambda 必须 >=0");
    if (!isfinite(d->d_cap) || d->d_cap < 0.0 || d->d_cap > 1.0)
        return vfail(errbuf, errlen, "dyn: d_cap 必须在 [0,1]");
    if (!isfinite(d->q_d) || d->q_d < 0.0 || !pos_finite(d->p0))
        return vfail(errbuf, errlen, "dyn: q_d 必须 >=0，p0 必须 >0");
    const vitality_obs_t *o = &cfg->obs;
    if (!pos_finite(o->sigma_hr) || !pos_finite(o->sigma_hrv) || !pos_finite(o->r_hr) ||
        !pos_finite(o->r_hrv) || !pos_finite(o->z_c) || !pos_finite(o->gate_sigma))
        return vfail(errbuf, errlen, "obs: sigma/r/z_c/gate_sigma 必须 >0");
    if (!isfinite(o->a_hr) || !isfinite(o->a_hrv) || !isfinite(o->drowsy_gain) ||
        !isfinite(o->y_min) || o->y_min > 0.0)
        return vfail(errbuf, errlen, "obs: a_hr/a_hrv/drowsy_gain 必须有限，y_min <= 0");
    if (!isfinite(o->rest_activity) || !isfinite(o->act_zero) || o->rest_activity < 0.0 ||
        o->act_zero <= o->rest_activity || !isfinite(o->load_zero) || o->load_zero <= 0.0)
        return vfail(errbuf, errlen, "obs: 0 <= rest_activity < act_zero，load_zero > 0");
    const vitality_event_t *e = &cfg->event;
    const double ev[] = {e->a_exercise, e->exercise_min_h, e->exercise_full_h, e->load_on,
                         e->load_off, e->a_nap, e->nap_min_h, e->nap_full_h, e->nap_max_h,
                         e->nap_z_hr, e->nap_tod_min, e->nap_tod_max, e->a_fatigue, e->fatigue_z,
                         e->fatigue_min_h, e->arousal_z_lo, e->arousal_z_hi, e->stress_min_h,
                         e->a_caffeine, e->caffeine_tau_h, e->a_meditation, e->a_stressor,
                         e->a_alcohol};
    for (size_t i = 0; i < sizeof(ev) / sizeof(ev[0]); i++) {
        if (!isfinite(ev[i])) return vfail(errbuf, errlen, "event: 参数必须有限");
    }
    if (e->exercise_full_h <= 0.0 || e->nap_full_h <= 0.0 || e->load_off >= e->load_on ||
        e->nap_min_h <= 0.0 || e->nap_max_h <= e->nap_min_h || e->arousal_z_lo >= e->arousal_z_hi ||
        e->nap_tod_min >= e->nap_tod_max || e->caffeine_tau_h < 0.0)
        return vfail(errbuf, errlen, "event: 时长/阈值区间非法");
    const vitality_morning_t *m = &cfg->morning;
    if (!isfinite(m->gain) || !isfinite(m->w_delta_e) || !isfinite(m->w_hrv) || !isfinite(m->w_sleep) ||
        !isfinite(m->w_spo2) || !pos_finite(m->spo2_dip_delta) || !pos_finite(m->spo2_mean_scale) ||
        !pos_finite(m->spo2_dip_scale) || !isfinite(m->spo2_min_cover) || m->spo2_min_cover < 0.0 ||
        m->spo2_min_cover > 1.0 || !pos_finite(m->sigma_hrv_night) || !isfinite(m->lambda) || m->lambda <= 0.0 || m->lambda > 1.0 ||
        m->cold_nights < 0 || !pos_finite(m->min_sleep_h) || !isfinite(m->min_sleep_prob) ||
        m->min_sleep_prob <= 0.0 || m->min_sleep_prob > 1.0)
        return vfail(errbuf, errlen, "morning: 参数非法");
    if (!isfinite(m->w_recovery) || m->w_recovery < 0.0 || m->w_recovery > 1.0 || !isfinite(m->rec_mid) ||
        m->rec_mid < 0.0 || m->rec_mid > 100.0 || !pos_finite(m->rec_scale) || !isfinite(m->rec_min_gap_h) ||
        m->rec_min_gap_h < 0.0)
        return vfail(errbuf, errlen, "morning: 恢复分参数非法（w_recovery∈[0,1]，rec_mid∈[0,100]，rec_scale>0）");
    const vitality_display_t *s = &cfg->display;
    if (!isfinite(s->rho) || s->rho < 0.0 || s->rho > 0.5 || !isfinite(s->peak_hour) ||
        !isfinite(s->dip_hour) || !isfinite(s->dip_depth) || !pos_finite(s->dip_width_h) ||
        !isfinite(s->smooth_minutes) || s->smooth_minutes < 0.0 || !pos_finite(s->band_z) ||
        s->score_mode < 0 || s->score_mode > 3 || !pos_finite(s->pct_half_life_days) ||
        s->pct_min_days < 1 || !isfinite(s->quadrant_e) || !isfinite(s->quadrant_v))
        return vfail(errbuf, errlen, "display: 参数非法");
    if (!pos_finite(s->rel_sigma_floor) || !pos_finite(s->rel_sigma_cap) || s->rel_sigma_cap < s->rel_sigma_floor ||
        !pos_finite(s->rel_prior_sigma) || !isfinite(s->rel_prior_n) || s->rel_prior_n < 0.0)
        return vfail(errbuf, errlen, "display: rel_sigma_floor/cap/prior_sigma 必须 >0 且 floor <= cap，rel_prior_n >= 0");
    if (!pos_finite(s->rel_fast_tau_h) || !isfinite(s->rel_fast_gain) || s->rel_fast_gain < 0.0 || s->rel_fast_gain > 25.0 ||
        !pos_finite(s->rel_fast_prior_sigma) || !pos_finite(s->rel_fast_floor) || !pos_finite(s->rel_fast_clip))
        return vfail(errbuf, errlen, "display: rel_fast_* 非法（gain 在 [0,25]，其余 >0）");
    if (!isfinite(s->level_z_lo) || !isfinite(s->level_z_hi) || s->level_z_lo >= 0.0 || s->level_z_hi <= 0.0 ||
        !isfinite(s->level_hysteresis) || s->level_hysteresis < 0.0 || s->level_hysteresis > 12.0)
        return vfail(errbuf, errlen, "display: level_z_lo < 0 < level_z_hi，level_hysteresis 在 [0,12]");
    return ENERGY_OK;
}

/* ------------------------------------------------------------------ */
/* 2. 子函数                                                            */
/* ------------------------------------------------------------------ */
double vitality_tau(const vitality_dyn_t *c, double e)
{
    const double ec = vclamp(e, 0.0, 1.0);
    return c->tau0_h * (1.0 + c->lambda * (1.0 - ec));
}

static void rhythm_cfg(const vitality_display_t *c, energy_display_cfg_t *d)
{
    energy_display_default(d);
    d->rho = c->rho;
    d->peak_hour = c->peak_hour;
    d->dip_hour = c->dip_hour;
    d->dip_depth = c->dip_depth;
    d->dip_width_h = c->dip_width_h;
}

/* V* = E · [1 + ρ·c(tod)]，c 已归一化到 [−1,1]（峰值 + 午后低谷，与能量展示层同一形状） */
double vitality_equilibrium(const vitality_display_t *c, double e, double tod)
{
    energy_display_cfg_t d;
    rhythm_cfg(c, &d);
    const double mod = 1.0 + c->rho * energy_circadian(&d, tod);
    return vclamp(vclamp(e, 0.0, 1.0) * mod, 0.0, 1.0);
}

/* 倒 U 唤醒变换（Yerkes–Dodson）：
 *   z ≥ 0：u = z·(1 − z/z_c)   峰值在 z_c/2（默认 1.4σ ≈ +10 bpm），z_c 处归零，再高为负；
 *   z < 0：u = drowsy_gain·z    心率低于常态 = 犯困 / 放松，轻度负向。
 * 下限 y_min 防止极端伪迹把 D 一次拉穿。 */
double vitality_arousal(const vitality_obs_t *c, double z_hr)
{
    if (!isfinite(z_hr)) return (double)NAN;
    double u;
    if (z_hr >= 0.0) {
        u = z_hr * (1.0 - z_hr / c->z_c);
    } else {
        u = c->drowsy_gain * z_hr;
    }
    return vclamp(u, c->y_min, c->z_c * 0.25); /* 峰值 z_c/4 */
}

double vitality_d_bounds(const vitality_dyn_t *c, double e, double v_star, double d)
{
    const double hi = c->d_cap * vclamp(e, 0.0, 1.0);
    const double lo = -vclamp(v_star, 0.0, 1.0);
    if (!isfinite(d)) return 0.0;
    return vclamp(d, lo, hi);
}

vitality_quadrant_t vitality_quadrant(const vitality_display_t *c, double e, double v)
{
    const int eh = e >= c->quadrant_e;
    const int vh = v >= c->quadrant_v;
    if (eh && vh) return VITALITY_Q_HIGH_HIGH;
    if (eh) return VITALITY_Q_HIGH_LOW;
    if (vh) return VITALITY_Q_LOW_HIGH;
    return VITALITY_Q_LOW_LOW;
}

/* 相对刻度：z = (v − median)/sigma；|z| ≤ 1.2 线性（25 分 / σ，覆盖 20–80 分），
 * 之外 g = sign·(1.2 + 0.8·tanh((|z| − 1.2)/0.8))，一阶连续，渐近 ±2 → relative ∈ (0, 100)，不贴边
 * （z = ±2 → 5 / 95 分）。 */
double vitality_relative_scale(const vitality_display_t *c, double v, double median, double sigma,
                               double v_slow, double fast_sigma)
{
    if (!isfinite(v) || !isfinite(median) || !(sigma > 0.0)) return (double)NAN;
    const double z = (v - median) / sigma;
    const double az = fabs(z);
    const double g = (az <= 1.2) ? az : 1.2 + 0.8 * tanh((az - 1.2) / 0.8);
    double r = 50.0 + 25.0 * (z < 0.0 ? -g : g);
    if (c->rel_fast_gain > 0.0 && isfinite(v_slow) && fast_sigma > 0.0) {
        r += c->rel_fast_gain * vclamp((v - v_slow) / fast_sigma, -c->rel_fast_clip, c->rel_fast_clip);
    }
    return vclamp(r, 0.0, 100.0);
}

/* 四档：边界 25·(2 + z_lo) / 50 / 25·(2 + z_hi)（默认 25 / 50 / 75），带滞回：
 * 要跨过边界 ± hysteresis 才换档；prev 为 NONE 时不滞回。 */
vitality_level_t vitality_level_of(const vitality_display_t *c, double relative, vitality_level_t prev, int ready)
{
    if (!ready || !isfinite(relative)) return VITALITY_LEVEL_NONE;
    const double edge[3] = {25.0 * (2.0 + c->level_z_lo), 50.0, 25.0 * (2.0 + c->level_z_hi)};
    int lvl = 0;
    while (lvl < 3 && relative >= edge[lvl]) lvl++;
    if (prev < VITALITY_LEVEL_LOW || prev > VITALITY_LEVEL_READY || c->level_hysteresis <= 0.0) return (vitality_level_t)lvl;
    const int p = (int)prev;
    if (lvl == p) return prev;
    /* 向上：须超过上边界 + h；向下：须低于下边界 − h。每次最多按实际落点换档 */
    int cur = p;
    while (cur < lvl && relative >= edge[cur] + c->level_hysteresis) cur++;
    while (cur > lvl && relative < edge[cur - 1] - c->level_hysteresis) cur--;
    return (vitality_level_t)cur;
}

/* 直方图分位（箱内线性插值） */
static double hist_quantile(const vitality_baseline_t *b, double q)
{
    const double target = q * b->w;
    double acc = 0.0;
    for (int i = 0; i < VITALITY_PCT_BINS; i++) {
        const double h = b->hist[i];
        if (acc + h >= target) {
            const double frac = (h > 0.0) ? (target - acc) / h : 0.5;
            return ((double)i + vclamp(frac, 0.0, 1.0)) / (double)VITALITY_PCT_BINS;
        }
        acc += h;
    }
    return 1.0;
}

int vitality_baseline_stats(const vitality_baseline_t *b, double *median, double *sigma)
{
    if (median) *median = (double)NAN;
    if (sigma) *sigma = (double)NAN;
    if (!b || !(b->w > 0.0)) return 0;
    const double m = hist_quantile(b, 0.5);
    const double q1 = hist_quantile(b, 0.25), q3 = hist_quantile(b, 0.75);
    if (median) *median = m;
    if (sigma) *sigma = (q3 - q1) / 1.349;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 3. 流式                                                              */
/* ------------------------------------------------------------------ */
static void habit_clear(vitality_habit_t *h)
{
    memset(h, 0, sizeof(*h));
    h->delta_e = h->hrv_wake = h->sleep_h = h->deep_frac = h->wake_frac = (double)NAN;
    h->spo2_mean = h->spo2_dip = (double)NAN;
}

static int habit_valid(const vitality_habit_t *h)
{
    if (!h || h->n_nights < 0 || h->n_spo2_nights < 0 || h->n_spo2_nights > h->n_nights) return 0;
    if (h->n_nights == 0) return 1;
    const double v[7] = {h->delta_e, h->hrv_wake, h->sleep_h, h->deep_frac, h->wake_frac,
                         h->spo2_mean, h->spo2_dip};
    for (int i = 0; i < 7; i++) {
        if (isnan(v[i])) continue; /* 单项缺失允许 */
        if (!isfinite(v[i])) return 0;
    }
    if (!isnan(h->sleep_h) && (h->sleep_h < 0.0 || h->sleep_h > 24.0)) return 0;
    if (!isnan(h->deep_frac) && (h->deep_frac < 0.0 || h->deep_frac > 1.0)) return 0;
    if (!isnan(h->wake_frac) && (h->wake_frac < 0.0 || h->wake_frac > 1.0)) return 0;
    /* 旧画像（memset 0）的 spo2_mean==0 视为缺失 */
    if (!isnan(h->spo2_mean) && h->spo2_mean != 0.0 && (h->spo2_mean < 50.0 || h->spo2_mean > 100.0)) return 0;
    if (!isnan(h->spo2_dip) && (h->spo2_dip < 0.0 || h->spo2_dip > 1.0)) return 0;
    return 1;
}

void vitality_ctx_init(vitality_ctx_t *ctx, const energy_config_t *ecfg, const vitality_config_t *vcfg)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    if (vcfg) {
        ctx->cfg = *vcfg;
    } else {
        vitality_config_default(&ctx->cfg);
    }
    ctx->config_error = (vitality_config_validate(&ctx->cfg, NULL, 0) != ENERGY_OK);
    if (ctx->config_error) vitality_config_default(&ctx->cfg);
    energy_ctx_init(&ctx->ectx, ecfg);
    if (ctx->ectx.config_error) ctx->config_error = 1;
    ctx->own_energy = 1;
    ctx->d = 0.0;
    ctx->p = ctx->cfg.dyn.p0;
    ctx->e_last = ctx->ectx.cfg.x0_e;
    ctx->t_h = ctx->tod = (double)NAN;
    ctx->rec_pending = ctx->rec_pending_t = ctx->rec_last_t = (double)NAN;
    habit_clear(&ctx->habit);
    ctx->pct_last_day = (double)NAN;
    ctx->last_level = VITALITY_LEVEL_NONE;
    ctx->snap_median = ctx->snap_sigma = (double)NAN;
    ctx->snap_w = 0.0;
    ctx->snap_valid = 0;
    ctx->cur_median = ctx->cur_sigma = (double)NAN;
    ctx->v_slow = (double)NAN;
    ctx->cur_fast_sigma = (double)NAN;
    ctx->last_v = (double)NAN;
    ctx->last_d_init = (double)NAN;
    ctx->last_close_t = (double)NAN;
    habit_clear(&ctx->habit_before_close);
    ctx->last_spo2_mean = ctx->last_spo2_dip = ctx->last_x_spo2 = (double)NAN;
}

int vitality_ctx_set_habit(vitality_ctx_t *ctx, const vitality_habit_t *h)
{
    if (!ctx || !habit_valid(h)) return ENERGY_EINVAL;
    ctx->habit = *h;
    if (ctx->habit.spo2_mean == 0.0) ctx->habit.spo2_mean = (double)NAN; /* 旧画像无血氧 */
    if (ctx->habit.n_spo2_nights == 0) ctx->habit.spo2_mean = ctx->habit.spo2_dip = (double)NAN;
    return ENERGY_OK;
}

void vitality_ctx_get_habit(const vitality_ctx_t *ctx, vitality_habit_t *out)
{
    if (!ctx || !out) return;
    *out = ctx->habit;
}

int vitality_ctx_set_baseline(vitality_ctx_t *ctx, const vitality_baseline_t *b)
{
    if (!ctx || !b) return ENERGY_EINVAL;
    if (!isfinite(b->w) || b->w < 0.0 || b->days < 0 || !isfinite(b->age_days) || b->age_days < 0.0) return ENERGY_EINVAL;
    if (!isfinite(b->fast_ss) || b->fast_ss < 0.0 || !isfinite(b->fast_w) || b->fast_w < 0.0) return ENERGY_EINVAL;
    double sum = 0.0;
    for (int i = 0; i < VITALITY_PCT_BINS; i++) {
        if (!isfinite(b->hist[i]) || b->hist[i] < 0.0) return ENERGY_EINVAL;
        sum += b->hist[i];
    }
    if (fabs(sum - b->w) > 1e-6 * (1.0 + sum)) return ENERGY_EINVAL;
    ctx->base = *b;
    if (b->age_days > 0.0) {
        const double k = pow(0.5, b->age_days / ctx->cfg.display.pct_half_life_days);
        for (int i = 0; i < VITALITY_PCT_BINS; i++) ctx->base.hist[i] *= k;
        ctx->base.w *= k;
        ctx->base.fast_ss *= k;
        ctx->base.fast_w *= k;
    }
    ctx->base.age_days = 0.0;
    /* 恢复的基线不属于当前日历日：下一次 pct_observe 会把今天计为新的一天并重新取快照 */
    ctx->day_seen = 0;
    ctx->snap_valid = 0;
    return ENERGY_OK;
}

void vitality_ctx_get_baseline(const vitality_ctx_t *ctx, vitality_baseline_t *out)
{
    if (!ctx || !out) return;
    *out = ctx->base;
    out->age_days = 0.0;
}

int vitality_ctx_event(vitality_ctx_t *ctx, vitality_event_type_t type, double amplitude)
{
    if (!ctx || ctx->config_error) return ENERGY_EINVAL;
    const vitality_event_t *e = &ctx->cfg.event;
    double amp = amplitude, tau = 0.0;
    const int given = isfinite(amplitude) && amplitude > 0.0;
    switch (type) {
    case VITALITY_EVENT_CAFFEINE:
        amp = given ? amplitude : e->a_caffeine;
        tau = e->caffeine_tau_h;
        break;
    case VITALITY_EVENT_MEDITATION:
        amp = given ? amplitude : e->a_meditation;
        break;
    case VITALITY_EVENT_NAP:
        amp = given ? amplitude : e->a_nap;
        break;
    case VITALITY_EVENT_STRESSOR:
        amp = given ? -amplitude : e->a_stressor;
        break;
    case VITALITY_EVENT_ALCOHOL:
        amp = given ? -amplitude : e->a_alcohol;
        break;
    case VITALITY_EVENT_CUSTOM:
        if (!isfinite(amplitude) || amplitude == 0.0) return ENERGY_EINVAL;
        amp = amplitude;
        break;
    default:
        return ENERGY_EINVAL;
    }
    amp = vclamp(amp, -1.0, 1.0);
    /* 多个事件叠加：幅度相加，起效时间取较慢者 */
    ctx->pend_amp += amp;
    if (tau > ctx->pend_tau_h) ctx->pend_tau_h = tau;
    ctx->pending_flags |= VITALITY_FLAG_USER;
    return ENERGY_OK;
}

/* ---- 睡眠段 / 晨起初始化 ------------------------------------------------------------------ */
#define V_RESUME_H 2.0 /* 主睡眠结束后 2 h 内再次入睡 → 视为同一夜的续睡，合并统计 */

static void sleep_begin(vitality_ctx_t *ctx, double t, double e)
{
    ctx->in_sleep = 1;
    ctx->awake_h = 0.0;
    ctx->hrv_recent_n = ctx->hrv_recent_i = 0;
    if (ctx->last_close_main && isfinite(ctx->last_close_t) && t - ctx->last_close_t < V_RESUME_H &&
        ctx->habit.n_nights > 0) {
        /* 续睡：保留 sleep_h / 分期累计 / 入睡时 E，不重置；夜间醒着的时间计入碎片化 */
        ctx->sleep_stage_acc[ENERGY_WAKE] += ctx->awake_between_h;
        /* 回滚上一次的常态更新，结束时按合并后的整夜重算 */
        ctx->habit = ctx->habit_before_close;
        return;
    }
    ctx->sleep_h = 0.0;
    ctx->sleep_t0 = t;
    ctx->sleep_e0 = e;
    memset(ctx->sleep_stage_acc, 0, sizeof(ctx->sleep_stage_acc));
    memset(ctx->spo2_hist, 0, sizeof(ctx->spo2_hist));
    ctx->spo2_n = 0;
    ctx->spo2_h = 0.0;
    ctx->spo2_sum = 0.0;
    ctx->last_close_main = 0;
    ctx->awake_between_h = 0.0;
}

/* ---- 夜间血氧 ------------------------------------------------------------------------ */
/* 血氧有效性：NAN / 0 / [50,100] 之外一律视为缺失 */
static int spo2_valid(double v)
{
    return isfinite(v) && v >= 50.0 && v <= 100.0;
}

static void spo2_observe(vitality_ctx_t *ctx, const energy_sample_t *sample, double w_h)
{
    if (!spo2_valid(sample->spo2)) return;
    double lo = spo2_valid(sample->spo2_min) ? fmin(sample->spo2_min, sample->spo2) : sample->spo2;
    int b = (int)floor(lo + 0.5) - VITALITY_SPO2_LO;
    if (b < 0) b = 0;
    if (b >= VITALITY_SPO2_BINS) b = VITALITY_SPO2_BINS - 1;
    ctx->spo2_hist[b]++;
    ctx->spo2_n++;
    ctx->spo2_h += w_h;
    ctx->spo2_sum += sample->spo2;
}

/* 本夜血氧统计：均值 + 瞬时下跌占比（样本低值 ≤ 中位数 − dip_delta）；
 * 覆盖不足（有效血氧不足 1 h、少于 12 个样本、或不足睡眠时长的 spo2_min_cover）返回 0 */
static int spo2_night_stats(const vitality_ctx_t *ctx, double *mean, double *dip)
{
    const vitality_morning_t *m = &ctx->cfg.morning;
    const int n = ctx->spo2_n;
    *mean = *dip = (double)NAN;
    if (n < 12 || ctx->spo2_h < 1.0 || ctx->spo2_h < m->spo2_min_cover * ctx->sleep_h) return 0;
    /* 直方图中位数 */
    int acc = 0, med = VITALITY_SPO2_BINS - 1;
    for (int b = 0; b < VITALITY_SPO2_BINS; b++) {
        acc += ctx->spo2_hist[b];
        if (2 * acc >= n) {
            med = b;
            break;
        }
    }
    const int thr = med - (int)ceil(m->spo2_dip_delta - 1e-9); /* 箱号 ≤ thr 即下跌 */
    int below = 0;
    for (int b = 0; b < VITALITY_SPO2_BINS && b <= thr; b++) below += ctx->spo2_hist[b];
    *mean = ctx->spo2_sum / (double)n;
    *dip = (double)below / (double)n;
    return 1;
}

static void habit_blend(double *field, double x, double lam)
{
    if (!isfinite(x)) return;
    if (!isfinite(*field)) {
        *field = x;
    } else {
        *field += lam * (x - *field);
    }
}

/* 睡眠中最近 window_h 小时 HRV 的时间加权均值（环形缓冲从新到旧累加到够 window_h 为止；
 * 任何采样间隔下都是“最近 1 小时”，而不是“最近 12 个样本”） */
static double hrv_recent_mean(const vitality_ctx_t *ctx, double window_h)
{
    const int cap = (int)(sizeof(ctx->hrv_recent) / sizeof(ctx->hrv_recent[0]));
    double s = 0.0, w = 0.0;
    for (int k = 0; k < ctx->hrv_recent_n && w < window_h - 1e-9; k++) {
        const int i = (ctx->hrv_recent_i - 1 - k + 2 * cap) % cap;
        double wh = ctx->hrv_recent_h[i];
        if (w + wh > window_h) wh = window_h - w;
        s += wh * ctx->hrv_recent[i];
        w += wh;
    }
    return w > 0.0 ? s / w : (double)NAN;
}

static void hrv_recent_push(vitality_ctx_t *ctx, double v, double w_h)
{
    const int cap = (int)(sizeof(ctx->hrv_recent) / sizeof(ctx->hrv_recent[0]));
    ctx->hrv_recent[ctx->hrv_recent_i] = v;
    ctx->hrv_recent_h[ctx->hrv_recent_i] = w_h;
    ctx->hrv_recent_i = (ctx->hrv_recent_i + 1) % cap;
    if (ctx->hrv_recent_n < cap) ctx->hrv_recent_n++;
}

/* 恢复分融合（v1.4）：D ← D + w·(D_rec − D)，D_rec = gain·clip((R − rec_mid)/rec_scale, ±0.5) */
static void recovery_fuse(vitality_ctx_t *ctx, double r, double t)
{
    const vitality_morning_t *m = &ctx->cfg.morning;
    const double x = vclamp((r - m->rec_mid) / m->rec_scale, -0.5, 0.5);
    const double d_rec = m->gain * x;
    ctx->d += m->w_recovery * (d_rec - ctx->d);
    ctx->p *= (1.0 - m->w_recovery);
    if (!(ctx->p > 1e-6)) ctx->p = 1e-6;
    ctx->last_d_init = ctx->d;
    ctx->rec_last_t = t;
}

/* 睡眠段结束：若是主睡眠 → 计算 D_init、更新常态；返回 1 表示发生了晨起初始化 */
static int sleep_close(vitality_ctx_t *ctx, double e_now, double *d_init_out)
{
    const vitality_morning_t *m = &ctx->cfg.morning;
    ctx->in_sleep = 0;
    const double sleep_h = ctx->sleep_h;
    if (sleep_h < m->min_sleep_h) return 0;

    double tot = 0.0;
    for (int i = 0; i < 4; i++) tot += ctx->sleep_stage_acc[i];
    const double deep = tot > 0.0 ? ctx->sleep_stage_acc[ENERGY_DEEP] / tot : (double)NAN;
    const double wake = tot > 0.0 ? ctx->sleep_stage_acc[ENERGY_WAKE] / tot : (double)NAN;
    const double hrv = hrv_recent_mean(ctx, 1.0); /* 醒前 1 h 的时间加权均值 */
    const double delta_e = e_now - ctx->sleep_e0;
    double spo2_mean, spo2_dip;
    const int has_spo2 = spo2_night_stats(ctx, &spo2_mean, &spo2_dip);

    /* 四项都相对本人常态；常态未知的项为 0（“不知道就不猜”） */
    const vitality_habit_t *h = &ctx->habit;
    double x1 = 0.0, x2 = 0.0, x3 = 0.0, x4 = 0.0;
    if (h->n_nights > 0) {
        if (isfinite(h->delta_e)) x1 = vclamp((delta_e - h->delta_e) / 0.30, -0.5, 0.5);
        if (isfinite(h->hrv_wake) && isfinite(hrv))
            x2 = vclamp((hrv - h->hrv_wake) / (2.0 * m->sigma_hrv_night), -0.5, 0.5);
        /* 睡眠结构：时长差 2 h / 深睡占比差 10 个百分点 / 碎片化差 10 个百分点 各记满分 */
        double q = 0.0;
        if (isfinite(h->sleep_h)) q += 0.5 * vclamp((sleep_h - h->sleep_h) / 2.0, -1.0, 1.0);
        if (isfinite(h->deep_frac) && isfinite(deep)) q += 0.25 * vclamp((deep - h->deep_frac) / 0.10, -1.0, 1.0);
        if (isfinite(h->wake_frac) && isfinite(wake)) q -= 0.25 * vclamp((wake - h->wake_frac) / 0.10, -1.0, 1.0);
        x3 = vclamp(q, -0.5, 0.5);
        /* 夜间血氧平稳度：均值低于常态 / 瞬时下跌比常态多 → 负；
         * 均值高于常态的正向收益减半（SpO₂ 有天花板，“比平时更高”信息量小） */
        if (has_spo2 && h->n_spo2_nights > 0) {
            double r = 0.0;
            if (isfinite(h->spo2_mean)) {
                const double dm = vclamp((spo2_mean - h->spo2_mean) / m->spo2_mean_scale, -1.0, 1.0);
                r += 0.5 * (dm > 0.0 ? 0.5 * dm : dm);
            }
            if (isfinite(h->spo2_dip)) r -= 0.5 * vclamp((spo2_dip - h->spo2_dip) / m->spo2_dip_scale, -1.0, 1.0);
            x4 = vclamp(r, -0.5, 0.5);
        }
    }
    const double d_init =
        m->gain * vclamp(m->w_delta_e * x1 + m->w_hrv * x2 + m->w_sleep * x3 + m->w_spo2 * x4, -0.5, 0.5);
    ctx->last_spo2_mean = has_spo2 ? spo2_mean : (double)NAN;
    ctx->last_spo2_dip = has_spo2 ? spo2_dip : (double)NAN;
    ctx->last_x_spo2 = has_spo2 ? x4 : (double)NAN;

    /* 更新常态：前 cold_nights 晚累计平均，之后 EWMA（先留快照，续睡合并时回滚重算） */
    ctx->habit_before_close = ctx->habit;
    const double lam = (h->n_nights < m->cold_nights) ? 1.0 / (double)(h->n_nights + 1) : m->lambda;
    habit_blend(&ctx->habit.delta_e, delta_e, lam);
    habit_blend(&ctx->habit.hrv_wake, hrv, lam);
    habit_blend(&ctx->habit.sleep_h, sleep_h, lam);
    habit_blend(&ctx->habit.deep_frac, deep, lam);
    habit_blend(&ctx->habit.wake_frac, wake, lam);
    if (has_spo2) {
        const double lam_s = (h->n_spo2_nights < m->cold_nights) ? 1.0 / (double)(h->n_spo2_nights + 1) : m->lambda;
        habit_blend(&ctx->habit.spo2_mean, spo2_mean, lam_s);
        habit_blend(&ctx->habit.spo2_dip, spo2_dip, lam_s);
        if (ctx->habit.n_spo2_nights < 1000000) ctx->habit.n_spo2_nights++;
    }
    if (ctx->habit.n_nights < 1000000) ctx->habit.n_nights++;

    ctx->d = d_init;
    ctx->p = ctx->cfg.dyn.p0;
    if (d_init_out) *d_init_out = d_init;
    return 1;
}

/* ---- 分位直方图 ----------------------------------------------------------------------- */
static int pct_bin(double v)
{
    int b = (int)floor(vclamp(v, 0.0, 1.0) * VITALITY_PCT_BINS);
    if (b >= VITALITY_PCT_BINS) b = VITALITY_PCT_BINS - 1;
    if (b < 0) b = 0;
    return b;
}

/* 日历日开始：用“到昨天为止”的直方图取一次快照（当天的相对刻度全天用它） */
static void snapshot_take(vitality_ctx_t *ctx)
{
    ctx->snap_valid = vitality_baseline_stats(&ctx->base, &ctx->snap_median, &ctx->snap_sigma);
    ctx->snap_w = ctx->base.w;
}

static void pct_observe(vitality_ctx_t *ctx, double v, double day, double h)
{
    const vitality_display_t *c = &ctx->cfg.display;
    if (!isfinite(ctx->pct_last_day)) {
        ctx->pct_last_day = day;
        snapshot_take(ctx);
    } else if (day > ctx->pct_last_day) {
        const double k = pow(0.5, (day - ctx->pct_last_day) / c->pct_half_life_days);
        for (int i = 0; i < VITALITY_PCT_BINS; i++) ctx->base.hist[i] *= k;
        ctx->base.w *= k;
        ctx->base.fast_ss *= k;
        ctx->base.fast_w *= k;
        ctx->pct_last_day = day;
        ctx->day_seen = 0;
        snapshot_take(ctx);
    }
    if (!ctx->day_seen) {
        ctx->day_seen = 1;
        if (ctx->base.days < 1000000) ctx->base.days++;
        if (!ctx->snap_valid) snapshot_take(ctx); /* set_baseline 之后的首个样本 */
    }
    const double w = v_weight_h(h) / V_RATE_UNIT_H; /* 按时间计权，5 min 一格时恰为 1 */
    ctx->base.hist[pct_bin(v)] += w;
    ctx->base.w += w;
}

/* 快变分量：v 相对其滑动均值的偏差；滑动均值只在日间推进，晨起 / 首样本重置为当前 v */
static void fast_observe(vitality_ctx_t *ctx, double v, double h, int reset)
{
    const vitality_display_t *c = &ctx->cfg.display;
    if (reset || !isfinite(ctx->v_slow)) {
        ctx->v_slow = v;
        return;
    }
    const double f = v - ctx->v_slow;
    const double w = v_weight_h(h) / V_RATE_UNIT_H;
    ctx->base.fast_ss += w * f * f;
    ctx->base.fast_w += w;
    const double a = (h > 0.0) ? 1.0 - exp(-h / c->rel_fast_tau_h) : 0.0;
    ctx->v_slow += a * f;
}

static int baseline_ready(const vitality_ctx_t *ctx)
{
    return ctx->base.days >= ctx->cfg.display.pct_min_days && ctx->base.w > 0.0;
}

static double pct_query(const vitality_ctx_t *ctx, double v)
{
    if (!baseline_ready(ctx)) return (double)NAN;
    const int b = pct_bin(v);
    double below = 0.0;
    for (int i = 0; i < b; i++) below += ctx->base.hist[i];
    const double cum = below + 0.5 * ctx->base.hist[b];
    return vclamp(100.0 * cum / ctx->base.w, 0.0, 100.0);
}

/* 本点所用基线：经验中位数 / 稳健标准差与先验（中心 = 均衡线 V*，σ = rel_prior_sigma）
 * 按样本权重混合，w = 0 时完全是先验，几小时后以经验为主；sigma 裁到 [floor, cap]。
 * 基线可用（≥ pct_min_days 个日间）后用当天晨起的快照，全天不变；建立期间逐样本实时更新，
 * 让第一、二天的曲线尽快有参照。 */
static void baseline_refresh(vitality_ctx_t *ctx, double v_star)
{
    const vitality_display_t *c = &ctx->cfg.display;
    double m_emp = (double)NAN, s_emp = (double)NAN, w = 0.0;
    if (baseline_ready(ctx) && ctx->snap_valid) {
        m_emp = ctx->snap_median;
        s_emp = ctx->snap_sigma;
        w = ctx->snap_w;
    } else if (vitality_baseline_stats(&ctx->base, &m_emp, &s_emp)) {
        w = ctx->base.w;
    }
    double m = v_star, s = c->rel_prior_sigma;
    if (isfinite(m_emp) && w > 0.0) {
        const double a = w / (w + c->rel_prior_n);
        m = (1.0 - a) * v_star + a * m_emp;
        s = (1.0 - a) * c->rel_prior_sigma + a * s_emp;
    }
    ctx->cur_median = m;
    ctx->cur_sigma = vclamp(s, c->rel_sigma_floor, c->rel_sigma_cap);
    /* σ_fast：经验 RMS 与先验按权重混合（同样的先验等效样本数） */
    double sf = c->rel_fast_prior_sigma;
    if (ctx->base.fast_w > 0.0) {
        const double a = ctx->base.fast_w / (ctx->base.fast_w + c->rel_prior_n);
        sf = (1.0 - a) * c->rel_fast_prior_sigma + a * sqrt(ctx->base.fast_ss / ctx->base.fast_w);
    }
    ctx->cur_fast_sigma = fmax(sf, c->rel_fast_floor);
}

/* ---- 输出 --------------------------------------------------------------------------- */
static double choose_score(const vitality_display_t *c, double vit, double pct, double rel)
{
    if (c->score_mode == 1) return isfinite(pct) ? pct : vit;
    if (c->score_mode == 2) return isfinite(pct) ? 0.5 * (vit + pct) : vit;
    if (c->score_mode == 3) return isfinite(rel) ? rel : vit;
    return vit;
}

static void fill_point(const vitality_ctx_t *ctx, const energy_point_t *ep, double v_star,
                       double v, double z_hr, double z_hrv, unsigned flags, vitality_point_t *out)
{
    const vitality_display_t *c = &ctx->cfg.display;
    memset(out, 0, sizeof(*out));
    out->t_h = ep->t_h;
    out->tod = ep->tod;
    out->e = vclamp(ep->e, 0.0, 1.0);
    out->v_star = v_star;
    out->d = ctx->d;
    out->d_init = (flags & (VITALITY_FLAG_WAKE | VITALITY_FLAG_RECOVERY)) ? ctx->last_d_init : (double)NAN;
    const int spo2_pt = (flags & VITALITY_FLAG_WAKE) && (flags & VITALITY_FLAG_SPO2);
    out->spo2_night = spo2_pt ? ctx->last_spo2_mean : (double)NAN;
    out->spo2_dip = spo2_pt ? ctx->last_spo2_dip : (double)NAN;
    /* V 对 E 的灵敏度 ≈ (1+ρc)，把能量的不确定性也折进带宽 */
    const double dv_de = (out->e > 0.0) ? v_star / out->e : 1.0;
    const double var = fmax(ctx->p, 0.0) + dv_de * dv_de * finite_or(ep->e_sd, 0.0) * finite_or(ep->e_sd, 0.0);
    out->d_sd = sqrt(var);
    out->v = vclamp(v, 0.0, 1.0);
    out->vitality = 100.0 * out->v;
    out->percentile = pct_query(ctx, out->v);
    out->base_median = ctx->cur_median;
    out->base_sigma = ctx->cur_sigma;
    out->v_slow = ctx->v_slow;
    out->base_fast_sigma = ctx->cur_fast_sigma;
    out->relative = vitality_relative_scale(c, out->v, out->base_median, out->base_sigma, out->v_slow,
                                            out->base_fast_sigma);
    out->level = vitality_level_of(c, out->relative, ctx->last_level, baseline_ready(ctx));
    out->score = choose_score(c, out->vitality, out->percentile, out->relative);
    out->lower = 100.0 * vclamp(out->v - c->band_z * out->d_sd, 0.0, 1.0);
    out->upper = 100.0 * vclamp(out->v + c->band_z * out->d_sd, 0.0, 1.0);
    out->energy = ep->energy;
    out->z_hr = z_hr;
    out->z_hrv = z_hrv;
    out->quadrant = vitality_quadrant(c, out->e, out->v);
    out->flags = flags;
    if (ctx->habit.n_nights < ctx->cfg.morning.cold_nights) out->flags |= VITALITY_FLAG_LEARNING;
    if (isfinite(out->percentile)) out->flags |= VITALITY_FLAG_PCT_READY;
}

/* 标量 Kalman 更新：y = a·D + ε，返回 1 表示已采纳 */
static int kf_update(vitality_ctx_t *ctx, double y, double a, double r)
{
    if (!isfinite(y) || !(r > 0.0)) return 0;
    const double S = a * a * ctx->p + r;
    if (!(S > 1e-12)) return 0;
    const double innov = y - a * ctx->d;
    if (fabs(innov) / sqrt(S) > ctx->cfg.obs.gate_sigma) return 0;
    const double K = a * ctx->p / S;
    ctx->d += K * innov;
    ctx->p = (1.0 - K * a) * ctx->p;
    if (!(ctx->p > 1e-8)) ctx->p = 1e-8;
    return 1;
}

int vitality_ctx_feed(vitality_ctx_t *ctx, const energy_sample_t *sample,
                      const energy_point_t *ep, vitality_point_t *vp)
{
    if (!ctx || !sample || !ep) return ENERGY_EINVAL;
    if (ctx->config_error) return ENERGY_EINVAL;
    const vitality_config_t *cfg = &ctx->cfg;
    const double t = ep->t_h;
    const double tod = finite_or(ep->tod, 0.0);
    if (!isfinite(t) || fabs(t) > V_MAX_HOURS) return ENERGY_EINVAL;
    const double e = vclamp(ep->e, 0.0, 1.0);

    double h = 0.0;
    if (!ctx->started) {
        ctx->started = 1;
        ctx->t0 = t;
        ctx->tod0 = tod;
        ctx->pct_last_day = (double)NAN;
        ctx->day_seen = 0;
    } else {
        h = t - ctx->t_h;
        if (!(h > 0.0)) return ENERGY_EDOMAIN;
    }
    ctx->t_h = t;
    ctx->tod = tod;
    ctx->e_last = e;

    unsigned flags = ctx->pending_flags;
    ctx->pending_flags = 0u;
    const int gap = (ep->flags & ENERGY_FLAG_GAP) != 0;
    if (gap) flags |= VITALITY_FLAG_GAP;

    /* ---- ① 预测：均值回复（解析解） ---- */
    if (h > 0.0) {
        const double tau = vitality_tau(&cfg->dyn, e);
        const double k = exp(-h / tau);
        ctx->d *= k;
        ctx->p = ctx->p * k * k + cfg->dyn.q_d * h;
        if (!isfinite(ctx->p) || ctx->p <= 0.0) ctx->p = cfg->dyn.p0;
    }
    /* 用户标记：咖啡等按一阶起效，其余即时 */
    if (ctx->pend_amp != 0.0) {
        const double frac = (ctx->pend_tau_h > 0.0 && h > 0.0) ? 1.0 - exp(-h / ctx->pend_tau_h) : 1.0;
        const double inj = ctx->pend_amp * frac;
        ctx->d += inj;
        ctx->pend_amp -= inj;
        if (fabs(ctx->pend_amp) < 1e-4) {
            ctx->pend_amp = 0.0;
            ctx->pend_tau_h = 0.0;
        }
        flags |= VITALITY_FLAG_USER;
    }

    /* ---- ② 睡眠段跟踪与晨起初始化 ---- */
    const int asleep = ep->sleep_prob >= cfg->morning.min_sleep_prob;
    const int nap_window = tod >= cfg->event.nap_tod_min && tod < cfg->event.nap_tod_max;
    if (asleep) flags |= VITALITY_FLAG_SLEEP;
    if (asleep && !ctx->in_sleep) sleep_begin(ctx, t, e);
    if (ctx->in_sleep) {
        double st[4];
        double tot = 0.0;
        for (int i = 0; i < 4; i++) {
            st[i] = vclamp(finite_or(sample->stage[i], 0.0), 0.0, 1.0);
            tot += st[i];
        }
        if (tot > 1.0) {
            for (int i = 0; i < 4; i++) st[i] /= tot;
        }
        const double w = v_weight_h(h);
        for (int i = 0; i < 4; i++) ctx->sleep_stage_acc[i] += st[i] * w;
        if (asleep) {
            ctx->sleep_h += w;
            ctx->awake_h = 0.0;
            spo2_observe(ctx, sample, w);
            if (isfinite(sample->hrv_log) && finite_or(sample->q_hrv, 1.0) > 0.0) {
                hrv_recent_push(ctx, sample->hrv_log, w);
            }
        } else {
            ctx->awake_h += w;
        }
        const int long_break = (!asleep && h > 1.0); /* 睡眠段后直接缺口 1 h 以上 */
        if ((!asleep && ctx->awake_h >= V_SLEEP_CLOSE_H - 1e-9) || long_break) {
            const double sleep_h = ctx->sleep_h;
            double d_init = 0.0;
            if (sleep_close(ctx, e, &d_init)) {
                ctx->last_d_init = d_init;
                ctx->last_close_t = t;
                ctx->last_close_main = 1;
                ctx->awake_between_h = 0.0;
                flags |= VITALITY_FLAG_WAKE;
                if (isfinite(ctx->last_x_spo2)) {
                    flags |= VITALITY_FLAG_SPO2;
                    if (ctx->last_x_spo2 <= -0.25) flags |= VITALITY_FLAG_BREATH;
                }
            } else if (cfg->detect_events && sleep_h >= cfg->event.nap_min_h && sleep_h <= cfg->event.nap_max_h &&
                       nap_window) {
                ctx->d += cfg->event.a_nap * fmin(1.0, sleep_h / cfg->event.nap_full_h);
                flags |= VITALITY_FLAG_NAP;
            }
            ctx->nap_run = 0;
            ctx->nap_h = 0.0;
        }
    }

    if (!asleep && !ctx->in_sleep && ctx->last_close_main) ctx->awake_between_h += v_weight_h(h);

    /* ---- ②b 恢复分融合（样本带 recovery：NAN / ≤0 视为无）。睡眠段还没结束就先到 → 暂存，
     *      等 D_init 算出后再融合，否则会被 D_init 覆盖；同一次醒来 rec_min_gap_h 内只用一次 ---- */
    if (cfg->morning.w_recovery > 0.0) {
        if (isfinite(sample->recovery) && sample->recovery > 0.0) {
            ctx->rec_pending = vclamp(sample->recovery, 0.0, 100.0);
            ctx->rec_pending_t = t;
        }
        if (isfinite(ctx->rec_pending) && !ctx->in_sleep) {
            if (!isfinite(ctx->rec_last_t) || t - ctx->rec_last_t >= cfg->morning.rec_min_gap_h - 1e-9) {
                recovery_fuse(ctx, ctx->rec_pending, t);
                flags |= VITALITY_FLAG_RECOVERY;
            }
            ctx->rec_pending = ctx->rec_pending_t = (double)NAN;
        }
    }

    /* ---- ③ 日间观测 z 化与事件检测 ---- */
    double z_hr = (double)NAN, z_hrv = (double)NAN;
    const double act = vclamp(finite_or(ep->activity, 0.0), 0.0, 1.0);
    const double load = vclamp(finite_or(ep->load, 0.0), 0.0, 1.0);
    const int awake = !asleep && !gap;
    if (awake) {
        if (isfinite(ep->hr_resid)) z_hr = vclamp(ep->hr_resid / cfg->obs.sigma_hr, -6.0, 6.0);
        if (isfinite(ep->hrv_resid)) z_hrv = vclamp(ep->hrv_resid / cfg->obs.sigma_hrv, -3.0, 3.0);
    }
    /* 观测权重：活动量 A（步数，外生）是主判据；负荷 L 含心率项，单独高不能证明在运动
     * （可能正是我们要测的交感激活），只有 A 也高时才按运动排除；L 高而 A 低时降权一半。 */
    double w_act = 0.0;
    if (awake) {
        w_act = vclamp(1.0 - (act - cfg->obs.rest_activity) / (cfg->obs.act_zero - cfg->obs.rest_activity),
                       0.0, 1.0);
        if (load > cfg->obs.load_zero) {
            if (act >= cfg->obs.rest_activity) {
                w_act = 0.0;
            } else {
                w_act *= 0.5;
            }
        }
    }

    if (cfg->detect_events && awake) {
        const vitality_event_t *ev = &cfg->event;
        /* 运动段：L ≥ load_on 且 A ≥ act_zero（确实在动，排除"心率高但没动"的交感激活）进入；
         * 结束后连续 10 min"负荷退下且基本不动"（L ≤ load_off，或 A 回到静息水平）→ 回弹 */
        const double wh = v_weight_h(h);
        if (load >= ev->load_on && act >= cfg->obs.act_zero) {
            ctx->ex_run++;
            ctx->ex_h += (h > 0.0) ? h : V_STEP_H;
            ctx->ex_cool_h = ctx->ex_idle_h = 0.0;
        } else if (ctx->ex_run > 0) {
            const int settled = (load <= ev->load_off) || (act <= cfg->obs.rest_activity);
            ctx->ex_cool_h = settled ? ctx->ex_cool_h + wh : 0.0;
            ctx->ex_idle_h += wh;
            if (settled && ctx->ex_cool_h >= V_EX_COOL_H - 1e-9) {
                if (ctx->ex_h >= ev->exercise_min_h) {
                    ctx->d += ev->a_exercise * fmin(1.0, ctx->ex_h / ev->exercise_full_h);
                    flags |= VITALITY_FLAG_EXERCISE;
                }
                ctx->ex_run = 0;
                ctx->ex_h = 0.0;
                ctx->ex_cool_h = ctx->ex_idle_h = 0.0;
            } else if (ctx->ex_idle_h >= V_EX_IDLE_RESET_H - 1e-9) {
                ctx->ex_run = 0;
                ctx->ex_h = 0.0;
                ctx->ex_cool_h = ctx->ex_idle_h = 0.0;
            }
        }
        /* 无分期的小睡：心率明显低于常态 + 不动，持续 ≥ nap_min_h，之后恢复 → 回弹 */
        const int nap_like = nap_window && act < 0.05 && isfinite(z_hr) && z_hr <= ev->nap_z_hr;
        if (nap_like) {
            ctx->nap_run++;
            ctx->nap_h += (h > 0.0) ? h : V_STEP_H;
        } else if (ctx->nap_run > 0) {
            if (ctx->nap_h >= ev->nap_min_h && ctx->nap_h <= ev->nap_max_h) {
                ctx->d += ev->a_nap * fmin(1.0, ctx->nap_h / ev->nap_full_h);
                flags |= VITALITY_FLAG_NAP;
            }
            ctx->nap_run = 0;
            ctx->nap_h = 0.0;
        }
        /* 持续低 HRV：拖拽项（持续作用，不是一次脉冲） */
        if (isfinite(z_hrv) && z_hrv <= ev->fatigue_z && w_act > 0.0) {
            ctx->fat_run++;
            ctx->fat_h += (h > 0.0) ? h : V_STEP_H;
            if (ctx->fat_h >= ev->fatigue_min_h) {
                ctx->d -= ev->a_fatigue * (-z_hrv) * ((h > 0.0) ? h : V_STEP_H);
                flags |= VITALITY_FLAG_FATIGUE;
            }
        } else if (isfinite(z_hrv) || !awake) {
            ctx->fat_run = 0;
            ctx->fat_h = 0.0;
        }
        /* 交感激活的标志位（效应本身走观测通道） */
        if (isfinite(z_hr) && w_act > 0.0) {
            if (z_hr > ev->arousal_z_hi) {
                ctx->stress_h += wh;
                if (ctx->stress_h >= ev->stress_min_h - 1e-9) flags |= VITALITY_FLAG_STRESS;
            } else {
                ctx->stress_h = 0.0;
                if (z_hr >= ev->arousal_z_lo) flags |= VITALITY_FLAG_AROUSAL;
            }
        } else {
            ctx->stress_h = 0.0;
        }
    } else if (!awake) {
        ctx->ex_run = 0;
        ctx->ex_h = 0.0;
        ctx->ex_cool_h = 0.0;
        ctx->fat_run = 0;
        ctx->fat_h = 0.0;
        ctx->stress_h = 0.0;
        if (asleep) {
            ctx->nap_run = 0;
            ctx->nap_h = 0.0;
        }
    }

    /* ---- ④ 观测更新（标量 KF，两通道顺序更新） ---- */
    int n_used = 0;
    if (cfg->use_obs && awake && w_act > 0.0) {
        /* 一个样本的残差是其区间内的均值：区间越长，这一次观测顶的 5 min 观测越多
         * （r_hr/r_hrv 按 5 min 一次标定），否则 30 min 算一次的流每小时只拿到 1/6 的观测权重 */
        const double obs_w = fmax(v_weight_h(h) / V_RATE_UNIT_H, 1.0);
        const double w_obs = w_act * obs_w;
        n_used += kf_update(ctx, vitality_arousal(&cfg->obs, z_hr), cfg->obs.a_hr, cfg->obs.r_hr / w_obs);
        n_used += kf_update(ctx, z_hrv, cfg->obs.a_hrv, cfg->obs.r_hrv / w_obs);
    }
    if (n_used == 0) flags |= VITALITY_FLAG_NO_OBS;

    /* ---- ⑤ 裁剪与合成 ---- */
    const double v_star = vitality_equilibrium(&cfg->display, e, tod);
    ctx->d = vitality_d_bounds(&cfg->dyn, e, v_star, ctx->d);
    if (ctx->p > 1.0) ctx->p = 1.0;
    const double v = vclamp(v_star + ctx->d, 0.0, 1.0);
    ctx->last_v = v;

    /* ---- ⑥ 分位刻度（只记日间清醒样本） ---- */
    if (awake) {
        pct_observe(ctx, v, floor((t - ctx->t0 + ctx->tod0) / 24.0 + 1e-9), h);
        fast_observe(ctx, v, h, (flags & VITALITY_FLAG_WAKE) != 0);
    } else if (asleep) {
        ctx->v_slow = (double)NAN; /* 睡眠段内不算快变；醒来后从当前 V 重新起算 */
    }

    /* ---- ⑦ 相对刻度基线与四档（滞回状态随流推进；vp 为 NULL 时也要推进） ---- */
    baseline_refresh(ctx, v_star);
    vitality_point_t local;
    vitality_point_t *outp = vp ? vp : &local;
    fill_point(ctx, ep, v_star, v, z_hr, z_hrv, flags, outp);
    if (outp->level != ctx->last_level) {
        ctx->last_level = outp->level;
        outp->flags |= VITALITY_FLAG_LEVEL_CHANGED;
    }
    return ENERGY_OK;
}

int vitality_ctx_step(vitality_ctx_t *ctx, const energy_sample_t *sample,
                      energy_point_t *ep, vitality_point_t *vp)
{
    if (!ctx || !sample) return ENERGY_EINVAL;
    if (ctx->config_error) return ENERGY_EINVAL;
    energy_point_t local;
    const int rc = energy_ctx_step(&ctx->ectx, sample, &local);
    if (rc != ENERGY_OK) return rc;
    if (ep) *ep = local;
    return vitality_ctx_feed(ctx, sample, &local, vp);
}

void vitality_ctx_peek(const vitality_ctx_t *ctx, double tod, vitality_point_t *out)
{
    if (!ctx || !out) return;
    if (!isfinite(tod) || tod < 0.0) tod = finite_or(ctx->tod, 0.0);
    energy_point_t ep;
    if (ctx->own_energy && ctx->ectx.started) {
        energy_ctx_peek(&ctx->ectx, tod, &ep);
    } else {
        memset(&ep, 0, sizeof(ep));
        ep.t_h = ctx->t_h;
        ep.tod = tod;
        ep.e = ctx->e_last;
        ep.energy = 100.0 * ctx->e_last;
    }
    ep.t_h = ctx->t_h;
    const double v_star = vitality_equilibrium(&ctx->cfg.display, ctx->e_last, tod);
    const double d = vitality_d_bounds(&ctx->cfg.dyn, ctx->e_last, v_star, ctx->d);
    fill_point(ctx, &ep, v_star, vclamp(v_star + d, 0.0, 1.0), (double)NAN, (double)NAN, 0u, out);
    out->d = d;
}

/* ------------------------------------------------------------------ */
/* 4. 批量                                                              */
/* ------------------------------------------------------------------ */
void vitality_output_free(vitality_output_t *out)
{
    if (!out) return;
    free(out->points);
    free(out->daily);
    energy_output_free(&out->energy);
    memset(out, 0, sizeof(*out));
}

/* 居中滑动平均，窗口按时间（minutes）而不是样本数：样本流可以是不等间隔的。双指针 O(n)。 */
static void smooth_centered(const double *t_h, const double *in, size_t n, double minutes, double *out)
{
    if (!(minutes > 0.0) || n < 3) {
        memcpy(out, in, n * sizeof(double));
        return;
    }
    const double half = minutes / 60.0 / 2.0 + 1e-9;
    size_t lo = 0, hi = 0;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        while (hi < n && t_h[hi] <= t_h[i] + half) acc += in[hi++];
        while (lo < hi && t_h[lo] < t_h[i] - half) acc -= in[lo++];
        const size_t c = hi - lo;
        out[i] = c > 0 ? acc / (double)c : in[i];
    }
}

static void daily_summary(const vitality_output_t *out, vitality_daily_t *daily, size_t n_daily)
{
    if (out->n_points == 0 || n_daily == 0) return;
    const double t0 = out->points[0].t_h, tod0 = out->points[0].tod;
    for (size_t d = 0; d < n_daily; d++) {
        vitality_daily_t *dd = &daily[d];
        memset(dd, 0, sizeof(*dd));
        dd->day = (int)d;
        if (d < out->energy.n_daily) memcpy(dd->date, out->energy.daily[d].date, sizeof(dd->date));
        dd->wake_score = dd->d_init = dd->peak_tod = dd->dip_tod = (double)NAN;
        dd->spo2_night = dd->spo2_dip = (double)NAN;
        dd->min = DBL_MAX;
        dd->max = -DBL_MAX;
    }
    for (size_t i = 0; i < out->n_points; i++) {
        const vitality_point_t *p = &out->points[i];
        const int day = (int)floor((p->t_h - t0 + tod0) / 24.0);
        if (day < 0 || (size_t)day >= n_daily) continue;
        vitality_daily_t *dd = &daily[day];
        dd->n++;
        if (p->flags & VITALITY_FLAG_WAKE) {
            dd->wake_score = p->score;
            dd->d_init = p->d_init;
            dd->spo2_night = p->spo2_night;
            dd->spo2_dip = p->spo2_dip;
        }
        if (p->flags & VITALITY_FLAG_RECOVERY) { /* 恢复分融合后的值才是当天的"晨起" */
            dd->wake_score = p->score;
            dd->d_init = p->d_init;
        }
        if (p->flags & VITALITY_FLAG_EXERCISE) dd->n_exercise++;
        if (p->flags & VITALITY_FLAG_NAP) dd->n_nap++;
        if (p->flags & VITALITY_FLAG_STRESS) dd->n_stress_samples++;
        if (p->flags & VITALITY_FLAG_USER) dd->n_user_events++;
        if (p->flags & VITALITY_FLAG_SLEEP) continue;
        if (p->level >= VITALITY_LEVEL_LOW && p->level <= VITALITY_LEVEL_READY) dd->n_level[(int)p->level]++;
        dd->mean += p->score;
        if (p->score < dd->min) {
            dd->min = p->score;
            if (p->tod >= 13.0 && p->tod < 17.0) dd->dip_tod = p->tod;
        }
        if (p->score > dd->max) {
            dd->max = p->score;
            if (p->tod >= 6.0 && p->tod < 16.0) dd->peak_tod = p->tod;
        }
    }
    for (size_t d = 0; d < n_daily; d++) {
        vitality_daily_t *dd = &daily[d];
        int n_awake = 0;
        for (size_t i = 0; i < out->n_points; i++) {
            const vitality_point_t *p = &out->points[i];
            const int day = (int)floor((p->t_h - t0 + tod0) / 24.0);
            if (day == (int)d && !(p->flags & VITALITY_FLAG_SLEEP)) n_awake++;
        }
        if (n_awake > 0) {
            dd->mean /= (double)n_awake;
        } else {
            dd->mean = dd->min = dd->max = (double)NAN;
        }
    }
}

int vitality_compute(const energy_sample_t *samples, size_t n,
                     const energy_config_t *ecfg, const vitality_config_t *vcfg,
                     vitality_output_t *out)
{
    if (!out) return ENERGY_EINVAL;
    memset(out, 0, sizeof(*out));
    if (!samples || n == 0) return ENERGY_EINVAL;

    vitality_config_t vc;
    if (vcfg) {
        vc = *vcfg;
    } else {
        vitality_config_default(&vc);
    }
    if (vitality_config_validate(&vc, NULL, 0) != ENERGY_OK) return ENERGY_EINVAL;
    energy_config_t ec;
    if (ecfg) {
        ec = *ecfg;
    } else {
        energy_config_default(&ec);
    }

    int rc = energy_compute(samples, n, &ec, &out->energy);
    if (rc != ENERGY_OK) {
        vitality_output_free(out);
        return rc;
    }
    if (out->energy.n_points != n) {
        vitality_output_free(out);
        return ENERGY_EINVAL;
    }
    out->points = (vitality_point_t *)calloc(n, sizeof(vitality_point_t));
    double *raw = (double *)malloc(n * sizeof(double));
    double *sm = (double *)malloc(n * sizeof(double));
    double *tax = (double *)malloc(n * sizeof(double));
    if (!out->points || !raw || !sm || !tax) {
        free(raw);
        free(sm);
        free(tax);
        vitality_output_free(out);
        return ENERGY_ENOMEM;
    }
    out->n_points = n;

    vitality_ctx_t *ctx = (vitality_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) {
        free(raw);
        free(sm);
        free(tax);
        vitality_output_free(out);
        return ENERGY_ENOMEM;
    }
    vitality_ctx_init(ctx, &ec, &vc);
    ctx->own_energy = 0;
    for (size_t i = 0; i < n; i++) {
        rc = vitality_ctx_feed(ctx, &samples[i], &out->energy.points[i], &out->points[i]);
        if (rc != ENERGY_OK) {
            free(ctx);
            free(raw);
            free(sm);
            free(tax);
            vitality_output_free(out);
            return rc;
        }
        raw[i] = out->points[i].v;
        tax[i] = out->points[i].t_h;
    }
    out->habit = ctx->habit;
    vitality_ctx_get_baseline(ctx, &out->baseline);
    free(ctx);

    /* 展示平滑：只平滑 vitality/relative/score/带，不改 v/d（那是状态）；
     * 四档按平滑后的 relative 重新带滞回判定，流式与批量在 level 上可能相差一两个点 */
    smooth_centered(tax, raw, n, vc.display.smooth_minutes, sm);
    double mn = DBL_MAX, mx = -DBL_MAX, sum = 0.0;
    vitality_level_t prev = VITALITY_LEVEL_NONE;
    for (size_t i = 0; i < n; i++) {
        vitality_point_t *p = &out->points[i];
        const double vs = vclamp(sm[i], 0.0, 1.0);
        p->vitality = 100.0 * vs;
        p->relative = vitality_relative_scale(&vc.display, vs, p->base_median, p->base_sigma, p->v_slow, p->base_fast_sigma);
        const int ready = (p->flags & VITALITY_FLAG_PCT_READY) != 0;
        p->flags &= ~VITALITY_FLAG_LEVEL_CHANGED;
        p->level = vitality_level_of(&vc.display, p->relative, prev, ready);
        if (p->level != prev) p->flags |= VITALITY_FLAG_LEVEL_CHANGED;
        prev = p->level;
        p->score = choose_score(&vc.display, p->vitality, p->percentile, p->relative);
        p->lower = 100.0 * vclamp(vs - vc.display.band_z * p->d_sd, 0.0, 1.0);
        p->upper = 100.0 * vclamp(vs + vc.display.band_z * p->d_sd, 0.0, 1.0);
        if (p->score < mn) mn = p->score;
        if (p->score > mx) mx = p->score;
        sum += p->score;
    }
    free(raw);
    free(sm);
    free(tax);
    out->stats_min = mn;
    out->stats_max = mx;
    out->stats_mean = sum / (double)n;

    const size_t n_daily = out->energy.n_daily;
    if (n_daily > 0) {
        out->daily = (vitality_daily_t *)calloc(n_daily, sizeof(vitality_daily_t));
        if (!out->daily) {
            vitality_output_free(out);
            return ENERGY_ENOMEM;
        }
        out->n_daily = n_daily;
        daily_summary(out, out->daily, n_daily);
    }
    return ENERGY_OK;
}
