/* MainDOB CMOS RTC Driver
 * Reads date/time from CMOS via ports 0x70/0x71 */

#include <unistd.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/cmos.h>

#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

/* Wait until RTC is not updating (bit 7 di status A) */
static void rtc_wait_ready(void)
{
    while (cmos_read(RTC_STATUS_A) & 0x80)
        ;
}

typedef struct
{
    uint8_t seconds, minutes, hours;
    uint8_t day, month;
    uint16_t year;
} rtc_time_t;

static void rtc_read_time(rtc_time_t *t)
{
    rtc_wait_ready();

    uint8_t status_b = cmos_read(RTC_STATUS_B);
    bool is_bcd = !(status_b & 0x04);
    bool is_24h = (status_b & 0x02) != 0;

    t->seconds = cmos_read(RTC_SECONDS);
    t->minutes = cmos_read(RTC_MINUTES);
    t->hours   = cmos_read(RTC_HOURS);
    t->day     = cmos_read(RTC_DAY);
    t->month   = cmos_read(RTC_MONTH);
    t->year    = cmos_read(RTC_YEAR);

    if (is_bcd)
    {
        t->seconds = bcd_to_bin(t->seconds);
        t->minutes = bcd_to_bin(t->minutes);
        t->hours   = bcd_to_bin(t->hours & 0x7F);
        t->day     = bcd_to_bin(t->day);
        t->month   = bcd_to_bin(t->month);
        t->year    = bcd_to_bin((uint8_t)t->year);
    }

    /* AM/PM handling */
    if (!is_24h && (cmos_read(RTC_HOURS) & 0x80))
    {
        t->hours = (t->hours + 12) % 24;
    }

    /* RTC only returns last 2 digits of year */
    t->year += 2000;
}

static dob_status_t handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    if (msg->code == 1)
    {
        rtc_time_t t;
        rtc_read_time(&t);
        reply->arg0 = t.hours;
        reply->arg1 = t.minutes;
        reply->arg2 = t.seconds;
        reply->arg3 = ((uint32_t)t.year << 16) | ((uint32_t)t.month << 8) | t.day;
        return DOB_OK;
    }
    return DOB_ERR_INVALID;
}

int main(void)
{
    debug_print("[rtc] Starting RTC driver...\n");

    rtc_time_t now;
    rtc_read_time(&now);

    dob_server_init("rtc");
    dob_server_register(handle_message);

    debug_print("[rtc] RTC driver ready.\n");
    dob_server_loop();
    return 0;
}
