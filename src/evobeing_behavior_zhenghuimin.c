/*
 * evobeing_behavior_zhenghuimin.c — 行为对晨起精力的影响（个体内归因，v2）
 *
 * 方法概要（详见 docs/behavior_impact_design.md）：
 *   1. 目标量 y_d：优先 100·D_init（相对本人常态的偏离），否则 wake_score。
 *   2. 去习惯吸收：D_init 是"今晚 − EWMA 常态"，常态轨迹 h_d − h_0 = λ Σ_{k<d} y_k 可由 y 序列
 *      反推，于是 r_d = y_d + λ Σ_{k<d} y_k 是相对固定参考的量；再减一条慢基线（时间常数
 *      slow_baseline_days）得到 ỹ_d。
 *   3. 递推贝叶斯：ỹ_d = x_dᵀ b + ε，b 服从随机游走（效应慢漂移，截距吸收基线残余），
 *      卡尔曼更新；观测噪声 σ² 在线估计。列：截距、周末、前一天偏差、记录日、
 *      每种行为 0/1、晚间子列、第二晚子列。列按首次出现惰性分配。
 *   4. 缺记录：手动行为在"没打开记录"的日子按未知处理，x 用该行为的已知出现率插补，不计入 n_with / n_without。
 *   5. 报告：后验均值 / 标准差 → 效应、区间、z、方向概率；HIGH 经 BH 控制。批量模式可选块自助法，
 *      区间取后验与自助法之宽者。
 *   6. 口径 B：用近期加权的二阶矩（Sxx, Sxy, Syy）算"把某列系数置 0 后 R² 的下降"。
 *   7. 一步预测技能：innov² 与 ỹ²（慢基线预测的误差）的近期加权比。
 */
#include "evobeing_behavior_zhenghuimin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXD  EVOBEING_BEHAVIOR_MAX_DAYS
#define MAXT  EVOBEING_BEHAVIOR_MAX_TYPES
#define MAXC  EVOBEING_BEHAVIOR_MAX_COLS
#define MAXCH EVOBEING_BEHAVIOR_MAX_CHANNELS
#define MAXBOOT 300
#define NAN_V (0.0 / 0.0)
#define STATE_MAGIC 0x45564232u  /* "EVB2" */
#define STATE_VERSION 2u

enum { KIND_CTRL = 0, KIND_MAIN = 1, KIND_LATE = 2, KIND_LAG = 3 };

static int finite_d(double v) { return v == v && v < 1e300 && v > -1e300; }

/* ------------------------------------------------------------------ */
/* 配置与名字                                                           */
/* ------------------------------------------------------------------ */
void evobeing_behavior_config_default(evobeing_behavior_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->min_with = 4;
    c->min_without = 0;
    c->habit_lambda = 0.20;
    c->slow_baseline_days = 30.0;
    c->prior_sd_effect = 8.0;
    c->prior_sd_control = 5.0;
    c->effect_drift_sd = 0.08;
    c->level_drift_sd = 0.3;
    c->sigma_init = 6.0;
    c->control_weekend = 1;
    c->control_prev = 1;
    c->manual_mask = EVOBEING_BEHAVIOR_MANUAL_MASK_DEFAULT;
    c->late[0].id = EVOBEING_BEHAVIOR_CAFFEINE; c->late[0].late_hour = 14.0;
    c->late[1].id = EVOBEING_BEHAVIOR_EXERCISE; c->late[1].late_hour = 20.0;
    c->late[2].id = EVOBEING_BEHAVIOR_ALCOHOL;  c->late[2].late_hour = 21.0;
    c->n_late = 3;
    c->lag2_ids[0] = EVOBEING_BEHAVIOR_ALCOHOL;
    c->lag2_ids[1] = EVOBEING_BEHAVIOR_OVERLOAD;
    c->lag2_ids[2] = EVOBEING_BEHAVIOR_LATE_BED;
    c->n_lag2 = 3;
    c->n_boot = 200;
    c->block_days = 4;
    c->seed = 12345u;
    c->z_high = 1.96;
    c->z_mid = 1.0;
    c->fdr_q = 0.10;
    c->report_half_life_days = 28.0;
}

static void sanitize_cfg(evobeing_behavior_config_t *c)
{
    if (c->min_with < 1) c->min_with = 1;
    if (c->min_without < 0) c->min_without = 0;
    if (!finite_d(c->habit_lambda) || c->habit_lambda < 0.0 || c->habit_lambda > 1.0) c->habit_lambda = 0.20;
    if (!(c->slow_baseline_days >= 2.0)) c->slow_baseline_days = 30.0;
    if (!(c->prior_sd_effect > 0.0)) c->prior_sd_effect = 8.0;
    if (!(c->prior_sd_control > 0.0)) c->prior_sd_control = 5.0;
    if (!(c->effect_drift_sd >= 0.0)) c->effect_drift_sd = 0.08;
    if (!(c->level_drift_sd >= 0.0)) c->level_drift_sd = 0.3;
    if (!(c->sigma_init > 0.0)) c->sigma_init = 6.0;
    if (c->n_late > EVOBEING_BEHAVIOR_MAX_LATE) c->n_late = EVOBEING_BEHAVIOR_MAX_LATE;
    if (c->n_late < 0) c->n_late = 0;
    if (c->n_lag2 > EVOBEING_BEHAVIOR_MAX_LAG) c->n_lag2 = EVOBEING_BEHAVIOR_MAX_LAG;
    if (c->n_lag2 < 0) c->n_lag2 = 0;
    if (c->n_boot < 0) c->n_boot = 0;
    if (c->n_boot > MAXBOOT) c->n_boot = MAXBOOT;
    if (c->block_days < 1) c->block_days = 4;
    if (!(c->z_high > 0.0)) c->z_high = 1.96;
    if (!(c->z_mid > 0.0)) c->z_mid = 1.0;
    if (!(c->fdr_q > 0.0 && c->fdr_q < 1.0)) c->fdr_q = 0.10;
    if (!(c->report_half_life_days >= 1.0)) c->report_half_life_days = 28.0;
}

const char *evobeing_behavior_name_zhenghuimin(int id)
{
    switch (id) {
    case EVOBEING_BEHAVIOR_ALCOHOL:    return "alcohol";
    case EVOBEING_BEHAVIOR_CAFFEINE:   return "caffeine";
    case EVOBEING_BEHAVIOR_MEDITATION: return "meditation";
    case EVOBEING_BEHAVIOR_STRESSOR:   return "stressor";
    case EVOBEING_BEHAVIOR_NAP:        return "nap";
    case EVOBEING_BEHAVIOR_EXERCISE:   return "exercise";
    case EVOBEING_BEHAVIOR_OVERLOAD:   return "overload";
    case EVOBEING_BEHAVIOR_LATE_BED:   return "late_bed";
    case EVOBEING_BEHAVIOR_LATE_MEAL:  return "late_meal";
    case EVOBEING_BEHAVIOR_SCREEN:     return "screen";
    default:                           return "custom";
    }
}

static void item_name(const evobeing_behavior_config_t *cfg, int id, int kind, char *buf, size_t n)
{
    const char *base = NULL;
    char tmp[16];
    if (id >= EVOBEING_BEHAVIOR_CUSTOM_BASE && id < EVOBEING_BEHAVIOR_CUSTOM_BASE + 8)
        base = cfg->custom_name[id - EVOBEING_BEHAVIOR_CUSTOM_BASE];
    if (!base) base = evobeing_behavior_name_zhenghuimin(id);
    if (!strcmp(base, "custom")) {
        snprintf(tmp, sizeof tmp, "custom_%d", id);
        base = tmp;
    }
    snprintf(buf, n, kind == KIND_LATE ? "%s_late" : kind == KIND_LAG ? "%s_2nd" : "%s", base);
}

static double late_hour_of(const evobeing_behavior_config_t *cfg, int id)
{
    int i;
    for (i = 0; i < cfg->n_late; ++i)
        if (cfg->late[i].id == id) return cfg->late[i].late_hour;
    return NAN_V;
}

static int wants_lag(const evobeing_behavior_config_t *cfg, int id)
{
    int i;
    for (i = 0; i < cfg->n_lag2; ++i)
        if (cfg->lag2_ids[i] == id) return 1;
    return 0;
}

static unsigned xorshift(unsigned *s)
{
    unsigned x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static double norm_cdf(double z) { return 0.5 * erfc(-z / sqrt(2.0)); }

/* ------------------------------------------------------------------ */
/* 卡尔曼滤波器                                                         */
/* ------------------------------------------------------------------ */
static void kf_init(evobeing_behavior_kf_t *k, double sigma_init)
{
    memset(k, 0, sizeof *k);
    k->sigma2 = sigma_init * sigma_init;
}

static void kf_add_col(evobeing_behavior_kf_t *k, int c, double prior_var)
{
    int j;
    k->m[c] = 0.0;
    for (j = 0; j < MAXC; ++j) k->P[c][j] = k->P[j][c] = 0.0;
    k->P[c][c] = prior_var;
    k->sum_xy[c] = k->sum_x[c] = 0.0;
    for (j = 0; j < MAXC; ++j) k->sum_xx[c][j] = k->sum_xx[j][c] = 0.0;
    if (c >= k->p) k->p = c + 1;
}

/* 预测步：加漂移，并把方差封顶在先验方差（未用到的列不会无限膨胀） */
static void kf_predict(evobeing_behavior_kf_t *k, const double *q, const double *cap)
{
    int i, j;
    for (i = 0; i < k->p; ++i) {
        k->P[i][i] += q[i];
        if (k->P[i][i] > cap[i] && k->P[i][i] > 0.0) {
            double f = sqrt(cap[i] / k->P[i][i]);
            for (j = 0; j < k->p; ++j) { k->P[i][j] *= f; k->P[j][i] *= f; }
            k->P[i][i] = cap[i];
        }
    }
}

/* 更新步。返回新息（一步预测误差）。 */
static double kf_update(evobeing_behavior_kf_t *k, const double *x, double y, double rho)
{
    double Px[MAXC], xPx = 0.0, yhat = 0.0, s, innov;
    int i, j, p = k->p;
    for (i = 0; i < p; ++i) {
        double a = 0.0;
        for (j = 0; j < p; ++j) a += k->P[i][j] * x[j];
        Px[i] = a;
        xPx += x[i] * a;
        yhat += x[i] * k->m[i];
    }
    innov = y - yhat;
    s = xPx + k->sigma2;
    if (s > 1e-12) {
        for (i = 0; i < p; ++i) k->m[i] += Px[i] * innov / s;
        for (i = 0; i < p; ++i)
            for (j = 0; j < p; ++j) k->P[i][j] -= Px[i] * Px[j] / s;
        for (i = 0; i < p; ++i)
            for (j = i + 1; j < p; ++j) { double v = 0.5 * (k->P[i][j] + k->P[j][i]); k->P[i][j] = k->P[j][i] = v; }
        for (i = 0; i < p; ++i) if (k->P[i][i] < 1e-12) k->P[i][i] = 1e-12;
    }
    /* 观测噪声在线估计：E[innov²] = xPx + σ² */
    {
        double est = innov * innov - xPx;
        if (est < 1.0) est = 1.0;
        k->sigma2 += 0.05 * (est - k->sigma2);
        if (k->sigma2 < 1.0) k->sigma2 = 1.0;
    }
    /* 一步预测统计与近期加权二阶矩 */
    k->se_model = rho * k->se_model + innov * innov;
    k->se_naive = rho * k->se_naive + y * y;
    k->se_w = rho * k->se_w + 1.0;
    k->sum_yy = rho * k->sum_yy + y * y;
    k->sum_w = rho * k->sum_w + 1.0;
    for (i = 0; i < p; ++i) {
        k->sum_xy[i] = rho * k->sum_xy[i] + x[i] * y;
        k->sum_x[i] = rho * k->sum_x[i] + x[i];
        for (j = 0; j < p; ++j) k->sum_xx[i][j] = rho * k->sum_xx[i][j] + x[i] * x[j];
    }
    /* 这里 sum_xx 对所有列都做了衰减（包括 i≥p 的空列，它们本来就是 0） */
    k->var_y = k->sum_w > 0.0 ? k->sum_yy / k->sum_w : 0.0;
    return innov;
}

/* 用系数 b 算近期加权 R²（ỹ 已近似零均值） */
static double kf_r2(const evobeing_behavior_kf_t *k, const double *b)
{
    double sse, bxy = 0.0, bxxb = 0.0;
    int i, j;
    if (k->sum_yy <= 1e-12) return 0.0;
    for (i = 0; i < k->p; ++i) {
        bxy += b[i] * k->sum_xy[i];
        for (j = 0; j < k->p; ++j) bxxb += b[i] * k->sum_xx[i][j] * b[j];
    }
    sse = k->sum_yy - 2.0 * bxy + bxxb;
    return 1.0 - sse / k->sum_yy;
}

/* ------------------------------------------------------------------ */
/* 状态                                                                 */
/* ------------------------------------------------------------------ */
void evobeing_behavior_state_init_zhenghuimin(evobeing_behavior_state_t *st, const evobeing_behavior_config_t *cfg)
{
    int i, t;
    memset(st, 0, sizeof *st);
    st->magic = STATE_MAGIC;
    st->version = STATE_VERSION;
    if (cfg) st->cfg = *cfg; else evobeing_behavior_config_default(&st->cfg);
    sanitize_cfg(&st->cfg);
    st->col_level = st->col_weekend = st->col_prev = st->col_logged = -1;
    for (t = 0; t < MAXT; ++t) st->col_main[t] = st->col_late[t] = st->col_lag[t] = -1;
    for (i = 0; i <= MAXCH; ++i) kf_init(&st->kf[i], st->cfg.sigma_init);
    st->target_is_d_init = -1;   /* 第一天决定 */
}

static int state_ok(const evobeing_behavior_state_t *st)
{
    return st && st->magic == STATE_MAGIC && st->version == STATE_VERSION && st->p >= 0 && st->p <= MAXC;
}

/* 分配一列（所有滤波器同步）。返回列号，满了返回 -1。 */
static int alloc_col(evobeing_behavior_state_t *st, int kind, int id)
{
    int c = st->p, i;
    double pv;
    if (c >= MAXC) return -1;
    pv = kind == KIND_CTRL ? st->cfg.prior_sd_control * st->cfg.prior_sd_control
                           : st->cfg.prior_sd_effect * st->cfg.prior_sd_effect;
    for (i = 0; i <= MAXCH; ++i) kf_add_col(&st->kf[i], c, pv);
    st->col_kind[c] = kind;
    st->col_id[c] = id;
    st->p = c + 1;
    return c;
}

/* 慢基线：返回去基线后的值并更新基线（基线只用之前的天） */
static double slow_baseline(double *base, int *n_base, double slow_days, double r)
{
    double out, alpha;
    if (*n_base == 0) { *base = r; *n_base = 1; return 0.0; }
    out = r - *base;
    alpha = 1.0 / (double)(*n_base + 1);
    if (alpha < 1.0 / slow_days) alpha = 1.0 / slow_days;
    *base += alpha * (r - *base);
    ++*n_base;
    return out;
}

int evobeing_behavior_update_zhenghuimin(evobeing_behavior_state_t *st, const evobeing_behavior_day_t *day)
{
    const evobeing_behavior_config_t *cfg;
    double x[MAXC], q[MAXC], cap[MAXC], y_raw, ytil, rho;
    int t, i, k, logged, known[MAXT];
    unsigned char xocc[MAXT];

    if (!state_ok(st) || !day) return -1;
    cfg = &st->cfg;
    rho = pow(0.5, 1.0 / cfg->report_half_life_days);
    st->n_days++;

    /* 百分比分母 */
    if (finite_d(day->wake_score) && day->wake_score >= 0.0 && day->wake_score <= 100.0) {
        st->w_score = rho * st->w_score + 1.0;
        st->mean_score += (day->wake_score - st->mean_score) / st->w_score;
    }

    /* 目标量 */
    if (st->target_is_d_init < 0) {
        if (finite_d(day->wake_d_init)) st->target_is_d_init = 1;
        else if (finite_d(day->wake_score)) st->target_is_d_init = 0;
    }
    y_raw = NAN_V;
    if (st->target_is_d_init == 1 && finite_d(day->wake_d_init)) y_raw = 100.0 * day->wake_d_init;
    else if (st->target_is_d_init == 0 && finite_d(day->wake_score) && day->wake_score >= 0.0 && day->wake_score <= 100.0)
        y_raw = day->wake_score;

    logged = day->logged;
    for (t = 0; t < MAXT; ++t) {
        int manual = (cfg->manual_mask >> (unsigned)t) & 1u;
        xocc[t] = day->occ[t] ? 1 : 0;
        known[t] = !(manual && logged == 0 && !xocc[t]);
    }

    if (!finite_d(y_raw)) {
        /* 这个早晨没有值：不更新模型，但"上一晚"信息要带到下一次 */
        for (t = 0; t < MAXT; ++t) { st->prev_occ[t] = xocc[t]; st->prev_known[t] = known[t]; }
        st->kf[0].prev_valid = 0;
        for (k = 0; k < MAXCH; ++k) st->kf[1 + k].prev_valid = 0;
        return 1;
    }
    st->n_valid++;

    /* 去习惯吸收 + 慢基线 */
    if (st->target_is_d_init == 1 && cfg->habit_lambda > 0.0) {
        double r = y_raw + cfg->habit_lambda * st->kf[0].habit_cum;
        st->kf[0].habit_cum += y_raw;
        y_raw = r;
    }
    ytil = slow_baseline(&st->kf[0].base, &st->kf[0].n_base, cfg->slow_baseline_days, y_raw);

    /* 列分配 */
    if (st->col_level < 0) st->col_level = alloc_col(st, KIND_CTRL, -1);
    if (cfg->control_weekend && day->weekday >= 0 && st->col_weekend < 0) st->col_weekend = alloc_col(st, KIND_CTRL, -2);
    if (cfg->control_prev && st->kf[0].prev_valid && st->col_prev < 0) st->col_prev = alloc_col(st, KIND_CTRL, -3);
    if (logged >= 0 && st->col_logged < 0) st->col_logged = alloc_col(st, KIND_CTRL, -4);
    for (t = 0; t < MAXT; ++t) {
        if (xocc[t] && st->col_main[t] < 0) { st->col_main[t] = alloc_col(st, KIND_MAIN, t); st->has_type[t] = 1; }
        if (xocc[t] && day->late[t] && finite_d(late_hour_of(cfg, t)) && st->col_late[t] < 0)
            st->col_late[t] = alloc_col(st, KIND_LATE, t);
        if (st->prev_occ[t] && wants_lag(cfg, t) && st->col_lag[t] < 0)
            st->col_lag[t] = alloc_col(st, KIND_LAG, t);
    }

    /* 设计向量 */
    for (i = 0; i < MAXC; ++i) x[i] = 0.0;
    if (st->col_level >= 0) x[st->col_level] = 1.0;
    if (st->col_weekend >= 0 && day->weekday >= 0) x[st->col_weekend] = (day->weekday == 5 || day->weekday == 6) ? 1.0 : 0.0;
    if (st->col_prev >= 0 && st->kf[0].prev_valid) x[st->col_prev] = st->kf[0].prev_resid / sqrt(st->kf[0].sigma2);
    if (st->col_logged >= 0) x[st->col_logged] = logged == 1 ? 1.0 : 0.0;
    for (t = 0; t < MAXT; ++t) {
        double xm;
        if (known[t]) {
            xm = xocc[t];
            /* 所有行为都计数：首次出现之前的日子同样是"没有"的对照 */
            if (xocc[t]) st->n_with[t]++; else st->n_without[t]++;
            st->rate[t] += (xm - st->rate[t]) / (double)(++st->n_rate[t]);
        } else {
            xm = st->n_rate[t] > 0 ? st->rate[t] : 0.0;   /* 未知日：用已知出现率插补 */
        }
        if (st->col_main[t] >= 0) x[st->col_main[t]] = xm;
        if (st->col_late[t] >= 0) {
            x[st->col_late[t]] = (xocc[t] && day->late[t]) ? 1.0 : 0.0;
            if (xocc[t] && day->late[t]) st->n_late_with[t]++;
        }
        if (st->col_lag[t] >= 0) {
            double xl = st->prev_known[t] ? (double)st->prev_occ[t] : (st->n_rate[t] > 0 ? st->rate[t] : 0.0);
            x[st->col_lag[t]] = xl;
            if (st->prev_occ[t]) st->n_lag_with[t]++;
        }
    }

    /* 漂移与封顶 */
    for (i = 0; i < st->p; ++i) {
        if (st->col_kind[i] == KIND_CTRL) {
            cap[i] = cfg->prior_sd_control * cfg->prior_sd_control;
            q[i] = (i == st->col_level) ? cfg->level_drift_sd * cfg->level_drift_sd
                                        : cfg->effect_drift_sd * cfg->effect_drift_sd;
        } else {
            cap[i] = cfg->prior_sd_effect * cfg->prior_sd_effect;
            q[i] = cfg->effect_drift_sd * cfg->effect_drift_sd;
        }
    }

    /* 主滤波器 */
    kf_predict(&st->kf[0], q, cap);
    kf_update(&st->kf[0], x, ytil, rho);
    st->kf[0].prev_resid = ytil;
    st->kf[0].prev_valid = 1;

    /* 通道滤波器：同一设计向量，只换目标与"前一天"列 */
    for (k = 0; k < st->n_channels && k < MAXCH; ++k) {
        evobeing_behavior_kf_t *kf = &st->kf[1 + k];
        double c = day->channel[k], ct, xk[MAXC];
        if (!finite_d(c)) { kf->prev_valid = 0; continue; }
        ct = slow_baseline(&kf->base, &kf->n_base, cfg->slow_baseline_days, c);
        /* 在线标准化到"10 分 = 1 个通道标准差"，这样噪声下限、先验、漂移都能沿用主滤波器的刻度 */
        if (kf->n_base <= 1) continue;
        kf->chan_var += (ct * ct - kf->chan_var) / (double)(++kf->chan_n < 30 ? kf->chan_n : 30);
        if (kf->chan_n < 5 || kf->chan_var <= 1e-12) { kf->prev_valid = 0; continue; }
        ct = 10.0 * ct / sqrt(kf->chan_var);
        memcpy(xk, x, sizeof xk);
        if (st->col_prev >= 0) xk[st->col_prev] = kf->prev_valid ? kf->prev_resid / sqrt(kf->sigma2) : 0.0;
        kf_predict(kf, q, cap);
        kf_update(kf, xk, ct, rho);
        kf->prev_resid = ct;
        kf->prev_valid = 1;
    }

    for (t = 0; t < MAXT; ++t) { st->prev_occ[t] = xocc[t]; st->prev_known[t] = known[t]; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 报告                                                                 */
/* ------------------------------------------------------------------ */
typedef struct { int t, kind, col_a, col_b; } variant_t;   /* 效应 = m[col_a] (+ m[col_b]) */

/* 列出某状态里可报告的变体（含 INSUFFICIENT 的） */
static int list_variants(const evobeing_behavior_state_t *st, variant_t *v, int *insufficient)
{
    int t, n = 0;
    const evobeing_behavior_config_t *cfg = &st->cfg;
    for (t = 0; t < MAXT; ++t) {
        int all_late;
        if (!st->has_type[t]) continue;
        all_late = st->col_late[t] >= 0 && st->n_late_with[t] >= st->n_with[t];
        /* 主效应 */
        v[n].t = t; v[n].kind = KIND_MAIN; v[n].col_a = st->col_main[t];
        v[n].col_b = all_late ? st->col_late[t] : -1;   /* 全部都在晚间：主列与晚间列重合，效应是两者之和 */
        insufficient[n] = !(st->n_with[t] >= cfg->min_with && st->n_without[t] >= 1 && st->n_without[t] >= cfg->min_without);
        ++n;
        /* 晚间 */
        if (st->col_late[t] >= 0) {
            v[n].t = t; v[n].kind = KIND_LATE; v[n].col_a = st->col_late[t]; v[n].col_b = st->col_main[t];
            insufficient[n] = !(st->n_late_with[t] >= cfg->min_with && st->n_late_with[t] < st->n_with[t]);
            ++n;
        }
        /* 第二晚 */
        if (st->col_lag[t] >= 0) {
            v[n].t = t; v[n].kind = KIND_LAG; v[n].col_a = st->col_lag[t]; v[n].col_b = -1;
            insufficient[n] = !(st->n_lag_with[t] >= cfg->min_with && st->n_lag_with[t] < st->n_valid);
            ++n;
        }
    }
    return n;
}

static double var_eff(const evobeing_behavior_kf_t *kf, const variant_t *v, double *mean)
{
    double m = kf->m[v->col_a], var = kf->P[v->col_a][v->col_a];
    if (v->col_b >= 0) {
        m += kf->m[v->col_b];
        var += kf->P[v->col_b][v->col_b] + 2.0 * kf->P[v->col_a][v->col_b];
    }
    if (var < 1e-12) var = 1e-12;
    *mean = m;
    return var;
}

static void sort_items(evobeing_behavior_output_t *out)
{
    int i;
    for (i = 1; i < out->n_behaviors; ++i) {
        evobeing_behavior_item_t key = out->items[i];
        int j = i - 1;
        double kv = key.status == EVOBEING_BEHAVIOR_INSUFFICIENT ? -1.0 : fabs(key.effect_pct);
        while (j >= 0) {
            double jv = out->items[j].status == EVOBEING_BEHAVIOR_INSUFFICIENT ? -1.0 : fabs(out->items[j].effect_pct);
            if (jv >= kv) break;
            out->items[j + 1] = out->items[j];
            --j;
        }
        out->items[j + 1] = key;
    }
}

/* 档位：z 门槛 + BH 多项比较（只在有数字的项之间） */
static void assign_status(evobeing_behavior_output_t *out, const evobeing_behavior_config_t *cfg, const int *nw)
{
    double pv[MAXT * 3];
    int idx[MAXT * 3], n = 0, i, j, k_max = -1;
    for (i = 0; i < out->n_behaviors; ++i) {
        evobeing_behavior_item_t *it = &out->items[i];
        if (it->status == EVOBEING_BEHAVIOR_INSUFFICIENT) continue;
        pv[n] = 2.0 * (1.0 - norm_cdf(fabs(it->z)));
        idx[n] = i; ++n;
    }
    /* 按 p 升序（插入排序） */
    for (i = 1; i < n; ++i) {
        double pk = pv[i]; int ik = idx[i];
        j = i - 1;
        while (j >= 0 && pv[j] > pk) { pv[j + 1] = pv[j]; idx[j + 1] = idx[j]; --j; }
        pv[j + 1] = pk; idx[j + 1] = ik;
    }
    for (i = 0; i < n; ++i)
        if (pv[i] <= cfg->fdr_q * (double)(i + 1) / (double)n) k_max = i;
    for (i = 0; i < n; ++i) {
        evobeing_behavior_item_t *it = &out->items[idx[i]];
        double az = fabs(it->z);
        int ci_excl0 = !(it->ci_lo_pct == it->ci_lo_pct) || (it->ci_lo_pct > 0.0 || it->ci_hi_pct < 0.0);
        if (i <= k_max && az >= cfg->z_high && nw[idx[i]] >= 6 && ci_excl0) it->status = EVOBEING_BEHAVIOR_HIGH;
        else if (az >= cfg->z_mid) it->status = EVOBEING_BEHAVIOR_MID;
        else it->status = EVOBEING_BEHAVIOR_LOW;
    }
}

/* 共同的报告生成；boot_sd / boot_lo / boot_hi 可为 NULL（按变体序号索引，单位：分） */
static int report_impl(const evobeing_behavior_state_t *st, evobeing_behavior_output_t *out,
                       const double *boot_sd, const double *boot_lo, const double *boot_hi)
{
    const evobeing_behavior_config_t *cfg;
    const evobeing_behavior_kf_t *kf;
    variant_t v[MAXT * 3];
    int insuff[MAXT * 3], nv, i, k, nw_of[MAXT * 3];
    double b_full[MAXC], b_zero[MAXC], r2_full, r2_ctrl, share_sum = 0.0, cap;

    if (!state_ok(st) || !out) return -1;
    cfg = &st->cfg;
    kf = &st->kf[0];
    memset(out, 0, sizeof *out);
    out->n_days_used = st->n_valid;
    out->target_is_d_init = st->target_is_d_init == 1;
    out->mean_score = st->w_score > 0.0 ? st->mean_score : NAN_V;
    out->sd_score = sqrt(kf->var_y);
    out->sigma = sqrt(kf->sigma2);
    out->n_channels = st->n_channels;
    for (k = 0; k < st->n_channels; ++k) memcpy(out->channel_name[k], st->channel_name[k], sizeof out->channel_name[k]);
    if (kf->se_w > 0.0) {
        out->pred_rmse = sqrt(kf->se_model / kf->se_w);
        out->naive_rmse = sqrt(kf->se_naive / kf->se_w);
        out->pred_skill_pct = kf->se_naive > 1e-12 ? 100.0 * (1.0 - kf->se_model / kf->se_naive) : 0.0;
    } else out->pred_rmse = out->naive_rmse = out->pred_skill_pct = NAN_V;

    nv = list_variants(st, v, insuff);
    if (st->n_valid < 7) {
        for (i = 0; i < nv; ++i) {
            evobeing_behavior_item_t *it = &out->items[i];
            it->id = v[i].t; it->is_late_variant = v[i].kind == KIND_LATE; it->lag_nights = v[i].kind == KIND_LAG ? 2 : 1;
            item_name(cfg, v[i].t, v[i].kind, it->name, sizeof it->name);
            it->n_with = v[i].kind == KIND_MAIN ? st->n_with[v[i].t] : v[i].kind == KIND_LATE ? st->n_late_with[v[i].t] : st->n_lag_with[v[i].t];
            it->n_without = v[i].kind == KIND_MAIN ? st->n_without[v[i].t] : st->n_valid - it->n_with;
            it->effect_pct = it->effect_points = it->raw_pct = it->ci_lo_pct = it->ci_hi_pct = it->sd_points = it->z = NAN_V;
            it->prob_dir = NAN_V;
            for (k = 0; k < MAXCH; ++k) it->channel_effect_sd[k] = it->channel_z[k] = NAN_V;
            it->status = EVOBEING_BEHAVIOR_INSUFFICIENT;
        }
        out->n_behaviors = nv; out->n_insufficient = nv;
        out->unexplained_pct = 100.0;
        return -2;
    }

    /* 口径 B 的基准 */
    memcpy(b_full, kf->m, sizeof b_full);
    r2_full = kf_r2(kf, b_full);
    memcpy(b_zero, kf->m, sizeof b_zero);
    for (i = 0; i < st->p; ++i) if (st->col_kind[i] != KIND_CTRL) b_zero[i] = 0.0;
    r2_ctrl = kf_r2(kf, b_zero);
    cap = 100.0 * (r2_full - r2_ctrl);
    if (cap < 0.0) cap = 0.0;
    if (cap > 100.0) cap = 100.0;

    for (i = 0; i < nv; ++i) {
        evobeing_behavior_item_t *it = &out->items[i];
        double m, var, sd;
        int t = v[i].t;
        it->id = t; it->is_late_variant = v[i].kind == KIND_LATE; it->lag_nights = v[i].kind == KIND_LAG ? 2 : 1;
        item_name(cfg, t, v[i].kind, it->name, sizeof it->name);
        it->n_with = v[i].kind == KIND_MAIN ? st->n_with[t] : v[i].kind == KIND_LATE ? st->n_late_with[t] : st->n_lag_with[t];
        it->n_without = v[i].kind == KIND_MAIN ? st->n_without[t] : st->n_valid - it->n_with;
        nw_of[i] = it->n_with;
        it->raw_pct = NAN_V;
        it->effect_pct = it->effect_points = it->ci_lo_pct = it->ci_hi_pct = it->sd_points = it->z = it->prob_dir = NAN_V;
        for (k = 0; k < MAXCH; ++k) it->channel_effect_sd[k] = it->channel_z[k] = NAN_V;
        it->status = EVOBEING_BEHAVIOR_INSUFFICIENT;
        if (insuff[i]) continue;

        var = var_eff(kf, &v[i], &m);
        sd = sqrt(var);
        if (boot_sd && finite_d(boot_sd[i]) && boot_sd[i] > sd) sd = boot_sd[i];
        it->effect_points = m;
        it->sd_points = sd;
        it->z = m / sd;
        it->prob_dir = norm_cdf(fabs(it->z));
        it->effect_pct = (finite_d(out->mean_score) && out->mean_score > 0.0) ? 100.0 * m / out->mean_score : NAN_V;
        {
            double lo = m - 1.645 * sd, hi = m + 1.645 * sd;
            if (boot_lo && finite_d(boot_lo[i]) && boot_lo[i] < lo) lo = boot_lo[i];
            if (boot_hi && finite_d(boot_hi[i]) && boot_hi[i] > hi) hi = boot_hi[i];
            if (finite_d(out->mean_score) && out->mean_score > 0.0) {
                it->ci_lo_pct = 100.0 * lo / out->mean_score;
                it->ci_hi_pct = 100.0 * hi / out->mean_score;
            }
        }
        /* 口径 B：把该变体涉及的列置 0 */
        {
            double b2[MAXC], r2d, sh;
            memcpy(b2, b_full, sizeof b2);
            b2[v[i].col_a] = 0.0;
            if (v[i].kind == KIND_MAIN && v[i].col_b >= 0) b2[v[i].col_b] = 0.0;
            r2d = kf_r2(kf, b2);
            sh = r2_full - r2d;
            it->share_pct = sh > 0.0 ? 100.0 * sh : 0.0;
            share_sum += it->share_pct;
        }
        /* 通道 */
        for (k = 0; k < st->n_channels && k < MAXCH; ++k) {
            const evobeing_behavior_kf_t *ck = &st->kf[1 + k];
            double cm, cv, csd;
            if (ck->chan_n < 7) continue;
            cv = var_eff(ck, &v[i], &cm);
            csd = 10.0;   /* 通道已标准化：10 分 = 1 个标准差 */
            it->channel_effect_sd[k] = cm / csd;
            it->channel_z[k] = cm / sqrt(cv);
        }
        it->status = EVOBEING_BEHAVIOR_LOW;   /* 先占位，下面统一定档 */
    }
    out->n_behaviors = nv;
    assign_status(out, cfg, nw_of);

    if (share_sum > cap && share_sum > 0.0) {
        for (i = 0; i < nv; ++i) out->items[i].share_pct *= cap / share_sum;
        share_sum = cap;
    }
    out->explained_pct = share_sum;
    out->unexplained_pct = 100.0 - share_sum;
    for (i = 0; i < nv; ++i) if (out->items[i].status == EVOBEING_BEHAVIOR_INSUFFICIENT) ++out->n_insufficient;
    sort_items(out);
    return 0;
}

int evobeing_behavior_report_zhenghuimin(const evobeing_behavior_state_t *st, evobeing_behavior_output_t *out)
{
    return report_impl(st, out, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* 事件折叠                                                             */
/* ------------------------------------------------------------------ */
int evobeing_behavior_fold_events_zhenghuimin(const evobeing_behavior_event_t *events, int n_events,
                                              const double *wake_t_h, int n_days,
                                              const evobeing_behavior_config_t *cfg,
                                              evobeing_behavior_day_t *days)
{
    evobeing_behavior_config_t def;
    int k, d, used = 0;
    if (!days || n_days <= 0 || (n_events > 0 && !events)) return 0;
    if (!cfg) { evobeing_behavior_config_default(&def); cfg = &def; }
    for (d = 0; d < n_days; ++d) { memset(days[d].occ, 0, sizeof days[d].occ); memset(days[d].late, 0, sizeof days[d].late); }
    for (k = 0; k < n_events; ++k) {
        const evobeing_behavior_event_t *e = &events[k];
        double th = e->t_h, tod, lh;
        if (e->id < 0 || e->id >= MAXT || !finite_d(th)) continue;
        for (d = 0; d < n_days; ++d) {
            double wk = wake_t_h ? wake_t_h[d] : (double)d * 24.0 + 7.0;
            if (finite_d(wk) && wk > th) break;
        }
        if (d >= n_days) continue;
        {
            double w0 = wake_t_h ? wake_t_h[0] : 7.0;
            if (d == 0 && finite_d(w0) && th < w0 - 24.0) continue;
        }
        days[d].occ[e->id] = 1;
        tod = th - 24.0 * floor(th / 24.0);
        lh = late_hour_of(cfg, e->id);
        if (finite_d(lh) && tod >= lh) days[d].late[e->id] = 1;
        ++used;
    }
    return used;
}

/* ------------------------------------------------------------------ */
/* 批量                                                                 */
/* ------------------------------------------------------------------ */
static void fill_day(evobeing_behavior_day_t *dy, const evobeing_behavior_input_t *in, int d)
{
    int k;
    dy->wake_score = in->wake_score[d];
    dy->wake_d_init = in->wake_d_init ? in->wake_d_init[d] : NAN_V;
    dy->weekday = in->weekday ? in->weekday[d] : -1;
    dy->logged = in->logged ? in->logged[d] : -1;
    for (k = 0; k < MAXCH; ++k)
        dy->channel[k] = (k < in->n_channels && in->channel[k]) ? in->channel[k][d] : NAN_V;
    /* 没起床记录的天：当天没有晨起值 */
    if (in->wake_t_h && !finite_d(in->wake_t_h[d])) dy->wake_score = dy->wake_d_init = NAN_V;
}

static void run_days(evobeing_behavior_state_t *st, const evobeing_behavior_config_t *cfg,
                     const evobeing_behavior_input_t *in, const evobeing_behavior_day_t *days, const int *order, int n)
{
    int i, k;
    evobeing_behavior_state_init_zhenghuimin(st, cfg);
    st->n_channels = in->n_channels > MAXCH ? MAXCH : (in->n_channels < 0 ? 0 : in->n_channels);
    for (k = 0; k < st->n_channels; ++k) {
        if (in->channel_name[k]) snprintf(st->channel_name[k], sizeof st->channel_name[k], "%s", in->channel_name[k]);
        else snprintf(st->channel_name[k], sizeof st->channel_name[k], "channel_%d", k);
    }
    for (i = 0; i < n; ++i) evobeing_behavior_update_zhenghuimin(st, &days[order ? order[i] : i]);
}

int evobeing_behavior_impact_zhenghuimin(const evobeing_behavior_input_t *in,
                                         const evobeing_behavior_config_t *cfg_in,
                                         evobeing_behavior_output_t *out)
{
    static evobeing_behavior_state_t st, stb;
    static evobeing_behavior_day_t days[MAXD];
    static double boot_eff[MAXT * 3][MAXBOOT];
    evobeing_behavior_config_t cfg;
    int n, d, i, rc, nv, nb_done = 0;
    variant_t v[MAXT * 3];
    int insuff[MAXT * 3];
    double boot_sd[MAXT * 3], boot_lo[MAXT * 3], boot_hi[MAXT * 3];

    if (!in || !out || in->n_days <= 0 || in->n_days > MAXD || !in->wake_score ||
        (in->n_events > 0 && !in->events) || in->n_events > EVOBEING_BEHAVIOR_MAX_EVENTS)
        return -1;
    if (cfg_in) cfg = *cfg_in; else evobeing_behavior_config_default(&cfg);
    sanitize_cfg(&cfg);
    n = in->n_days;

    /* 1. 事件 → 每天 */
    evobeing_behavior_fold_events_zhenghuimin(in->events, in->n_events, in->wake_t_h, n, &cfg, days);
    for (d = 0; d < n; ++d) fill_day(&days[d], in, d);

    /* 2. 顺序跑一遍 */
    run_days(&st, &cfg, in, days, NULL, n);
    nv = list_variants(&st, v, insuff);
    for (i = 0; i < nv; ++i) boot_sd[i] = boot_lo[i] = boot_hi[i] = NAN_V;

    /* 3. 块自助法：按块重排天序，重新跑滤波，取最终效应的分布 */
    if (cfg.n_boot > 0 && st.n_valid >= 7 && nv > 0) {
        unsigned s = cfg.seed ? cfg.seed : 12345u;
        int order[MAXD], r, nblk = cfg.block_days;
        if (nblk > n) nblk = n;
        for (r = 0; r < cfg.n_boot; ++r) {
            int pos = 0;
            while (pos < n) {
                int startb = (int)(xorshift(&s) % (unsigned)(n - nblk + 1)), j;
                for (j = 0; j < nblk && pos < n; ++j) order[pos++] = startb + j;
            }
            run_days(&stb, &cfg, in, days, order, n);
            for (i = 0; i < nv; ++i) {
                /* 在自助状态里找同一 (t, kind) 的列 */
                int t = v[i].t, ca = -1, cb = -1;
                double m;
                variant_t vb;
                if (v[i].kind == KIND_MAIN) { ca = stb.col_main[t]; cb = (stb.col_late[t] >= 0 && stb.n_late_with[t] >= stb.n_with[t]) ? stb.col_late[t] : -1; }
                else if (v[i].kind == KIND_LATE) { ca = stb.col_late[t]; cb = stb.col_main[t]; }
                else { ca = stb.col_lag[t]; }
                if (ca < 0) { boot_eff[i][nb_done] = NAN_V; continue; }
                vb.t = t; vb.kind = v[i].kind; vb.col_a = ca; vb.col_b = cb;
                var_eff(&stb.kf[0], &vb, &m);
                boot_eff[i][nb_done] = m;
            }
            ++nb_done;
        }
        for (i = 0; i < nv; ++i) {
            double tmp[MAXBOOT], mean = 0.0, var = 0.0;
            int k, cnt = 0;
            for (k = 0; k < nb_done; ++k) if (finite_d(boot_eff[i][k])) tmp[cnt++] = boot_eff[i][k];
            if (cnt < 20) continue;
            for (k = 0; k < cnt; ++k) mean += tmp[k];
            mean /= cnt;
            for (k = 0; k < cnt; ++k) var += (tmp[k] - mean) * (tmp[k] - mean);
            var /= (cnt - 1);
            qsort(tmp, (size_t)cnt, sizeof(double), cmp_double);
            boot_sd[i] = sqrt(var);
            boot_lo[i] = tmp[(int)floor(0.05 * (cnt - 1))];
            boot_hi[i] = tmp[(int)ceil(0.95 * (cnt - 1))];
        }
    }

    /* 4. 报告（变体顺序与 list_variants 一致） */
    rc = report_impl(&st, out, nb_done > 0 ? boot_sd : NULL, nb_done > 0 ? boot_lo : NULL, nb_done > 0 ? boot_hi : NULL);

    /* 5. 简单对比 raw_pct（近期加权，有 vs 无） */
    if (rc == 0 && finite_d(out->mean_score) && out->mean_score > 0.0) {
        double rho = pow(0.5, 1.0 / cfg.report_half_life_days);
        for (i = 0; i < out->n_behaviors; ++i) {
            evobeing_behavior_item_t *it = &out->items[i];
            double ws = 0.0, wn = 0.0, ss = 0.0, sn = 0.0;
            int t = it->id;
            for (d = 0; d < n; ++d) {
                double y = in->wake_d_init ? (finite_d(in->wake_d_init[d]) ? 100.0 * in->wake_d_init[d] : NAN_V) : in->wake_score[d];
                double w = pow(rho, (double)(n - 1 - d));
                int on;
                if (!finite_d(y)) continue;
                if (in->wake_t_h && !finite_d(in->wake_t_h[d])) continue;
                if (it->lag_nights == 2) on = d > 0 && days[d - 1].occ[t];
                else if (it->is_late_variant) on = days[d].occ[t] && days[d].late[t];
                else on = days[d].occ[t];
                if (on) { ws += w; ss += w * y; } else { wn += w; sn += w * y; }
            }
            it->raw_pct = (ws > 0.0 && wn > 0.0) ? 100.0 * (ss / ws - sn / wn) / out->mean_score : NAN_V;
        }
    }
    return rc;
}
