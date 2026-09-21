
#include "guest_threads.h"
#include "a64_runtime.h"
#include "guest_runtime.h"
#include "elf_loader.h"
#include "host_time.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MR_MAX_THREADS 48
#define MR_MAX_MUTEXES 512
#define MR_MAX_CONDS 256
#define MR_MAX_ONCE 256
#define MR_MAX_TLS_KEYS 128
#define MR_BIONIC_TLS_SLOTS 64
#define MR_TLS_FIRST_USER 5
#define MR_TLS_AREA_OFFSET 0x100u
#define MR_MAIN_STACK (16u * 1024u * 1024u)
#define MR_THREAD_STACK (8u * 1024u * 1024u)
#define MR_DEFAULT_QUANTUM 8192u
#define MR_MIN_QUANTUM 64u
#define MR_MAX_QUANTUM 1000000u
#define MR_IDLE_SLICE_NS 2000000ull // Two milliseconds between idle checks.
#define MR_DESTRUCTOR_PASSES 4
#define MR_ONCE_NESTING 16

#define MR_THREAD_RETURN_MAGIC 0xFFFF0010u
#define MR_ONCE_RETURN_MAGIC 0xFFFF0020u
#define MR_TLS_RETURN_MAGIC 0xFFFF0030u

#define MR_ATTR_MAGIC 0x41545452u // "ATTR"
#define MR_MUTEXATTR_TYPE_MASK 0x000Fu

// pthread mutex types using bionic-compatible values.
enum { MR_MUTEX_NORMAL = 0, MR_MUTEX_RECURSIVE = 1, MR_MUTEX_ERRORCHECK = 2 };
enum { MR_CREATE_JOINABLE = 0, MR_CREATE_DETACHED = 1 };

typedef enum { TS_UNUSED = 0, TS_RUNNABLE, TS_BLOCKED, TS_ZOMBIE } thread_state;

typedef enum {
    WAIT_NONE = 0,
    WAIT_MUTEX,
    WAIT_COND,
    WAIT_COND_MUTEX,
    WAIT_JOIN,
    WAIT_SLEEP,
    WAIT_ONCE,
    WAIT_FUTEX,
    WAIT_CALLBACK
} wait_kind;

typedef struct {
    uint32_t addr;
    uint32_t owner;
    uint32_t recursion;
    int type;
    int used;
} guest_mutex;

typedef struct {
    uint32_t addr;
    int used;
} guest_cond;

typedef struct {
    uint32_t addr;
    uint32_t owner;
    int state; // 0 new, 1 running, 2 complete.
    int used;
} guest_once;

typedef struct {
    int used;
    uint32_t destructor;
} guest_tls_key;

typedef struct mr_guest_thread {
    int used;
    thread_state state;
    uint32_t tid;
    int detached;
    int joined;

    mr_cpu owned_cpu;
    mr_cpu *cpu;
    uint32_t stack_low, stack_high;
    uint32_t errno_addr;
    uint32_t tls_addr;
    int errno_value;

    uint32_t retval;
    wait_kind wait;
    uint32_t wait_obj;
    uint32_t wait_aux;
    uint64_t deadline_ns;
    int wait_result;
    int wait_ready;

    // Return information for nested pthread_once initializers.
    uint32_t once_addr[MR_ONCE_NESTING];
    uint32_t once_saved_lr[MR_ONCE_NESTING];
    int once_depth;

    // TLS destructor execution.
    uint32_t tls[MR_MAX_TLS_KEYS];
    int exiting;
    int dtor_pass;
    int dtor_index;
    int dtor_ran;
    int callback_worker;
    int callback_started;
} mr_guest_thread;

struct mr_scheduler {
    mr_cpu *main_cpu;
    mr_guest_thread threads[MR_MAX_THREADS];
    int capacity;
    int current;
    uint32_t next_tid;
    uint32_t total_created;
    uint32_t quantum;
    int running;
    void (*audio_pump)(mr_cpu *);

    guest_mutex mutexes[MR_MAX_MUTEXES];
    guest_cond conds[MR_MAX_CONDS];
    guest_once once[MR_MAX_ONCE];
    guest_tls_key keys[MR_MAX_TLS_KEYS];
};

#define A0 (c->r[0])
#define A1 (c->r[1])
#define A2 (c->r[2])
#define A3 (c->r[3])
#define RET(x) (c->r[0] = (uint32_t)(x))

static void host_sleep_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static mr_scheduler *sched_of(const mr_cpu *c) {
    return c ? c->scheduler : NULL;
}

static mr_guest_thread *thread_by_cpu(mr_scheduler *s, const mr_cpu *c) {
    if (!s || !c) return NULL;
    for (int i = 0; i < s->capacity; i++)
        if (s->threads[i].used && s->threads[i].cpu == c) return &s->threads[i];
    return NULL;
}

static mr_guest_thread *thread_by_tid(mr_scheduler *s, uint32_t tid) {
    if (!s || !tid) return NULL;
    for (int i = 0; i < s->capacity; i++)
        if (s->threads[i].used && s->threads[i].tid == tid) return &s->threads[i];
    return NULL;
}

static void init_guest_tls(mr_guest_thread *t) {
    if (!t || !t->cpu) return;
    t->tls_addr = t->stack_low + MR_TLS_AREA_OFFSET;
    if (!mr_mem_ok(t->cpu, t->tls_addr, MR_BIONIC_TLS_SLOTS * 4u)) {
        t->tls_addr = 0;
        return;
    }
    memset(mr_mem(t->cpu, t->tls_addr), 0, MR_BIONIC_TLS_SLOTS * 4u);
    // bionic well-known slots: self, thread descriptor, and errno.
    mr_st32(t->cpu, t->tls_addr + 0u * 4u, t->tls_addr);
    mr_st32(t->cpu, t->tls_addr + 1u * 4u, t->tid);
    mr_st32(t->cpu, t->tls_addr + 2u * 4u, t->errno_addr);
}

static void set_guest_errno(mr_guest_thread *t, int value) {
    if (!t || !t->cpu) return;
    t->errno_value = value;
    if (t->errno_addr && mr_mem_ok(t->cpu, t->errno_addr, 4))
        mr_st32(t->cpu, t->errno_addr, (uint32_t)value);
}

static void clear_wait(mr_guest_thread *t) {
    t->wait = WAIT_NONE;
    t->wait_obj = 0;
    t->wait_aux = 0;
    t->deadline_ns = 0;
    t->wait_result = 0;
    t->wait_ready = 0;
}

static void block_current(mr_cpu *c, wait_kind kind, uint32_t obj, uint32_t aux,
                          uint64_t deadline) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    if (!t) {
        RET(EINVAL);
        return;
    }
    t->state = TS_BLOCKED;
    t->wait = kind;
    t->wait_obj = obj;
    t->wait_aux = aux;
    t->deadline_ns = deadline;
    t->wait_ready = 0;
    c->thunk_blocked = 1;
}

static void make_runnable(mr_guest_thread *t, int result) {
    if (!t || !t->used || t->state != TS_BLOCKED) return;
    t->state = TS_RUNNABLE;
    t->wait_result = result;
    t->wait_ready = 1;
}

static guest_mutex *mutex_find(mr_scheduler *s, uint32_t addr, int create) {
    guest_mutex *free_slot = NULL;
    for (int i = 0; i < MR_MAX_MUTEXES; i++) {
        if (s->mutexes[i].used && s->mutexes[i].addr == addr) return &s->mutexes[i];
        if (!s->mutexes[i].used && !free_slot) free_slot = &s->mutexes[i];
    }
    if (!create || !free_slot) return NULL;
    *free_slot = (guest_mutex){.addr = addr, .type = MR_MUTEX_NORMAL, .used = 1};
    return free_slot;
}

static guest_cond *cond_find(mr_scheduler *s, uint32_t addr, int create) {
    guest_cond *free_slot = NULL;
    for (int i = 0; i < MR_MAX_CONDS; i++) {
        if (s->conds[i].used && s->conds[i].addr == addr) return &s->conds[i];
        if (!s->conds[i].used && !free_slot) free_slot = &s->conds[i];
    }
    if (!create || !free_slot) return NULL;
    *free_slot = (guest_cond){.addr = addr, .used = 1};
    return free_slot;
}

static guest_once *once_find(mr_scheduler *s, uint32_t addr, int create) {
    guest_once *free_slot = NULL;
    for (int i = 0; i < MR_MAX_ONCE; i++) {
        if (s->once[i].used && s->once[i].addr == addr) return &s->once[i];
        if (!s->once[i].used && !free_slot) free_slot = &s->once[i];
    }
    if (!create || !free_slot) return NULL;
    *free_slot = (guest_once){.addr = addr, .used = 1};
    return free_slot;
}

static int mutex_try_acquire(mr_guest_thread *t, guest_mutex *m, int try_only) {
    if (!m || !t) return EINVAL;
    if (!m->owner) {
        m->owner = t->tid;
        m->recursion = 1;
        return 0;
    }
    if (m->owner == t->tid) {
        if (m->type == MR_MUTEX_RECURSIVE) {
            if (m->recursion == UINT_MAX) return EAGAIN;
            m->recursion++;
            return 0;
        }
        if (m->type == MR_MUTEX_ERRORCHECK) return EDEADLK;
        return try_only ? EBUSY : EDEADLK;
    }
    return EBUSY;
}

static void wake_one_waiter(mr_scheduler *s, wait_kind kind, uint32_t obj) {
    if (!s) return;
    int start = s->current;
    for (int n = 1; n <= s->capacity; n++) {
        int i = (start + n) % s->capacity;
        mr_guest_thread *t = &s->threads[i];
        if (t->used && t->state == TS_BLOCKED && t->wait == kind && t->wait_obj == obj) {
            make_runnable(t, 0);
            return;
        }
    }
}

static void wake_one_mutex_waiter(mr_scheduler *s, uint32_t mutex_addr) {
    if (!s) return;
    int start = s->current;
    for (int n = 1; n <= s->capacity; n++) {
        int i = (start + n) % s->capacity;
        mr_guest_thread *t = &s->threads[i];
        if (!t->used || t->state != TS_BLOCKED) continue;
        if ((t->wait == WAIT_MUTEX && t->wait_obj == mutex_addr) ||
            (t->wait == WAIT_COND_MUTEX && t->wait_aux == mutex_addr)) {
            make_runnable(t, t->wait_result);
            return;
        }
    }
}

static int mutex_unlock_owned(mr_scheduler *s, mr_guest_thread *t, guest_mutex *m) {
    if (!m || !t) return EINVAL;
    if (m->owner != t->tid) return EPERM;
    if (m->recursion > 1) {
        m->recursion--;
        return 0;
    }
    m->owner = 0;
    m->recursion = 0;
    wake_one_mutex_waiter(s, m->addr);
    return 0;
}

static void wake_joiners(mr_scheduler *s, uint32_t tid) {
    for (int i = 0; i < s->capacity; i++) {
        mr_guest_thread *w = &s->threads[i];
        if (w->used && w->state == TS_BLOCKED && w->wait == WAIT_JOIN && w->wait_obj == tid)
            make_runnable(w, 0);
    }
}

static void reap_thread(mr_scheduler *s, mr_guest_thread *t) {
    (void)s;
    if (!t || !t->used || t->tid == 1) return;
    mr_cpu *cpu = t->cpu;
    uint32_t low = t->stack_low, high = t->stack_high;
    memset(t, 0, sizeof(*t));
    t->stack_low = low;
    t->stack_high = high;
    if (cpu) cpu->scheduler = NULL;
}

static void finalize_thread(mr_scheduler *s, mr_guest_thread *t) {
    if (!s || !t || !t->used) return;
    t->state = TS_ZOMBIE;
    t->cpu->yield_requested = 1;
    wake_joiners(s, t->tid);
    if (t->detached) reap_thread(s, t);
}

static int start_next_destructor(mr_scheduler *s, mr_guest_thread *t) {
    while (t->dtor_pass < MR_DESTRUCTOR_PASSES) {
        while (t->dtor_index < MR_MAX_TLS_KEYS) {
            int k = t->dtor_index++;
            if (!s->keys[k].used || !s->keys[k].destructor || !t->tls[k]) continue;
            uint32_t value = t->tls[k];
            t->tls[k] = 0;
            t->dtor_ran = 1;
            t->cpu->r[0] = value;
            t->cpu->r[MR_R_LR] = MR_TLS_RETURN_MAGIC;
            t->cpu->thumb = s->keys[k].destructor & 1u;
            t->cpu->r[MR_R_PC] = s->keys[k].destructor & ~1u;
            return 1;
        }
        if (!t->dtor_ran) break;
        t->dtor_pass++;
        t->dtor_index = 0;
        t->dtor_ran = 0;
    }
    finalize_thread(s, t);
    return 0;
}

static void begin_thread_exit(mr_scheduler *s, mr_guest_thread *t) {
    t->retval = t->cpu->r[0];
    t->exiting = 1;
    t->dtor_pass = 0;
    t->dtor_index = 0;
    t->dtor_ran = 0;
    start_next_destructor(s, t);
}

static void propagate_fault(mr_scheduler *s, mr_guest_thread *t) {
    if (!s || !t || !t->cpu || !t->cpu->fault) return;
    mr_cpu *main = s->main_cpu;
    if (t->cpu != main) {
        main->fault = t->cpu->fault;
        main->fault_addr = t->cpu->fault_addr;
        main->halted = 1;
        main->r[MR_R_PC] = t->cpu->r[MR_R_PC];
        main->call_ring_pos = t->cpu->call_ring_pos;
        memcpy(main->call_ring, t->cpu->call_ring, sizeof(main->call_ring));
    }
}

static int handle_magic(mr_scheduler *s, mr_guest_thread *t) {
    mr_cpu *c = t->cpu;
    uint32_t pc = c->r[MR_R_PC];
    if (pc == MR_THREAD_RETURN_MAGIC) {
        if (t->callback_worker) {
            t->state = TS_BLOCKED;
            clear_wait(t);
            t->wait = WAIT_CALLBACK;
            t->callback_started = 0;
            c->yield_requested = 1;
            return 1;
        }
        begin_thread_exit(s, t);
        return 1;
    }
    if (pc == MR_TLS_RETURN_MAGIC) {
        start_next_destructor(s, t);
        return 1;
    }
    if (pc == MR_ONCE_RETURN_MAGIC) {
        if (t->once_depth <= 0) {
            c->fault = "pthread_once return stack underflow";
            c->fault_addr = pc;
            c->halted = 1;
            return 1;
        }
        int frame = --t->once_depth;
        uint32_t once_addr = t->once_addr[frame];
        uint32_t ret = t->once_saved_lr[frame];
        t->once_addr[frame] = 0;
        t->once_saved_lr[frame] = 0;
        guest_once *o = once_find(s, once_addr, 0);
        if (o && o->owner == t->tid) {
            o->state = 2;
            o->owner = 0;
            if (mr_mem_ok(c, o->addr, 4)) mr_st32(c, o->addr, 2);
            for (int i = 0; i < s->capacity; i++) {
                mr_guest_thread *w = &s->threads[i];
                if (w->used && w->state == TS_BLOCKED && w->wait == WAIT_ONCE &&
                    w->wait_obj == o->addr)
                    make_runnable(w, 0);
            }
        }
        c->r[0] = 0;
        c->r[MR_R_LR] = ret;
        c->thumb = ret & 1u;
        c->r[MR_R_PC] = ret & ~1u;
        return 1;
    }
    return 0;
}

static void process_timeouts(mr_scheduler *s, uint64_t now) {
    for (int i = 0; i < s->capacity; i++) {
        mr_guest_thread *t = &s->threads[i];
        if (!t->used || t->state != TS_BLOCKED || !t->deadline_ns || now < t->deadline_ns) continue;
        if (t->wait == WAIT_SLEEP)
            make_runnable(t, 0);
        else if (t->wait == WAIT_COND || t->wait == WAIT_FUTEX)
            make_runnable(t, ETIMEDOUT);
    }
}

static uint64_t nearest_deadline(mr_scheduler *s) {
    uint64_t best = 0;
    for (int i = 0; i < s->capacity; i++) {
        mr_guest_thread *t = &s->threads[i];
        if (!t->used || t->state != TS_BLOCKED || !t->deadline_ns) continue;
        if (!best || t->deadline_ns < best) best = t->deadline_ns;
    }
    return best;
}

int mr_sched_init(mr_cpu *main_cpu, uint32_t bottom, uint32_t top) {
    if (!main_cpu || top <= bottom) return -1;
    if (main_cpu->scheduler) mr_sched_destroy(main_cpu);

    bottom = (bottom + 0xFFFFu) & ~0xFFFFu;
    top &= ~0xFFFFu;
    if (top <= bottom || top - bottom < MR_MAIN_STACK + MR_THREAD_STACK) return -1;

    mr_scheduler *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->main_cpu = main_cpu;
    s->next_tid = 2;
    s->total_created = 1;
    s->quantum = MR_DEFAULT_QUANTUM;
    const char *q = getenv("MR_THREAD_QUANTUM");
    if (q) {
        unsigned long v = strtoul(q, NULL, 0);
        if (v >= MR_MIN_QUANTUM && v <= MR_MAX_QUANTUM) s->quantum = (uint32_t)v;
    }

    uint32_t worker_bytes = top - bottom - MR_MAIN_STACK;
    int workers = (int)(worker_bytes / MR_THREAD_STACK);
    if (workers > MR_MAX_THREADS - 1) workers = MR_MAX_THREADS - 1;
    s->capacity = 1 + workers;

    mr_guest_thread *m = &s->threads[0];
    m->used = 1;
    m->state = TS_RUNNABLE;
    m->tid = 1;
    m->cpu = main_cpu;
    m->stack_high = top;
    m->stack_low = top - MR_MAIN_STACK;
    m->errno_addr = m->stack_low + 4;
    main_cpu->scheduler = s;
    main_cpu->guest_tid = 1;
    main_cpu->thunk_blocked = 0;
    main_cpu->yield_requested = 0;
    init_guest_tls(m);
    s->keys[3].used = 1;
    s->keys[4].used = 1;
    set_guest_errno(m, 0);

    uint32_t worker_top = m->stack_low;
    for (int i = 1; i < s->capacity; i++) {
        s->threads[i].stack_high = worker_top - (uint32_t)(i - 1) * MR_THREAD_STACK;
        s->threads[i].stack_low = s->threads[i].stack_high - MR_THREAD_STACK;
    }
    return 0;
}

void mr_sched_destroy(mr_cpu *main_cpu) {
    if (!main_cpu || !main_cpu->scheduler) return;
    mr_scheduler *s = main_cpu->scheduler;
    for (int i = 0; i < s->capacity; i++)
        if (s->threads[i].used && s->threads[i].cpu) s->threads[i].cpu->scheduler = NULL;
    main_cpu->scheduler = NULL;
    main_cpu->guest_tid = 0;
    free(s);
}

enum {
    RUN_QUANTUM_FAULT = -1,
    RUN_QUANTUM_OK = 0,
    RUN_QUANTUM_MAIN_DONE = 1,
    RUN_QUANTUM_BUDGET_DONE = 2,
};

static int run_thread_quantum(mr_scheduler *s, mr_guest_thread *t, mr_cpu *main_cpu,
                              uint32_t stop_addr, uint64_t limit, uint64_t *executed,
                              int limit_is_fault) {
    mr_cpu *c = t->cpu;
    if (t->callback_worker) t->callback_started = 1;
    c->yield_requested = 0;

    uint32_t q = 0;
    while (q < s->quantum && t->used && t->state == TS_RUNNABLE) {
        if (c == main_cpu && c->r[MR_R_PC] == stop_addr) {
            t->state = TS_RUNNABLE;
            return RUN_QUANTUM_MAIN_DONE;
        }
        if (handle_magic(s, t)) {
            if (!t->used || t->state != TS_RUNNABLE) break;
            continue;
        }

        if (limit && *executed >= limit) {
            if (!limit_is_fault) return RUN_QUANTUM_BUDGET_DONE;
            main_cpu->fault = "utasitaskorlat elerve";
            main_cpu->fault_addr = c->r[MR_R_PC];
            main_cpu->halted = 1;
            return RUN_QUANTUM_FAULT;
        }

        uint32_t budget = s->quantum - q;
        if (limit && limit - *executed < budget) budget = (uint32_t)(limit - *executed);
        if (!budget) return RUN_QUANTUM_BUDGET_DONE;

        uint32_t ran = mr_a64_run(c, budget);
        if (ran) {
            q += ran;
            *executed += ran;
            c->insn_count += ran;
            if (c != main_cpu) main_cpu->insn_count += ran;
            continue;
        }

        if (mr_guest_step_boundary(c) != 0 || c->halted) {
            propagate_fault(s, t);
            return RUN_QUANTUM_FAULT;
        }
        q++;
        (*executed)++;
        if (c != main_cpu) main_cpu->insn_count++;
        if (c->thunk_blocked || c->yield_requested) break;
    }
    return RUN_QUANTUM_OK;
}

static int pick_callback(const mr_scheduler *s) {
    for (int i = 0; i < s->capacity; i++) {
        const mr_guest_thread *t = &s->threads[i];
        if (t->used && t->callback_worker && t->state == TS_RUNNABLE) return i;
    }
    return -1;
}

static int pick_worker(mr_scheduler *s, int include_main) {
    for (int n = 1; n <= s->capacity; n++) {
        int i = (s->current + n) % s->capacity;
        if ((!include_main && i == 0) || !s->threads[i].used || s->threads[i].state != TS_RUNNABLE)
            continue;
        return i;
    }
    return -1;
}

int mr_sched_run(mr_cpu *main_cpu, uint32_t stop_addr, uint64_t max_insn) {
    if (!main_cpu || !main_cpu->scheduler) return -1;
    mr_scheduler *s = main_cpu->scheduler;
    if (s->running) return -1;
    s->running = 1;

    mr_guest_thread *main_t = &s->threads[0];
    main_t->state = TS_RUNNABLE;
    clear_wait(main_t);
    uint64_t executed = 0;
    uint64_t limit = max_insn ? max_insn : main_cpu->insn_limit;

    for (;;) {
        if (main_cpu->halted) {
            s->running = 0;
            return -1;
        }
        if (main_cpu->r[MR_R_PC] == stop_addr) {
            main_t->state = TS_RUNNABLE;
            s->running = 0;
            return 0;
        }

        uint64_t now = mr_mono_ns();
        process_timeouts(s, now);
        if (s->audio_pump) s->audio_pump(main_cpu);

        int picked = pick_callback(s);
        if (picked < 0 && main_t->state == TS_RUNNABLE) picked = 0;
        if (picked < 0) picked = pick_worker(s, 0);

        if (picked < 0) {
            uint64_t deadline = nearest_deadline(s);
            if (deadline) {
                now = mr_mono_ns();
                uint64_t nap = deadline > now ? deadline - now : 0;
                if (nap > MR_IDLE_SLICE_NS) nap = MR_IDLE_SLICE_NS;
                if (nap) host_sleep_ns(nap);
                continue;
            }
            main_cpu->fault = "guest-thread deadlock: no runnable thread";
            main_cpu->fault_addr = main_cpu->r[MR_R_PC];
            main_cpu->halted = 1;
            s->running = 0;
            return -1;
        }

        s->current = picked;
        int rc =
            run_thread_quantum(s, &s->threads[picked], main_cpu, stop_addr, limit, &executed, 1);
        if (rc == RUN_QUANTUM_FAULT) {
            s->running = 0;
            return -1;
        }
        if (rc == RUN_QUANTUM_MAIN_DONE) {
            main_t->state = TS_RUNNABLE;
            s->running = 0;
            return 0;
        }
    }
}

int mr_sched_service_idle(mr_cpu *main_cpu, uint64_t max_insn, uint64_t max_ns,
                          uint64_t *executed_out) {
    if (executed_out) *executed_out = 0;
    if (!main_cpu || !main_cpu->scheduler || !max_insn) return 0;
    mr_scheduler *s = main_cpu->scheduler;
    if (s->running) return 0;
    s->running = 1;

    uint64_t executed = 0;
    uint64_t deadline = max_ns ? mr_mono_ns() + max_ns : 0;
    while (executed < max_insn) {
        if (main_cpu->halted) {
            s->running = 0;
            return -1;
        }
        process_timeouts(s, mr_mono_ns());
        if (s->audio_pump) s->audio_pump(main_cpu);

        int picked = pick_callback(s);
        if (picked < 0) picked = pick_worker(s, 0);
        if (picked < 0) break;

        s->current = picked;
        int rc = run_thread_quantum(s, &s->threads[picked], main_cpu, 0, max_insn, &executed, 0);
        if (rc == RUN_QUANTUM_FAULT) {
            s->running = 0;
            return -1;
        }
        if (rc == RUN_QUANTUM_BUDGET_DONE) break;
        if (deadline && mr_mono_ns() >= deadline) break;
    }

    s->running = 0;
    if (executed_out) *executed_out = executed;
    return 0;
}

void mr_sched_set_audio_pump(mr_cpu *main_cpu, void (*pump)(mr_cpu *)) {
    mr_scheduler *s = sched_of(main_cpu);
    if (s) s->audio_pump = pump;
}

int mr_sched_busy(const mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    return s && s->running;
}

int mr_sched_live_threads(const mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    if (!s) return cpu ? 1 : 0;
    int n = 0;
    for (int i = 0; i < s->capacity; i++)
        if (s->threads[i].used && s->threads[i].state != TS_ZOMBIE) n++;
    return n;
}

int mr_sched_runnable(const mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    if (!s) return 0;
    int n = 0;
    for (int i = 0; i < s->capacity; i++)
        if (s->threads[i].used && s->threads[i].state == TS_RUNNABLE) n++;
    return n;
}

uint32_t mr_sched_total_created(const mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    return s ? s->total_created : (cpu ? 1u : 0u);
}

void mr_sched_dump(const mr_cpu *cpu) {
    static const char *WAITN[] = {"-",     "mutex", "condvar", "cond-mutex", "join",
                                  "sleep", "once",  "guard",   "futex",      "callback"};
    enum { WAITN_N = (int)(sizeof(WAITN) / sizeof(WAITN[0])) };
    mr_scheduler *s = sched_of(cpu);
    if (!s) {
        printf("  (no scheduler)\n");
        return;
    }

    int blocked[WAITN_N] = {0}, zombie = 0;
    for (int i = 0; i < s->capacity; i++) {
        const mr_guest_thread *t = &s->threads[i];
        if (!t->used) continue;
        if (t->state == TS_ZOMBIE)
            zombie++;
        else if (t->state != TS_RUNNABLE && (unsigned)t->wait < WAITN_N)
            blocked[t->wait]++;
    }
    printf("  runnable: %d, zombie: %d\n", mr_sched_runnable(cpu), zombie);
    for (int w = 1; w < WAITN_N; w++)
        if (blocked[w]) printf("  blocked %-11s %d\n", WAITN[w], blocked[w]);

    for (int i = 0; i < s->capacity; i++) {
        const mr_guest_thread *t = &s->threads[i];
        if (!t->used || t->state != TS_BLOCKED ||
            (t->wait != WAIT_MUTEX && t->wait != WAIT_COND_MUTEX))
            continue;
        uint32_t addr = t->wait == WAIT_MUTEX ? t->wait_obj : t->wait_aux;
        const guest_mutex *m = mutex_find((mr_scheduler *)s, addr, 0);
        const mr_guest_thread *ow = m ? thread_by_tid((mr_scheduler *)s, m->owner) : NULL;
        printf("    tid %-3u mutex 0x%08x  owner tid %u (%s)\n", t->tid, addr, m ? m->owner : 0,
               !ow                        ? "no such thread"
               : ow->state == TS_ZOMBIE   ? "already exited"
               : ow->state == TS_RUNNABLE ? "runnable"
                                          : WAITN[(unsigned)ow->wait < WAITN_N ? ow->wait : 0]);
    }

    for (int i = 0; i < s->capacity; i++) {
        const mr_guest_thread *t = &s->threads[i];
        if (!t->used || t->state != TS_BLOCKED || t->wait != WAIT_ONCE) continue;
        const guest_once *o = NULL;
        for (int k = 0; k < MR_MAX_ONCE; k++)
            if (s->once[k].used && s->once[k].addr == t->wait_obj) {
                o = &s->once[k];
                break;
            }
        const mr_guest_thread *ow = o ? thread_by_tid((mr_scheduler *)s, o->owner) : NULL;
        printf("    tid %-3u once 0x%08x  owner tid %u (%s)\n", t->tid, t->wait_obj,
               o ? o->owner : 0,
               !ow                        ? "no such thread"
               : ow->state == TS_ZOMBIE   ? "already exited"
               : ow->state == TS_RUNNABLE ? "runnable"
                                          : WAITN[(unsigned)ow->wait < WAITN_N ? ow->wait : 0]);
    }
}

uint32_t mr_sched_current_tid(const mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    if (!s) return cpu ? cpu->guest_tid : 0;
    mr_guest_thread *t = thread_by_cpu(s, cpu);
    return t ? t->tid : 0;
}

uint32_t mr_sched_errno_addr(mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    mr_guest_thread *t = thread_by_cpu(s, cpu);
    return t ? t->errno_addr : 0;
}

uint32_t mr_sched_tls_addr(mr_cpu *cpu) {
    mr_scheduler *s = sched_of(cpu);
    mr_guest_thread *t = thread_by_cpu(s, cpu);
    return t ? t->tls_addr : 0;
}

void mr_sched_set_errno(mr_cpu *cpu, int value) {
    mr_scheduler *s = sched_of(cpu);
    mr_guest_thread *t = thread_by_cpu(s, cpu);
    if (t) set_guest_errno(t, value);
}

// pthread shims.

static void prepare_thread_call(mr_guest_thread *slot, uint32_t entry, const uint32_t *argv,
                                int argc) {
    mr_cpu *n = slot->cpu;
    memset(n->r, 0, sizeof n->r);
    memset(&n->f, 0, sizeof n->f);
    memset(&n->v, 0, sizeof n->v);
    memset(n->call_ring, 0, sizeof n->call_ring);
    n->fpscr = 0;
    n->thunk_redirect = 0;
    n->thumb = entry & 1u;
    n->itstate = 0;
    n->call_ring_pos = 0;
    n->thunk_blocked = 0;
    n->yield_requested = 0;
    n->halted = 0;
    n->fault = NULL;
    n->fault_addr = 0;
    slot->callback_started = 0;

    for (int i = 0; i < argc && i < 4; i++)
        n->r[i] = argv[i];
    n->r[MR_R_SP] = (slot->stack_high - 64u) & ~7u;
    n->r[MR_R_LR] = MR_THREAD_RETURN_MAGIC;
    n->r[MR_R_PC] = entry & ~1u;
    slot->state = TS_RUNNABLE;
    clear_wait(slot);
}

static uint32_t spawn_thread(mr_scheduler *s, uint32_t entry, const uint32_t *argv, int argc,
                             int detached) {
    mr_guest_thread *slot = NULL;
    for (int i = 1; i < s->capacity; i++)
        if (!s->threads[i].used) {
            slot = &s->threads[i];
            break;
        }
    if (!slot) return 0;

    uint32_t low = slot->stack_low, high = slot->stack_high;
    memset(slot, 0, sizeof(*slot));
    slot->stack_low = low;
    slot->stack_high = high;
    slot->used = 1;
    slot->tid = s->next_tid++;
    if (!slot->tid) slot->tid = s->next_tid++;
    slot->detached = detached;
    slot->cpu = &slot->owned_cpu;
    slot->errno_addr = low + 4;

    mr_cpu *n = slot->cpu;
    memset(n, 0, sizeof(*n));
    n->mem_host = s->main_cpu->mem_host;
    n->mem_guest_base = s->main_cpu->mem_guest_base;
    n->mem_size = s->main_cpu->mem_size;
    n->ro_start = s->main_cpu->ro_start;
    n->ro_end = s->main_cpu->ro_end;
    n->exec_start = s->main_cpu->exec_start;
    n->exec_end = s->main_cpu->exec_end;
    n->thunks = s->main_cpu->thunks;
    n->thunk_count = s->main_cpu->thunk_count;
    n->insn_limit = s->main_cpu->insn_limit;
    n->scheduler = s;
    n->guest_tid = slot->tid;

    init_guest_tls(slot);
    set_guest_errno(slot, 0);
    prepare_thread_call(slot, entry, argv, argc);
    s->total_created++;
    return slot->tid;
}

int mr_sched_post_callback(mr_cpu *main_cpu, uint32_t *thread_id, uint32_t entry, int argc,
                           const uint32_t *argv) {
    mr_scheduler *s = sched_of(main_cpu);
    if (!s || !thread_id || !entry || argc < 0 || argc > 4 || (argc && !argv)) return -1;

    mr_guest_thread *t = *thread_id ? thread_by_tid(s, *thread_id) : NULL;
    if (t && !t->callback_worker) return -1;
    if (t) {
        if (t->state != TS_BLOCKED || t->wait != WAIT_CALLBACK) return 0;
        prepare_thread_call(t, entry, argv, argc);
        return 1;
    }

    *thread_id = 0;
    uint32_t tid = spawn_thread(s, entry, argv, argc, 1);
    if (!tid) return -1;
    t = thread_by_tid(s, tid);
    if (!t) return -1;
    t->callback_worker = 1;
    *thread_id = tid;
    return 1;
}

int mr_sched_cancel_callback(mr_cpu *cpu, uint32_t thread_id) {
    mr_scheduler *s = sched_of(cpu);
    mr_guest_thread *t = thread_by_tid(s, thread_id);
    if (!s || !t || !t->callback_worker) return -1;
    if (thread_by_cpu(s, cpu) == t) return 0;
    if (t->state == TS_BLOCKED && t->wait == WAIT_CALLBACK) return 1;
    if (t->state != TS_RUNNABLE || t->callback_started) return 0;

    t->state = TS_BLOCKED;
    clear_wait(t);
    t->wait = WAIT_CALLBACK;
    t->cpu->yield_requested = 1;
    return 1;
}

static void s_pthread_create(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || !A0 || !A2) {
        RET(EINVAL);
        return;
    }

    int detached = MR_CREATE_JOINABLE;
    if (A1 && mr_mem_ok(c, A1, 12) && mr_ld32(c, A1) == MR_ATTR_MAGIC) {
        if (mr_ld32(c, A1 + 4) > MR_THREAD_STACK) {
            RET(EINVAL);
            return;
        }
        detached = (int)mr_ld32(c, A1 + 8);
    }

    uint32_t tid = spawn_thread(s, A2, &A3, 1, detached == MR_CREATE_DETACHED);
    if (!tid) {
        RET(EAGAIN);
        return;
    }
    mr_st32(c, A0, tid);
    RET(0);
}

static void s_pthread_self(mr_cpu *c) {
    RET(mr_sched_current_tid(c));
}
static void s_pthread_equal(mr_cpu *c) {
    RET(A0 == A1);
}

static void s_pthread_join(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *self = thread_by_cpu(s, c);
    mr_guest_thread *target = thread_by_tid(s, A0);
    if (!s || !self || !target) {
        RET(ESRCH);
        return;
    }
    if (target == self) {
        RET(EDEADLK);
        return;
    }
    if (target->detached || target->joined) {
        RET(EINVAL);
        return;
    }
    if (target->state != TS_ZOMBIE) {
        block_current(c, WAIT_JOIN, target->tid, A1, 0);
        return;
    }
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, target->retval);
    target->joined = 1;
    reap_thread(s, target);
    clear_wait(self);
    RET(0);
}

static void s_pthread_detach(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_tid(s, A0);
    if (!t) {
        RET(ESRCH);
        return;
    }
    if (t->detached || t->joined) {
        RET(EINVAL);
        return;
    }
    t->detached = 1;
    if (t->state == TS_ZOMBIE) reap_thread(s, t);
    RET(0);
}

static void s_pthread_once(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    if (!s || !t || !A0 || !A1) {
        RET(EINVAL);
        return;
    }
    guest_once *o = once_find(s, A0, 1);
    if (!o) {
        RET(EAGAIN);
        return;
    }
    if (o->state == 2 || (mr_mem_ok(c, A0, 4) && mr_ld32(c, A0) == 2)) {
        o->state = 2;
        clear_wait(t);
        RET(0);
        return;
    }
    if (!o->owner) {
        if (t->once_depth >= MR_ONCE_NESTING) {
            RET(EAGAIN);
            return;
        }
        o->owner = t->tid;
        o->state = 1;
        if (mr_mem_ok(c, A0, 4)) mr_st32(c, A0, 1);
        int frame = t->once_depth++;
        t->once_addr[frame] = A0;
        t->once_saved_lr[frame] = c->r[MR_R_LR];
        clear_wait(t);
        c->r[MR_R_LR] = MR_ONCE_RETURN_MAGIC;
        c->thunk_redirect = A1;
        RET(0);
        return;
    }
    if (o->owner == t->tid) {
        RET(EDEADLK);
        return;
    }
    block_current(c, WAIT_ONCE, A0, 0, 0);
}

static void s_mutexattr_init(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 4)) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A0, MR_MUTEX_NORMAL);
    RET(0);
}

static void s_mutexattr_settype(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 4) || A1 > MR_MUTEX_ERRORCHECK) {
        RET(EINVAL);
        return;
    }
    uint32_t attr = mr_ld32(c, A0);
    mr_st32(c, A0, (attr & ~MR_MUTEXATTR_TYPE_MASK) | A1);
    RET(0);
}

static void s_mutexattr_destroy(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 4)) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A0, 0xFFFFFFFFu);
    RET(0);
}

static void s_mutex_init(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || !A0) {
        RET(EINVAL);
        return;
    }
    guest_mutex *m = mutex_find(s, A0, 1);
    if (!m) {
        RET(EAGAIN);
        return;
    }
    m->owner = 0;
    m->recursion = 0;
    m->type = MR_MUTEX_NORMAL;
    if (A1) {
        if (!mr_mem_ok(c, A1, 4)) {
            RET(EINVAL);
            return;
        }
        uint32_t type = mr_ld32(c, A1) & MR_MUTEXATTR_TYPE_MASK;
        if (type > MR_MUTEX_ERRORCHECK) {
            RET(EINVAL);
            return;
        }
        m->type = (int)type;
    }
    if (mr_mem_ok(c, A0, 4)) mr_st32(c, A0, 0);
    RET(0);
}

static void mutex_lock_common(mr_cpu *c, int try_only) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    if (!s || !t || !A0) {
        RET(EINVAL);
        return;
    }
    guest_mutex *m = mutex_find(s, A0, 1); // Static PTHREAD_MUTEX_INITIALIZER.
    if (!m) {
        RET(EAGAIN);
        return;
    }
    int rc = mutex_try_acquire(t, m, try_only);
    if (rc == EBUSY && !try_only) {
        block_current(c, WAIT_MUTEX, A0, 0, 0);
        return;
    }
    if (rc == 0) clear_wait(t);
    RET(rc);
}
static void s_mutex_lock(mr_cpu *c) {
    mutex_lock_common(c, 0);
}
static void s_mutex_trylock(mr_cpu *c) {
    mutex_lock_common(c, 1);
}

static void s_mutex_unlock(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    guest_mutex *m = s ? mutex_find(s, A0, 0) : NULL;
    RET(mutex_unlock_owned(s, t, m));
}

static void s_mutex_destroy(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    guest_mutex *m = s ? mutex_find(s, A0, 0) : NULL;
    if (!m) {
        RET(0);
        return;
    }
    if (m->owner) {
        RET(EBUSY);
        return;
    }
    for (int i = 0; i < s->capacity; i++) {
        mr_guest_thread *t = &s->threads[i];
        if (!t->used || t->state == TS_ZOMBIE) continue;
        if ((t->wait == WAIT_MUTEX && t->wait_obj == A0) ||
            ((t->wait == WAIT_COND || t->wait == WAIT_COND_MUTEX) && t->wait_aux == A0)) {
            RET(EBUSY);
            return;
        }
    }
    memset(m, 0, sizeof(*m));
    if (mr_mem_ok(c, A0, 4)) mr_st32(c, A0, 0);
    RET(0);
}

static void s_cond_init(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || !A0) {
        RET(EINVAL);
        return;
    }
    guest_cond *v = cond_find(s, A0, 1);
    if (!v) {
        RET(EAGAIN);
        return;
    }
    if (mr_mem_ok(c, A0, 4)) mr_st32(c, A0, 0);
    RET(0);
}

static uint64_t realtime_absolute_to_mono(mr_cpu *c, uint32_t p, int *valid) {
    *valid = 0;
    if (!p || !mr_mem_ok(c, p, 8)) return 0;
    uint32_t sec = mr_ld32(c, p), nsec = mr_ld32(c, p + 4);
    if (nsec >= 1000000000u) return 0;
    uint64_t abs_real = (uint64_t)sec * 1000000000ull + nsec;
    uint64_t now_real = mr_real_ns(), now_mono = mr_mono_ns();
    *valid = 1;
    return abs_real <= now_real ? now_mono : now_mono + (abs_real - now_real);
}

static void cond_wait_common(mr_cpu *c, int timed) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    if (!s || !t || !A0 || !A1) {
        RET(EINVAL);
        return;
    }
    guest_cond *cv = cond_find(s, A0, 1);
    guest_mutex *m = mutex_find(s, A1, 0);
    if (!cv || !m) {
        RET(EINVAL);
        return;
    }

    if ((t->wait == WAIT_COND || t->wait == WAIT_COND_MUTEX) && t->wait_obj == A0 &&
        t->wait_aux == A1 && t->wait_ready) {
        int result = t->wait_result;
        int rc = mutex_try_acquire(t, m, 0);
        if (rc == EBUSY) {
            t->state = TS_BLOCKED;
            t->wait = WAIT_COND_MUTEX;
            t->deadline_ns = 0;
            c->thunk_blocked = 1;
            return;
        }
        clear_wait(t);
        RET(rc ? rc : result);
        return;
    }
    if (m->owner != t->tid) {
        RET(EPERM);
        return;
    }

    uint64_t deadline = 0;
    if (timed) {
        int valid;
        deadline = realtime_absolute_to_mono(c, A2, &valid);
        if (!valid) {
            RET(EINVAL);
            return;
        }
    }
    int rc = mutex_unlock_owned(s, t, m);
    if (rc) {
        RET(rc);
        return;
    }
    block_current(c, WAIT_COND, A0, A1, deadline);
}

static void s_cond_wait(mr_cpu *c) {
    cond_wait_common(c, 0);
}
static void s_cond_timedwait(mr_cpu *c) {
    cond_wait_common(c, 1);
}

static void s_cond_signal(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || !A0) {
        RET(EINVAL);
        return;
    }
    cond_find(s, A0, 1);
    wake_one_waiter(s, WAIT_COND, A0);
    RET(0);
}

static void s_cond_broadcast(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || !A0) {
        RET(EINVAL);
        return;
    }
    cond_find(s, A0, 1);
    for (int i = 0; i < s->capacity; i++) {
        mr_guest_thread *t = &s->threads[i];
        if (t->used && t->state == TS_BLOCKED && t->wait == WAIT_COND && t->wait_obj == A0)
            make_runnable(t, 0);
    }
    RET(0);
}

static void s_cond_destroy(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    guest_cond *cv = s ? cond_find(s, A0, 0) : NULL;
    if (!cv) {
        RET(0);
        return;
    }
    for (int i = 0; i < s->capacity; i++)
        if (s->threads[i].used && s->threads[i].state != TS_ZOMBIE &&
            (s->threads[i].wait == WAIT_COND || s->threads[i].wait == WAIT_COND_MUTEX) &&
            s->threads[i].wait_obj == A0) {
            RET(EBUSY);
            return;
        }
    memset(cv, 0, sizeof(*cv));
    if (mr_mem_ok(c, A0, 4)) mr_st32(c, A0, 0);
    RET(0);
}

static void s_key_create(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || !A0) {
        RET(EINVAL);
        return;
    }
    for (int i = MR_TLS_FIRST_USER; i < MR_MAX_TLS_KEYS; i++) {
        if (!s->keys[i].used) {
            s->keys[i].used = 1;
            s->keys[i].destructor = A1;
            mr_st32(c, A0, (uint32_t)i);
            RET(0);
            return;
        }
    }
    RET(EAGAIN);
}

static void s_key_delete(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    if (!s || A0 >= MR_MAX_TLS_KEYS || !s->keys[A0].used) {
        RET(EINVAL);
        return;
    }
    s->keys[A0].used = 0;
    s->keys[A0].destructor = 0;
    for (int i = 0; i < s->capacity; i++) {
        mr_guest_thread *t = &s->threads[i];
        t->tls[A0] = 0;
        if (t->used && t->tls_addr && A0 < MR_BIONIC_TLS_SLOTS)
            mr_st32(t->cpu, t->tls_addr + A0 * 4u, 0);
    }
    RET(0);
}

static void s_getspecific(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    RET((s && t && A0 < MR_MAX_TLS_KEYS && s->keys[A0].used) ? t->tls[A0] : 0);
}

static void s_setspecific(mr_cpu *c) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    if (!s || !t || A0 >= MR_MAX_TLS_KEYS || !s->keys[A0].used) {
        RET(EINVAL);
        return;
    }
    t->tls[A0] = A1;
    if (t->tls_addr && A0 < MR_BIONIC_TLS_SLOTS) mr_st32(c, t->tls_addr + A0 * 4u, A1);
    RET(0);
}

static void s_attr_init(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 12)) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A0, MR_ATTR_MAGIC);
    mr_st32(c, A0 + 4, 0);
    mr_st32(c, A0 + 8, MR_CREATE_JOINABLE);
    RET(0);
}
static void s_attr_destroy(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 4)) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A0, 0);
    RET(0);
}
static void s_attr_setstacksize(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 12) || mr_ld32(c, A0) != MR_ATTR_MAGIC || A1 < 16384u ||
        A1 > MR_THREAD_STACK) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A0 + 4, A1);
    RET(0);
}
static void s_attr_setdetachstate(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 12) || mr_ld32(c, A0) != MR_ATTR_MAGIC || A1 > 1) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A0 + 8, A1);
    RET(0);
}
static void s_attr_getdetachstate(mr_cpu *c) {
    if (!A0 || !A1 || !mr_mem_ok(c, A0, 12) || !mr_mem_ok(c, A1, 4) ||
        mr_ld32(c, A0) != MR_ATTR_MAGIC) {
        RET(EINVAL);
        return;
    }
    mr_st32(c, A1, mr_ld32(c, A0 + 8));
    RET(0);
}

static void s_sched_yield(mr_cpu *c) {
    c->yield_requested = 1;
    RET(0);
}

static uint64_t relative_deadline(uint64_t ns) {
    uint64_t now = mr_mono_ns();
    return UINT64_MAX - now < ns ? UINT64_MAX : now + ns;
}

int mr_sched_sleep(mr_cpu *c, uint64_t ns) {
    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    if (!s || !t) {
        host_sleep_ns(ns);
        return 0;
    }
    if (t->wait == WAIT_SLEEP && t->wait_ready) {
        clear_wait(t);
        return 0;
    }
    if (!ns) {
        c->yield_requested = 1;
        return 0;
    }
    block_current(c, WAIT_SLEEP, 0, 0, relative_deadline(ns));
    return 1;
}

static void sleep_common(mr_cpu *c, uint64_t ns) {
    if (!mr_sched_sleep(c, ns)) RET(0);
}
static void s_usleep(mr_cpu *c) {
    sleep_common(c, (uint64_t)A0 * 1000ull);
}
static void s_sleep(mr_cpu *c) {
    sleep_common(c, (uint64_t)A0 * 1000000000ull);
}
static void s_nanosleep(mr_cpu *c) {
    if (!A0 || !mr_mem_ok(c, A0, 8)) {
        RET((uint32_t)-1);
        mr_sched_set_errno(c, EFAULT);
        return;
    }
    uint32_t sec = mr_ld32(c, A0), nsec = mr_ld32(c, A0 + 4);
    if (nsec >= 1000000000u) {
        RET((uint32_t)-1);
        mr_sched_set_errno(c, EINVAL);
        return;
    }
    sleep_common(c, (uint64_t)sec * 1000000000ull + nsec);
    if (!c->thunk_blocked && A1 && mr_mem_ok(c, A1, 8)) {
        mr_st32(c, A1, 0);
        mr_st32(c, A1 + 4, 0);
    }
}

static void s_getschedparam(mr_cpu *c) {
    if (A1 && mr_mem_ok(c, A1, 4)) mr_st32(c, A1, 0); // SCHED_OTHER.
    if (A2 && mr_mem_ok(c, A2, 4)) mr_st32(c, A2, 0); // Priority.
    RET(0);
}
static void s_setschedparam(mr_cpu *c) {
    RET(0);
}
static void s_sched_prio_min(mr_cpu *c) {
    RET(0);
}
static void s_sched_prio_max(mr_cpu *c) {
    RET(0);
}

static void s_syscall(mr_cpu *c) {
    // ARM EABI uses __NR_gettid=224 and __NR_futex=240. Android cacheflush is
    // the 0x0f0002 pseudo-system-call.
    if (A0 == 224u) {
        RET(mr_sched_current_tid(c));
        return;
    }
    if (A0 == 0x0f0002u) {
        RET(0);
        return;
    }
    if (A0 != 240u) {
        mr_sched_set_errno(c, ENOSYS);
        RET((uint32_t)-1);
        return;
    }

    mr_scheduler *s = sched_of(c);
    mr_guest_thread *t = thread_by_cpu(s, c);
    uint32_t uaddr = A1, op = A2 & 0x7fu, val = A3;
    if (!s || !t || !uaddr || !mr_mem_ok(c, uaddr, 4)) {
        mr_sched_set_errno(c, EFAULT);
        RET((uint32_t)-1);
        return;
    }
    if (op == 0) { // FUTEX_WAIT
        if (t->wait == WAIT_FUTEX && t->wait_obj == uaddr && t->wait_ready) {
            int result = t->wait_result;
            clear_wait(t);
            if (result) {
                mr_sched_set_errno(c, result);
                RET((uint32_t)-1);
            } else
                RET(0);
            return;
        }
        if (mr_ld32(c, uaddr) != val) {
            mr_sched_set_errno(c, EAGAIN);
            RET((uint32_t)-1);
            return;
        }
        uint64_t deadline = 0;
        uint32_t timeout = mr_ld32(c, c->r[MR_R_SP]);
        if (timeout) {
            if (!mr_mem_ok(c, timeout, 8)) {
                mr_sched_set_errno(c, EFAULT);
                RET((uint32_t)-1);
                return;
            }
            uint32_t sec = mr_ld32(c, timeout), nsec = mr_ld32(c, timeout + 4);
            if (nsec >= 1000000000u) {
                mr_sched_set_errno(c, EINVAL);
                RET((uint32_t)-1);
                return;
            }
            deadline = relative_deadline((uint64_t)sec * 1000000000ull + nsec);
        }
        block_current(c, WAIT_FUTEX, uaddr, val, deadline);
        return;
    }
    if (op == 1) { // FUTEX_WAKE
        uint32_t woke = 0;
        for (int i = 0; i < s->capacity && woke < val; i++) {
            mr_guest_thread *w = &s->threads[i];
            if (w->used && w->state == TS_BLOCKED && w->wait == WAIT_FUTEX &&
                w->wait_obj == uaddr) {
                make_runnable(w, 0);
                woke++;
            }
        }
        RET(woke);
        return;
    }
    mr_sched_set_errno(c, ENOSYS);
    RET((uint32_t)-1);
}

static const mr_thunk THREAD_SHIMS[] = {
    {"pthread_create", s_pthread_create},
    {"pthread_self", s_pthread_self},
    {"pthread_equal", s_pthread_equal},
    {"pthread_join", s_pthread_join},
    {"pthread_detach", s_pthread_detach},
    {"pthread_once", s_pthread_once},
    {"pthread_mutexattr_init", s_mutexattr_init},
    {"pthread_mutexattr_settype", s_mutexattr_settype},
    {"pthread_mutexattr_destroy", s_mutexattr_destroy},
    {"pthread_mutex_init", s_mutex_init},
    {"pthread_mutex_lock", s_mutex_lock},
    {"pthread_mutex_trylock", s_mutex_trylock},
    {"pthread_mutex_unlock", s_mutex_unlock},
    {"pthread_mutex_destroy", s_mutex_destroy},
    {"pthread_cond_init", s_cond_init},
    {"pthread_cond_wait", s_cond_wait},
    {"pthread_cond_timedwait", s_cond_timedwait},
    {"pthread_cond_signal", s_cond_signal},
    {"pthread_cond_broadcast", s_cond_broadcast},
    {"pthread_cond_destroy", s_cond_destroy},
    {"pthread_key_create", s_key_create},
    {"pthread_key_delete", s_key_delete},
    {"pthread_getspecific", s_getspecific},
    {"pthread_setspecific", s_setspecific},
    {"pthread_attr_init", s_attr_init},
    {"pthread_attr_destroy", s_attr_destroy},
    {"pthread_attr_setstacksize", s_attr_setstacksize},
    {"pthread_attr_setdetachstate", s_attr_setdetachstate},
    {"pthread_attr_getdetachstate", s_attr_getdetachstate},
    {"sched_yield", s_sched_yield},
    {"usleep", s_usleep},
    {"sleep", s_sleep},
    {"nanosleep", s_nanosleep},
    {"syscall", s_syscall},
    {"pthread_getschedparam", s_getschedparam},
    {"pthread_setschedparam", s_setschedparam},
    {"sched_get_priority_min", s_sched_prio_min},
    {"sched_get_priority_max", s_sched_prio_max},
};

mr_thunk_fn mr_thread_shim_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(THREAD_SHIMS) / sizeof(THREAD_SHIMS[0]); i++)
        if (strcmp(THREAD_SHIMS[i].name, name) == 0) return THREAD_SHIMS[i].fn;
    return NULL;
}
