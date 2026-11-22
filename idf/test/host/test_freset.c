/* CFG-5 (issue #266): the sequence of the factory reset, with a power cut at each point.
 *
 * The storage is a model: the facts that the sequence and the boot read (the markers) and the
 * old data that the reset must erase. A power cut stops a step with one of its part results,
 * and the code under test gets no more effect from the storage after it. Then the model boots
 * as the firmware does (main.c): gw_freset_resume(), a restart after a completed reset, and
 * gw_freset_boot() for the source of the configuration. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gw_freset.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------- the model of the flash ---------- */

typedef struct {
    /* the NVS partition */
    bool nvs_usable;   /* the NVS starts at boot; without it, nothing of the NVS can be read */
    bool intent;       /* the intent "reset pending" */
    bool factory;      /* the factory marker (commissioning) */
    bool secrets;      /* data from before the reset: the passwords, the certificate */
    /* the LittleFS partition */
    bool fs;           /* it mounts; without it, nothing of LittleFS can be read */
    bool file;         /* the configuration file from before the reset */
    bool pem;          /* another file from before the reset */
    bool mark;         /* the marker file "reset pending" */
} flash_t;

/* A fault that stays (a hardware fault, a full NVS): the step fails and returns. */
typedef struct {
    bool intent_once;   /* the NVS refuses the intent until it is erased (it is full) */
    bool nvs_write;     /* the NVS refuses each write */
    bool nvs_erase;     /* the NVS erase fails */
    bool fs_wipe;       /* the wipe of LittleFS fails */
    bool fs_write;      /* LittleFS refuses the marker file */
    bool fs_unlink;     /* LittleFS refuses the removal of the marker file */
} fault_t;

typedef struct {
    flash_t f;
    fault_t fault;
    /* the power cut: at the step call number cut_call (0: the first), with part result
     * cut_variant. -1: no cut */
    int cut_call;
    int cut_variant;
    int calls;          /* step calls up to now, in this power-up */
    bool dead;          /* the power is off: nothing has an effect any more */
    bool cut_done;      /* the cut occurred */
    bool no_variant;    /* cut_variant is not a part result of that step */
    /* the step calls of this power-up, for the tests of the order */
    gw_freset_step_t log[32];
    int log_n;
} model_t;

static bool op_nvs_usable(void *ctx)
{
    model_t *m = ctx;
    return !m->dead && m->f.nvs_usable;
}

static bool op_intent(void *ctx)
{
    model_t *m = ctx;
    return !m->dead && m->f.nvs_usable && m->f.intent;
}

static bool op_fs_mark(void *ctx)
{
    model_t *m = ctx;
    return !m->dead && m->f.fs && m->f.mark;
}

/* The part results of an NVS erase: each subset of the entries is erased, and the NVS starts or
 * does not start at the next boot (a page that is partly erased). Variant 0 erases nothing; the
 * last one is the full erase (the power went off after it, before the step returned). */
#define NVS_ERASE_VARIANTS 16

static bool cut_nvs_erase(flash_t *f, int v)
{
    if (v < 0 || v >= NVS_ERASE_VARIANTS) {
        return false;
    }
    f->intent = f->intent && (v & 1) == 0;
    f->factory = f->factory && (v & 2) == 0;
    f->secrets = f->secrets && (v & 4) == 0;
    f->nvs_usable = (v & 8) != 0;
    return true;
}

/* The part results of the wipe of LittleFS: the erase goes block by block, so each subset of
 * the files can stay, in a file system that mounts or does not mount. The last variant is the
 * complete wipe with the format. */
#define FS_WIPE_VARIANTS 17

static bool cut_fs_wipe(flash_t *f, int v)
{
    if (v < 0 || v >= FS_WIPE_VARIANTS) {
        return false;
    }
    if (v == FS_WIPE_VARIANTS - 1) {
        f->fs = true;
        f->file = f->pem = f->mark = false;
        return true;
    }
    f->file = f->file && (v & 1) == 0;
    f->pem = f->pem && (v & 2) == 0;
    f->mark = f->mark && (v & 4) == 0;
    f->fs = f->fs && (v & 8) == 0;
    return true;
}

/* A write of one entry or one file is atomic: variant 0 did not write, variant 1 wrote. */
static bool cut_atomic(bool *item, bool value, int v)
{
    if (v < 0 || v > 1) {
        return false;
    }
    if (v == 1) {
        *item = value;
    }
    return true;
}

static bool cut_step(model_t *m, gw_freset_step_t step)
{
    flash_t *f = &m->f;
    int v = m->cut_variant;
    switch (step) {
    case GW_FRESET_NVS_OPEN:
    case GW_FRESET_ERASE_NVS:
        return cut_nvs_erase(f, v);
    case GW_FRESET_INTENT:
        return f->nvs_usable ? cut_atomic(&f->intent, true, v) : v == 0;
    case GW_FRESET_WIPE_FS:
        return cut_fs_wipe(f, v);
    case GW_FRESET_FS_MARK:
        return f->fs ? cut_atomic(&f->mark, true, v) : v == 0;
    case GW_FRESET_FACTORY:
        return f->nvs_usable ? cut_atomic(&f->factory, true, v) : v == 0;
    case GW_FRESET_FS_UNMARK:
        return f->fs ? cut_atomic(&f->mark, false, v) : v == 0;
    case GW_FRESET_STEPS:
    default:
        return false;
    }
}

static bool op_step(void *ctx, gw_freset_step_t step)
{
    model_t *m = ctx;
    flash_t *f = &m->f;
    if (m->dead) {
        return false;   /* the code under test cannot know: in the firmware it does not run */
    }
    if (m->log_n < (int)(sizeof(m->log) / sizeof(m->log[0]))) {
        m->log[m->log_n++] = step;
    }
    if (m->calls++ == m->cut_call) {
        m->no_variant = !cut_step(m, step);
        m->cut_done = true;
        m->dead = true;
        return false;
    }
    switch (step) {
    case GW_FRESET_NVS_OPEN:
    case GW_FRESET_ERASE_NVS:
        if (m->fault.nvs_erase) {
            return false;
        }
        f->intent = f->factory = f->secrets = false;
        f->nvs_usable = true;
        m->fault.intent_once = false;   /* the erased NVS has space */
        return true;
    case GW_FRESET_INTENT:
        if (!f->nvs_usable || m->fault.nvs_write || m->fault.intent_once) {
            return false;
        }
        f->intent = true;
        return true;
    case GW_FRESET_WIPE_FS:
        if (m->fault.fs_wipe) {
            return false;
        }
        f->fs = true;
        f->file = f->pem = f->mark = false;
        return true;
    case GW_FRESET_FS_MARK:
        if (!f->fs || m->fault.fs_write) {
            return false;
        }
        f->mark = true;
        return true;
    case GW_FRESET_FACTORY:
        if (!f->nvs_usable || m->fault.nvs_write) {
            return false;
        }
        f->factory = true;
        return true;
    case GW_FRESET_FS_UNMARK:
        if (!f->fs || m->fault.fs_unlink) {
            return false;
        }
        f->mark = false;
        return true;
    case GW_FRESET_STEPS:
    default:
        return false;
    }
}

static gw_freset_ops_t ops_of(model_t *m)
{
    const gw_freset_ops_t ops = {.ctx = m, .nvs_usable = op_nvs_usable, .intent = op_intent,
                                 .fs_mark = op_fs_mark, .step = op_step};
    return ops;
}

/* A new power-up on the flash of @p m: the cut plan (-1: no cut) and an empty call log. */
static void power_up(model_t *m, int cut_call, int cut_variant)
{
    m->cut_call = cut_call;
    m->cut_variant = cut_variant;
    m->calls = 0;
    m->dead = m->cut_done = m->no_variant = false;
    m->log_n = 0;
}

/* The request of the owner in a running gateway. */
static gw_freset_result_t request(model_t *m, int cut_call, int cut_variant,
                                  gw_freset_step_t *failed)
{
    power_up(m, cut_call, cut_variant);
    gw_freset_ops_t ops = ops_of(m);
    return gw_freset_run(&ops, failed);
}

/* The boot of main.c: the remaining steps of a pending reset (not in safe mode), a restart after
 * a completed reset, then the source of the configuration. With a cut, the result has no
 * meaning: the caller boots again. */
static gw_freset_boot_t boot(model_t *m, int cut_call, int cut_variant, bool safe_mode)
{
    power_up(m, cut_call, cut_variant);
    gw_freset_ops_t ops = ops_of(m);
    if (!safe_mode && gw_freset_resume(&ops, NULL) == GW_FRESET_DONE && !m->dead) {
        /* the restart: this boot finds nothing pending */
        TEST_ASSERT_EQUAL_INT(GW_FRESET_NONE, gw_freset_resume(&ops, NULL));
    }
    bool pending = gw_freset_pending(&ops) != GW_FRESET_IDLE;
    flash_t *f = &m->f;
    return gw_freset_boot(pending, f->nvs_usable && f->factory, f->fs, f->fs && f->file);
}

/* The reset is complete: nothing from before it is left, and the next boot is commissioning. */
static bool complete(const flash_t *f)
{
    return f->nvs_usable && f->factory && !f->intent && !f->secrets && f->fs && !f->file &&
           !f->pem && !f->mark;
}

static bool same_fs(const flash_t *a, const flash_t *b)
{
    return a->fs == b->fs && a->file == b->file && a->pem == b->pem && a->mark == b->mark;
}

static bool same_nvs(const flash_t *a, const flash_t *b)
{
    return a->nvs_usable == b->nvs_usable && a->intent == b->intent &&
           a->factory == b->factory && a->secrets == b->secrets;
}

static const char *text_of(const flash_t *f, char *buf, size_t cap)
{
    snprintf(buf, cap, "NVS{usable %d intent %d factory %d secrets %d} FS{mounts %d file %d pem %d "
                       "mark %d}", f->nvs_usable, f->intent, f->factory, f->secrets, f->fs,
             f->file, f->pem, f->mark);
    return buf;
}

/* ---------- each cut point, also in the boots that follow ---------- */

#define MAX_BOOT_CUTS 8

static unsigned bits_of(const flash_t *f)
{
    return (f->nvs_usable ? 1u : 0u) | (f->intent ? 2u : 0u) | (f->factory ? 4u : 0u) |
           (f->secrets ? 8u : 0u) | (f->fs ? 16u : 0u) | (f->file ? 32u : 0u) |
           (f->pem ? 64u : 0u) | (f->mark ? 128u : 0u);
}

static flash_t flash_of(unsigned bits)
{
    const flash_t f = {.nvs_usable = (bits & 1u) != 0, .intent = (bits & 2u) != 0,
                       .factory = (bits & 4u) != 0, .secrets = (bits & 8u) != 0,
                       .fs = (bits & 16u) != 0, .file = (bits & 32u) != 0,
                       .pem = (bits & 64u) != 0, .mark = (bits & 128u) != 0};
    return f;
}

typedef struct {
    flash_t start;          /* the flash when the owner asks for the reset */
    flash_t before;         /* ... after a boot without a cut: what the owner had */
    gw_freset_boot_t mode0; /* the source of the configuration of that boot */
    /* the states of the flash that this search examined already, for each number of cut boots
     * that can still follow: the result of a state does not depend on the way to it */
    bool seen[MAX_BOOT_CUTS + 1][256];
    unsigned long states;      /* states examined (after a boot without a cut) */
    unsigned long completed;   /* ... in which the reset was complete */
} search_t;

/* @p f after power cuts: a boot without a cut must give a permitted state. */
static void check_final(search_t *s, const flash_t *f)
{
    model_t m = {.f = *f};
    gw_freset_boot_t mode = boot(&m, -1, 0, false);
    char a[160], b[160], msg[420];
    snprintf(msg, sizeof(msg), "start %s; after the cuts and a boot %s; mode %d",
             text_of(&s->start, a, sizeof(a)), text_of(&m.f, b, sizeof(b)), (int)mode);
    s->states++;
    /* a boot without a cut and without a fault always completes a pending reset */
    TEST_ASSERT_MESSAGE(mode != GW_FRESET_BOOT_PENDING, msg);
    if (complete(&m.f)) {
        TEST_ASSERT_MESSAGE(mode == GW_FRESET_BOOT_COMMISSIONING, msg);
        s->completed++;
        return;
    }
    /* Not complete: nothing of LittleFS changed, and the NVS is as before. The only exception is
     * an NVS that was not usable when the reset started: its erase comes first (gw_freset.h). */
    TEST_ASSERT_MESSAGE(same_fs(&m.f, &s->before), msg);
    TEST_ASSERT_MESSAGE(same_nvs(&m.f, &s->before) || !s->start.nvs_usable, msg);
    /* never the example configuration, except for a gateway that ran it before the reset */
    TEST_ASSERT_MESSAGE(mode != GW_FRESET_BOOT_EXAMPLE || s->mode0 == GW_FRESET_BOOT_EXAMPLE, msg);
    /* the configuration file is read only when the reset erased nothing of LittleFS */
    TEST_ASSERT_MESSAGE(mode != GW_FRESET_BOOT_FILE || s->mode0 == GW_FRESET_BOOT_FILE, msg);
}

/* Boots with a cut at each step call and each part result, @p depth times in sequence. */
static void cut_the_boots(search_t *s, const flash_t *f, int depth)
{
    if (s->seen[depth][bits_of(f)]) {
        return;
    }
    s->seen[depth][bits_of(f)] = true;
    check_final(s, f);
    if (depth == 0) {
        return;
    }
    for (int call = 0;; call++) {
        bool reached = false;
        for (int variant = 0;; variant++) {
            model_t m = {.f = *f};
            boot(&m, call, variant, false);
            if (!m.cut_done || m.no_variant) {
                break;
            }
            reached = true;
            cut_the_boots(s, &m.f, depth - 1);
        }
        if (!reached) {
            break;
        }
    }
}

static void search_from(search_t *s, const flash_t *start, int boot_cuts)
{
    memset(s->seen, 0, sizeof(s->seen));
    s->start = *start;
    model_t ref = {.f = *start};
    s->mode0 = boot(&ref, -1, 0, false);
    s->before = ref.f;
    /* no cut in the request */
    model_t whole = {.f = *start};
    gw_freset_step_t failed = GW_FRESET_STEPS;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_DONE, request(&whole, -1, 0, &failed));
    TEST_ASSERT_TRUE(complete(&whole.f));
    cut_the_boots(s, &whole.f, boot_cuts);
    /* a cut at each step call of the request, with each part result */
    for (int call = 0;; call++) {
        bool reached = false;
        for (int variant = 0;; variant++) {
            model_t m = {.f = *start};
            request(&m, call, variant, NULL);
            if (!m.cut_done || m.no_variant) {
                break;
            }
            reached = true;
            cut_the_boots(s, &m.f, boot_cuts);
        }
        if (!reached) {
            break;
        }
    }
}

/* Each state of the flash as the start, also the states that a cut reset leaves: a cut in the
 * request, then a cut in each of the three boots that follow. */
static void test_a_cut_at_each_point_from_each_state(void)
{
    static search_t s;
    memset(&s, 0, sizeof(s));
    for (unsigned bits = 0; bits < 256u; bits++) {
        flash_t start = flash_of(bits);
        TEST_ASSERT_EQUAL_UINT(bits, bits_of(&start));
        search_from(&s, &start, 3);
    }
    /* the search is not empty, and it has both results */
    TEST_ASSERT_TRUE(s.states > 256ul);
    TEST_ASSERT_TRUE(s.completed > 0 && s.completed < s.states);
}

/* A configured gateway (the usual start), with a cut in the request and in eight boots. */
static void test_eight_cut_boots_of_a_configured_gateway(void)
{
    const flash_t start = {.nvs_usable = true, .secrets = true, .fs = true, .file = true,
                           .pem = true};
    static search_t s;
    memset(&s, 0, sizeof(s));
    search_from(&s, &start, MAX_BOOT_CUTS);
    TEST_ASSERT_TRUE(s.completed > 0 && s.completed < s.states);
}

/* ---------- the rows of the table of issue #266 ---------- */

static const flash_t k_configured = {.nvs_usable = true, .secrets = true, .fs = true,
                                     .file = true, .pem = true};

/* The call number of the first @p step in a request from @p start without a cut. */
static int call_of(const flash_t *start, gw_freset_step_t step)
{
    model_t m = {.f = *start};
    request(&m, -1, 0, NULL);
    for (int i = 0; i < m.log_n; i++) {
        if (m.log[i] == step) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE("the request does not do this step");
    return -1;
}

static void test_the_order_of_the_steps(void)
{
    model_t m = {.f = k_configured};
    gw_freset_step_t failed = GW_FRESET_STEPS;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_DONE, request(&m, -1, 0, &failed));
    const gw_freset_step_t want[] = {GW_FRESET_INTENT, GW_FRESET_WIPE_FS, GW_FRESET_FS_MARK,
                                     GW_FRESET_ERASE_NVS, GW_FRESET_FACTORY, GW_FRESET_FS_UNMARK};
    TEST_ASSERT_EQUAL_INT(6, m.log_n);
    TEST_ASSERT_EQUAL_INT_ARRAY(want, m.log, 6);
    TEST_ASSERT_TRUE(complete(&m.f));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
    TEST_ASSERT_EQUAL_INT(0, m.log_n);   /* nothing is pending: the boot does no step */
    /* an NVS that is not usable is erased first, so that it can hold the intent */
    flash_t no_nvs = k_configured;
    no_nvs.nvs_usable = false;
    model_t n = {.f = no_nvs};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_DONE, request(&n, -1, 0, &failed));
    TEST_ASSERT_EQUAL_INT(7, n.log_n);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_NVS_OPEN, n.log[0]);
    TEST_ASSERT_EQUAL_INT_ARRAY(want, &n.log[1], 6);
    TEST_ASSERT_TRUE(complete(&n.f));
}

/* Before the intent is durable: the state before the reset. */
static void test_cut_before_the_intent(void)
{
    model_t m = {.f = k_configured};
    request(&m, call_of(&k_configured, GW_FRESET_INTENT), 0, NULL);
    TEST_ASSERT_TRUE(m.cut_done);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_FILE, boot(&m, -1, 0, false));
    TEST_ASSERT_TRUE(same_fs(&m.f, &k_configured) && same_nvs(&m.f, &k_configured));
}

/* During the erase of LittleFS: it does not mount, and the NVS is complete. Before #266 this
 * gave recovery mode with the old secrets; now the boot completes the reset. */
static void test_cut_during_the_wipe_of_littlefs(void)
{
    for (int v = 0; v < FS_WIPE_VARIANTS - 1; v++) {
        model_t m = {.f = k_configured};
        request(&m, call_of(&k_configured, GW_FRESET_WIPE_FS), v, NULL);
        TEST_ASSERT_TRUE(m.cut_done && !m.no_variant);
        TEST_ASSERT_TRUE(m.f.secrets && m.f.intent);
        TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
        TEST_ASSERT_EQUAL_INT(GW_FRESET_WIPE_FS, m.log[0]);
        TEST_ASSERT_TRUE(complete(&m.f));
    }
}

/* After the erase of LittleFS, before the erase of the NVS: no configuration file, and the old
 * secrets in the NVS. This was the defect of #266: the example configuration in normal mode. */
static void test_cut_between_the_two_erases(void)
{
    /* the wipe is done, the marker file is not written (0) or written (1) */
    for (int v = 0; v <= 1; v++) {
        model_t m = {.f = k_configured};
        request(&m, call_of(&k_configured, GW_FRESET_FS_MARK), v, NULL);
        TEST_ASSERT_TRUE(m.cut_done && m.f.fs && !m.f.file && m.f.secrets && !m.f.factory);
        TEST_ASSERT_EQUAL_INT(v == 1, m.f.mark);
        /* what the boot of the firmware before #266 looked at gives the example configuration */
        TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_EXAMPLE,
                              gw_freset_boot(false, m.f.factory, m.f.fs, m.f.file));
        TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
        TEST_ASSERT_TRUE(complete(&m.f));
    }
}

/* During the erase of the NVS, or after it and before the factory marker: all, a part or none
 * of the old secrets, with or without the intent, and an NVS that starts or does not start.
 * This was the second defect row of #266. */
static void test_cut_during_the_erase_of_the_nvs(void)
{
    for (int v = 0; v < NVS_ERASE_VARIANTS; v++) {
        model_t m = {.f = k_configured};
        m.f.factory = true;   /* the factory marker of an earlier reset is also there */
        request(&m, call_of(&k_configured, GW_FRESET_ERASE_NVS), v, NULL);
        TEST_ASSERT_TRUE(m.cut_done && !m.no_variant && m.f.mark && !m.f.file);
        bool intent = m.f.nvs_usable && m.f.intent;
        TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
        /* with the intent, the boot wipes LittleFS again; without it, the marker file tells it
         * to erase the NVS again, also when the old factory marker is there */
        TEST_ASSERT_EQUAL_INT(intent ? GW_FRESET_WIPE_FS : GW_FRESET_ERASE_NVS, m.log[0]);
        TEST_ASSERT_TRUE(complete(&m.f));
    }
    /* the erase is done, the factory marker is not written */
    model_t m = {.f = k_configured};
    request(&m, call_of(&k_configured, GW_FRESET_FACTORY), 0, NULL);
    TEST_ASSERT_TRUE(m.cut_done && !m.f.factory && !m.f.secrets && m.f.mark);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
    TEST_ASSERT_TRUE(complete(&m.f));
}

/* After the factory marker, before the removal of the marker file. */
static void test_cut_after_the_factory_marker(void)
{
    model_t m = {.f = k_configured};
    request(&m, call_of(&k_configured, GW_FRESET_FS_UNMARK), 0, NULL);
    TEST_ASSERT_TRUE(m.cut_done && m.f.factory && m.f.mark);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_ERASE_NVS, m.log[0]);
    TEST_ASSERT_TRUE(complete(&m.f));
}

/* ---------- a board after its first flash, and the other boots without a reset ---------- */

static void test_the_boot_without_a_reset_stays_as_it_was(void)
{
    /* the first flash: an empty NVS, and a LittleFS image without a configuration file */
    model_t fresh = {.f = {.nvs_usable = true, .fs = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_EXAMPLE, boot(&fresh, -1, 0, false));
    TEST_ASSERT_EQUAL_INT(0, fresh.log_n);
    /* an erased flash without a LittleFS image: recovery mode */
    model_t blank = {.f = {.nvs_usable = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_NO_FS, boot(&blank, -1, 0, false));
    /* a configuration file */
    model_t cfg = {.f = k_configured};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_FILE, boot(&cfg, -1, 0, false));
    /* the factory marker: commissioning, also with a LittleFS that does not mount (the CFG-7
     * migration was cut in its wipe) */
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, gw_freset_boot(false, true, true, false));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, gw_freset_boot(false, true, false, false));
    /* a configuration flashed after the reset: the file */
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_FILE, gw_freset_boot(false, true, true, true));
    /* a pending reset is stronger than each other fact */
    for (unsigned bits = 0; bits < 8u; bits++) {
        TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING,
                              gw_freset_boot(true, (bits & 1u) != 0, (bits & 2u) != 0,
                                             (bits & 4u) != 0));
    }
}

/* ---------- steps that fail without a power cut ---------- */

/* A step that fails stops the sequence; the reset stays pending, and the next boot does it. */
static void test_a_failed_step_keeps_the_reset_pending(void)
{
    gw_freset_step_t failed = GW_FRESET_STEPS;
    /* LittleFS cannot be wiped: nothing more is erased, the intent stays */
    model_t m = {.f = k_configured, .fault = {.fs_wipe = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FAILED, request(&m, -1, 0, &failed));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_WIPE_FS, failed);
    TEST_ASSERT_TRUE(m.f.intent && m.f.file && m.f.secrets);
    /* the fault stays: recovery mode, not the configuration file */
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&m, -1, 0, false));
    /* the fault is gone: the boot completes the reset */
    m.fault.fs_wipe = false;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&m, -1, 0, false));
    TEST_ASSERT_TRUE(complete(&m.f));

    /* the marker file cannot be written: the NVS (with the intent) is not erased */
    model_t w = {.f = k_configured, .fault = {.fs_write = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FAILED, request(&w, -1, 0, &failed));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FS_MARK, failed);
    TEST_ASSERT_TRUE(w.f.intent && w.f.secrets && !w.f.file);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&w, -1, 0, false));

    /* the NVS cannot be erased: the marker file stays */
    model_t e = {.f = k_configured, .fault = {.nvs_erase = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FAILED, request(&e, -1, 0, &failed));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_ERASE_NVS, failed);
    TEST_ASSERT_TRUE(e.f.mark && !e.f.file);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&e, -1, 0, false));
    e.fault.nvs_erase = false;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&e, -1, 0, false));
    TEST_ASSERT_TRUE(complete(&e.f));

    /* the marker file cannot be removed: the next boot erases the NVS again and removes it */
    model_t u = {.f = k_configured, .fault = {.fs_unlink = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FAILED, request(&u, -1, 0, &failed));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FS_UNMARK, failed);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&u, -1, 0, false));
    u.fault.fs_unlink = false;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&u, -1, 0, false));
    TEST_ASSERT_TRUE(complete(&u.f));
}

/* An NVS that refuses the intent (it is full) is erased first, then it holds the intent. */
static void test_a_full_nvs_does_not_stop_the_reset(void)
{
    model_t m = {.f = k_configured, .fault = {.intent_once = true}};
    gw_freset_step_t failed = GW_FRESET_STEPS;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_DONE, request(&m, -1, 0, &failed));
    const gw_freset_step_t want[] = {GW_FRESET_INTENT, GW_FRESET_NVS_OPEN, GW_FRESET_INTENT,
                                     GW_FRESET_WIPE_FS};
    TEST_ASSERT_EQUAL_INT_ARRAY(want, m.log, 4);
    TEST_ASSERT_TRUE(complete(&m.f));
}

/* An NVS with a hardware fault: the erase of LittleFS goes on (the owner must be able to erase
 * the configuration), and the marker file keeps the gateway out of normal mode. */
static void test_an_nvs_that_cannot_be_written(void)
{
    model_t m = {.f = k_configured, .fault = {.nvs_write = true}};
    gw_freset_step_t failed = GW_FRESET_STEPS;
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FAILED, request(&m, -1, 0, &failed));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FACTORY, failed);
    TEST_ASSERT_TRUE(!m.f.file && !m.f.pem && !m.f.secrets && m.f.mark && !m.f.factory);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&m, -1, 0, false));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&m, -1, 0, false));
}

/* Safe mode does not do the steps: the reset stays pending, and no configuration is read. A
 * new request in that boot starts from the intent, so that the next boot wipes LittleFS again:
 * the marker file of the first reset must not hide files that came after it. */
static void test_safe_mode_and_a_second_request(void)
{
    model_t m = {.f = k_configured};
    request(&m, call_of(&k_configured, GW_FRESET_ERASE_NVS), 0, NULL);   /* intent and file */
    TEST_ASSERT_TRUE(m.f.intent && m.f.mark && m.f.secrets);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&m, -1, 0, true));
    TEST_ASSERT_EQUAL_INT(0, m.log_n);
    TEST_ASSERT_TRUE(m.f.intent && m.f.mark && m.f.secrets);

    /* only the marker file is left, and the owner loads a PEM file in safe mode */
    model_t s = {.f = {.nvs_usable = true, .secrets = true, .fs = true, .mark = true}};
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_PENDING, boot(&s, -1, 0, true));
    s.f.pem = true;
    flash_t start = s.f;
    request(&s, call_of(&start, GW_FRESET_WIPE_FS), 0, NULL);   /* cut: nothing wiped */
    TEST_ASSERT_TRUE(s.cut_done && s.f.intent && s.f.mark && s.f.pem);
    gw_freset_ops_t ops = ops_of(&s);
    power_up(&s, -1, 0);
    TEST_ASSERT_EQUAL_INT(GW_FRESET_FROM_WIPE, gw_freset_pending(&ops));
    TEST_ASSERT_EQUAL_INT(GW_FRESET_BOOT_COMMISSIONING, boot(&s, -1, 0, false));
    TEST_ASSERT_TRUE(complete(&s.f));
}

static void test_the_step_names(void)
{
    for (int s = 0; s < (int)GW_FRESET_STEPS; s++) {
        const char *name = gw_freset_step_name((gw_freset_step_t)s);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 1);
    }
    TEST_ASSERT_EQUAL_STRING("?", gw_freset_step_name(GW_FRESET_STEPS));
}

/* Issue #280: a blank LittleFS partition (a new board, a flash with a full erase and no LittleFS
 * image) is formatted only when the NVS can hold the factory marker first, and never while a
 * factory reset is pending or when the partition mounts or is not blank. */
static void test_a_blank_partition_starts_in_commissioning(void)
{
    for (int i = 0; i < 16; i++) {
        bool pending = (i & 1) != 0, nvs = (i & 2) != 0, fs = (i & 4) != 0, blank = (i & 8) != 0;
        bool want = !pending && nvs && !fs && blank;
        TEST_ASSERT_EQUAL(want, gw_freset_adopt_blank(pending, nvs, fs, blank));
    }
    /* after the adoption: the factory marker, a new LittleFS, no file */
    TEST_ASSERT_EQUAL(GW_FRESET_BOOT_COMMISSIONING, gw_freset_boot(false, true, true, false));
    /* no adoption without the NVS: the partition stays blank, recovery mode, not the example */
    TEST_ASSERT_FALSE(gw_freset_adopt_blank(false, false, false, true));
    TEST_ASSERT_EQUAL(GW_FRESET_BOOT_NO_FS, gw_freset_boot(false, false, false, false));
    /* a partition with data that does not mount is never formatted here (#104, #109) */
    TEST_ASSERT_FALSE(gw_freset_adopt_blank(false, true, false, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_order_of_the_steps);
    RUN_TEST(test_cut_before_the_intent);
    RUN_TEST(test_cut_during_the_wipe_of_littlefs);
    RUN_TEST(test_cut_between_the_two_erases);
    RUN_TEST(test_cut_during_the_erase_of_the_nvs);
    RUN_TEST(test_cut_after_the_factory_marker);
    RUN_TEST(test_the_boot_without_a_reset_stays_as_it_was);
    RUN_TEST(test_a_failed_step_keeps_the_reset_pending);
    RUN_TEST(test_a_full_nvs_does_not_stop_the_reset);
    RUN_TEST(test_an_nvs_that_cannot_be_written);
    RUN_TEST(test_safe_mode_and_a_second_request);
    RUN_TEST(test_a_cut_at_each_point_from_each_state);
    RUN_TEST(test_eight_cut_boots_of_a_configured_gateway);
    RUN_TEST(test_the_step_names);
    RUN_TEST(test_a_blank_partition_starts_in_commissioning);
    return UNITY_END();
}
