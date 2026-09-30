/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - interrupt.c                                              *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define M64P_CORE_PROTOTYPES 1

#include "interrupt.h"

#ifdef __MINGW32__
#define _CRT_RAND_S
#endif
#include <stdlib.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "device/pif/bootrom_hle.h"
#include "device/r4300/cp0.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/ai_controller.h"
#include "device/rcp/rdp/vi_controller.h"
#include "main/main.h"


/***************************************************************************
 * Pool of Single Linked List Nodes
 **************************************************************************/

static struct node* alloc_node(struct pool* p);
static void free_node(struct pool* p, struct node* node);
static void clear_pool(struct pool* p);


/* node allocation/deallocation on a given pool */
static struct node* alloc_node(struct pool* p)
{
    /* return NULL if pool is too small */
    if (p->index >= INTERRUPT_NODES_POOL_CAPACITY) {
        return NULL;
    }

    return p->stack[p->index++];
}

static void free_node(struct pool* p, struct node* node)
{
    if (p->index == 0 || node == NULL) {
        return;
    }

    p->stack[--p->index] = node;
}

/* release all nodes */
static void clear_pool(struct pool* p)
{
    size_t i;

    for (i = 0; i < INTERRUPT_NODES_POOL_CAPACITY; ++i) {
        p->stack[i] = &p->nodes[i];
    }

    p->index = 0;
}

/***************************************************************************
 * Interrupt Queue
 **************************************************************************/

static void clear_queue(struct interrupt_queue* q)
{
    q->first = NULL;
    clear_pool(&q->pool);
}

static void update_next_interrupt(struct cp0* cp0)
{
    if (cp0->q.first == NULL)
    {
        cp0->next_interrupt = 0;
        cp0->cycle_count = 0;
        return;
    }

    cp0->next_interrupt = cp0->q.first->data.count;
    cp0->cycle_count = cp0->count_clock - cp0->q.first->data.deadline;
}

static void insert_interrupt_event(struct cp0* cp0, int type, uint32_t count, int64_t deadline)
{
    struct node* event;
    struct node* e;

    if (get_event(&cp0->q, type)) {
        DebugMessage(M64MSG_WARNING, "two events of type 0x%x in interrupt queue", type);
    }

    event = alloc_node(&cp0->q.pool);
    if (event == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Failed to allocate node for new interrupt event");
        return;
    }

    event->data.count = count;
    event->data.type = type;
    event->data.deadline = deadline;

    if (cp0->q.first == NULL || deadline < cp0->q.first->data.deadline)
    {
        event->next = cp0->q.first;
        cp0->q.first = event;
    }
    else
    {
        e = cp0->q.first;
        while (e->next != NULL && e->next->data.deadline <= deadline)
            e = e->next;

        event->next = e->next;
        e->next = event;
    }

    update_next_interrupt(cp0);
}

unsigned int add_random_interrupt_time(struct r4300_core* r4300)
{
    if (r4300->randomize_interrupt) {
        unsigned int value;
#ifdef __MINGW32__
        rand_s(&value);
#else
        value = rand();
#endif
        return value % 0x40;
    } else
        return 0;
}

void add_interrupt_event(struct cp0* cp0, int type, unsigned int delay)
{
    const uint32_t count = cp0->regs[CP0_COUNT_REG] + delay;
    insert_interrupt_event(cp0, type, count, cp0->count_clock + delay);
}

void add_interrupt_event_count(struct cp0* cp0, int type, unsigned int count)
{
    /* Device events expressed as an absolute COUNT value are always close to
     * the current time.  Interpret the modular difference as a signed 32-bit
     * displacement so a slightly overdue periodic event stays overdue instead
     * of being moved almost a full COUNT wrap into the future. */
    const int64_t delta = (int32_t)(count - cp0->regs[CP0_COUNT_REG]);
    insert_interrupt_event(cp0, type, count, cp0->count_clock + delta);
}

void remove_interrupt_event(struct cp0* cp0)
{
    struct node* e;

    e = cp0->q.first;
    if (e == NULL)
        return;

    cp0->q.first = e->next;
    free_node(&cp0->q.pool, e);
    update_next_interrupt(cp0);
}

unsigned int* get_event(const struct interrupt_queue* q, int type)
{
    struct node* e = q->first;

    if (e == NULL) {
        return NULL;
    }

    if (e->data.type == type) {
        return &e->data.count;
    }

    for (; e->next != NULL && e->next->data.type != type; e = e->next);

    return (e->next != NULL)
        ? &e->next->data.count
        : NULL;
}

int get_next_event_type(const struct interrupt_queue* q)
{
    return (q->first == NULL)
        ? 0
        : q->first->data.type;
}

void remove_event(struct interrupt_queue* q, int type)
{
    struct node* to_del;
    struct node* e = q->first;

    if (e == NULL) {
        return;
    }

    if (e->data.type == type)
    {
        q->first = e->next;
        free_node(&q->pool, e);
    }
    else
    {
        for (; e->next != NULL && e->next->data.type != type; e = e->next);

        if (e->next != NULL)
        {
            to_del = e->next;
            e->next = to_del->next;
            free_node(&q->pool, to_del);
        }
    }
}

void schedule_compare(struct cp0* cp0)
{
    int64_t delta = (uint32_t)(cp0->regs[CP0_COMPARE_REG] - cp0->regs[CP0_COUNT_REG]);

    remove_event(&cp0->q, COMPARE_INT);

    /* CP0 COMPARE matches on equality of the 32-bit COUNT register.  If both
     * registers are currently equal, the next equality is one complete wrap
     * later, not immediately.  A 64-bit internal deadline can represent that
     * directly and removes the need for a fake half-wrap event. */
    if (delta == 0)
        delta = INT64_C(0x100000000);

    insert_interrupt_event(cp0, COMPARE_INT, cp0->regs[CP0_COMPARE_REG], cp0->count_clock + delta);
}

void translate_event_queue(struct cp0* cp0, unsigned int base)
{
    struct node* e;
    uint32_t* cp0_regs = r4300_cp0_regs(cp0);
    const uint32_t old_count = cp0_regs[CP0_COUNT_REG];

    remove_event(&cp0->q, COMPARE_INT);

    for (e = cp0->q.first; e != NULL; e = e->next)
    {
        e->data.count = (e->data.count - old_count) + base;
    }

    cp0_regs[CP0_COUNT_REG] = base;
    cp0->count_phase = 0;

    schedule_compare(cp0);
}

int save_eventqueue_infos(const struct cp0* cp0, char *buf)
{
    int len;
    struct node* e;

    len = 0;

    for (e = cp0->q.first; e != NULL; e = e->next)
    {
        memcpy(buf + len    , &e->data.type , 4);
        memcpy(buf + len + 4, &e->data.count, 4);
        len += 8;
    }

    *((unsigned int*)&buf[len]) = 0xFFFFFFFF;
    return len+4;
}

void load_eventqueue_infos(struct cp0* cp0, const char *buf)
{
    int len = 0;
    enum { LEGACY_HALF_WRAP_EVENT = 0x020 };

    clear_queue(&cp0->q);
    cp0->count_clock = 0;
    cp0->cycle_count = 0;

    while (*((const unsigned int*)&buf[len]) != 0xFFFFFFFF)
    {
        int type = *((const unsigned int*)&buf[len]);
        unsigned int count = *((const unsigned int*)&buf[len+4]);
        /* The old fake half-wrap event existed only to keep a signed 32-bit scheduler delta
         * inside +/-2^31.  Ignore it when loading legacy states.  Rebuild
         * COMPARE below from the architectural registers so old states which
         * intentionally omitted a distant COMPARE event also load correctly. */
        if (type != LEGACY_HALF_WRAP_EVENT && type != COMPARE_INT)
            add_interrupt_event_count(cp0, type, count);
        len += 8;
    }

    schedule_compare(cp0);
}

void init_interrupt(struct cp0* cp0)
{
    clear_queue(&cp0->q);
    cp0->count_clock = 0;
    cp0->cycle_count = 0;
    schedule_compare(cp0);
}

void r4300_check_interrupt(struct r4300_core* r4300, uint32_t cause_ip, int set_cause)
{
    struct node* event;
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);

    if (set_cause) {
        cp0_regs[CP0_CAUSE_REG] = cp0_regs[CP0_CAUSE_REG] | cause_ip;
    }
    else {
        cp0_regs[CP0_CAUSE_REG] &= ~cause_ip;
    }

    if ((cp0_regs[CP0_STATUS_REG] & (CP0_STATUS_IE | CP0_STATUS_EXL | CP0_STATUS_ERL)) != CP0_STATUS_IE) {
        return;
    }
    if (cp0_regs[CP0_STATUS_REG] & cp0_regs[CP0_CAUSE_REG] & UINT32_C(0xFF00))
    {
        if (get_event(&r4300->cp0.q, CHECK_INT) != NULL) return;
        event = alloc_node(&r4300->cp0.q.pool);
        if (event == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Failed to allocate node for new interrupt event");
            return;
        }

        /* Preserve the original CHECK_INT priority: an interrupt becoming
         * unmasked is serviced before any already queued event, even if the
         * latter is slightly overdue from instruction-granularity stepping. */
        event->data.count = cp0_regs[CP0_COUNT_REG];
        event->data.type = CHECK_INT;
        event->data.deadline = r4300->cp0.count_clock;
        event->next = r4300->cp0.q.first;
        r4300->cp0.q.first = event;
        r4300->cp0.next_interrupt = cp0_regs[CP0_COUNT_REG];
        r4300->cp0.cycle_count = 0;
    }
}

void raise_maskable_interrupt(struct r4300_core* r4300, uint32_t cause_ip)
{
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);
    cp0_regs[CP0_CAUSE_REG] |= cause_ip;

    if (!(cp0_regs[CP0_STATUS_REG] & cp0_regs[CP0_CAUSE_REG] & UINT32_C(0xff00))) {
        return;
    }

    if ((cp0_regs[CP0_STATUS_REG] & (CP0_STATUS_IE | CP0_STATUS_EXL | CP0_STATUS_ERL)) != CP0_STATUS_IE) {
        return;
    }

    cp0_regs[CP0_CAUSE_REG] &= ~UINT32_C(0x3000007c);
    exception_general(r4300);
}

void compare_int_handler(void* opaque)
{
    struct r4300_core* r4300 = (struct r4300_core*)opaque;
    schedule_compare(&r4300->cp0);

    raise_maskable_interrupt(r4300, CP0_CAUSE_IP7);
}

void check_int_handler(void* opaque)
{
    raise_maskable_interrupt((struct r4300_core*)opaque, 0);
}

/* XXX: this should only require r4300 struct not device ? */
/* XXX: This is completly WTF ! */
void nmi_int_handler(void* opaque)
{
    struct device* dev = (struct device*)opaque;
    struct r4300_core* r4300 = &dev->r4300;
    uint32_t* cp0_regs = r4300_cp0_regs(&r4300->cp0);

    reset_pif(&dev->pif, 1);

    // setup r4300 Status flags: reset TS and SR, set BEV, ERL, and SR
    cp0_regs[CP0_STATUS_REG] = (cp0_regs[CP0_STATUS_REG] & ~(CP0_STATUS_SR | CP0_STATUS_TS | UINT32_C(0x00080000))) | (CP0_STATUS_ERL | CP0_STATUS_BEV | CP0_STATUS_SR);
    cp0_regs[CP0_CAUSE_REG]  = 0x00000000;
    // simulate the soft reset code which would run from the PIF ROM
    pif_bootrom_hle_execute(r4300);
    // clear all interrupts, reset interrupt counters back to 0
    cp0_regs[CP0_COUNT_REG] = 0;
    r4300->cp0.count_phase = 0;
    g_gs_vi_counter = 0;
    init_interrupt(&r4300->cp0);

    vi_schedule_vertical_interrupt(&dev->vi);

    // clear the audio status register so that subsequent write_ai() calls will work properly
    dev->ai.regs[AI_STATUS_REG] = 0;
    // set ErrorEPC with the last instruction address
    cp0_regs[CP0_ERROREPC_REG] = *r4300_pc(r4300);
    r4300->cp0.regs_hi[CP0_ERROREPC_REG] = (int32_t)*r4300_pc(r4300) < 0 ? UINT32_MAX : 0;
    // adjust ErrorEPC if we were in a delay slot, and clear the r4300->delay_slot and r4300->recomp.dyna_interp flags
    if(r4300->delay_slot==1 || r4300->delay_slot==3)
    {
        cp0_regs[CP0_ERROREPC_REG]-=4;
    }
    r4300->delay_slot = 0;

    // set next instruction address to reset vector
    r4300->cp0.last_addr = r4300->start_address;
    generic_jump_to(r4300, r4300->start_address);
}


/* XXX: needs to be properly reworked */
void reset_hard_handler(void* opaque)
{
    struct device* dev = (struct device*)opaque;
    struct r4300_core* r4300 = &dev->r4300;

    poweron_device(dev);

    pif_bootrom_hle_execute(r4300);
    r4300->cp0.last_addr = r4300->start_address;
    *r4300_cp0_next_interrupt(&r4300->cp0) = 624999;
    *r4300_cp0_cycle_count(&r4300->cp0) = 0;
    init_interrupt(&r4300->cp0);
    *r4300_pc_struct(r4300) = &r4300->interp_PC;
    generic_jump_to(r4300, r4300->cp0.last_addr);
}


static void call_interrupt_handler(const struct cp0* cp0, size_t index)
{
    assert(index < CP0_INTERRUPT_HANDLERS_COUNT);

    const struct interrupt_handler* handler = &cp0->interrupt_handlers[index];

    handler->callback(handler->opaque);
}

void gen_interrupt(struct r4300_core* r4300)
{
    if (*r4300_stop(r4300) == 1)
    {
        g_gs_vi_counter = 0; // debug
    }

    if (!r4300->cp0.interrupt_unsafe_state)
    {

        if (r4300->reset_hard_job)
        {
            call_interrupt_handler(&r4300->cp0, 10);
            return;
        }
    }

    if (r4300->skip_jump)
    {
        uint32_t dest = r4300->skip_jump;
        r4300->skip_jump = 0;
        update_next_interrupt(&r4300->cp0);

        r4300->cp0.last_addr = dest;
        generic_jump_to(r4300, dest);
        return;
    }

    switch (r4300->cp0.q.first->data.type)
    {
        case VI_INT:
            call_interrupt_handler(&r4300->cp0, 0);
            break;

        case COMPARE_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 1);
            break;

        case CHECK_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 2);
            break;

        case SI_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 3);
            break;

        case PI_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 4);
            break;

        case AI_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 5);
            break;

        case SP_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 6);
            break;

        case DP_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 7);
            break;

        case HW2_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 8);
            break;

        case NMI_INT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 9);
            break;

        case RSP_DMA_EVT:
            remove_interrupt_event(&r4300->cp0);
            call_interrupt_handler(&r4300->cp0, 11);
            break;

        default:
            DebugMessage(M64MSG_ERROR, "Unknown interrupt queue event type %.8X.", r4300->cp0.q.first->data.type);
            remove_interrupt_event(&r4300->cp0);
            exception_general(r4300);
            break;
    }
}

