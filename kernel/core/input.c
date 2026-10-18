/* input.c - lock-free-ish ring buffer between IRQ handlers (producers) and the main loop. */
#include "kernel.h"

#define QSIZE 512
static input_event_t queue[QSIZE];
static volatile uint32_t qhead, qtail;   /* head: next write, tail: next read */

void input_push(const input_event_t *ev) {
    uint32_t next = (qhead + 1) % QSIZE;
    if (next == qtail) {
        /* full: merge consecutive mouse motion into the newest slot if possible */
        uint32_t last = (qhead + QSIZE - 1) % QSIZE;
        if (ev->type == IN_MOUSE && queue[last].type == IN_MOUSE && queue[last].buttons == ev->buttons) {
            queue[last].dx = (int16_t)(queue[last].dx + ev->dx);
            queue[last].dy = (int16_t)(queue[last].dy + ev->dy);
            queue[last].dz = (int16_t)(queue[last].dz + ev->dz);
        }
        return;
    }
    queue[qhead] = *ev;
    qhead = next;
}

bool input_pop(input_event_t *ev) {
    uint32_t f = irq_save();
    bool ok = qtail != qhead;
    if (ok) {
        *ev = queue[qtail];
        qtail = (qtail + 1) % QSIZE;
    }
    irq_restore(f);
    return ok;
}

bool input_pending(void) { return qtail != qhead; }
