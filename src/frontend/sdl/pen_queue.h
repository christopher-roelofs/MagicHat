#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* Replay host pen events on the guest clock. SDL can deliver an entire drag
 * in one poll batch. Preserve edges and advance toward the latest target
 * along a bounded trajectory instead of replaying stale motion history. */
typedef struct mrc_pen_event {
    uint64_t at;
    unsigned x, y;
    bool down, press;
    struct mrc_pen_event *next;
} mrc_pen_event;
typedef struct {
    mrc_pen_event *head, *tail;
    uint64_t press_slot;
    uint64_t last_slot;
    uint32_t last_tick;
    bool clock_started;
    /* Last position actually delivered to the guest, not the pending target. */
    unsigned delivered_x, delivered_y;
    uint64_t delivered_at, contact_at, min_hold;
    bool contact;
} mrc_pen_queue;

static inline bool mrc_pen_enqueue(mrc_pen_queue *q, uint64_t now,
        uint64_t hz, uint64_t min_hold, uint32_t tick, bool press,
        bool down, unsigned x, unsigned y)
{
    q->min_hold = min_hold;
    mrc_pen_event *e = (mrc_pen_event *)malloc(sizeof(*e));
    if (!e) return false;
    /* Modular subtraction handles timestamp wrap; stale events must not
     * schedule input weeks into the future. */
    int32_t delta = (int32_t)(tick-q->last_tick);
    uint64_t at = q->clock_started ? q->last_slot +
        (uint64_t)(delta > 0 ? delta : 0)*hz/1000 : now;
    const uint64_t sample = hz / 60 ? hz / 60 : 1;
    if (press && !q->head) at=now;
    /* A late poll must not collapse motion and release into the past.
     * The guest still needs execution time at the final sensor position. */
    if (at < now) at=now;
    if (at > now + sample) at=now + sample;
    if (!q->clock_started || delta >= 0) q->last_tick=tick;
    q->clock_started=true;
    if (q->tail && at < q->tail->at) at=q->tail->at;
    if (press) q->press_slot=at;
    if (down && !press && q->tail && q->tail->press &&
        at < q->tail->at + sample) at=q->tail->at + sample;
    if (!down) {
        if (at < q->press_slot + min_hold) at=q->press_slot + min_hold;
        /* Give the final position a sampling interval before pen-up. */
        if (q->tail && q->tail->down && !q->tail->press &&
            at < q->tail->at + sample) at=q->tail->at + sample;
    }
    if (down && !press && q->tail && q->tail->down && !q->tail->press) {
        /* Keep the original deadline: moving must not postpone sampling
         * indefinitely. Only the sensor position is replaced. */
        if (q->tail->at < now) q->tail->at=now;
        q->tail->x=x; q->tail->y=y;
        q->last_slot=q->tail->at;
        free(e);
        return true;
    }
    q->last_slot=at;
    *e = (mrc_pen_event){at,x,y,down,press,NULL};
    if (q->tail) q->tail->next=e; else q->head=e;
    q->tail=e;
    return true;
}
static inline void mrc_pen_pop(mrc_pen_queue *q)
{
    mrc_pen_event *e=q->head;
    if (!e) return;
    q->head=e->next;
    if (!q->head) q->tail=NULL;
    free(e);
}
/* Host input translation: advance toward the latest target at up to
 * 16 panel pixels per axis per 1/120 guest second. This bounds a full-width
 * traversal to about 250 ms, without accumulating a history of mouse moves.
 * It is not an analog digitizer model or a change to guest sampling clocks. */
static inline bool mrc_pen_deliver(mrc_pen_queue *q, uint64_t now,
        uint64_t hz, mrc_pen_event *out)
{
    mrc_pen_event *e = q->head;
    if (!e || e->at > now) return false;
    const uint64_t interval = hz / 120 ? hz / 120 : 1;
    const uint64_t settle = hz / 60 ? hz / 60 : 1;
    uint64_t earliest = now;
    if (!e->down) {
        earliest = q->contact_at + q->min_hold;
        if (earliest < q->delivered_at + settle)
            earliest = q->delivered_at + settle;
    } else if (!e->press && q->contact) {
        earliest = q->delivered_at + interval;
    }
    if (earliest > now) { e->at = earliest; return false; }
    *out = *e;
    out->at = now;
    out->next = NULL;
    bool reached = true;
    if (e->down && !e->press && q->contact) {
        const int64_t dx = (int64_t)e->x - q->delivered_x;
        const int64_t dy = (int64_t)e->y - q->delivered_y;
        const int64_t ax = dx < 0 ? -dx : dx;
        const int64_t ay = dy < 0 ? -dy : dy;
        const int64_t distance = ax > ay ? ax : ay;
        if (distance > 16) {
            out->x = (unsigned)((int64_t)q->delivered_x + dx * 16 / distance);
            out->y = (unsigned)((int64_t)q->delivered_y + dy * 16 / distance);
            e->at = now + interval;
            reached = false;
        }
    }
    if (out->press) q->contact_at = now;
    q->contact = out->down;
    q->delivered_at = now;
    if (out->down) { q->delivered_x = out->x; q->delivered_y = out->y; }
    if (reached) mrc_pen_pop(q);
    return true;
}
static inline void mrc_pen_clear(mrc_pen_queue *q)
{
    while(q->head) mrc_pen_pop(q);
}
