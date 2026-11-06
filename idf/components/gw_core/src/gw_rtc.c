#include "gw_rtc.h"

#include <stddef.h>

uint8_t gw_bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10u + (bcd & 0x0Fu));
}

uint8_t gw_bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10u) << 4) | (bin % 10u));
}

static bool is_bcd(uint8_t v)
{
    return (v & 0x0Fu) <= 9u && (v >> 4) <= 9u;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (int64_t)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (int64_t)d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static unsigned month_days(int64_t y, unsigned m)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29u : days[m - 1];
}

gw_rtc_status_t gw_rtc_decode(const uint8_t regs[GW_RTC_REGS], int64_t *epoch_s)
{
    if (regs[0] & GW_RTC_OS_BIT) {
        return GW_RTC_STOPPED;
    }
    const uint8_t mask[GW_RTC_REGS] = {0x7F, 0x7F, 0x3F, 0x3F, 0x07, 0x1F, 0xFF};
    uint8_t v[GW_RTC_REGS];
    for (size_t i = 0; i < GW_RTC_REGS; i++) {
        v[i] = (uint8_t)(regs[i] & mask[i]);
        if (!is_bcd(v[i])) {
            return GW_RTC_INVALID;
        }
    }
    unsigned sec = gw_bcd_to_bin(v[0]), min = gw_bcd_to_bin(v[1]), hour = gw_bcd_to_bin(v[2]);
    unsigned day = gw_bcd_to_bin(v[3]), mon = gw_bcd_to_bin(v[5]);
    int64_t year = 2000 + gw_bcd_to_bin(v[6]);
    if (sec > 59 || min > 59 || hour > 23 || mon < 1 || mon > 12 || day < 1 ||
        day > month_days(year, mon) || v[4] > 6) {
        return GW_RTC_INVALID;
    }
    int64_t t = days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    if (t < GW_RTC_MIN_EPOCH) {
        return GW_RTC_IMPLAUSIBLE;
    }
    *epoch_s = t;
    return GW_RTC_OK;
}

bool gw_rtc_encode(int64_t epoch_s, uint8_t regs[GW_RTC_REGS])
{
    if (epoch_s < GW_RTC_MIN_EPOCH || epoch_s > GW_RTC_MAX_EPOCH) {
        return false;
    }
    int64_t days = epoch_s / 86400, rem = epoch_s % 86400;
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    regs[0] = gw_bin_to_bcd((uint8_t)(rem % 60));            /* OS bit clear */
    regs[1] = gw_bin_to_bcd((uint8_t)(rem / 60 % 60));
    regs[2] = gw_bin_to_bcd((uint8_t)(rem / 3600));
    regs[3] = gw_bin_to_bcd((uint8_t)d);
    regs[4] = (uint8_t)((days + 4) % 7);                     /* 1970-01-01 was a Thursday */
    regs[5] = gw_bin_to_bcd((uint8_t)m);
    regs[6] = gw_bin_to_bcd((uint8_t)(y - 2000));
    return true;
}

const char *gw_rtc_status_str(gw_rtc_status_t st)
{
    switch (st) {
    case GW_RTC_OK:
        return "ok";
    case GW_RTC_STOPPED:
        return "oscillator stopped (OS bit): time lost";
    case GW_RTC_INVALID:
        return "invalid registers";
    case GW_RTC_IMPLAUSIBLE:
        return "implausible time (before 2025)";
    default:
        return "?";
    }
}
