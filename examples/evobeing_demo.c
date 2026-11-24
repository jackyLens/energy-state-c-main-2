/*
 * evobeing_demo.c —— 主接口调用示例：每 15 分钟喂一个样本（日夜统一），取能量与精力
 *
 * 合成 4 天数据（23:00–07:00 睡眠，日间静坐，第 2 天 18:00 跑步 30 分钟），
 * 第 4 天醒来后的第一个样本带 App 恢复分（RECOVERY 环境变量，默认 85），
 * 打印每天 07:15 / 08:00 / 12:00 / 18:30 / 22:00 的能量与精力。
 */
#include "evobeing_zhenghuimin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void make_sample(evobeing_sample_t *s, double t)
{
    evobeing_sample_init_zhenghuimin(s); /* recovery 默认 NAN */
    const double tod = fmod(t, 24.0);
    const int day = (int)floor(t / 24.0);
    const int asleep = (tod >= 23.0 || tod < 7.0);
    s->t_h = t;
    s->tod = tod;
    if (asleep) {
        const double phase = fmod(tod + 1.0, 24.0) / 8.0;
        const double deep = (phase < 0.5) ? 0.45 : 0.15;
        s->stage[2] = deep;                                /* 深睡 */
        s->stage[3] = (phase < 0.5) ? 0.15 : 0.35;         /* REM */
        s->stage[1] = 1.0 - deep - s->stage[3] - 0.05;     /* 浅睡 */
        s->stage[0] = 0.05;                                /* 清醒 */
        s->hr = 54.0 + 3.0 * phase;
        s->hrv_log = log(48.0 + 4.0 * sin(phase * 6.28));  /* 夜间逐拍 RR → RMSSD → ln */
        s->spo2 = 97.0;
        s->spo2_min = 96.0;
    } else {
        const double w = 2.0 * M_PI / 24.0;
        s->hr = 70.0 - 2.0 * cos(w * tod) - 3.46 * sin(w * tod);
        s->hrv_log = NAN;                                  /* 日间只有心率 */
        s->steps = 120.0;                                  /* 15 min 合计 */
        s->kcal = 6.0;
        if (day == 1 && tod >= 18.0 && tod < 18.5) {       /* 跑步 30 分钟（两个样本） */
            s->hr = 150.0;
            s->steps = 1950.0;
            s->kcal = 120.0;
        }
    }
}

int main(void)
{
    evobeing_ctx_t ctx;
    evobeing_init_zhenghuimin(&ctx, NULL);

    printf("evobeing %s (%s)\n", EVOBEING_VERSION, EVOBEING_AUTHOR);
    const char *env = getenv("RECOVERY");
    const double recovery = env ? atof(env) : 85.0;        /* App 算好的昨夜恢复分 0–100 */
    printf("第 4 天晨起恢复分 R = %.0f（RECOVERY=.. 可改；0 = 不传）\n", recovery);
    printf("%4s %6s %8s %8s  %s\n", "day", "tod", "energy", "vitality", "flags");
    const int per_day = 24 * 4;                            /* 15 min 一格 */
    const int n = 4 * per_day;
    for (int i = 0; i < n; i++) {
        evobeing_sample_t s;
        make_sample(&s, (double)i / 4.0);
        if (i / per_day == 3 && fabs(s.tod - 7.0) < 1e-9 && recovery > 0.0) s.recovery = recovery;
        double energy = 0.0, vitality = 0.0;
        if (evobeing_energy_zhenghuimin(&ctx, &s, &energy) != EVOBEING_OK) continue;
        if (evobeing_vitality_zhenghuimin(&ctx, &vitality) != EVOBEING_OK) continue;
        const double tod = s.tod;
        const int rec = (ctx.energy_point.flags & ENERGY_FLAG_RECOVERY) ||
                        (ctx.vitality_point.flags & VITALITY_FLAG_RECOVERY);
        if (rec || fabs(tod - 7.25) < 1e-9 || fabs(tod - 8.0) < 1e-9 || fabs(tod - 12.0) < 1e-9 ||
            fabs(tod - 18.5) < 1e-9 || fabs(tod - 22.0) < 1e-9) {
            printf("%4d %6.2f %8.1f %8.1f  %s%s\n", i / per_day, tod, energy, vitality,
                   (ctx.energy_point.flags & ENERGY_FLAG_RECOVERY) ? "E:恢复分锚定 " : "",
                   (ctx.vitality_point.flags & VITALITY_FLAG_RECOVERY) ? "V:恢复分融合" : "");
        }
    }

    evobeing_profile_t profile;
    evobeing_get_profile_zhenghuimin(&ctx, &profile);
    printf("已学习夜数：能量基线 %d 晚，精力常态 %d 晚\n",
           profile.energy.n[ENERGY_BL_HR_NIGHT], profile.vitality.n_nights);
    return 0;
}
