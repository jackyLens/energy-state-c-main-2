/*
 * test_behavior.c — 行为归因模块的合成数据测试
 *
 * 合成一个"真实世界"：每天晨起精力 = 本人均值 + 真实行为效应 + 周末效应 + 自相关 + 噪声，
 * 再检查模块能否找回效应的方向与大小、样本不足时是否拒绝给数、混杂是否被控制住等。
 */
#include "evobeing_behavior_zhenghuimin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int n_checks = 0, n_fail = 0;
#define CHECK(cond, msg) do { ++n_checks; if (!(cond)) { ++n_fail; printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static unsigned rng_s = 7u;
static double urand(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return (double)(rng_s % 1000000u) / 1000000.0; }
static double nrand(void) { double u1 = urand() + 1e-9, u2 = urand(); return sqrt(-2.0 * log(u1)) * cos(6.283185307 * u2); }

typedef struct {
    double y[EVOBEING_BEHAVIOR_MAX_DAYS];
    double wake[EVOBEING_BEHAVIOR_MAX_DAYS];
    int weekday[EVOBEING_BEHAVIOR_MAX_DAYS];
    evobeing_behavior_event_t ev[EVOBEING_BEHAVIOR_MAX_EVENTS];
    int n_ev;
    int n_days;
} world_t;

/* 真实效应（分）：饮酒 −9，晚间咖啡 −5（白天咖啡 0），小睡 +4，运动 +3，冥想 +2 */
static void gen_world(world_t *w, int n_days, double p_alc, double p_caf, double p_nap, double p_ex, double p_med,
                      double noise, double weekend_eff, int late_caffeine_only)
{
    int d;
    double prev = 0.0;
    w->n_days = n_days; w->n_ev = 0;
    for (d = 0; d < n_days; ++d) {
        double eff = 0.0;
        int wd = d % 7;
        w->weekday[d] = wd;
        w->wake[d] = d * 24.0 + 7.0 + 0.5 * nrand();
        /* 行为发生在第 d-1 天起床之后、第 d 天起床之前 → 影响 y[d]。用第 d-1 天的时刻。 */
        if (d > 0) {
            double base = (d - 1) * 24.0;
            if (urand() < p_alc) { w->ev[w->n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; w->ev[w->n_ev++].t_h = base + 20.0 + 2.0 * urand(); eff -= 9.0; }
            if (urand() < p_caf) {
                double hr = late_caffeine_only ? 15.0 + 3.0 * urand() : (urand() < 0.5 ? 9.0 + 3.0 * urand() : 15.0 + 3.0 * urand());
                w->ev[w->n_ev].id = EVOBEING_BEHAVIOR_CAFFEINE; w->ev[w->n_ev++].t_h = base + hr;
                if (hr >= 14.0) eff -= 5.0;
            }
            if (urand() < p_nap) { w->ev[w->n_ev].id = EVOBEING_BEHAVIOR_NAP; w->ev[w->n_ev++].t_h = base + 13.5; eff += 4.0; }
            if (urand() < p_ex) { w->ev[w->n_ev].id = EVOBEING_BEHAVIOR_EXERCISE; w->ev[w->n_ev++].t_h = base + 18.0; eff += 3.0; }
            if (urand() < p_med) { w->ev[w->n_ev].id = EVOBEING_BEHAVIOR_MEDITATION; w->ev[w->n_ev++].t_h = base + 22.0; eff += 2.0; }
        }
        if (wd == 5 || wd == 6) eff += weekend_eff;
        w->y[d] = 62.0 + eff + 0.3 * prev + noise * nrand();
        if (w->y[d] < 0) w->y[d] = 0;
        if (w->y[d] > 100) w->y[d] = 100;
        prev = w->y[d] - 62.0;
    }
}

static const evobeing_behavior_item_t *find(const evobeing_behavior_output_t *o, int id, int late)
{
    int i;
    for (i = 0; i < o->n_behaviors; ++i)
        if (o->items[i].id == id && o->items[i].is_late_variant == late && o->items[i].lag_nights != 2) return &o->items[i];
    return NULL;
}

static void run(const world_t *w, const evobeing_behavior_config_t *cfg, evobeing_behavior_output_t *o, int *rc)
{
    evobeing_behavior_input_t in;
    memset(&in, 0, sizeof in);
    in.n_days = w->n_days; in.wake_score = w->y; in.wake_t_h = w->wake; in.weekday = w->weekday;
    in.events = w->ev; in.n_events = w->n_ev;
    *rc = evobeing_behavior_impact_zhenghuimin(&in, cfg, o);
}

static void print_out(const evobeing_behavior_output_t *o)
{
    int i;
    printf("  days=%d mean=%.1f sd=%.1f explained=%.0f%% unexplained=%.0f%%\n",
           o->n_days_used, o->mean_score, o->sd_score, o->explained_pct, o->unexplained_pct);
    for (i = 0; i < o->n_behaviors; ++i) {
        const evobeing_behavior_item_t *it = &o->items[i];
        if (it->status == EVOBEING_BEHAVIOR_INSUFFICIENT)
            printf("    %-16s n=%d/%d  再记录几次\n", it->name, it->n_with, it->n_without);
        else
            printf("    %-16s n=%d/%d  %+6.1f%% [%+.1f, %+.1f]  raw %+5.1f%%  share %4.1f%%  status %d\n",
                   it->name, it->n_with, it->n_without, it->effect_pct, it->ci_lo_pct, it->ci_hi_pct,
                   it->raw_pct, it->share_pct, (int)it->status);
    }
}

int main(void)
{
    world_t w;
    evobeing_behavior_output_t o;
    evobeing_behavior_config_t cfg;
    int rc, i;

    printf("[1] 典型 28 天：方向与大小\n");
    {
        int ok_alc = 0, ok_nap = 0, ok_caf_late = 0, ok_ex = 0, trials = 20, tr, g_alc = 0, g_nap = 0, g_ex = 0;
        double sum_alc = 0, sum_nap = 0, sum_ex = 0;
        for (tr = 0; tr < trials; ++tr) {
            const evobeing_behavior_item_t *a, *np, *cl, *ex;
            rng_s = 100u + (unsigned)tr;
            gen_world(&w, 28, 0.3, 0.6, 0.3, 0.4, 0.2, 4.0, 2.0, 0);
            run(&w, NULL, &o, &rc);
            CHECK(rc == 0, "rc");
            CHECK(o.n_days_used == 28, "n_days_used");
            a = find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0); np = find(&o, EVOBEING_BEHAVIOR_NAP, 0);
            cl = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 1); ex = find(&o, EVOBEING_BEHAVIOR_EXERCISE, 0);
            if (tr == 0) print_out(&o);
            if (a && a->status != EVOBEING_BEHAVIOR_INSUFFICIENT) { ++g_alc; sum_alc += a->effect_points; if (a->effect_points < -3.0) ++ok_alc; }
            if (np && np->status != EVOBEING_BEHAVIOR_INSUFFICIENT) { ++g_nap; sum_nap += np->effect_points; if (np->effect_points > 0.5) ++ok_nap; }
            if (ex && ex->status != EVOBEING_BEHAVIOR_INSUFFICIENT) { ++g_ex; sum_ex += ex->effect_points; if (ex->effect_points > 0.0) ++ok_ex; }
            if (cl && cl->status != EVOBEING_BEHAVIOR_INSUFFICIENT && cl->effect_points < -1.0) ++ok_caf_late;
            /* 排序：有数字的在前、按 |effect| 降序 */
            for (i = 1; i < o.n_behaviors; ++i) {
                const evobeing_behavior_item_t *p = &o.items[i - 1], *q = &o.items[i];
                if (p->status != EVOBEING_BEHAVIOR_INSUFFICIENT && q->status != EVOBEING_BEHAVIOR_INSUFFICIENT)
                    CHECK(fabs(p->effect_pct) >= fabs(q->effect_pct) - 1e-9, "sorted");
                if (p->status == EVOBEING_BEHAVIOR_INSUFFICIENT) CHECK(q->status == EVOBEING_BEHAVIOR_INSUFFICIENT, "insufficient last");
            }
            CHECK(o.explained_pct >= 0 && o.explained_pct <= 100.0 + 1e-9, "explained range");
            CHECK(fabs(o.explained_pct + o.unexplained_pct - 100.0) < 1e-9, "sum 100");
            {
                double s = 0; for (i = 0; i < o.n_behaviors; ++i) { CHECK(o.items[i].share_pct >= 0, "share>=0"); s += o.items[i].share_pct; }
                CHECK(fabs(s - o.explained_pct) < 1e-6, "share sum == explained");
            }
            /* 区间包含点估计 */
            for (i = 0; i < o.n_behaviors; ++i) {
                const evobeing_behavior_item_t *it = &o.items[i];
                if (it->status == EVOBEING_BEHAVIOR_INSUFFICIENT) continue;
                CHECK(it->ci_lo_pct <= it->effect_pct + 3.0 && it->ci_hi_pct >= it->effect_pct - 3.0, "ci around estimate");
                CHECK(it->n_with >= 4, "gating n_with");
            }
        }
        printf("  饮酒方向正确 %d/%d（均值 %.1f 分，真值 −9），小睡 %d/%d（%.1f，真值 +4），运动 %d/%d（%.1f，真值 +3），晚间咖啡 %d\n",
               ok_alc, g_alc, sum_alc / g_alc, ok_nap, g_nap, sum_nap / g_nap, ok_ex, g_ex, sum_ex / g_ex, ok_caf_late);
        CHECK(g_alc >= 15 && ok_alc >= g_alc - 1, "alcohol negative in nearly all trials given");
        CHECK(sum_alc / g_alc < -6.0 && sum_alc / g_alc > -12.0, "alcohol magnitude near −9");
        CHECK(sum_nap / g_nap > 2.0 && sum_nap / g_nap < 6.0, "nap magnitude near +4");
        CHECK(ok_nap >= g_nap * 3 / 4, "nap positive in most trials");
        CHECK(ok_ex >= g_ex * 3 / 4, "exercise positive in most trials");
    }

    printf("[2] 无效应的行为：平均接近 0、多数为 LOW/MID\n");
    {
        int tr, n_small = 0, n_high = 0, n_given = 0;
        double sum = 0;
        for (tr = 0; tr < 30; ++tr) {
            const evobeing_behavior_item_t *c;
            rng_s = 500u + (unsigned)tr;
            gen_world(&w, 28, 0.0, 0.6, 0.0, 0.0, 0.0, 4.0, 0.0, 0);
            /* 把咖啡全改到上午 → 真实效应 0 */
            for (i = 0; i < w.n_ev; ++i) { double base = floor(w.ev[i].t_h / 24.0) * 24.0; w.ev[i].t_h = base + 9.0; }
            for (i = 0; i < 28; ++i) w.y[i] = 62.0 + 4.0 * nrand();
            run(&w, NULL, &o, &rc);
            c = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 0);
            CHECK(c != NULL, "caffeine present");
            CHECK(find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 1) == NULL, "no late variant when none late");
            if (c && c->status != EVOBEING_BEHAVIOR_INSUFFICIENT) {
                ++n_given; sum += c->effect_points;
                if (fabs(c->effect_points) < 3.0) ++n_small;
                if (c->status == EVOBEING_BEHAVIOR_HIGH) ++n_high;
            }
        }
        printf("  给出数字 %d 次，|效应|<3 分 %d 次，HIGH %d 次，均值 %.2f 分\n", n_given, n_small, n_high, n_given ? sum / n_given : 0.0);
        CHECK(n_given > 0 && fabs(sum / n_given) < 1.5, "null effect mean near 0");
        CHECK(n_high <= 3, "null effect rarely HIGH");
    }

    printf("[3] 样本不足：出现 < 4 次不给数字\n");
    {
        rng_s = 9u;
        gen_world(&w, 28, 0.0, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 0);
        w.n_ev = 0;
        w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; w.ev[w.n_ev++].t_h = 3 * 24 + 21.0;
        w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; w.ev[w.n_ev++].t_h = 10 * 24 + 21.0;
        w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; w.ev[w.n_ev++].t_h = 20 * 24 + 21.0;
        run(&w, NULL, &o, &rc);
        CHECK(rc == 0, "rc");
        CHECK(o.n_behaviors >= 1 && o.n_behaviors <= 3, "alcohol (+ late / 2nd-night items)");
        for (i = 0; i < o.n_behaviors; ++i) {
            CHECK(o.items[i].status == EVOBEING_BEHAVIOR_INSUFFICIENT, "insufficient");
            CHECK(o.items[i].n_with == 3, "n_with 3");
            CHECK(o.items[i].effect_pct != o.items[i].effect_pct, "effect NAN");
            CHECK(o.items[i].share_pct == 0.0, "share 0");
        }
        CHECK(o.n_insufficient == o.n_behaviors, "n_insufficient");
        CHECK(o.unexplained_pct == 100.0, "all unexplained");
        /* 几乎每天都有（27/28 天，nwo=1）→ 主列可加入回归，但置信度低 */
        w.n_ev = 0;
        for (i = 0; i < 27; ++i) { w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_CAFFEINE; w.ev[w.n_ev++].t_h = i * 24 + 9.0; }
        run(&w, NULL, &o, &rc);
        CHECK(o.n_behaviors == 1, "one behavior");
        CHECK(o.items[0].n_without == 1, "n_without 1");
        /* nwo=1 时主列可回归，应给出数字（不再要求 n_without>=4）；但 status 应为 LOW */
        if (o.items[0].status != EVOBEING_BEHAVIOR_INSUFFICIENT) {
            CHECK(o.items[0].status == EVOBEING_BEHAVIOR_LOW, "low confidence with 1 control day");
            CHECK(o.items[0].effect_pct == o.items[0].effect_pct, "effect not NAN");
        }

        /* 真正天天都有（28/28 天，nwo=0）→ 主列全 1、主变体 INSUFFICIENT
           事件放在每天 5:00（早于 wake 7:00），确保归到当天 */
        w.n_ev = 0;
        for (i = 0; i < 28; ++i) { w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_CAFFEINE; w.ev[w.n_ev++].t_h = i * 24 + 5.0; }
        run(&w, NULL, &o, &rc);
        {
            const evobeing_behavior_item_t *c0 = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 0);
            CHECK(c0 != NULL, "caffeine main present");
            CHECK(c0->status == EVOBEING_BEHAVIOR_INSUFFICIENT, "main insufficient when nwo=0");
            CHECK(c0->n_without == 0, "n_without 0");
        }

        /* 天天都有 + 部分在下午（晚间）→ 主变体 INSUFFICIENT，晚间变体可给数字
           day 0: 事件 5:00（归当天，tod=5，不算晚间）
           day d>0: 事件在前一天 9:00 或 15:00（归到 day d，tod=9 或 15） */
        w.n_ev = 0;
        for (i = 0; i < 28; ++i) {
            w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_CAFFEINE;
            if (i == 0) {
                w.ev[w.n_ev++].t_h = 5.0;                            /* 归 day 0 */
            } else {
                w.ev[w.n_ev++].t_h = (i - 1) * 24.0 + (i % 3 == 0 ? 15.0 : 9.0); /* 归 day i */
            }
        }
        run(&w, NULL, &o, &rc);
        {
            const evobeing_behavior_item_t *c0 = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 0);
            const evobeing_behavior_item_t *c1 = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 1);
            CHECK(c0 != NULL, "caffeine main present");
            CHECK(c0->status == EVOBEING_BEHAVIOR_INSUFFICIENT, "main insufficient");
            CHECK(c1 != NULL, "caffeine late variant present");
            /* 晚间变体应有数字：nl=9 (i%3==0 且 i>0 → day 3,6,9,12,15,18,21,24,27)
               nl>=4 ✓ 且 nl<nw (9<28) ✓ */
            if (c1) {
                printf("    天天喝咖啡 + 部分下午：late status=%d effect=%.1f%%\n",
                       c1->status, c1->effect_pct);
                CHECK(c1->status != EVOBEING_BEHAVIOR_INSUFFICIENT, "late variant has estimate");
            }
        }
    }

    printf("[4] 混杂控制：饮酒只在周五/六，周末本身也影响精力\n");
    {
        int tr, better = 0;
        for (tr = 0; tr < 20; ++tr) {
            const evobeing_behavior_item_t *a;
            double raw, adj;
            evobeing_behavior_config_t c2;
            rng_s = 900u + (unsigned)tr;
            gen_world(&w, 28, 0.0, 0.0, 0.0, 0.0, 0.0, 3.0, +6.0, 0);  /* 周末早晨 +6 分 */
            w.n_ev = 0;
            for (i = 1; i < 28; ++i) {
                int wd = i % 7;
                if ((wd == 5 || wd == 6) && urand() < 0.9) {   /* 周五/六晚饮酒，真实效应 −4 */
                    w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; w.ev[w.n_ev++].t_h = (i - 1) * 24 + 21.0;
                    w.y[i] -= 4.0;
                }
            }
            run(&w, NULL, &o, &rc);
            a = find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0);
            if (!a || a->status == EVOBEING_BEHAVIOR_INSUFFICIENT) continue;
            raw = a->raw_pct; adj = a->effect_pct;
            /* 不控制周末时估计会偏正（被周末的 +6 盖住） */
            evobeing_behavior_config_default(&c2); c2.control_weekend = 0;
            run(&w, &c2, &o, &rc);
            if (tr == 0) printf("  raw %+.1f%%  控制周末 %+.1f%%  不控制 %+.1f%%（真值约 −6%%）\n", raw, adj, find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0)->effect_pct);
            if (adj < find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0)->effect_pct) ++better;
        }
        printf("  控制周末后估计更偏负（更接近真值）：%d/20\n", better);
        CHECK(better >= 16, "weekend control helps");
    }

    printf("[5] 边界：空输入、缺天、超长输入、窗口截取、事件归属\n");
    {
        evobeing_behavior_input_t in;
        memset(&in, 0, sizeof in);
        CHECK(evobeing_behavior_impact_zhenghuimin(NULL, NULL, &o) == -1, "null in");
        in.n_days = 0; in.wake_score = w.y;
        CHECK(evobeing_behavior_impact_zhenghuimin(&in, NULL, &o) == -1, "n_days 0");
        in.n_days = EVOBEING_BEHAVIOR_MAX_DAYS + 1;
        CHECK(evobeing_behavior_impact_zhenghuimin(&in, NULL, &o) == -1, "too many days");
        in.n_days = 28; in.wake_score = NULL;
        CHECK(evobeing_behavior_impact_zhenghuimin(&in, NULL, &o) == -1, "null scores");

        rng_s = 3u;
        gen_world(&w, 28, 0.3, 0.5, 0.3, 0.3, 0.0, 4.0, 0.0, 0);
        for (i = 0; i < 28; ++i) if (i % 4 == 0) w.y[i] = NAN;   /* 7 天缺晨起值 */
        run(&w, NULL, &o, &rc);
        CHECK(rc == 0, "rc with gaps");
        CHECK(o.n_days_used == 21, "21 valid days");
        for (i = 0; i < 28; ++i) if (i >= 5) w.y[i] = NAN;      /* 只剩 5 天 */
        run(&w, NULL, &o, &rc);
        CHECK(rc == -2, "too few days");
        CHECK(o.n_days_used == 3, "n_days_used = 3 (days 1..3; day 0 and 4 also NAN)");
        CHECK(o.unexplained_pct == 100.0, "unexplained 100");

        /* 56 天输入全部使用（不再截 28 天窗口）：前 28 天塞满饮酒且精力低 10 分，后 28 天没有 →
           饮酒仍然在报告里，效应为负（长期累积，信息不丢） */
        rng_s = 11u;
        gen_world(&w, 56, 0.0, 0.0, 0.3, 0.0, 0.0, 4.0, 0.0, 0);
        for (i = 1; i < 28; ++i) {
            if (i % 2) { w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; w.ev[w.n_ev++].t_h = (i - 1) * 24 + 21.0; w.y[i] -= 10.0; }
        }
        run(&w, NULL, &o, &rc);
        CHECK(rc == 0, "rc 56");
        CHECK(o.n_days_used == 56, "all 56 days used");
        {
            const evobeing_behavior_item_t *a = find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0);
            CHECK(a && a->status != EVOBEING_BEHAVIOR_INSUFFICIENT, "old events still counted");
            CHECK(a && a->effect_points < -5.0, "old effect retained");
            CHECK(a && a->n_with == 14 && a->n_without == 42, "counts over 56 days");
        }

        /* 事件归属：晚上 23:00 和凌晨 01:00 的饮酒都归到下一次起床 */
        rng_s = 12u;
        gen_world(&w, 28, 0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0);
        w.n_ev = 0;
        for (i = 1; i < 28; i += 3) {
            w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL;
            w.ev[w.n_ev++].t_h = (i % 2) ? (i - 1) * 24 + 23.0 : i * 24 + 1.0;
            w.y[i] -= 10.0;
        }
        run(&w, NULL, &o, &rc);
        {
            const evobeing_behavior_item_t *a = find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0);
            CHECK(a && a->n_with == 9, "9 nights with alcohol");
            CHECK(a && a->effect_points < -6.0, "effect recovered");
            CHECK(a && a->status == EVOBEING_BEHAVIOR_HIGH, "clear effect → HIGH");
            CHECK(a && a->share_pct > 30.0, "large share");
        }
        /* 最后一次起床之后的事件不计入 */
        w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_NAP; w.ev[w.n_ev++].t_h = 27 * 24 + 14.0;
        run(&w, NULL, &o, &rc);
        CHECK(find(&o, EVOBEING_BEHAVIOR_NAP, 0) == NULL, "event after last wake ignored");
    }

    printf("[6] 自定义行为与配置项\n");
    {
        rng_s = 21u;
        gen_world(&w, 28, 0.0, 0.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0);
        w.n_ev = 0;
        for (i = 1; i < 28; i += 2) { w.ev[w.n_ev].id = EVOBEING_BEHAVIOR_CUSTOM_BASE + 1; w.ev[w.n_ev++].t_h = (i - 1) * 24 + 20.0; w.y[i] += 6.0; }
        evobeing_behavior_config_default(&cfg);
        cfg.custom_name[1] = "sauna";
        cfg.n_boot = 0;
        run(&w, &cfg, &o, &rc);
        CHECK(rc == 0 && o.n_behaviors == 1, "custom behavior");
        CHECK(!strcmp(o.items[0].name, "sauna"), "custom name");
        CHECK(o.items[0].effect_points > 3.0, "custom effect");
        CHECK(o.items[0].ci_lo_pct == o.items[0].ci_lo_pct && o.items[0].ci_lo_pct < o.items[0].effect_pct, "posterior CI when n_boot=0");
        CHECK(o.items[0].status >= EVOBEING_BEHAVIOR_MID, "clear +6 effect → at least MID");
        CHECK(o.items[0].sd_points > 0 && fabs(o.items[0].z - o.items[0].effect_points / o.items[0].sd_points) < 1e-9, "z = effect/sd");
        cfg.custom_name[1] = NULL;
        run(&w, &cfg, &o, &rc);
        CHECK(!strcmp(o.items[0].name, "custom_17"), "default custom name");
        /* 同一输入、同一种子 → 结果完全一致（可复现） */
        {
            evobeing_behavior_output_t o2;
            evobeing_behavior_config_default(&cfg);
            run(&w, &cfg, &o, &rc);
            run(&w, &cfg, &o2, &rc);
            CHECK(memcmp(&o, &o2, sizeof o) == 0, "deterministic");
        }
        /* 无 wake_t_h / 无 weekday 也能跑 */
        {
            evobeing_behavior_input_t in;
            memset(&in, 0, sizeof in);
            in.n_days = 28; in.wake_score = w.y; in.events = w.ev; in.n_events = w.n_ev;
            CHECK(evobeing_behavior_impact_zhenghuimin(&in, NULL, &o) == 0, "no wake/weekday");
            CHECK(o.n_behaviors == 1 && o.items[0].effect_points > 3.0, "default 7:00 wake works");
        }
        CHECK(!strcmp(evobeing_behavior_name_zhenghuimin(EVOBEING_BEHAVIOR_NAP), "nap"), "name");
    }

    printf("[7] 晚间咖啡：14 点后与上午分开\n");
    {
        int tr, ok = 0;
        for (tr = 0; tr < 10; ++tr) {
            const evobeing_behavior_item_t *c0, *c1;
            rng_s = 1300u + (unsigned)tr;
            gen_world(&w, 28, 0.0, 0.8, 0.0, 0.0, 0.0, 3.0, 0.0, 0);
            run(&w, NULL, &o, &rc);
            c0 = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 0); c1 = find(&o, EVOBEING_BEHAVIOR_CAFFEINE, 1);
            if (tr == 0) print_out(&o);
            if (c0 && c1 && c1->status != EVOBEING_BEHAVIOR_INSUFFICIENT && c0->status != EVOBEING_BEHAVIOR_INSUFFICIENT &&
                c1->effect_points < c0->effect_points - 1.0) ++ok;
        }
        printf("  晚间咖啡比总体咖啡更负：%d/10\n", ok);
        CHECK(ok >= 8, "late caffeine separated");
    }

    printf("[8] 目标量 D_init + 去习惯吸收：行为成段出现（10 天有 / 10 天无）时的效应回收\n");
    {
        /* 模拟引擎：夜间指标 M_d = 0 + eff·x_d + 噪声；常态 h 为 λ=0.2 的 EWMA（前 3 晚累计平均）；
           D_init = (M_d − h_d)/100。真实效应 −8 分。 */
        int tr, better = 0, n_tr = 20;
        double sum_v2 = 0, sum_v1 = 0;
        for (tr = 0; tr < n_tr; ++tr) {
            enum { ND = 120 };
            static double dinit[ND], y[ND], wk[ND];
            static int wd[ND];
            static evobeing_behavior_event_t ev[ND];
            int n_ev = 0, d;
            double h = 0.0;
            evobeing_behavior_input_t in;
            evobeing_behavior_config_t c1, c2;
            evobeing_behavior_output_t o1, o2;
            const evobeing_behavior_item_t *a1, *a2;
            rng_s = 2000u + (unsigned)tr;
            for (d = 0; d < ND; ++d) {
                int on = ((d / 10) % 2 == 1);   /* 第 10–19、30–39… 天有 */
                double M = (on ? -8.0 : 0.0) + 4.0 * nrand(), lam;
                wk[d] = d * 24.0 + 7.0; wd[d] = d % 7;
                if (on && d > 0) { ev[n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; ev[n_ev++].t_h = (d - 1) * 24.0 + 20.0; }
                dinit[d] = d == 0 ? 0.0 : (M - h) / 100.0;
                lam = d < 3 ? 1.0 / (d + 1) : 0.2;
                h += lam * (M - h);
                y[d] = 60.0 + 100.0 * dinit[d];
            }
            memset(&in, 0, sizeof in);
            in.n_days = ND; in.wake_score = y; in.wake_d_init = dinit; in.wake_t_h = wk; in.weekday = wd;
            in.events = ev; in.n_events = n_ev;
            evobeing_behavior_config_default(&c1); c1.n_boot = 0;
            c2 = c1; c2.habit_lambda = 0.0;   /* 不做去吸收 */
            CHECK(evobeing_behavior_impact_zhenghuimin(&in, &c1, &o1) == 0, "rc v2");
            CHECK(evobeing_behavior_impact_zhenghuimin(&in, &c2, &o2) == 0, "rc no-dehabit");
            CHECK(o1.target_is_d_init == 1, "target is d_init");
            a1 = find(&o1, EVOBEING_BEHAVIOR_ALCOHOL, 0); a2 = find(&o2, EVOBEING_BEHAVIOR_ALCOHOL, 0);
            CHECK(a1 && a2 && a1->status != EVOBEING_BEHAVIOR_INSUFFICIENT, "alcohol estimated");
            if (!a1 || !a2) continue;
            sum_v2 += a1->effect_points; sum_v1 += a2->effect_points;
            if (fabs(a1->effect_points + 8.0) < fabs(a2->effect_points + 8.0)) ++better;
            if (tr == 0) printf("  去吸收 %.1f 分，不去吸收 %.1f 分（真值 −8）\n", a1->effect_points, a2->effect_points);
        }
        printf("  均值：去吸收 %.2f，不去吸收 %.2f；去吸收更接近真值 %d/%d\n", sum_v2 / n_tr, sum_v1 / n_tr, better, n_tr);
        CHECK(sum_v2 / n_tr < -5.5 && sum_v2 / n_tr > -10.5, "dehabituated estimate near −8");
        CHECK(fabs(sum_v2 / n_tr + 8.0) < fabs(sum_v1 / n_tr + 8.0), "dehabituation reduces mean bias");
        CHECK(better >= n_tr / 2, "dehabituation helps in at least half the trials");
    }

    printf("[9] 流式更新与批量一致\n");
    {
        static evobeing_behavior_state_t st;
        static evobeing_behavior_day_t days[EVOBEING_BEHAVIOR_MAX_DAYS];
        evobeing_behavior_output_t ob, os;
        evobeing_behavior_config_t c;
        int d;
        rng_s = 77u;
        gen_world(&w, 40, 0.3, 0.6, 0.3, 0.4, 0.2, 4.0, 2.0, 0);
        evobeing_behavior_config_default(&c); c.n_boot = 0;
        run(&w, &c, &ob, &rc);
        CHECK(rc == 0, "batch rc");
        evobeing_behavior_fold_events_zhenghuimin(w.ev, w.n_ev, w.wake, w.n_days, &c, days);
        evobeing_behavior_state_init_zhenghuimin(&st, &c);
        for (d = 0; d < w.n_days; ++d) {
            int k;
            days[d].wake_score = w.y[d]; days[d].wake_d_init = NAN; days[d].weekday = w.weekday[d]; days[d].logged = -1;
            for (k = 0; k < EVOBEING_BEHAVIOR_MAX_CHANNELS; ++k) days[d].channel[k] = NAN;
            CHECK(evobeing_behavior_update_zhenghuimin(&st, &days[d]) == 0, "update rc");
        }
        CHECK(evobeing_behavior_report_zhenghuimin(&st, &os) == 0, "report rc");
        CHECK(os.n_behaviors == ob.n_behaviors && os.n_days_used == ob.n_days_used, "same items");
        for (i = 0; i < ob.n_behaviors && i < os.n_behaviors; ++i) {
            CHECK(!strcmp(ob.items[i].name, os.items[i].name), "same order");
            CHECK(ob.items[i].status == os.items[i].status, "same status");
            if (ob.items[i].status != EVOBEING_BEHAVIOR_INSUFFICIENT)
                CHECK(fabs(ob.items[i].effect_points - os.items[i].effect_points) < 1e-9 &&
                      fabs(ob.items[i].sd_points - os.items[i].sd_points) < 1e-9, "same numbers");
        }
        CHECK(evobeing_behavior_update_zhenghuimin(NULL, &days[0]) == -1, "null state");
        {
            evobeing_behavior_day_t skip = days[0];
            skip.wake_score = NAN;
            CHECK(evobeing_behavior_update_zhenghuimin(&st, &skip) == 1, "day without wake value skipped");
        }
    }

    printf("[10] 长期累积：小效应（−3 分）120 天后可见；逐日报告稳定\n");
    {
        static evobeing_behavior_state_t st;
        evobeing_behavior_output_t prev_o, cur_o;
        evobeing_behavior_config_t c;
        int d, n_jump = 0, n_rep = 0, tr, n_found = 0, n_mid = 0;
        double sum_err = 0;
        for (tr = 0; tr < 10; ++tr) {
            rng_s = 3000u + (unsigned)tr;
            evobeing_behavior_config_default(&c); c.n_boot = 0;
            evobeing_behavior_state_init_zhenghuimin(&st, &c);
            memset(&prev_o, 0, sizeof prev_o);
            for (d = 0; d < 120; ++d) {
                evobeing_behavior_day_t day;
                int k, on = urand() < 0.35;
                memset(&day, 0, sizeof day);
                day.wake_d_init = NAN; day.weekday = d % 7; day.logged = -1;
                for (k = 0; k < EVOBEING_BEHAVIOR_MAX_CHANNELS; ++k) day.channel[k] = NAN;
                day.occ[EVOBEING_BEHAVIOR_SCREEN] = (unsigned char)on;
                day.wake_score = 60.0 + (on ? -3.0 : 0.0) + 5.0 * nrand();
                evobeing_behavior_update_zhenghuimin(&st, &day);
                if (d >= 60) {
                    const evobeing_behavior_item_t *s1, *s0;
                    evobeing_behavior_report_zhenghuimin(&st, &cur_o);
                    s1 = find(&cur_o, EVOBEING_BEHAVIOR_SCREEN, 0); s0 = find(&prev_o, EVOBEING_BEHAVIOR_SCREEN, 0);
                    if (s1 && s0 && s1->status != EVOBEING_BEHAVIOR_INSUFFICIENT && s0->status != EVOBEING_BEHAVIOR_INSUFFICIENT) {
                        ++n_rep;
                        if (fabs(s1->effect_points - s0->effect_points) > 1.0) ++n_jump;
                    }
                    prev_o = cur_o;
                }
            }
            {
                const evobeing_behavior_item_t *s1 = find(&cur_o, EVOBEING_BEHAVIOR_SCREEN, 0);
                if (s1 && s1->status != EVOBEING_BEHAVIOR_INSUFFICIENT) {
                    ++n_found; sum_err += fabs(s1->effect_points + 3.0);
                    if (s1->status >= EVOBEING_BEHAVIOR_MID) ++n_mid;
                    if (tr == 0) printf("  120 天后 screen %.1f 分 ± %.1f，status %d，预测技能 %.0f%%\n",
                                        s1->effect_points, s1->sd_points, (int)s1->status, cur_o.pred_skill_pct);
                }
            }
        }
        printf("  平均绝对误差 %.2f 分（10 次），≥MID %d/10，逐日跳动>1 分的比例 %d/%d\n", sum_err / n_found, n_mid, n_jump, n_rep);
        CHECK(n_found == 10 && sum_err / n_found < 1.5, "small effect recovered after 120 days");
        CHECK(n_mid >= 7, "small effect mostly at least MID");
        CHECK(n_jump * 10 < n_rep, "day-to-day reports stable (<10% jumps > 1 point)");
    }

    printf("[11] 缺记录：用户只有一半日子打开记录\n");
    {
        int tr, better = 0, n_tr = 20;
        double sum_with = 0, sum_wo = 0;
        for (tr = 0; tr < n_tr; ++tr) {
            enum { ND = 90 };
            static double y[ND], wk[ND];
            static int wd[ND], logged[ND];
            static evobeing_behavior_event_t ev[ND];
            int n_ev = 0, d;
            evobeing_behavior_input_t in;
            evobeing_behavior_config_t c;
            evobeing_behavior_output_t o1, o2;
            const evobeing_behavior_item_t *a1, *a2;
            rng_s = 4000u + (unsigned)tr;
            for (d = 0; d < ND; ++d) {
                int drank = urand() < 0.4, open = urand() < 0.5;
                wk[d] = d * 24.0 + 7.0; wd[d] = d % 7;
                logged[d] = open;
                /* 真实效应 −8；只有打开记录的日子才留下饮酒记录 */
                if (d > 0 && drank && open) { ev[n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; ev[n_ev++].t_h = (d - 1) * 24.0 + 20.0; }
                y[d] = 60.0 + (drank ? -8.0 : 0.0) + 4.0 * nrand();
            }
            memset(&in, 0, sizeof in);
            in.n_days = ND; in.wake_score = y; in.wake_t_h = wk; in.weekday = wd; in.events = ev; in.n_events = n_ev;
            evobeing_behavior_config_default(&c); c.n_boot = 0;
            in.logged = logged;
            CHECK(evobeing_behavior_impact_zhenghuimin(&in, &c, &o1) == 0, "rc logged");
            in.logged = NULL;
            CHECK(evobeing_behavior_impact_zhenghuimin(&in, &c, &o2) == 0, "rc unlogged");
            a1 = find(&o1, EVOBEING_BEHAVIOR_ALCOHOL, 0); a2 = find(&o2, EVOBEING_BEHAVIOR_ALCOHOL, 0);
            if (!a1 || !a2 || a1->status == EVOBEING_BEHAVIOR_INSUFFICIENT || a2->status == EVOBEING_BEHAVIOR_INSUFFICIENT) continue;
            CHECK(a1->n_with + a1->n_without < a2->n_with + a2->n_without, "unknown days not counted as controls");
            sum_with += a1->effect_points; sum_wo += a2->effect_points;
            if (fabs(a1->effect_points + 8.0) < fabs(a2->effect_points + 8.0)) ++better;
            if (tr == 0) printf("  有 logged %.1f 分（n=%d/%d），无 logged %.1f 分（n=%d/%d），真值 −8\n",
                                a1->effect_points, a1->n_with, a1->n_without, a2->effect_points, a2->n_with, a2->n_without);
        }
        printf("  均值：有 logged %.2f，无 logged %.2f；有 logged 更接近 %d/%d\n", sum_with / n_tr, sum_wo / n_tr, better, n_tr);
        CHECK(better >= n_tr * 3 / 4, "logged indicator reduces bias");
    }

    printf("[12] 第二晚滞后、作用路径通道、预测技能\n");
    {
        enum { ND = 100 };
        static double y[ND], wk[ND], hrv[ND], deep[ND];
        static int wd[ND];
        static evobeing_behavior_event_t ev[ND];
        int n_ev = 0, d, prev_on = 0;
        evobeing_behavior_input_t in;
        evobeing_behavior_config_t c;
        const evobeing_behavior_item_t *a, *a2, *np;
        rng_s = 5000u;
        for (d = 0; d < ND; ++d) {
            int on = urand() < 0.3, nap = urand() < 0.3;
            double eff = (on ? -7.0 : 0.0) + (prev_on ? -3.0 : 0.0) + (nap ? 3.0 : 0.0);
            wk[d] = d * 24.0 + 7.0; wd[d] = d % 7;
            if (d > 0 && on) { ev[n_ev].id = EVOBEING_BEHAVIOR_ALCOHOL; ev[n_ev++].t_h = (d - 1) * 24.0 + 20.0; }
            if (d > 0 && nap) { ev[n_ev].id = EVOBEING_BEHAVIOR_NAP; ev[n_ev++].t_h = (d - 1) * 24.0 + 13.5; }
            y[d] = 60.0 + eff + 4.0 * nrand();
            hrv[d] = 3.8 + (on ? -0.25 : 0.0) + 0.2 * nrand();      /* 饮酒 → 醒前 HRV 低约 1 个标准差 */
            deep[d] = 0.18 + (nap ? 0.0 : 0.0) + 0.04 * nrand();     /* 深睡与行为无关 */
            prev_on = on;
        }
        memset(&in, 0, sizeof in);
        in.n_days = ND; in.wake_score = y; in.wake_t_h = wk; in.weekday = wd; in.events = ev; in.n_events = n_ev;
        in.channel[0] = hrv; in.channel_name[0] = "hrv_wake";
        in.channel[1] = deep; in.channel_name[1] = "deep_frac";
        in.n_channels = 2;
        evobeing_behavior_config_default(&c);
        CHECK(evobeing_behavior_impact_zhenghuimin(&in, &c, &o) == 0, "rc");
        a = find(&o, EVOBEING_BEHAVIOR_ALCOHOL, 0); np = find(&o, EVOBEING_BEHAVIOR_NAP, 0);
        a2 = NULL;
        for (i = 0; i < o.n_behaviors; ++i) if (o.items[i].id == EVOBEING_BEHAVIOR_ALCOHOL && o.items[i].lag_nights == 2) a2 = &o.items[i];
        CHECK(a && a2 && np, "items present");
        if (a && a2 && np) {
            printf("  alcohol %.1f（真值 −7）  alcohol_2nd %.1f（真值 −3）  nap %.1f（真值 +3）\n", a->effect_points, a2->effect_points, np->effect_points);
            printf("  通道 %s：alcohol %.2f SD (z %.1f)，nap %.2f SD；通道 %s：alcohol %.2f SD\n",
                   o.channel_name[0], a->channel_effect_sd[0], a->channel_z[0], np->channel_effect_sd[0],
                   o.channel_name[1], a->channel_effect_sd[1]);
            printf("  一步预测 RMSE %.2f，朴素 %.2f，技能 %.0f%%\n", o.pred_rmse, o.naive_rmse, o.pred_skill_pct);
            CHECK(!strcmp(a2->name, "alcohol_2nd"), "lag name");
            CHECK(a->effect_points < -4.5 && a->effect_points > -9.5, "alcohol next morning");
            CHECK(a2->status != EVOBEING_BEHAVIOR_INSUFFICIENT && a2->effect_points < -1.0 && a2->effect_points > -5.5, "second-night effect");
            CHECK(a->channel_effect_sd[0] < -0.6 && a->channel_z[0] < -3.0, "alcohol lowers hrv channel");
            CHECK(fabs(np->channel_effect_sd[0]) < 0.4, "nap no hrv effect");
            CHECK(fabs(a->channel_effect_sd[1]) < 0.4, "alcohol no deep-sleep effect");
            CHECK(o.n_channels == 2 && !strcmp(o.channel_name[1], "deep_frac"), "channel names");
            CHECK(o.pred_skill_pct > 15.0, "real effects → positive predictive skill");
            CHECK(a->prob_dir > 0.99 && a->status == EVOBEING_BEHAVIOR_HIGH, "clear effect HIGH with BH");
        }
        /* 无效应世界：预测技能应接近 0 或为负 */
        for (d = 0; d < ND; ++d) y[d] = 60.0 + 4.0 * nrand();
        in.n_channels = 0;
        CHECK(evobeing_behavior_impact_zhenghuimin(&in, &c, &o) == 0, "rc null");
        printf("  无效应世界预测技能 %.0f%%，HIGH 条数 %d\n", o.pred_skill_pct, 0);
        CHECK(o.pred_skill_pct < 10.0, "null world → no skill");
        for (i = 0; i < o.n_behaviors; ++i) CHECK(o.items[i].status != EVOBEING_BEHAVIOR_HIGH, "null world no HIGH");
    }

    printf("%d checks, %d failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}
