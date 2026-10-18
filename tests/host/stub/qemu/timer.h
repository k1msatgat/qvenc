#ifndef STUB_TIMER_H
#define STUB_TIMER_H
typedef enum { QEMU_CLOCK_VIRTUAL = 0 } QEMUClockType;
typedef struct QEMUTimer {
    void (*cb)(void *);
    void *opaque;
    int64_t expire;         /* -1 == 안 걸려 있음 */
} QEMUTimer;

extern int64_t sim_now_ms;
static inline int64_t qemu_clock_get_ms(QEMUClockType t) { (void)t; return sim_now_ms; }
static inline void timer_init_ms(QEMUTimer *ts, QEMUClockType t,
                                 void (*cb)(void *), void *opaque)
{ (void)t; ts->cb = cb; ts->opaque = opaque; ts->expire = -1; }
static inline void timer_mod(QEMUTimer *ts, int64_t expire) { ts->expire = expire; }
static inline void timer_del(QEMUTimer *ts) { ts->expire = -1; }
/* 테스트가 직접 만기를 돌린다 */
static inline bool sim_timer_pending(const QEMUTimer *ts) { return ts->expire >= 0; }
static inline bool sim_timer_fire(QEMUTimer *ts)
{
    if (ts->expire < 0) return false;
    sim_now_ms = ts->expire;
    ts->expire = -1;
    ts->cb(ts->opaque);
    return true;
}
#endif
