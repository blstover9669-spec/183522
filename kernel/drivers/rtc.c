/* rtc.c - CMOS real-time clock and date/time helpers. */
#include "kernel.h"

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, (uint8_t)(reg | 0x80));   /* keep NMI disabled */
    return inb(0x71);
}

static bool rtc_updating(void) { return cmos_read(0x0A) & 0x80; }

static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(datetime_t *dt) {
    int sec, min, hour, day, mon, year, cent = 0;
    int ls, lm, lh, ld, lmo, ly;
    /* read twice until consistent (avoid catching an update mid-way) */
    int guard = 0;
    while (rtc_updating() && guard++ < 100000) {}
    sec = cmos_read(0x00); min = cmos_read(0x02); hour = cmos_read(0x04);
    day = cmos_read(0x07); mon = cmos_read(0x08); year = cmos_read(0x09);
    do {
        ls = sec; lm = min; lh = hour; ld = day; lmo = mon; ly = year;
        guard = 0;
        while (rtc_updating() && guard++ < 100000) {}
        sec = cmos_read(0x00); min = cmos_read(0x02); hour = cmos_read(0x04);
        day = cmos_read(0x07); mon = cmos_read(0x08); year = cmos_read(0x09);
    } while (ls != sec || lm != min || lh != hour || ld != day || lmo != mon || ly != year);
    cent = cmos_read(0x32);
    uint8_t regb = cmos_read(0x0B);
    bool pm = hour & 0x80;
    hour &= 0x7F;
    if (!(regb & 0x04)) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
        cent = bcd(cent);
    }
    if (!(regb & 0x02)) {            /* 12-hour mode */
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }
    if (cent >= 19 && cent <= 30) year += cent * 100;
    else year += (year < 70) ? 2000 : 1900;
    dt->year = year; dt->month = mon; dt->day = day;
    dt->hour = hour; dt->minute = min; dt->second = sec;
    sys_time_to_datetime(sys_datetime_to_time(dt), dt);   /* fills weekday */
}

/* ---- time keeping: RTC sampled at boot, then advanced with the PIT --------- */
static uint32_t boot_epoch;      /* sys_time() at g_ticks == boot_ticks */
static uint32_t boot_ticks;

void rtc_init(void) {
    datetime_t dt;
    rtc_read(&dt);
    boot_epoch = sys_datetime_to_time(&dt);
    boot_ticks = g_ticks;
    klog("rtc: %04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

uint32_t sys_time(void) {
    uint32_t t = boot_epoch + (g_ticks - boot_ticks) / 1000;
    /* resync with the RTC every 10 minutes to avoid drift */
    if (g_ticks - boot_ticks > 600000) {
        datetime_t dt;
        rtc_read(&dt);
        boot_epoch = sys_datetime_to_time(&dt);
        boot_ticks = g_ticks;
        t = boot_epoch;
    }
    return t;
}

void sys_datetime(datetime_t *dt) { sys_time_to_datetime(sys_time(), dt); }

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

uint32_t sys_datetime_to_time(const datetime_t *dt) {
    uint32_t days = 0;
    for (int y = 1970; y < dt->year; y++) days += is_leap(y) ? 366 : 365;
    for (int m = 1; m < dt->month && m <= 12; m++) days += (uint32_t)(mdays[m - 1] + (m == 2 && is_leap(dt->year)));
    days += (uint32_t)(dt->day - 1);
    return days * 86400u + (uint32_t)(dt->hour * 3600 + dt->minute * 60 + dt->second);
}

void sys_time_to_datetime(uint32_t t, datetime_t *dt) {
    uint32_t days = t / 86400u, rem = t % 86400u;
    dt->hour = (int)(rem / 3600); dt->minute = (int)(rem % 3600 / 60); dt->second = (int)(rem % 60);
    dt->weekday = (int)((days + 4) % 7);   /* 1970-01-01 was a Thursday */
    int y = 1970;
    for (;;) {
        uint32_t yd = is_leap(y) ? 366u : 365u;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    int m = 0;
    for (;;) {
        uint32_t md = (uint32_t)(mdays[m] + (m == 1 && is_leap(y)));
        if (days < md) break;
        days -= md;
        m++;
    }
    dt->year = y; dt->month = m + 1; dt->day = (int)days + 1;
}

static const char *months[] = { "January", "February", "March", "April", "May", "June", "July",
                                "August", "September", "October", "November", "December" };
static const char *wdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

const char *sys_month_name(int m) { return (m >= 1 && m <= 12) ? months[m - 1] : "?"; }
const char *sys_weekday_name(int wd) { return (wd >= 0 && wd <= 6) ? wdays[wd] : "?"; }
