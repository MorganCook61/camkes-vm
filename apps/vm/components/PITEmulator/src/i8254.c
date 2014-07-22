/*
 * @TAG(OTHER_BSD)
 */

/*
 * QEMU 8253/8254 interval timer emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <autoconf.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include <stdio.h>
#include <PITEmulator.h>

//#define DEBUG_PIT

#define RW_STATE_LSB 1
#define RW_STATE_MSB 2
#define RW_STATE_WORD0 3
#define RW_STATE_WORD1 4

#define PIT_FREQ 1193182

typedef struct PITChannelState {
    int count; /* can be 65536 */
    uint16_t latched_count;
    uint8_t count_latched;
    uint8_t status_latched;
    uint8_t status;
    uint8_t read_state;
    uint8_t write_state;
    uint8_t write_latch;
    uint8_t rw_mode;
    uint8_t mode;
    uint8_t bcd; /* not supported */
    uint8_t gate; /* timer start */
    int64_t count_load_time;
    /* irq handling */
    int64_t next_transition_time;
//    QEMUTimer *irq_timer;
    int irq_timer;
//    qemu_irq irq;
    int irq_level;
    uint64_t timer_status;
} PITChannelState;

typedef struct PITState {
//    ISADevice dev;
//    MemoryRegion ioports;
    uint32_t irq;
    uint32_t iobase;
    PITChannelState channels[3];
} PITState;

static PITState pit_state;

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time);

/* defines nanoseconds per second */
static inline int64_t get_ticks_per_sec(void)
{
    return 1000000000LL;
}

/* compute with 96 bit intermediate result: (a*b)/c */
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{
    union {
        uint64_t ll;
        struct {
#ifdef HOST_WORDS_BIGENDIAN
            uint32_t high, low;
#else
            uint32_t low, high;
#endif
        } l;
    } u, res;
    uint64_t rl, rh;

    u.ll = a;
    rl = (uint64_t)u.l.low * (uint64_t)b;
    rh = (uint64_t)u.l.high * (uint64_t)b;
    rh += (rl >> 32);
    res.l.high = rh / c;
    res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
    return res.ll;
}

static int pit_get_count(PITChannelState *s)
{
    uint64_t d;
    int counter;

//    d = muldiv64(qemu_get_clock_ns(vm_clock) - s->count_load_time, PIT_FREQ,
    d = muldiv64(timer_time() - s->count_load_time, PIT_FREQ,
                 get_ticks_per_sec());
    switch(s->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        counter = (s->count - d) & 0xffff;
        break;
    case 3:
        /* XXX: may be incorrect for odd counts */
        counter = s->count - ((2 * d) % s->count);
        break;
    default:
        counter = s->count - (d % s->count);
        break;
    }
    return counter;
}

/* get pit output bit */
static int pit_get_out1(PITChannelState *s, int64_t current_time)
{
    uint64_t d;
    int out;

    d = muldiv64(current_time - s->count_load_time, PIT_FREQ,
                 get_ticks_per_sec());
    switch(s->mode) {
    default:
    case 0:
        out = (d >= s->count);
        break;
    case 1:
        out = (d < s->count);
        break;
    case 2:
        if ((d % s->count) == 0 && d != 0)
            out = 1;
        else
            out = 0;
        break;
    case 3:
        out = (d % s->count) < ((s->count + 1) >> 1);
        break;
    case 4:
    case 5:
        out = (d == s->count);
        break;
    }
    return out;
}

#if 0
int pit_get_out(ISADevice *dev, int channel, int64_t current_time)
{
    PITState *pit = DO_UPCAST(PITState, dev, dev);
    PITChannelState *s = &pit->channels[channel];
    return pit_get_out1(s, current_time);
}
#endif

/* return -1 if no transition will occur.  */
static int64_t pit_get_next_transition_time(PITChannelState *s,
                                            int64_t current_time)
{
    uint64_t d, next_time, base;
    int period2;

    d = muldiv64(current_time - s->count_load_time, PIT_FREQ,
                 get_ticks_per_sec());
    switch(s->mode) {
    default:
    case 0:
    case 1:
        if (d < s->count)
            next_time = s->count;
        else
            return -1;
        break;
    case 2:
        base = (d / s->count) * s->count;
        if ((d - base) == 0 && d != 0)
            next_time = base + s->count;
        else
            next_time = base + s->count + 1;
        break;
    case 3:
        base = (d / s->count) * s->count;
        period2 = ((s->count + 1) >> 1);
        if ((d - base) < period2)
            next_time = base + period2;
        else
            next_time = base + s->count;
        break;
    case 4:
    case 5:
        if (d < s->count)
            next_time = s->count;
        else if (d == s->count)
            next_time = s->count + 1;
        else
            return -1;
        break;
    }
    /* convert to timer units */
    next_time = s->count_load_time + muldiv64(next_time, get_ticks_per_sec(),
                                              PIT_FREQ);
    /* fix potential rounding problems */
    /* XXX: better solution: use a clock at PIT_FREQ Hz */
    if (next_time <= current_time)
        next_time = current_time + 1;
    return next_time;
}

#if 0
/* val must be 0 or 1 */
void pit_set_gate(ISADevice *dev, int channel, int val)
{
    PITState *pit = DO_UPCAST(PITState, dev, dev);
    PITChannelState *s = &pit->channels[channel];

    switch(s->mode) {
    default:
    case 0:
    case 4:
        /* XXX: just disable/enable counting */
        break;
    case 1:
    case 5:
        if (s->gate < val) {
            /* restart counting on rising edge */
            s->count_load_time = qemu_get_clock_ns(vm_clock);
            pit_irq_timer_update(s, s->count_load_time);
        }
        break;
    case 2:
    case 3:
        if (s->gate < val) {
            /* restart counting on rising edge */
            s->count_load_time = qemu_get_clock_ns(vm_clock);
            pit_irq_timer_update(s, s->count_load_time);
        }
        /* XXX: disable/enable counting */
        break;
    }
    s->gate = val;
}

int pit_get_gate(ISADevice *dev, int channel)
{
    PITState *pit = DO_UPCAST(PITState, dev, dev);
    PITChannelState *s = &pit->channels[channel];
    return s->gate;
}

int pit_get_initial_count(ISADevice *dev, int channel)
{
    PITState *pit = DO_UPCAST(PITState, dev, dev);
    PITChannelState *s = &pit->channels[channel];
    return s->count;
}

int pit_get_mode(ISADevice *dev, int channel)
{
    PITState *pit = DO_UPCAST(PITState, dev, dev);
    PITChannelState *s = &pit->channels[channel];
    return s->mode;
}
#endif

static inline void pit_load_count(PITChannelState *s, int val)
{
    if (val == 0)
        val = 0x10000;
//    s->count_load_time = qemu_get_clock_ns(vm_clock);
    s->count_load_time = timer_time();
    s->count = val;
    pit_irq_timer_update(s, s->count_load_time);
}

/* if already latched, do not latch again */
static void pit_latch_count(PITChannelState *s)
{
    if (!s->count_latched) {
        s->latched_count = pit_get_count(s);
        s->count_latched = s->rw_mode;
    }
}

static void pit_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    PITState *pit = opaque;
    int channel, access;
    PITChannelState *s;

    addr &= 3;
    if (addr == 3) {
        channel = val >> 6;
        if (channel == 3) {
            /* read back command */
            for(channel = 0; channel < 3; channel++) {
                s = &pit->channels[channel];
                if (val & (2 << channel)) {
                    if (!(val & 0x20)) {
                        pit_latch_count(s);
                    }
                    if (!(val & 0x10) && !s->status_latched) {
                        /* status latch */
                        /* XXX: add BCD and null count */
//                        s->status =  (pit_get_out1(s, qemu_get_clock_ns(vm_clock)) << 7) |
                        s->status =  (pit_get_out1(s, timer_time()) << 7) |
                            (s->rw_mode << 4) |
                            (s->mode << 1) |
                            s->bcd;
                        s->status_latched = 1;
                    }
                }
            }
        } else {
            s = &pit->channels[channel];
            access = (val >> 4) & 3;
            if (access == 0) {
                pit_latch_count(s);
            } else {
                s->rw_mode = access;
                s->read_state = access;
                s->write_state = access;

                s->mode = (val >> 1) & 7;
                s->bcd = val & 1;
                /* XXX: update irq timer ? */
            }
        }
    } else {
        s = &pit->channels[addr];
        switch(s->write_state) {
        default:
        case RW_STATE_LSB:
            pit_load_count(s, val);
            break;
        case RW_STATE_MSB:
            pit_load_count(s, val << 8);
            break;
        case RW_STATE_WORD0:
            s->write_latch = val;
            s->write_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            pit_load_count(s, s->write_latch | (val << 8));
            s->write_state = RW_STATE_WORD0;
            break;
        }
    }
}

static uint32_t pit_ioport_read(void *opaque, uint32_t addr)
{
    PITState *pit = opaque;
    int ret, count;
    PITChannelState *s;

    addr &= 3;
    s = &pit->channels[addr];
    if (s->status_latched) {
        s->status_latched = 0;
        ret = s->status;
    } else if (s->count_latched) {
        switch(s->count_latched) {
        default:
        case RW_STATE_LSB:
            ret = s->latched_count & 0xff;
            s->count_latched = 0;
            break;
        case RW_STATE_MSB:
            ret = s->latched_count >> 8;
            s->count_latched = 0;
            break;
        case RW_STATE_WORD0:
            ret = s->latched_count & 0xff;
            s->count_latched = RW_STATE_MSB;
            break;
        }
    } else {
        switch(s->read_state) {
        default:
        case RW_STATE_LSB:
            count = pit_get_count(s);
            ret = count & 0xff;
            break;
        case RW_STATE_MSB:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            break;
        case RW_STATE_WORD0:
            count = pit_get_count(s);
            ret = count & 0xff;
            s->read_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            s->read_state = RW_STATE_WORD0;
            break;
        }
    }
    return ret;
}

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time)
{
    int64_t expire_time;
    int irq_level;

    if (!s->irq_timer)
        return;
    expire_time = pit_get_next_transition_time(s, current_time);
    irq_level = pit_get_out1(s, current_time);
//    qemu_set_irq(s->irq, irq_level);
    if (irq_level) {
        pit_edge_irq_emit();
//        if (!s->irq_level) {
//            pit_irq_raise();
//            s->irq_level = 1;
//        }
    } else {
//        if (s->irq_level) {
//            pit_irq_lower();
//            s->irq_level = 0;
//        }
    }
#ifdef DEBUG_PIT
    printf("irq_level=%d next_delay=%f\n",
           irq_level,
           (double)(expire_time - current_time) / get_ticks_per_sec());
#endif
    s->next_transition_time = expire_time;
/*    if (expire_time != -1)
        qemu_mod_timer(s->irq_timer, expire_time);
    else
        qemu_del_timer(s->irq_timer);*/
    if (expire_time != -1) {
        if (s->timer_status != expire_time) {
            timer_oneshot_absolute(expire_time);
            s->timer_status = expire_time;
        }
    }
    else {
        if (s->timer_status) {
            timer_stop();
            s->timer_status = 0;
        }
    }
}
#if 0

static void pit_irq_timer(void *opaque)
{
    PITChannelState *s = opaque;

    pit_irq_timer_update(s, s->next_transition_time);
}

static const VMStateDescription vmstate_pit_channel = {
    .name = "pit channel",
    .version_id = 2,
    .minimum_version_id = 2,
    .minimum_version_id_old = 2,
    .fields      = (VMStateField []) {
        VMSTATE_INT32(count, PITChannelState),
        VMSTATE_UINT16(latched_count, PITChannelState),
        VMSTATE_UINT8(count_latched, PITChannelState),
        VMSTATE_UINT8(status_latched, PITChannelState),
        VMSTATE_UINT8(status, PITChannelState),
        VMSTATE_UINT8(read_state, PITChannelState),
        VMSTATE_UINT8(write_state, PITChannelState),
        VMSTATE_UINT8(write_latch, PITChannelState),
        VMSTATE_UINT8(rw_mode, PITChannelState),
        VMSTATE_UINT8(mode, PITChannelState),
        VMSTATE_UINT8(bcd, PITChannelState),
        VMSTATE_UINT8(gate, PITChannelState),
        VMSTATE_INT64(count_load_time, PITChannelState),
        VMSTATE_INT64(next_transition_time, PITChannelState),
        VMSTATE_END_OF_LIST()
    }
};

static int pit_load_old(QEMUFile *f, void *opaque, int version_id)
{
    PITState *pit = opaque;
    PITChannelState *s;
    int i;

    if (version_id != 1)
        return -EINVAL;

    for(i = 0; i < 3; i++) {
        s = &pit->channels[i];
        s->count=qemu_get_be32(f);
        qemu_get_be16s(f, &s->latched_count);
        qemu_get_8s(f, &s->count_latched);
        qemu_get_8s(f, &s->status_latched);
        qemu_get_8s(f, &s->status);
        qemu_get_8s(f, &s->read_state);
        qemu_get_8s(f, &s->write_state);
        qemu_get_8s(f, &s->write_latch);
        qemu_get_8s(f, &s->rw_mode);
        qemu_get_8s(f, &s->mode);
        qemu_get_8s(f, &s->bcd);
        qemu_get_8s(f, &s->gate);
        s->count_load_time=qemu_get_be64(f);
        if (s->irq_timer) {
            s->next_transition_time=qemu_get_be64(f);
            qemu_get_timer(f, s->irq_timer);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_pit = {
    .name = "i8254",
    .version_id = 2,
    .minimum_version_id = 2,
    .minimum_version_id_old = 1,
    .load_state_old = pit_load_old,
    .fields      = (VMStateField []) {
        VMSTATE_STRUCT_ARRAY(channels, PITState, 3, 2, vmstate_pit_channel, PITChannelState),
        VMSTATE_TIMER(channels[0].irq_timer, PITState),
        VMSTATE_END_OF_LIST()
    }
};
#endif

//static void pit_reset(DeviceState *dev)
static void pit_reset(PITState *pit)
{
//    PITState *pit = container_of(dev, PITState, dev.qdev);
    PITChannelState *s;
    int i;

    for(i = 0;i < 3; i++) {
        s = &pit->channels[i];
        s->mode = 3;
        s->gate = (i != 2);
        pit_load_count(s, 0);
    }
}

#if 0
/* When HPET is operating in legacy mode, i8254 timer0 is disabled */
void hpet_pit_disable(void) {
    PITChannelState *s;
    s = &pit_state.channels[0];
    if (s->irq_timer)
        qemu_del_timer(s->irq_timer);
}

/* When HPET is reset or leaving legacy mode, it must reenable i8254
 * timer 0
 */

void hpet_pit_enable(void)
{
    PITState *pit = &pit_state;
    PITChannelState *s;
    s = &pit->channels[0];
    s->mode = 3;
    s->gate = 1;
    pit_load_count(s, 0);
}

static const MemoryRegionPortio pit_portio[] = {
    { 0, 4, 1, .write = pit_ioport_write },
    { 0, 3, 1, .read = pit_ioport_read },
    PORTIO_END_OF_LIST()
};

static const MemoryRegionOps pit_ioport_ops = {
    .old_portio = pit_portio
};

static int pit_initfn(ISADevice *dev)
{
    PITState *pit = DO_UPCAST(PITState, dev, dev);
    PITChannelState *s;

    s = &pit->channels[0];
    /* the timer 0 is connected to an IRQ */
    s->irq_timer = qemu_new_timer_ns(vm_clock, pit_irq_timer, s);
    s->irq = isa_get_irq(pit->irq);

    memory_region_init_io(&pit->ioports, &pit_ioport_ops, pit, "pit", 4);
    isa_register_ioport(dev, &pit->ioports, pit->iobase);

    qdev_set_legacy_instance_id(&dev->qdev, pit->iobase, 2);

    return 0;
}

static ISADeviceInfo pit_info = {
    .qdev.name     = "isa-pit",
    .qdev.size     = sizeof(PITState),
    .qdev.vmsd     = &vmstate_pit,
    .qdev.reset    = pit_reset,
    .qdev.no_user  = 1,
    .init          = pit_initfn,
    .qdev.props = (Property[]) {
        DEFINE_PROP_UINT32("irq", PITState, irq,  -1),
        DEFINE_PROP_HEX32("iobase", PITState, iobase,  -1),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void pit_register(void)
{
    isa_qdev_register(&pit_info);
}
device_init(pit_register)
#endif

static void timer_interrupt(void *cookie) {
    pit_lock();
    PITState *pit = (PITState*)cookie;
    PITChannelState *s;
    s = &pit->channels[0];
    s->timer_status = 0;
    pit_irq_timer_update(s, s->next_transition_time);
    timer_interrupt_reg_callback(timer_interrupt, cookie);
    pit_unlock();
}

void pre_init(void) {
    pit_lock();
    set_putchar(putchar_putchar);
    for (int i = 0; i < 3; i++) {
        pit_state.channels[i].irq_level = 0;
        pit_state.channels[i].timer_status = 0;
        pit_state.channels[i].irq_timer = 0;
    }
    pit_state.channels[0].irq_timer = 1;
    pit_reset(&pit_state);
    timer_interrupt_reg_callback(timer_interrupt, &pit_state);
    pit_unlock();
}

int i8254port_port_in(unsigned int port_no, unsigned int size, unsigned int *result) {
    if (size != 1) {
        LOG_ERROR("i8254 only supports reads of size 1");
        return -1;
    }
    pit_lock();
    *result = pit_ioport_read(&pit_state, port_no);
    pit_unlock();
    return 0;
}

int i8254port_port_out(unsigned int port_no, unsigned int size, unsigned int value) {
    if (size != 1) {
        LOG_ERROR("i8254 only supports writes of size 1");
        return -1;
    }
    pit_lock();
    pit_ioport_write(&pit_state, port_no, value);
    pit_unlock();
    return 0;
}
