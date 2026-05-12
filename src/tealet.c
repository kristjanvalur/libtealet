#include "tealet.h"
#include <stackman.h>

#include <stddef.h>
#if !defined(_MSC_VER) || _MSC_VER >= 1600 /* VS2010 and above */
#include <stdint.h>                        /* for intptr_t */
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEALET_WITH_STACK_GUARD
#define TEALET_WITH_STACK_GUARD 1
#endif

#ifndef TEALET_WITH_STACK_SNAPSHOT
#define TEALET_WITH_STACK_SNAPSHOT 1
#endif

#ifndef TEALET_WITH_TESTING
#define TEALET_WITH_TESTING 0
#endif

#if TEALET_WITH_STACK_GUARD && !defined(_WIN32)
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#define TEALET_GUARD_MPROTECT 1
#else
#define TEALET_GUARD_MPROTECT 0
#endif

/* enable collection of tealet stats - default enabled, define
 * TEALET_WITH_STATS=0 to disable */
#ifndef TEALET_WITH_STATS
#define TEALET_WITH_STATS 1
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Internal main-tealet flags (stored in tealet_main_t::g_flags).
 * One-shot transfer signals are set by xfer preparation and consumed/cleared
 * by tealet_switchstack().
 */
#define TEALET_MFLAGS_PANIC (1 << 16)

/* Internal per-tealet flags (stored in tealet_sub_t::flags).
 * TEALET_TFLAGS_SAVEFORCE is a one-shot transfer signal set by xfer
 * preparation and consumed/cleared by tealet_save_state().
 */
#define TEALET_TFLAGS_BOUND (1u << 0)
#define TEALET_TFLAGS_EXITING (1u << 1)
#define TEALET_TFLAGS_EXITED (1u << 2)
#define TEALET_TFLAGS_DEFUNCT (1u << 3)
#define TEALET_TFLAGS_MAIN_LINEAGE (1u << 4)
#define TEALET_TFLAGS_FORK (1u << 5)
#define TEALET_TFLAGS_AUTODELETE (1u << 6)
#define TEALET_TFLAGS_SAVEFORCE (1u << 7)

/* Internal per-stack flags (stored in tealet_stack_t::flags). */
#define TEALET_SFLAGS_DEFUNCT (1u << 0)

/* ----------------------------------------------------------------
 * Structures for maintaining copies of the C stack.
 */

/* a chunk represents a single segment of saved stack */
typedef struct tealet_chunk_t {
  int refcount;                /* controls chunk lifetime */
  struct tealet_chunk_t *next; /* additional chunks */
  char *stack_near;            /* near stack address */
  size_t size;                 /* amount of data saved */
  char data[1];                /* the data follows here */
} tealet_chunk_t;

/* The main stack structure, contains the initial chunk and a link to further
 * segments.  Stacks can be shared by different tealets, hence the reference
 * count.  They can also be linked into a list of partially unsaved
 * stacks, that are saved only on demand.
 */
typedef struct tealet_stack_t {
  int refcount;                 /* controls lifetime */
  struct tealet_stack_t **prev; /* previous 'next' pointer */
  struct tealet_stack_t *next;  /* next unsaved stack */
  unsigned int flags;           /* internal per-stack state flags */
  char *stack_far;              /* the far boundary of this stack (or STACKMAN_SP_FURTHEST
                                   for unbounded) */
  size_t saved;                 /* total amount of memory saved in all chunks */
  struct tealet_chunk_t *last;  /* last chunk in the chain */
  struct tealet_chunk_t chunk;  /* the initial chunk */
} tealet_stack_t;

/* the actual tealet structure as used internally
 * The main tealet will have stack_far set to STACKMAN_SP_FURTHEST,
 * representing an unbounded stack extent (the entire process stack).
 * "stack" is zero for a running tealet, otherwise it points
 * to the saved stack.
 */
typedef struct tealet_sub_t {
  tealet_t base;         /* the public part of the tealet */
  char *stack_far;       /* the "far" end of the stack, or STACKMAN_SP_FURTHEST
                            for unbounded */
  tealet_stack_t *stack; /* saved stack or 0 if active */
  unsigned int flags;    /* internal per-tealet state flags */
#if TEALET_WITH_STATS
  struct tealet_sub_t *next_tealet; /* next in circular list of all tealets */
  struct tealet_sub_t *prev_tealet; /* prev in circular list of all tealets */
#endif
#ifndef NDEBUG
  int id; /* number of this tealet */
#endif
} tealet_sub_t;

/* a structure incorporating extra data */
typedef struct tealet_nonmain_t {
  tealet_sub_t base;
  double _extra[1]; /* start of any extra data */
} tealet_nonmain_t;

#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
typedef struct tealet_integrity_data_t {
  char *stack_base; /* the "base" of the monitored stack interval (the end of
                       the stack slice) */
#if TEALET_WITH_STACK_SNAPSHOT
  size_t snapshot_bytes;    /* the size of the monitored stack interval */
  char *snapshot_block;     /* fixed snapshot workspace, stores current snapshot */
  size_t snapshot_capacity; /* capacity of the snapshot workspace */
#endif
#if TEALET_WITH_STACK_GUARD
  char *guard_base;   /* the "base" of the guard interval */
  size_t guard_bytes; /* the size of the guard interval */
#endif
} tealet_integrity_data_t;
#endif

/* an enum to maintain state for the save/restore callback
 * which is called twice (with old and new stack pointer)
 */
typedef enum tealet_sr_e {
  SW_NOP,     /* do nothing (no-restore stack) */
  SW_RESTORE, /* restore stack */
  SW_ERR,     /* error occurred when saving */
} tealet_sr_e;

/* The main tealet has additional fields for housekeeping */
typedef struct tealet_main_t {
  tealet_sub_t base;
  void *g_user; /* user data pointer for main */
  char *g_main_stack_probe;
  tealet_sub_t *g_current;
  tealet_sub_t *g_previous;
  tealet_sub_t *g_target;   /* Temporary store when switching */
  void *g_arg;              /* argument passed around when switching */
  tealet_alloc_t g_alloc;   /* the allocation context used */
  tealet_lock_t g_locking;  /* optional external lock callbacks */
  tealet_stack_t *g_prev;   /* previously active unsaved stacks */
  tealet_sr_e g_sw;         /* save/restore state */
  int g_flags;              /* default flags when tealet exits */
  unsigned int g_cfg_flags; /* canonicalized runtime config flags */
  size_t g_cfg_stack_integrity_bytes;
  int g_cfg_stack_guard_mode;
  int g_cfg_stack_integrity_fail_policy;
  char *g_cfg_stack_guard_limit;
  size_t g_cfg_max_stack_size;
#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
  tealet_integrity_data_t g_integrity_data;
#endif
  int g_tealets; /* number of active tealets excluding main */
  int g_counter; /* total number of tealets */
#if TEALET_WITH_STATS
  /* Extended memory statistics */
  size_t g_bytes_allocated;        /* Current heap allocation */
  size_t g_bytes_allocated_peak;   /* Peak heap allocation */
  size_t g_blocks_allocated;       /* Current number of allocated blocks */
  size_t g_blocks_allocated_peak;  /* Peak number of allocated blocks */
  size_t g_blocks_allocated_total; /* Total allocation calls */
  size_t g_stack_bytes;            /* Bytes used for stack storage */
  size_t g_stack_count;            /* Number of stack structures currently allocated */
  size_t g_stack_chunk_count;      /* Number of stack chunks currently allocated
                                      (including initial) */
#endif
  size_t g_extrasize; /* amount of extra memory in tealets */
  double _extra[1];   /* start of any extra data */
} tealet_main_t;

/* Check if a tealet has an unbounded stack (FURTHEST sentinel).
 * Only one tealet can have an unbounded stack at a time (the active one on the
 * C stack). All other tealets must have bounded stacks (stack_far set to a
 * specific boundary).
 */
#define TEALET_STACK_IS_UNBOUNDED(t) (((tealet_sub_t *)(t))->stack_far == STACKMAN_SP_FURTHEST)
#define TEALET_GET_MAIN(t) ((tealet_main_t *)(((tealet_t *)(t))->main))

/* ----------------------------------------------------------------
 * Statistics tracking macros
 */
#if TEALET_WITH_STATS
/* Forward declaration for list verification */
/* Link a tealet into the circular list after main tealet */
#define TEALET_LIST_ADD(main, t)                                                                                       \
  do {                                                                                                                 \
    (t)->next_tealet = (main)->base.next_tealet;                                                                       \
    (t)->prev_tealet = (tealet_sub_t *)(main);                                                                         \
    (main)->base.next_tealet->prev_tealet = (t);                                                                       \
    (main)->base.next_tealet = (t);                                                                                    \
  } while (0)

/* Unlink a tealet from the circular list */
#define TEALET_LIST_REMOVE(t)                                                                                          \
  do {                                                                                                                 \
    (t)->prev_tealet->next_tealet = (t)->next_tealet;                                                                  \
    (t)->next_tealet->prev_tealet = (t)->prev_tealet;                                                                  \
  } while (0)

#define STATS_ADD_ALLOC(main, size)                                                                                    \
  do {                                                                                                                 \
    (main)->g_bytes_allocated += (size);                                                                               \
    (main)->g_blocks_allocated++;                                                                                      \
    (main)->g_blocks_allocated_total++;                                                                                \
    if ((main)->g_bytes_allocated > (main)->g_bytes_allocated_peak)                                                    \
      (main)->g_bytes_allocated_peak = (main)->g_bytes_allocated;                                                      \
    if ((main)->g_blocks_allocated > (main)->g_blocks_allocated_peak)                                                  \
      (main)->g_blocks_allocated_peak = (main)->g_blocks_allocated;                                                    \
  } while (0)

#define STATS_SUB_ALLOC(main, size)                                                                                    \
  do {                                                                                                                 \
    (main)->g_bytes_allocated -= (size);                                                                               \
    (main)->g_blocks_allocated--;                                                                                      \
  } while (0)
#else
#define TEALET_LIST_ADD(main, t) ((void)0)
#define TEALET_LIST_REMOVE(t) ((void)0)
#define STATS_ADD_ALLOC(main, size) ((void)0)
#define STATS_SUB_ALLOC(main, size) ((void)0)
#endif

/* ----------------------------------------------------------------
 * helpers to call the malloc functions provided by the user
 */
static void *tealet_int_malloc(tealet_main_t *main, size_t size) {
  return main->g_alloc.malloc_p(size, main->g_alloc.context);
}
static void tealet_int_free(tealet_main_t *main, void *ptr) { main->g_alloc.free_p(ptr, main->g_alloc.context); }

/* Debug-only sanity check that caller stack plausibly matches current tealet.
 *
 * This is intentionally best-effort: we do not know the full virtual-memory
 * extent for the current stack slice, so for bounded stacks we can only check
 * ordering against current->stack_far. That catches some foreign-thread calls,
 * but not all of them.
 */
static void tealet_verify_current_matches_caller(tealet_sub_t *current) {
#ifndef NDEBUG
  char stack_probe = 0;
  char *sp = &stack_probe;

  assert(current != NULL);
  assert((current->flags & TEALET_TFLAGS_EXITED) == 0);

  if (current->stack_far != STACKMAN_SP_FURTHEST) {
    assert(current->stack_far != NULL);
    assert(STACKMAN_SP_LS(sp, current->stack_far));
  }
#else
  (void)current;
#endif
}

/* ----------------------------------------------------------------
 * Stack integrity helpers (snapshot + guard planning).
 */
#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
static void tealet_integrity_data_clear(tealet_integrity_data_t *data) {
  data->stack_base = NULL;
#if TEALET_WITH_STACK_SNAPSHOT
  data->snapshot_bytes = 0;
#endif
#if TEALET_WITH_STACK_GUARD
  data->guard_base = NULL;
  data->guard_bytes = 0;
#endif
}

/** Initialize persistent integrity workspace state.
 *
 * This differs from tealet_integrity_data_clear() in that this function also
 * initializes long-lived storage fields (snapshot workspace pointer/capacity)
 * that are only reset at initialize/finalize boundaries.
 */
static void tealet_integrity_data_init(tealet_integrity_data_t *data) {
  tealet_integrity_data_clear(data);
#if TEALET_WITH_STACK_SNAPSHOT
  data->snapshot_block = NULL;
  data->snapshot_capacity = 0;
#endif
}

/** Release integrity workspace allocations and return all transient plan state
 * to a disabled/empty state.
 */
static void tealet_integrity_data_free(tealet_main_t *g_main, tealet_integrity_data_t *data) {
#if TEALET_WITH_STACK_SNAPSHOT
  if (data->snapshot_block != NULL) {
    tealet_int_free(g_main, data->snapshot_block);
    data->snapshot_block = NULL;
  }
  data->snapshot_capacity = 0;
#endif
  tealet_integrity_data_clear(data);
}

#if TEALET_GUARD_MPROTECT
static size_t tealet_guard_pagesize(void) {
  long page_size = sysconf(_SC_PAGESIZE);

  if (page_size <= 0)
    return 0;
  return (size_t)page_size;
}
#endif

/** Build the integrity plan for the currently running tealet.
 *
 * The monitored interval is always a linear byte range [begin, end).
 * Snapshot and page-guard split the same interval:
 *
 * - snapshot protects the sub-page bytes that cannot be represented by
 * mprotect() under the chosen alignment strategy;
 * - mprotect() guards whole pages.
 *
 * For descending stacks, snapshot keeps the low-address prefix before the first
 * guarded page. For ascending stacks, snapshot keeps the high-address suffix
 * after the last guarded page.
 */
static void tealet_integrity_plan_for_current(tealet_main_t *g_main) {
  tealet_integrity_data_t *plan = &g_main->g_integrity_data;
  tealet_sub_t *current;
  unsigned int flags;
  size_t integrity_bytes;
  uintptr_t far_limit;
#if TEALET_GUARD_MPROTECT
  uintptr_t begin;
  uintptr_t end;
#endif
#if TEALET_WITH_STACK_SNAPSHOT
  size_t snapshot_bytes;
#endif

  tealet_integrity_data_clear(plan);

  current = g_main->g_current;
  tealet_verify_current_matches_caller(current);
  assert(current != NULL);

  if (current->stack_far == STACKMAN_SP_FURTHEST)
    return;

  flags = g_main->g_cfg_flags;
  if ((flags & TEALET_CONFIGF_STACK_INTEGRITY) == 0)
    return;

  integrity_bytes = g_main->g_cfg_stack_integrity_bytes;
  if (integrity_bytes == 0)
    return;

  if (g_main->g_cfg_stack_guard_limit != NULL) {
    far_limit = (uintptr_t)g_main->g_cfg_stack_guard_limit;
#if STACK_DIRECTION == 0
    if (far_limit <= (uintptr_t)current->stack_far)
      return;
    integrity_bytes = MIN(integrity_bytes, (size_t)(far_limit - (uintptr_t)current->stack_far));
#else
    if (far_limit >= (uintptr_t)current->stack_far)
      return;
    integrity_bytes = MIN(integrity_bytes, (size_t)((uintptr_t)current->stack_far - far_limit));
#endif
    if (integrity_bytes == 0)
      return;
  }

#if STACK_DIRECTION == 0
  plan->stack_base = current->stack_far;
#else
  plan->stack_base = current->stack_far - integrity_bytes;
#endif

#if TEALET_GUARD_MPROTECT
  begin = (uintptr_t)plan->stack_base;
  end = begin + integrity_bytes; /* half-open interval [begin, end) */
#endif

#if TEALET_WITH_STACK_SNAPSHOT
  snapshot_bytes = 0;
  if ((flags & TEALET_CONFIGF_STACK_SNAPSHOT) != 0)
    snapshot_bytes = integrity_bytes;
#endif

#if TEALET_GUARD_MPROTECT
  if ((flags & TEALET_CONFIGF_STACK_GUARD) != 0) {
    size_t page_size = tealet_guard_pagesize();

    if (page_size != 0) {
      if (end > begin) {
        uintptr_t page_mask = (uintptr_t)(page_size - 1);
        uintptr_t aligned_begin;
        uintptr_t aligned_end;

#if STACK_DIRECTION == 0
        /* Descending stack:
         *  - guard starts at the first page boundary at/above begin;
         *  - guard end rounds up so we can cover the full interval with pages.
         */
        aligned_begin = (begin + page_mask) & ~page_mask;
        aligned_end = (end + page_mask) & ~page_mask;

        if (aligned_begin >= end)
          aligned_end = aligned_begin;

#if TEALET_WITH_STACK_SNAPSHOT
        if (snapshot_bytes != 0) {
          /* Snapshot the unguardable prefix [begin, aligned_begin). */
          size_t prefix_bytes = (size_t)(aligned_begin - begin);

          snapshot_bytes = MIN(snapshot_bytes, prefix_bytes);
        }
#endif

#else
        /* Ascending stack:
         *  - guard start rounds down so guarded bytes are page-granular;
         *  - guard end rounds down to the last full page below end.
         */
        aligned_begin = begin & ~page_mask;
        aligned_end = end & ~page_mask;

        if (aligned_end <= aligned_begin)
          aligned_begin = aligned_end;

#if TEALET_WITH_STACK_SNAPSHOT
        if (snapshot_bytes != 0) {
          /* Snapshot the unguardable suffix [aligned_end, end). */
          size_t suffix_bytes = (size_t)(end - aligned_end);

          snapshot_bytes = MIN(snapshot_bytes, suffix_bytes);
          if (snapshot_bytes != 0)
            plan->stack_base = (char *)(end - snapshot_bytes);
        }
#endif
#endif

        if (aligned_end > aligned_begin) {
          plan->guard_base = (char *)aligned_begin;
          plan->guard_bytes = (size_t)(aligned_end - aligned_begin);
        }
      }
    }
  }
#endif

#if TEALET_WITH_STACK_SNAPSHOT
  if (snapshot_bytes != 0) {
    assert(g_main->g_integrity_data.snapshot_capacity >= snapshot_bytes);
    if (g_main->g_integrity_data.snapshot_capacity >= snapshot_bytes)
      plan->snapshot_bytes = snapshot_bytes;
  }
#endif
}
#endif

/* ----------------------------------------------------------------
 * Stack guard helpers.
 */
#if TEALET_WITH_STACK_GUARD
#if TEALET_GUARD_MPROTECT
static void tealet_guard_unprotect_current(tealet_main_t *g_main) {
  const tealet_integrity_data_t *plan = &g_main->g_integrity_data;

  if (plan->guard_bytes == 0)
    return;
  assert(plan->guard_base != NULL);

  /* Best-effort unprotect: if this fails, we still clear active guard state.
   * There is no in-place recovery path here, and keeping stale guard metadata
   * would make subsequent behavior inconsistent.
   */
  errno = 0;
  mprotect(plan->guard_base, plan->guard_bytes, PROT_READ | PROT_WRITE);
  g_main->g_integrity_data.guard_bytes = 0;
}

static void tealet_guard_protect_current(tealet_main_t *g_main) {
  const tealet_integrity_data_t *plan = &g_main->g_integrity_data;
  int guard_mode;
  int prot;
  int rc;

  if (plan->guard_bytes == 0)
    return;
  assert(plan->guard_base != NULL);

  guard_mode = g_main->g_cfg_stack_guard_mode;
  if (guard_mode == TEALET_STACK_GUARD_MODE_READONLY)
    prot = PROT_READ;
  else
    prot = PROT_NONE;

  /* Best-effort protect: mprotect can fail (for example if the interval is not
   * representable in mapped stack pages). We intentionally degrade by clearing
   * active guard state because there is no reliable recovery path here.
   */
  errno = 0;
  rc = mprotect(plan->guard_base, plan->guard_bytes, prot);

  if (rc != 0) {
    g_main->g_integrity_data.guard_bytes = 0;
    return;
  }
}
#else
static void tealet_guard_unprotect_current(tealet_main_t *g_main) { (void)g_main; }

static void tealet_guard_protect_current(tealet_main_t *g_main) { (void)g_main; }
#endif

#else
static void tealet_guard_unprotect_current(tealet_main_t *g_main) { (void)g_main; }

static void tealet_guard_protect_current(tealet_main_t *g_main) { (void)g_main; }
#endif

/* ----------------------------------------------------------------
 * Stack snapshot helpers.
 */
#if TEALET_WITH_STACK_SNAPSHOT
static size_t tealet_snapshot_required_capacity(const tealet_config_t *config) {
  size_t required = 0;

  if (config->flags & TEALET_CONFIGF_STACK_SNAPSHOT)
    required = config->stack_integrity_bytes;
#if TEALET_GUARD_MPROTECT
  if ((config->flags & (TEALET_CONFIGF_STACK_SNAPSHOT | TEALET_CONFIGF_STACK_GUARD)) ==
      (TEALET_CONFIGF_STACK_SNAPSHOT | TEALET_CONFIGF_STACK_GUARD)) {
    size_t page_size = tealet_guard_pagesize();

    if (page_size > 1) {
      size_t prefix_limit = page_size - 1;

      required = MIN(required, prefix_limit);
    }
  }
#endif
  return required;
}

/** Ensure snapshot workspace can hold the largest snapshot implied by the
 * effective configuration.  This is called at configure-set time so switching
 * never needs to allocate snapshot buffers.
 */
static int tealet_snapshot_ensure_capacity(tealet_main_t *g_main, size_t required) {
  char *new_block;

  if (required == 0)
    return 0;
  if (g_main->g_integrity_data.snapshot_capacity >= required)
    return 0;

  new_block = (char *)tealet_int_malloc(g_main, required);
  if (new_block == NULL)
    return TEALET_ERR_MEM;

  /* Resize invalidates any active snapshot that was captured into the old
   * buffer. We allow reconfiguration during active execution, so clear the
   * active marker before replacing storage.
   */
  g_main->g_integrity_data.snapshot_bytes = 0;
  g_main->g_integrity_data.stack_base = NULL;

  if (g_main->g_integrity_data.snapshot_block) {
    tealet_int_free(g_main, g_main->g_integrity_data.snapshot_block);
  }
  g_main->g_integrity_data.snapshot_block = new_block;
  g_main->g_integrity_data.snapshot_capacity = required;
  return 0;
}

/** Capture monitored stack bytes for the current tealet into the fixed snapshot
 * workspace.  Planning decides stack_base/size; this routine only copies.
 */
static void tealet_snapshot_capture_current(tealet_main_t *g_main) {
  tealet_sub_t *current = g_main->g_current;
  char *stack_base = g_main->g_integrity_data.stack_base;
  size_t size = g_main->g_integrity_data.snapshot_bytes;

  tealet_verify_current_matches_caller(current);

  if (size == 0)
    return;

  assert(stack_base != NULL);
  assert(current != NULL);
  assert(current->stack_far != NULL);

  assert(size <= g_main->g_integrity_data.snapshot_capacity);
  if (current->stack_far == STACKMAN_SP_FURTHEST)
    return;

  memcpy(g_main->g_integrity_data.snapshot_block, stack_base, size);
  g_main->g_integrity_data.stack_base = stack_base;
  g_main->g_integrity_data.snapshot_bytes = size;
}

/** Verify previously captured bytes for the current tealet and clear the active
 * snapshot marker.  Mismatch handling follows configured fail policy.
 *
 * Note: on mismatch we intentionally clear the active snapshot marker before
 * returning. In FAIL_ERROR mode this lets the caller continue execution; there
 * is no API to "repair and retry" the same captured snapshot in-place.
 */
static int tealet_snapshot_verify_current(tealet_main_t *g_main) {
  int cmp;
  assert(g_main->g_current != NULL);

  if (g_main->g_integrity_data.snapshot_bytes == 0) {
    return 0;
  }
  assert(g_main->g_integrity_data.stack_base != NULL);

  cmp = memcmp(g_main->g_integrity_data.snapshot_block, g_main->g_integrity_data.stack_base,
               g_main->g_integrity_data.snapshot_bytes);
  g_main->g_integrity_data.snapshot_bytes = 0;
  if (cmp != 0) {
    int policy = g_main->g_cfg_stack_integrity_fail_policy;

    if (policy == TEALET_STACK_INTEGRITY_FAIL_ABORT)
      abort();
    if (policy == TEALET_STACK_INTEGRITY_FAIL_ERROR)
      return TEALET_ERR_INTEGRITY;
    assert(0 && "stack integrity snapshot mismatch");
    return TEALET_ERR_INTEGRITY;
  }

  return 0;
}
#else
static size_t tealet_snapshot_required_capacity(const tealet_config_t *config) {
  (void)config;
  return 0;
}

static int tealet_snapshot_ensure_capacity(tealet_main_t *g_main, size_t required) {
  (void)g_main;
  (void)required;
  return 0;
}

static int tealet_snapshot_verify_current(tealet_main_t *g_main) {
  (void)g_main;
  return 0;
}
#endif

/* helper to compute absolute stack distance, without overflowing */
/* ----------------------------------------------------------------
 * Config helpers.
 */
static unsigned int tealet_config_supported_flags(void) {
  unsigned int supported = 0;
#if TEALET_GUARD_MPROTECT
  supported |= TEALET_CONFIGF_STACK_GUARD;
#endif
#if TEALET_WITH_STACK_SNAPSHOT
  supported |= TEALET_CONFIGF_STACK_SNAPSHOT;
#endif
  if (supported != 0)
    supported |= TEALET_CONFIGF_STACK_INTEGRITY;
  return supported;
}

static int tealet_config_has_header(const tealet_config_t *config) {
  size_t min_header = offsetof(tealet_config_t, version) + sizeof(config->version);
  return config != NULL && config->size >= min_header;
}

/** Canonicalize caller-provided configuration in-place.
 *
 * Rules enforced here:
 *  - drop unsupported feature flags for this build/platform,
 *  - maintain dependency invariants (integrity requires at least one backend),
 *  - normalize mode/policy enums to valid values,
 *  - zero stack_integrity_bytes when integrity is disabled.
 */
static void tealet_config_canonicalize(tealet_config_t *config) {
  unsigned int flags;
  unsigned int supported;

  flags = config->flags;
  flags &= (TEALET_CONFIGF_STACK_INTEGRITY | TEALET_CONFIGF_STACK_GUARD | TEALET_CONFIGF_STACK_SNAPSHOT);

  supported = tealet_config_supported_flags();
  flags &= supported;

  if ((flags & TEALET_CONFIGF_STACK_INTEGRITY) == 0) {
    flags &= ~(TEALET_CONFIGF_STACK_GUARD | TEALET_CONFIGF_STACK_SNAPSHOT);
  }

  if ((flags & (TEALET_CONFIGF_STACK_GUARD | TEALET_CONFIGF_STACK_SNAPSHOT)) == 0) {
    flags &= ~TEALET_CONFIGF_STACK_INTEGRITY;
  }

  if (config->stack_integrity_bytes == 0) {
    flags &= ~(TEALET_CONFIGF_STACK_INTEGRITY | TEALET_CONFIGF_STACK_GUARD | TEALET_CONFIGF_STACK_SNAPSHOT);
  }

  if ((flags & TEALET_CONFIGF_STACK_GUARD) == 0) {
    config->stack_guard_mode = TEALET_STACK_GUARD_MODE_NONE;
  } else {
    if (config->stack_guard_mode != TEALET_STACK_GUARD_MODE_READONLY &&
        config->stack_guard_mode != TEALET_STACK_GUARD_MODE_NOACCESS) {
      config->stack_guard_mode = TEALET_STACK_GUARD_MODE_NOACCESS;
    }
  }

  if (config->stack_integrity_fail_policy != TEALET_STACK_INTEGRITY_FAIL_ASSERT &&
      config->stack_integrity_fail_policy != TEALET_STACK_INTEGRITY_FAIL_ERROR &&
      config->stack_integrity_fail_policy != TEALET_STACK_INTEGRITY_FAIL_ABORT) {
    config->stack_integrity_fail_policy = TEALET_STACK_INTEGRITY_FAIL_ASSERT;
  }

  config->flags = flags;
  if ((flags & TEALET_CONFIGF_STACK_INTEGRITY) == 0)
    config->stack_integrity_bytes = 0;

  if ((flags & TEALET_CONFIGF_STACK_INTEGRITY) == 0)
    config->stack_guard_limit = NULL;
}

/** Populate a config struct from current runtime state, then canonicalize to
 * ensure returned values obey the same invariants as configure_set().
 */
static void tealet_config_fill_from_main(tealet_main_t *g_main, tealet_config_t *config) {
  config->version = TEALET_CONFIG_CURRENT_VERSION;
  config->flags = g_main->g_cfg_flags;
  config->stack_integrity_bytes = g_main->g_cfg_stack_integrity_bytes;
  config->stack_guard_mode = g_main->g_cfg_stack_guard_mode;
  config->stack_integrity_fail_policy = g_main->g_cfg_stack_integrity_fail_policy;
  config->stack_guard_limit = g_main->g_cfg_stack_guard_limit;
  config->max_stack_size = g_main->g_cfg_max_stack_size;
  tealet_config_canonicalize(config);
}

/** Free a tealet, unlinking it from the circular list first */
static void tealet_free_tealet(tealet_main_t *main, tealet_sub_t *t) {
  size_t basesize = offsetof(tealet_nonmain_t, _extra);
  size_t size = basesize + main->g_extrasize;

  if (main->g_previous == t)
    main->g_previous = NULL;

#if TEALET_WITH_STATS
  TEALET_LIST_REMOVE(t);
#endif
  STATS_SUB_ALLOC(main, size);
  tealet_int_free(main, t);
}

/* ----------------------------------------------------------------
 * actual stack management routines.  Copying, growing
 * restoring, duplicating, deleting
 */
static tealet_stack_t *tealet_stack_new(tealet_main_t *main, char *stack_near, char *stack_far, size_t size) {
  size_t tsize;
  tealet_stack_t *s;

  tsize = offsetof(tealet_stack_t, chunk.data[0]) + size;
  s = (tealet_stack_t *)tealet_int_malloc(main, tsize);
  if (!s)
    return NULL;
  STATS_ADD_ALLOC(main, tsize);
#if TEALET_WITH_STATS
  main->g_stack_count++;
  main->g_stack_chunk_count++; /* Initial chunk counts */
  main->g_stack_bytes += tsize;
#endif
  s->refcount = 1;
  s->prev = NULL;
  s->stack_far = stack_far;
  s->flags = 0;
  s->saved = size;
  s->last = &s->chunk;

  s->chunk.next = NULL;
  s->chunk.refcount = 1;
  s->chunk.stack_near = stack_near;
  s->chunk.size = size;
#if STACK_DIRECTION == 0
  memcpy(&s->chunk.data[0], stack_near, size);
#else
  memcpy(&s->chunk.data[0], stack_near - size, size);
#endif
  return s;
}

static int tealet_stack_grow(tealet_main_t *main, tealet_stack_t *stack, size_t size) {
  tealet_chunk_t *chunk;
  size_t tsize, diff;
  assert(size > stack->saved);

  diff = size - stack->saved;
  tsize = offsetof(tealet_chunk_t, data[0]) + diff;
  chunk = (tealet_chunk_t *)tealet_int_malloc(main, tsize);
  if (!chunk)
    return TEALET_ERR_MEM;
  STATS_ADD_ALLOC(main, tsize);
#if TEALET_WITH_STATS
  main->g_stack_chunk_count++; /* Additional chunk */
  main->g_stack_bytes += tsize;
#endif
  chunk->refcount = 1;
#if STACK_DIRECTION == 0
  chunk->stack_near = stack->chunk.stack_near + stack->saved;
  memcpy(&chunk->data[0], chunk->stack_near, diff);
#else
  chunk->stack_near = stack->chunk.stack_near - stack->saved;
  memcpy(&chunk->data[0], chunk->stack_near - diff, diff);
#endif
  chunk->size = diff;
  chunk->next = NULL;
  assert(stack->last != NULL);
  stack->last->next = chunk;
  stack->last = chunk;
  stack->saved = size;
  return 0;
}

static void tealet_stack_restore(tealet_stack_t *stack) {
  tealet_chunk_t *chunk = &stack->chunk;
  do {
#if STACK_DIRECTION == 0
    memcpy(chunk->stack_near, &chunk->data[0], chunk->size);
#else
    memcpy(chunk->stack_near - chunk->size, &chunk->data[0], chunk->size);
#endif
    chunk = chunk->next;
  } while (chunk);
}

static tealet_stack_t *tealet_stack_dup(tealet_stack_t *stack) {
  stack->refcount += 1;
  return stack;
}

static void tealet_stack_link(tealet_stack_t *stack, tealet_stack_t **head) {
  assert(stack->prev == NULL);
  assert(*head != stack);
  if (*head)
    assert((*head)->prev == head);
  stack->next = *head;
  if (stack->next)
    stack->next->prev = &stack->next;
  stack->prev = head;
  *head = stack;
}

static void tealet_stack_unlink(tealet_stack_t *stack) {
  tealet_stack_t *next = stack->next;
  assert(stack->prev);
  assert(*stack->prev == stack);
  if (next)
    assert(next->prev == &stack->next);

  if (next)
    next->prev = stack->prev;
  *stack->prev = next;
  stack->prev = NULL;
}

static void tealet_chunk_decref(tealet_main_t *main, tealet_chunk_t *chunk) {
  while (chunk != NULL) {
    tealet_chunk_t *next;

    if (--chunk->refcount > 0)
      return;

    /* Only release the remainder of the chain if this node was actually
     * released. Shared nodes keep ownership of their successors.
     */
    next = chunk->next;
    STATS_SUB_ALLOC(main, offsetof(tealet_chunk_t, data[0]) + chunk->size);
#if TEALET_WITH_STATS
    main->g_stack_chunk_count--; /* Additional chunk */
    main->g_stack_bytes -= offsetof(tealet_chunk_t, data[0]) + chunk->size;
#endif
    tealet_int_free(main, (void *)chunk);
    chunk = next;
  }
}

static void tealet_stack_decref(tealet_main_t *main, tealet_stack_t *stack) {
  tealet_chunk_t *chunk;
  if (stack == NULL || --stack->refcount > 0)
    return;
  if (stack->prev)
    tealet_stack_unlink(stack);

  chunk = stack->chunk.next;
  STATS_SUB_ALLOC(main, offsetof(tealet_stack_t, chunk.data[0]) + stack->chunk.size);
#if TEALET_WITH_STATS
  main->g_stack_count--;
  main->g_stack_chunk_count--; /* Initial chunk */
  main->g_stack_bytes -= offsetof(tealet_stack_t, chunk.data[0]) + stack->chunk.size;
#endif
  tealet_int_free(main, (void *)stack);
  if (chunk != NULL)
    tealet_chunk_decref(main, chunk);
}

static void tealet_stack_defunct(tealet_main_t *main, tealet_stack_t *stack) {
  /* stack couldn't be grown.  Release any extra chunks and mark stack as
   * defunct */
  tealet_chunk_t *chunk;
  chunk = stack->chunk.next;
  stack->chunk.next = NULL;
  stack->last = &stack->chunk;
  stack->flags |= TEALET_SFLAGS_DEFUNCT;
  stack->saved = 0;
  if (chunk != NULL)
    tealet_chunk_decref(main, chunk);
}

static int tealet_is_defunct(tealet_sub_t *tealet) {
  if (tealet->flags & TEALET_TFLAGS_DEFUNCT)
    return 1;
  if (tealet->stack != NULL && (tealet->stack->flags & TEALET_SFLAGS_DEFUNCT)) {
    tealet->flags |= TEALET_TFLAGS_DEFUNCT;
    return 1;
  }
  return 0;
}

/* ----------------------------------------------------------------
 * utility functions for allocating and growing stacks
 */

/** save a new stack, at least up to "saveto" */
static tealet_stack_t *tealet_stack_saveto(tealet_main_t *main, char *stack_near, char *stack_far, char *saveto,
                                           int *full) {
  ptrdiff_t size;
  /* boundary convention for saveto in copied range:
   *  - descending stacks: [stack_near, saveto)  (saveto is exclusive)
   *  - ascending stacks:  [saveto, stack_near)  (saveto is inclusive)
   */
  if (STACKMAN_SP_LE(stack_far, saveto)) {
    saveto = stack_far;
    *full = 1;
  } else
    *full = 0;
  assert(saveto != STACKMAN_SP_FURTHEST); /* can't save all of memory */
  size = STACKMAN_SP_DIFF(saveto, stack_near);
  if (size < 0)
    size = 0;
  return tealet_stack_new(main, (char *)stack_near, stack_far, size);
}

static int tealet_stack_growto(tealet_main_t *main, tealet_stack_t *stack, char *saveto, int *full, int fail_ok) {
  /** Save more of g's stack into the heap -- at least up to 'saveto'

     g->stack_stop |________|
                   |        |
                   |    __ stop       .........
                   |        |    ==>  :       :
                   |________|         :_______:
                   |        |         |       |
                   |        |         |       |
    g->stack_start |        |         |_______| g->stack_copy

   */
  ptrdiff_t size, saved = (ptrdiff_t)stack->saved;
  int fail;

  /* We shouldn't be completely saved already */
  if (stack->stack_far != STACKMAN_SP_FURTHEST)
    assert(STACKMAN_SP_DIFF(stack->stack_far, stack->chunk.stack_near) > saved);

  /* truncate the "stop" */
  if (STACKMAN_SP_LE(stack->stack_far, saveto)) {
    saveto = stack->stack_far;
    *full = 1;
  } else
    *full = 0;

  /* total saved size expected after this */
  assert(saveto != STACKMAN_SP_FURTHEST); /* can't save them all */
  size = STACKMAN_SP_DIFF(saveto, stack->chunk.stack_near);
  if (size <= saved)
    return 0; /* nothing to do */

  fail = tealet_stack_grow(main, stack, size);
  if (fail == 0)
    return 0;

  if (fail_ok)
    return fail; /* caller can deal with failures */

  /* Check if this is main's stack - we cannot mark main as defunct */
  if (stack == ((tealet_sub_t *)main)->stack)
    return fail; /* force the operation to fail, will redirect to main */

  /* we cannot fail.  Mark this stack as defunct and continue */
  tealet_stack_defunct(main, stack);
  *full = 1;
  return 0;
}

/** Grow a list of stacks to a certain limit.  Unlink those that
 * become fully saved.
 */
static int tealet_stack_grow_list(tealet_main_t *main, tealet_stack_t *list, char *saveto, tealet_stack_t *target,
                                  int fail_ok) {
  while (list) {
    int fail;
    int full;
    if (list == target) {
      /* this is the stack we are switching to.  We should stop here
       * since previous stacks are already fully saved wrt. this.
       * also, if this stack is not shared, it need not be saved
       */
      if (list->refcount > 1) {
        /* saving because the target stack is shared.  If failure cannot
         * be handled, the target will be marked invalid on error. But
         * the caller of this function will have already checked that
         * and will complete the switch despite such a flag.  Only
         * subsequent uses of this stack will fail.
         */
        fail = tealet_stack_growto(main, list, saveto, &full, fail_ok);
        if (fail)
          return fail;
        if (fail_ok)
          assert(full); /* we saved it entirely */
      }
      tealet_stack_unlink(list);
      return 0;
    }

    fail = tealet_stack_growto(main, list, saveto, &full, fail_ok);
    if (fail)
      return fail;
    if (full)
      tealet_stack_unlink(list);
    list = list->next;
  }
  return 0;
}

/* ----------------------------------------------------------------
 * the save and restore callbacks.  These implement all the stack
 * save and restore logic using previously defined functions
 */

/** main->g_target contains the tealet we are switching to:
 * target->stack_far is the limit to which we must save the old stack
 * target->stack can be NULL, indicating that the target stack
 * needs not be restored.
 *
 * One-shot transfer signals consumed here:
 * - current->SAVEFORCE: read once for this save operation, then cleared.
 * - current->EXITING/AUTODELETE: consumed when exiting path is taken.
 */
static int tealet_save_state(tealet_main_t *g_main, void *old_stack_pointer) {
  tealet_sub_t *g_target = g_main->g_target;
  tealet_sub_t *g_current = g_main->g_current;
  char *target_stop = g_target->stack_far;
  int exiting, force, fail, fail_ok, auto_delete;

  tealet_verify_current_matches_caller(g_current);

  assert((g_target->flags & TEALET_TFLAGS_EXITED) == 0); /* target isn't exiting */
  assert(g_current != g_target);

  exiting = ((g_current->flags & TEALET_TFLAGS_EXITING) != 0);
  force = ((g_current->flags & TEALET_TFLAGS_SAVEFORCE) != 0);
  /* SAVEFORCE is transient: it only influences this save operation. */
  g_current->flags &= ~TEALET_TFLAGS_SAVEFORCE;
  /* Force mode requests non-failable save behavior under memory pressure.
   * A stack that cannot be saved may be marked defunct so the switch can
   * proceed. The main tealet is never marked defunct; forced saves from main
   * must still fail with MEM.
   */
  fail_ok = (!force || TEALET_IS_MAIN((tealet_t *)g_current));

  /* save and unlink older stacks on demand */
  /* when coming from unbounded stack, there should be no list of unsaved stacks
   */
  if (TEALET_STACK_IS_UNBOUNDED(g_main->g_current)) {
    assert(!exiting);
    assert(g_main->g_prev == NULL);
  }
  fail = tealet_stack_grow_list(g_main, g_main->g_prev, target_stop, g_target->stack, fail_ok);
  if (fail)
    return -1;
  /* when returning to unbounded stack, there should now be no list of unsaved
   * stacks */
  if (TEALET_STACK_IS_UNBOUNDED(g_main->g_target))
    assert(g_main->g_prev == NULL);

  if (exiting) {
    /* tealet is exiting. We don't save its stack. */
    assert(!TEALET_IS_MAIN((tealet_t *)g_current));
    auto_delete = ((g_current->flags & TEALET_TFLAGS_AUTODELETE) != 0);
    g_current->flags &= ~(TEALET_TFLAGS_EXITING | TEALET_TFLAGS_AUTODELETE | TEALET_TFLAGS_SAVEFORCE);
    g_current->flags |= TEALET_TFLAGS_EXITED;
    if (auto_delete) {
      /* auto-delete the tealet */
#if TEALET_WITH_STATS
      g_main->g_tealets--;
#endif
      tealet_free_tealet(g_main, g_current);
    } else {
      /* keep tealet alive after exit */
      g_current->stack = NULL;
    }
  } else {
    /* save the initial stack chunk */
    int full;
    tealet_stack_t *stack =
        tealet_stack_saveto(g_main, (char *)old_stack_pointer, g_current->stack_far, target_stop, &full);
    if (!stack) {
      if (fail_ok)
        return -1;
      /* Main tealet's stack must always be saveable - can't mark it defunct.
       * Only intermediate targets ever become defunct (the target tealet does not need
       * saving) and we only invoke !fail_ok) when switching to main)
       */
      assert(!TEALET_IS_MAIN((tealet_t *)g_current));
      g_current->flags |= TEALET_TFLAGS_DEFUNCT;
    } else {
      g_current->stack = stack;
      /* if it is partially saved, link it in to previous stacks */
      if (TEALET_STACK_IS_UNBOUNDED(g_current))
        assert(!full); /* unbounded stack is never fully saved */
      if (!full)
        tealet_stack_link(stack, &g_main->g_prev);
    }
  }
  return 0;
}

static void tealet_restore_state(tealet_main_t *g_main, void *new_stack_pointer) {
  tealet_sub_t *g = g_main->g_target;

  /* Restore the heap copy back into the C stack */
  assert(g->stack != NULL);
  assert((char *)new_stack_pointer == g->stack->chunk.stack_near);
  tealet_stack_restore(g->stack);
  tealet_stack_decref(g_main, g->stack);
  g->stack = NULL;
}

/** this callback is called twice from the raw switch code, once after saving
 * registers, where it should save the stack (if needed) and once after
 * updating the stack pointer, where it should restore the stack (if needed)
 */
static void *tealet_save_restore_cb(void *context, int opcode, void *stack_pointer) {
  tealet_main_t *g_main = (tealet_main_t *)context;
  tealet_sub_t *g_target = g_main->g_target;

  if (opcode == STACKMAN_OP_SAVE) {
    int result = tealet_save_state(g_main, stack_pointer);
    if (result) {
      g_main->g_sw = SW_ERR;
      return stack_pointer;
    }
    if (g_target->stack == NULL) {
      /* save only, no restore, keep stack pointer */
      g_main->g_sw = SW_NOP;
      return stack_pointer;
    } else {
      /* return new stack pointer and flag us for restore */
      g_main->g_sw = SW_RESTORE;
      return g_target->stack->chunk.stack_near;
    }
  }
  assert(opcode == STACKMAN_OP_RESTORE);
  if (g_main->g_sw == SW_RESTORE) {
    tealet_restore_state(g_main, stack_pointer);
    return NULL;
  } else if (g_main->g_sw == SW_ERR) {
    /* called second time, but error happened the first time */
    return (void *)-1;
  }
  /* called second time but no restore should happen */
  assert(g_main->g_sw == SW_NOP);
  return NULL;
}

/** helper to compute absolute stack distance, without overflowing */
static uintptr_t tealet_abs_ptr_distance(const void *a, const void *b) {
  uintptr_t ua = (uintptr_t)a;
  uintptr_t ub = (uintptr_t)b;

  if (ua > ub)
    return ua - ub;
  return ub - ua;
}

/** helper to verify if the current thread appears to match the
 * current tealet.  The current tealet is maintained by the
 * library, but there is no guarantee that a calling thread
 * actually _is_ the current tealet.
 * We don't maintain any TLS keys in this library but we can
 * at least check if the current stack pointer is within the range
 * expected by the current tealet, thereby providing a minimal
 * sanity check.
 */
static int tealet_check_caller_is_current(tealet_sub_t *g_current) {
  char stack_probe = 0;
  char *sp = &stack_probe;
  tealet_main_t *g_main;
  size_t max_stack_size;
  uintptr_t dist;

  assert(g_current != NULL);
  assert((g_current->flags & TEALET_TFLAGS_EXITED) == 0);
  tealet_verify_current_matches_caller(g_current);

  g_main = TEALET_GET_MAIN(g_current);
  max_stack_size = g_main->g_cfg_max_stack_size;

  if (g_current->stack_far != STACKMAN_SP_FURTHEST) {
    /* if we know the far boundary, we check against it.  We should be
     * closer than the stack boundary, but not more than max_stack_size if that is configured.
     */
    if (!STACKMAN_SP_LS(sp, g_current->stack_far))
      return TEALET_ERR_INVAL;
    if (max_stack_size != 0) {
      dist = tealet_abs_ptr_distance(g_current->stack_far, sp);
      if (dist > max_stack_size)
        return TEALET_ERR_INVAL;
    }
  } else {
    /* this is the main stack, with no defined limit. Check absolute distance from the main stack probe. */
    if (max_stack_size != 0 && g_main->g_main_stack_probe != NULL) {
      dist = tealet_abs_ptr_distance(g_main->g_main_stack_probe, sp);
      if (dist > max_stack_size)
        return TEALET_ERR_INVAL;
    }
  }
  return 0;
}

/** switch the stack and pass arguments.  we need a separate argument for
 * in and out, because sometimes we need to pass a value in but cannot
 * pass a value out, e.g. when a tealet exits, there is no valid stack
 * location for that.
 */
static int tealet_switchstack(tealet_main_t *g_main, tealet_sub_t *target, void *in_arg, void **out_arg) {
  tealet_sub_t *old_previous = g_main->g_previous;
  int err_result, switch_result;
  assert(target);
  assert(target != g_main->g_current);

  /* Locking is owned by top-level switching APIs (new/create/switch/exit/fork).
   * This helper assumes the caller already established the desired lock scope.
   *
   * PANIC is a one-shot transfer signal (set before calling this helper).
   * This helper always consumes/clears PANIC before returning, on both
   * success and error paths.
   */

  if (target->flags & TEALET_TFLAGS_EXITED) {
    g_main->g_flags &= ~TEALET_MFLAGS_PANIC;
    return TEALET_ERR_INVAL;
  }

  /* if the target saved stack is invalid (due to a failure to save it
   * during the exit of another tealet), we detect this here and
   * report an error
   * return value is:
   *  0 = successful switch
   *  1 = successful save only
   * -1 = error, couldn't save state
   * -2 = error, target tealet corrupt
   */
  if (tealet_is_defunct(target)) {
    g_main->g_flags &= ~TEALET_MFLAGS_PANIC;
    return TEALET_ERR_DEFUNCT;
  }

  err_result = tealet_check_caller_is_current(g_main->g_current);
  if (err_result) {
    g_main->g_flags &= ~TEALET_MFLAGS_PANIC;
    return err_result;
  }

  tealet_guard_unprotect_current(g_main);

  err_result = tealet_snapshot_verify_current(g_main);
  if (err_result)
  /* Intentional behavior: on integrity error we return without re-arming
   * current guard/snapshot state. This is a best-effort mode that favors
   * continued execution after reporting the error.
   */
  {
    g_main->g_flags &= ~TEALET_MFLAGS_PANIC;
    return err_result;
  }

  g_main->g_previous = g_main->g_current;
  g_main->g_target = target;
  g_main->g_arg = in_arg;

  /* stackman switch is an external function so an optizer
   * cannot assume that any pointers reachable by
   * g_main stay unchanged across the switch
   */
  stackman_switch(tealet_save_restore_cb, (void *)g_main);

  if (g_main->g_sw != SW_ERR) {
    g_main->g_current = g_main->g_target;
  } else {
    g_main->g_previous = old_previous;
    g_main->g_target = NULL;
    g_main->g_arg = NULL;
#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
    tealet_integrity_plan_for_current(g_main);
#endif
#if TEALET_WITH_STACK_SNAPSHOT
    tealet_snapshot_capture_current(g_main);
#endif
#if TEALET_WITH_STACK_GUARD
    tealet_guard_protect_current(g_main);
#endif
    g_main->g_flags &= ~TEALET_MFLAGS_PANIC;
    return TEALET_ERR_MEM;
  }
  g_main->g_target = NULL;
  if (out_arg)
    *out_arg = g_main->g_arg;
  g_main->g_arg = NULL;

#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
  tealet_integrity_plan_for_current(g_main);
#endif
#if TEALET_WITH_STACK_SNAPSHOT
  tealet_snapshot_capture_current(g_main);
#endif
#if TEALET_WITH_STACK_GUARD
  tealet_guard_protect_current(g_main);
#endif

  switch_result = (g_main->g_sw == SW_RESTORE ? 0 : 1);
  if (g_main->g_flags & TEALET_MFLAGS_PANIC) {
    g_main->g_flags &= ~TEALET_MFLAGS_PANIC;
    return TEALET_ERR_PANIC;
  }

  return switch_result;
}

/* Internal helpers used by switching APIs only.
 * Public tealet_lock()/tealet_unlock() remain manual wrappers that always
 * forward configured callbacks regardless of mode.
 */
static void tealet_lock_switch(tealet_main_t *g_main) {
  if (g_main->g_locking.mode != TEALET_LOCK_SWITCH)
    return;
  if (g_main->g_locking.lock)
    g_main->g_locking.lock(g_main->g_locking.arg);
}

static void tealet_unlock_switch(tealet_main_t *g_main) {
  if (g_main->g_locking.mode != TEALET_LOCK_SWITCH)
    return;
  if (g_main->g_locking.unlock)
    g_main->g_locking.unlock(g_main->g_locking.arg);
}

/** We are initializing a new tealet, either switching to it and
 * running it, or switching from it (saving its virgin stack) back
 * to the caller, in order to switch to it later and run it.
 * stack_far is the far end of this stack and must be
 * far enough that local variables in this function get saved.
 * A stack variable in the calling function is sufficient.
 * The lock is held on entry.
 */
static int tealet_initialstub(tealet_main_t *g_main, tealet_sub_t *g_new, tealet_sub_t *g_target, tealet_run_t run,
                              void **parg, void *stack_far) {
  int result;
  tealet_sub_t *g_exit_target;
  int exit_flags;
  void *exit_arg;
  int run_on_switch = g_new == g_target; /* true for tealet_run(..., TEALET_START_SWITCH) */
  void *run_arg, *switch_arg;
  void *initial_run_arg;
  assert(g_new->stack == NULL); /* it is fresh */
  assert(run);

  if (run_on_switch) {
    /* TEALET_START_SWITCH case */
    assert(parg != NULL);
    /* Capture *parg before switching stacks.  After the switch, the
     * creator's stack region may be snapshot-checked and/or page-guarded,
     * so dereferencing the original parg pointer from the new tealet can
     * fault.
     */
    initial_run_arg = *parg;
  } else {
    /* TEALET_START_DEFAULT case */
    assert(parg == NULL);
    initial_run_arg = NULL;
  }

  g_new->stack_far = (char *)stack_far;
  result = tealet_switchstack(g_main, g_target, NULL, &switch_arg);
  if (result < 0) {
    /* couldn't allocate stack */
    return result;
  }

  assert(result == 0 || result == 1);
  /* 'result' is 1 if this was just the necessary stack 'save' to create
   * a new tealet, with no restore of an existing stack
   */
  if (run_on_switch == result) {
    /* need to run the actual code.  In the 'run_on_switch' case this is
     * done on the initial save.  The current tealet is the new tealet,
     * the previous tealet's stack was saved, and we run as the new one.
     * In the '!run_on_switch' case, the initial save was the new tealet
     * and we just returned immediately to the calling one.  We are now
     * returning here on a switch, to run the tealet
     */

    /* the following assertion may be invalid, if a TEALET_START_DEFAULT tealet
     * was duplicated.  We may now be a copy
     */
    if (run_on_switch) {
      assert(g_main->g_current == g_new); /* only valid for TEALET_START_SWITCH */
      run_arg = initial_run_arg;          /* captured before stack switch */
    } else {
      run_arg = switch_arg; /* TEALET_START_DEFAULT: use the arg from the switch */
    }
    assert(g_main->g_current->stack == NULL); /* running */

    /* release switching lock (if enabled) and run the tealet */
    tealet_unlock_switch(g_main);
    g_exit_target = (tealet_sub_t *)(run((tealet_t *)g_main->g_current, run_arg));

    /* Resolve any deferred-exit state explicitly so implicit return uses one
     * consistent (target,arg,flags) policy.
     */
    exit_flags = TEALET_XFER_DEFAULT;
    exit_arg = NULL;
    if (g_main->g_flags & TEALET_EXIT_DEFER) {
      exit_flags = g_main->g_flags & (~TEALET_EXIT_DEFER);
      exit_arg = g_main->g_arg;
      g_main->g_flags = 0;
      g_main->g_arg = NULL;
    }

    result = tealet_exit((tealet_t *)g_exit_target, exit_arg, exit_flags | TEALET_XFER_NOFAIL);
    (void)result;
    assert(!"Implicit return transfer failed");
    abort();
  } else {
    /* Either just a default-mode capture with no run, or a switch back
     * into the TEALET_START_SWITCH caller.
     */
    if (run_on_switch) {
      /* TEALET_START_SWITCH case, switch back - return the switch arg */
      *parg = switch_arg;
    }
  }
  return 0;
}

static tealet_sub_t *tealet_alloc_raw(tealet_main_t *g_main, tealet_alloc_t *alloc, size_t basesize, size_t extrasize) {
  tealet_sub_t *g;
  size_t size = basesize + extrasize;
  g = (tealet_sub_t *)alloc->malloc_p(size, alloc->context);
  if (g == NULL)
    return NULL;
  if (g_main == NULL) {
    g_main = (tealet_main_t *)g;
#if TEALET_WITH_STATS
    g_main->g_counter = 0;
    /* Initialize stats before tracking this allocation */
    g_main->g_bytes_allocated = 0;
    g_main->g_blocks_allocated = 0;
    g_main->g_blocks_allocated_total = 0;
#endif
  }
  /* Track tealet structure allocation */
  STATS_ADD_ALLOC(g_main, size);
  g->base.main = (tealet_t *)g_main;
  if (extrasize)
    g->base.extra = (void *)((char *)g + basesize);
  else
    g->base.extra = NULL;
  g->stack = NULL;
  g->stack_far = NULL;
  g->flags = 0;
#ifndef NDEBUG
  g->id = 0;
#endif
#if TEALET_WITH_STATS
  g_main->g_counter++;
#ifndef NDEBUG
  g->id = g_main->g_counter;
#endif
  /* Link into the circular list (but not the main tealet itself during init) */
  if (g != (tealet_sub_t *)g_main) {
    TEALET_LIST_ADD(g_main, g);
  }
#endif
  return g;
}

static tealet_sub_t *tealet_alloc_main(tealet_alloc_t *alloc, size_t extrasize) {
  size_t basesize = offsetof(tealet_main_t, _extra);
  return tealet_alloc_raw(NULL, alloc, basesize, extrasize);
}

static tealet_sub_t *tealet_alloc(tealet_main_t *g_main) {
  tealet_sub_t *result;
  size_t basesize = offsetof(tealet_nonmain_t, _extra);
  size_t extrasize = g_main->g_extrasize;

  result = tealet_alloc_raw(g_main, &g_main->g_alloc, basesize, extrasize);
#if TEALET_WITH_STATS
  if (result != NULL)
    g_main->g_tealets++;
#endif
  return result;
}

/* ----------------------------------------------------------------
 * Public API - core lifecycle and switching
 */

tealet_t *tealet_initialize(tealet_alloc_t *alloc, size_t extrasize) {
  char stack_probe;
  tealet_sub_t *g;
  tealet_main_t *g_main;
  g = tealet_alloc_main(alloc, extrasize);
  if (g == NULL)
    return NULL;
  g_main = (tealet_main_t *)g;
  g->stack = NULL;
  g->stack_far = STACKMAN_SP_FURTHEST;
  g->flags |= (TEALET_TFLAGS_MAIN_LINEAGE | TEALET_TFLAGS_BOUND);
  g_main->g_user = NULL;
  g_main->g_main_stack_probe = &stack_probe;
  g_main->g_current = g;
  g_main->g_previous = NULL;
  g_main->g_target = NULL;
  g_main->g_arg = NULL;
  g_main->g_alloc = *alloc;
  g_main->g_locking.mode = TEALET_LOCK_OFF;
  g_main->g_locking.lock = NULL;
  g_main->g_locking.unlock = NULL;
  g_main->g_locking.arg = NULL;
  g_main->g_prev = NULL;
  g_main->g_extrasize = extrasize;
  g_main->g_sw = SW_NOP;
  g_main->g_flags = 0;
  g_main->g_cfg_flags = 0;
  g_main->g_cfg_stack_integrity_bytes = 0;
  g_main->g_cfg_stack_guard_mode = TEALET_STACK_GUARD_MODE_NONE;
  g_main->g_cfg_stack_integrity_fail_policy = TEALET_STACK_INTEGRITY_FAIL_ASSERT;
  g_main->g_cfg_stack_guard_limit = NULL;
  g_main->g_cfg_max_stack_size = TEALET_DEFAULT_MAX_STACK_SIZE;
#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
  tealet_integrity_data_init(&g_main->g_integrity_data);
#endif
#if TEALET_WITH_STATS
  /* Initialize circular list - main tealet points to itself */
  g->next_tealet = g;
  g->prev_tealet = g;
  /* init these.  the main tealet counts as one */
  g_main->g_tealets = 1;
  assert(g_main->g_counter == 1); /* set in alloc_raw */
  /* bytes_allocated, blocks_allocated already set in tealet_alloc_raw */
  g_main->g_bytes_allocated_peak = g_main->g_bytes_allocated;
  g_main->g_blocks_allocated_peak = g_main->g_blocks_allocated;
  g_main->g_stack_bytes = 0;
  g_main->g_stack_count = 0;
  g_main->g_stack_chunk_count = 0;
#endif
  assert(TEALET_IS_MAIN((tealet_t *)g_main));
  return (tealet_t *)g_main;
}

tealet_t *tealet_new(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  tealet_sub_t *result;

  result = tealet_alloc(g_main);
  if (result == NULL)
    return NULL;

  /* Unbound tealets intentionally start with no stack boundary and no bound
   * execution state.
   */
  result->stack_far = NULL;
  result->stack = NULL;
  result->flags = 0;
  return (tealet_t *)result;
}

void tealet_finalize(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  assert(TEALET_IS_MAIN(tealet));
  assert(g_main->g_current == (tealet_sub_t *)g_main);
#if TEALET_WITH_STACK_GUARD
  tealet_guard_unprotect_current(g_main);
#endif
#if TEALET_WITH_STACK_SNAPSHOT || TEALET_WITH_STACK_GUARD
  tealet_integrity_data_free(g_main, &g_main->g_integrity_data);
#endif
  tealet_int_free(g_main, g_main);
}

/** choose initial far boundary for tealet creation.
 * 'hint' can only extend capture range (be farther), never shrink it.
 */
static void *tealet_pick_initial_far(void *default_far, void *hint) {
  if (hint == NULL)
    return default_far;
  return tealet_stack_further(default_far, hint);
}

/* Prime/run a NEW tealet target.
 *
 * Switching API lock policy:
 * all switching APIs (`tealet_run()`, `tealet_fork()`,
 * `tealet_switch()`, `tealet_exit()`) acquire/release
 * the lock internally.
 */
int tealet_run(tealet_t *tealet, tealet_run_t run, void **parg, void *stack_far, int flags) {
  tealet_sub_t *result = (tealet_sub_t *)tealet;
  int fail;
  int api_result;
  void *arg = NULL;
  void *default_far;
  void *stack_far_used;
  int switch_now;
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  tealet_sub_t *previous;
  tealet_sub_t *current;

  assert(run != NULL);
  assert((flags & ~TEALET_START_SWITCH) == 0);

  if (result->flags != 0)
    return TEALET_ERR_INVAL;

  switch_now = ((flags & TEALET_START_SWITCH) != 0);

  current = g_main->g_current;
  tealet_verify_current_matches_caller(current);

  default_far = (void *)&result;
  stack_far_used = tealet_pick_initial_far(default_far, stack_far);
  result->flags |= TEALET_TFLAGS_BOUND;

  tealet_lock_switch(g_main);
  assert(!g_main->g_target);

  if (switch_now) {
    fail = tealet_initialstub(g_main, result, result, run, (parg != NULL ? parg : &arg), stack_far_used);
  } else {
    previous = g_main->g_previous;
    g_main->g_current = result;
    fail = tealet_initialstub(g_main, result, current, run, NULL, stack_far_used);
    if (fail == 0)
      g_main->g_previous = previous;
  }

  if (fail) {
    result->stack = NULL;
    result->stack_far = NULL;
    result->flags = 0;
    if (!switch_now)
      g_main->g_current = current;
    api_result = fail;
    goto done;
  }

  api_result = 0;
done:
  tealet_unlock_switch(g_main);
  return api_result;
}

/* Fork the active tealet.
 *
 * Switching API lock policy:
 * all four switching APIs (`tealet_run()`, `tealet_fork()`,
 * `tealet_switch()`, `tealet_exit()`) acquire/release
 * the lock internally.
 */

int tealet_fork(tealet_t *_tealet, void **parg, int flags) {
  tealet_sub_t *g_child = (tealet_sub_t *)_tealet;
  tealet_main_t *g_main = TEALET_GET_MAIN(g_child);
  tealet_sub_t *g_current;
  tealet_sub_t *previous;
  int result;
  int switch_now;
  int api_result;

  g_current = g_main->g_current;
  tealet_verify_current_matches_caller(g_current);

  assert((flags & ~TEALET_START_SWITCH) == 0);

  switch_now = ((flags & TEALET_START_SWITCH) != 0);

  /* Fork target must be a NEW/unbound tealet */
  if (g_child->flags != 0) {
    return TEALET_ERR_INVAL;
  }

  /* Current tealet must have a bounded stack (far boundary set).
   * Even the main tealet can fork if its far boundary has been set.
   */
  if (TEALET_STACK_IS_UNBOUNDED(g_current)) {
    return TEALET_ERR_UNFORKABLE;
  }

  /* Active tealets have NULL stack (implied by the check above) */
  assert(g_current->stack == NULL);

  /* Copy the far boundary */
  g_child->stack_far = g_current->stack_far;
  g_child->flags |= TEALET_TFLAGS_FORK;
  g_child->flags |= TEALET_TFLAGS_BOUND;
  if (g_current->flags & TEALET_TFLAGS_MAIN_LINEAGE)
    g_child->flags |= TEALET_TFLAGS_MAIN_LINEAGE;

  tealet_lock_switch(g_main);

  /* result of tealet_switchstack is:
   * 1 if this was just a save
   * 0 if this was a restore (switch back)
   * <0 on error
   */
  if (switch_now) {
    /* save parent, switch to child*/
    result = tealet_switchstack(g_main, g_child, NULL, parg);
  } else {
    /* we are just saving the child's stack for later.
     *Save the stack in the child, don't modify 'previous'
     */
    previous = g_main->g_previous;
    g_main->g_current = g_child; /* Child becomes current (temporarily) */
    result = tealet_switchstack(g_main, g_current, NULL, parg);
    /* only in the case of just saving do we restore previous. otherwise
     * we respect the previous from the switch
     */
    if (result == 1)
      g_main->g_previous = previous;
  }

  if (result < 0) {
    /* Failed to save/restore; keep child reusable as NEW. */
    g_child->stack = NULL;
    g_child->stack_far = NULL;
    g_child->flags = 0;
    if (!switch_now)
      g_main->g_current = g_current;
    api_result = result;
    goto done;
  }

  api_result = 0;

done:
  /* we are now back.  If successful, we are either conceptually the same stack as when we called,
   * or we have been switched to (via tealet_switch or tealet_exit().  in either case,
   * release the lock.
   */
  tealet_unlock_switch(g_main);
  return api_result;
}

/* Switch to a tealet and back.
 *
 * Switching API lock policy:
 * all four switching APIs (`tealet_run()`, `tealet_fork()`,
 * `tealet_switch()`, `tealet_exit()`) acquire/release
 * the lock internally.
 */

static int tealet_xfer_inner(tealet_t *target, void *in_arg, void **out_arg, int flags, int is_exit) {
  tealet_sub_t *g_target = (tealet_sub_t *)target;
  tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
  tealet_sub_t *g_current = g_main->g_current;
  int panic_requested;
  int result;

  if ((g_target->flags & TEALET_TFLAGS_BOUND) == 0)
    return TEALET_ERR_INVAL;

  if (is_exit) {
    assert(g_current != (tealet_sub_t *)g_main); /* mustn't exit main */
    if (g_target == g_current)
      return TEALET_ERR_INVAL; /* invalid tealet */
    g_current->flags |= TEALET_TFLAGS_EXITING;
    assert(g_current->stack == NULL);
    assert((g_current->flags & TEALET_TFLAGS_AUTODELETE) == 0);
    if (flags & TEALET_EXIT_DELETE)
      g_current->flags |= TEALET_TFLAGS_AUTODELETE;
  } else {
    if (g_target == g_current) {
      panic_requested = ((flags & TEALET_XFER_PANIC) != 0);
      g_main->g_previous = g_current;
      return panic_requested ? TEALET_ERR_PANIC : 0; /* switch to self */
    }
  }

  panic_requested = ((flags & TEALET_XFER_PANIC) != 0);

  /* One-shot transfer signals are staged here before transfer and consumed by
   * lower-level transfer machinery.
   */
  if (flags & TEALET_XFER_FORCE)
    g_current->flags |= TEALET_TFLAGS_SAVEFORCE;
  if (panic_requested)
    g_main->g_flags |= TEALET_MFLAGS_PANIC;

  result = tealet_switchstack(g_main, g_target, in_arg, out_arg);

  if (result < 0) {
    g_current->flags &= ~(TEALET_TFLAGS_EXITING | TEALET_TFLAGS_AUTODELETE | TEALET_TFLAGS_SAVEFORCE);
  }

  if (is_exit) {
    assert(result != TEALET_ERR_PANIC);
    assert(result < 0); /* only return here if there was failure */
  }

  return result;
}

int tealet_switch(tealet_t *stub, void **parg, int flags) {
  tealet_sub_t *g_target = (tealet_sub_t *)stub;
  tealet_sub_t *g_current;
  void *in_arg;
  void **out_arg;
  int flags_used;
  int retry_flags;
  int result;
  tealet_main_t *g_main = TEALET_GET_MAIN(g_target);

  assert((flags & ~(TEALET_XFER_FORCE | TEALET_XFER_PANIC | TEALET_XFER_NOFAIL)) == 0);

  g_current = g_main->g_current;
  tealet_verify_current_matches_caller(g_current);
  tealet_lock_switch(g_main);
  in_arg = parg ? *parg : NULL;
  out_arg = parg;

  flags_used = flags;
  if (flags_used & TEALET_XFER_NOFAIL) {
    flags_used &= ~TEALET_XFER_NOFAIL;
    retry_flags = flags_used | TEALET_XFER_FORCE;

    result = tealet_xfer_inner(stub, in_arg, out_arg, retry_flags, 0);
    if (result == TEALET_ERR_MEM || result == TEALET_ERR_DEFUNCT) {
      retry_flags = flags_used | TEALET_XFER_PANIC | TEALET_XFER_FORCE;
      result = tealet_xfer_inner((tealet_t *)g_main, in_arg, out_arg, retry_flags, 0);
    }
  } else {
    result = tealet_xfer_inner(stub, in_arg, out_arg, flags_used, 0);
  }

  tealet_unlock_switch(g_main);
  return result;
}

/* Exit current tealet and transfer control to requested target.
 *
 * Switching API lock policy:
 * all four switching APIs (`tealet_run()`, `tealet_fork()`,
 * `tealet_switch()`, `tealet_exit()`) acquire/release
 * the lock internally.
 */

int tealet_exit(tealet_t *target, void *arg, int flags) {
  tealet_sub_t *g_target = (tealet_sub_t *)target;
  tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
  tealet_sub_t *g_current;
  int flags_used;
  int retry_flags;
  int result;

  assert((flags &
          ~(TEALET_EXIT_DELETE | TEALET_EXIT_DEFER | TEALET_XFER_FORCE | TEALET_XFER_PANIC | TEALET_XFER_NOFAIL)) == 0);
  assert(!((flags & TEALET_EXIT_DEFER) && (flags & TEALET_XFER_PANIC)));

  g_current = g_main->g_current;
  tealet_verify_current_matches_caller(g_current);

  tealet_lock_switch(g_main);

  if (flags & TEALET_EXIT_DEFER) {
    /* setting up arg and flags for the run() return value */
    /* We temporarily borrow g_flags/g_arg storage on main until the
     * deferred exit is consummated.
     */
    assert(g_main->g_flags == 0);
    g_main->g_arg = arg;
    g_main->g_flags = flags;
    tealet_unlock_switch(g_main);
    return 0; /* deferred exit, we are done here */
  }
  if (g_main->g_flags & TEALET_EXIT_DEFER) {
    /* Called second time (e.g. from return of run())
     * use arg and flags from last time
     */
    flags = g_main->g_flags & (~TEALET_EXIT_DEFER);
    arg = g_main->g_arg;
    g_main->g_flags = 0;
    g_main->g_arg = 0;
  }
  flags_used = flags;
  if (flags_used & TEALET_XFER_NOFAIL) {
    flags_used &= ~TEALET_XFER_NOFAIL;
    retry_flags = flags_used | TEALET_XFER_FORCE;

    result = tealet_xfer_inner(target, arg, NULL, retry_flags, 1);
    if (result == TEALET_ERR_MEM || result == TEALET_ERR_DEFUNCT) {
      retry_flags = flags_used | TEALET_XFER_PANIC | TEALET_XFER_FORCE;
      result = tealet_xfer_inner((tealet_t *)g_main, arg, NULL, retry_flags, 1);
    }
  } else {
    result = tealet_xfer_inner(target, arg, NULL, flags_used, 1);
  }

  tealet_unlock_switch(g_main);
  assert(result < 0);
  return result;
}

tealet_t *tealet_duplicate(tealet_t *tealet) {
  tealet_sub_t *g_tealet = (tealet_sub_t *)tealet;
  tealet_main_t *g_main = TEALET_GET_MAIN(g_tealet);
  tealet_sub_t *g_copy;

  /* can't dup the current or the main tealet */
  assert(g_tealet != g_main->g_current && g_tealet != (tealet_sub_t *)g_main);
  g_copy = tealet_alloc(g_main);
  if (g_copy == NULL)
    return NULL;
  g_copy->stack_far = g_tealet->stack_far;
  g_copy->flags = g_tealet->flags;
  if (g_tealet->stack != NULL)
    g_copy->stack = tealet_stack_dup(g_tealet->stack); /* can't fail */
  else
    g_copy->stack = NULL;
  if (g_main->g_extrasize)
    memcpy(g_copy->base.extra, g_tealet->base.extra, g_main->g_extrasize);
  return (tealet_t *)g_copy;
}

void tealet_delete(tealet_t *target) {
  tealet_sub_t *g_target = (tealet_sub_t *)target;
  tealet_main_t *g_main = TEALET_GET_MAIN(g_target);
  assert(!TEALET_IS_MAIN(target));
  tealet_stack_decref(g_main, g_target->stack);
#if TEALET_WITH_STATS
  g_main->g_tealets--;
#endif
  tealet_free_tealet(g_main, g_target);
}

/* ----------------------------------------------------------------
 * Public API - status and query
 */

tealet_t *tealet_current(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  return (tealet_t *)g_main->g_current;
}

tealet_t *tealet_previous(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  return (tealet_t *)g_main->g_previous;
}

void **tealet_main_userpointer(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  return &g_main->g_user;
}

unsigned int tealet_get_origin(tealet_t *_tealet) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  unsigned int origin = 0;

  if (tealet->flags & TEALET_TFLAGS_MAIN_LINEAGE)
    origin |= TEALET_ORIGIN_MAIN_LINEAGE;
  if (tealet->flags & TEALET_TFLAGS_FORK)
    origin |= TEALET_ORIGIN_FORK;
  return origin;
}

int tealet_status(tealet_t *_tealet) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  if ((tealet->flags & TEALET_TFLAGS_BOUND) == 0)
    return TEALET_STATUS_NEW;
  if (tealet->flags & TEALET_TFLAGS_EXITED)
    return TEALET_STATUS_EXITED;
  if (tealet_is_defunct(tealet))
    return TEALET_STATUS_DEFUNCT;
  return TEALET_STATUS_ACTIVE;
}

size_t tealet_get_stacksize(tealet_t *_tealet) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  if (tealet_is_defunct(tealet))
    return 0;
  if (tealet->stack)
    return tealet->stack->saved;
  return 0;
}

void *tealet_get_far(tealet_t *_tealet) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  return tealet->stack_far;
}

void tealet_get_stats(tealet_t *tealet, tealet_stats_t *stats) {
#if !TEALET_WITH_STATS
  (void)tealet; /* unused */
  memset(stats, 0, sizeof(*stats));
#else
  tealet_main_t *tmain = TEALET_GET_MAIN(tealet);

  /* Basic tealet counts */
  stats->n_active = tmain->g_tealets;
  stats->n_total = tmain->g_counter;

  /* Memory usage statistics */
  stats->bytes_allocated = tmain->g_bytes_allocated;
  stats->bytes_allocated_peak = tmain->g_bytes_allocated_peak;
  stats->blocks_allocated = tmain->g_blocks_allocated;
  stats->blocks_allocated_peak = tmain->g_blocks_allocated_peak;
  stats->blocks_allocated_total = tmain->g_blocks_allocated_total;

  /* Stack memory storage statistics - from tracked values */
  stats->stack_bytes = tmain->g_stack_bytes;
  stats->stack_count = tmain->g_stack_count;
  stats->stack_chunk_count = tmain->g_stack_chunk_count;

  /* Compute expanded and naive sizes by walking all tealets */
  stats->stack_bytes_expanded = 0;
  stats->stack_bytes_naive = 0;

  /* Walk the circular list of all tealets */
  tealet_sub_t *start = (tealet_sub_t *)tmain;
  tealet_sub_t *t = start;
  int stack_num = 0;
  int tealet_count = 0;
  do {
    tealet_count++;
    /* Count tealets with saved stacks (current tealet won't have stack saved)
     */
    if (t->stack) {
      tealet_stack_t *stack = t->stack;
      tealet_chunk_t *chunk;
      size_t this_naive = 0;
      size_t this_expanded = 0;
      void *effective_far;

      /* Compute the effective "far" boundary for naive calculation */
      if (stack->stack_far == STACKMAN_SP_FURTHEST) {
        /* For unbounded stacks, recompute the furthest point from actual chunks
         */
        /* Compute far = near + size for the initial chunk */
        effective_far = (void *)STACKMAN_SP_ADD((ptrdiff_t)stack->chunk.stack_near, (ptrdiff_t)stack->chunk.size);
        chunk = stack->chunk.next;
        while (chunk) {
          /* Compute far for this chunk and keep the furthest */
          void *chunk_far = (void *)STACKMAN_SP_ADD((ptrdiff_t)chunk->stack_near, (ptrdiff_t)chunk->size);
          if (STACKMAN_SP_DIFF((ptrdiff_t)chunk_far, (ptrdiff_t)effective_far) > 0)
            effective_far = chunk_far;
          chunk = chunk->next;
        }
      } else {
        /* For bounded stacks, use the recorded far boundary */
        effective_far = stack->stack_far;
      }

      /* Compute naive size: extent from effective_far to near, plus overhead */
      size_t extent = (size_t)STACKMAN_SP_DIFF((ptrdiff_t)effective_far, (ptrdiff_t)stack->chunk.stack_near);
      this_naive = offsetof(tealet_stack_t, chunk.data[0]) + extent;
      stats->stack_bytes_naive += this_naive;

      /* Add up all chunk allocations for this tealet (counts shared chunks
       * multiple times) */
      /* Initial chunk is part of stack structure */
      this_expanded = offsetof(tealet_stack_t, chunk.data[0]) + stack->chunk.size;

      /* Count additional chunks */
      chunk = stack->chunk.next;
      int chunk_count = 1;
      while (chunk) {
        this_expanded += offsetof(tealet_chunk_t, data[0]) + chunk->size;
        chunk_count++;
        chunk = chunk->next;
      }
      stats->stack_bytes_expanded += this_expanded;

      stack_num++;
    }
    t = t->next_tealet;
  } while (t != start);
#endif
}

void tealet_reset_peak_stats(tealet_t *tealet) {
#if TEALET_WITH_STATS
  tealet_main_t *tmain = TEALET_GET_MAIN(tealet);
  tmain->g_bytes_allocated_peak = tmain->g_bytes_allocated;
  tmain->g_blocks_allocated_peak = tmain->g_blocks_allocated;
  /* Note: We don't track stack_bytes_naive_peak as it would require walking
   * all tealets on every allocation. stack_bytes_naive is computed on demand.
   */
#else
  (void)tealet; /* unused */
#endif
}

/* ----------------------------------------------------------------
 * Public API - configuration
 */

int tealet_set_far(tealet_t *_tealet, void *far_boundary) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;

  /* Only the main tealet can have its far boundary set */
  if (!TEALET_IS_MAIN(_tealet))
    return TEALET_ERR_INVAL;

  /* Set the far boundary */
  tealet->stack_far = (char *)far_boundary;
  return 0;
}

/* Return effective configuration for this main tealet.
 *
 * This is a size-versioned copy-out API: only the caller-provided prefix is
 * written, enabling forward/backward ABI-compatible evolution of the struct.
 */
int tealet_configure_get(tealet_t *_tealet, tealet_config_t *config) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  tealet_main_t *g_main;
  tealet_config_t effective;
  size_t copy_size;

  if (!tealet_config_has_header(config))
    return TEALET_ERR_INVAL;

  g_main = TEALET_GET_MAIN(tealet);

  memset(&effective, 0, sizeof(effective));
  effective.size = sizeof(tealet_config_t);
  effective.version = TEALET_CONFIG_CURRENT_VERSION;
  effective.stack_guard_mode = TEALET_STACK_GUARD_MODE_NONE;
  effective.stack_integrity_fail_policy = TEALET_STACK_INTEGRITY_FAIL_ASSERT;
  effective.size = config->size;
  tealet_config_fill_from_main(g_main, &effective);

  copy_size = MIN(config->size, sizeof(tealet_config_t));
  memcpy(config, &effective, copy_size);
  return 0;
}

/* Apply runtime configuration from caller input.
 *
 * The request is copied, canonicalized, and then committed atomically into the
 * main tealet state. Any required snapshot workspace is preallocated before
 * commit so switching paths do not allocate.
 */
int tealet_configure_set(tealet_t *_tealet, tealet_config_t *config) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  tealet_main_t *g_main;
  tealet_config_t requested;
  size_t copy_size;
  size_t snapshot_required;
  int result;

  if (!tealet_config_has_header(config))
    return TEALET_ERR_INVAL;
  if (config->version != TEALET_CONFIG_VERSION_1)
    return TEALET_ERR_INVAL;

  g_main = TEALET_GET_MAIN(tealet);

  memset(&requested, 0, sizeof(requested));
  requested.size = sizeof(tealet_config_t);
  requested.version = TEALET_CONFIG_CURRENT_VERSION;
  requested.stack_guard_mode = TEALET_STACK_GUARD_MODE_NONE;
  requested.stack_integrity_fail_policy = TEALET_STACK_INTEGRITY_FAIL_ASSERT;
  requested.max_stack_size = g_main->g_cfg_max_stack_size;
  requested.size = config->size;
  copy_size = MIN(config->size, sizeof(tealet_config_t));
  memcpy(&requested, config, copy_size);
  requested.version = TEALET_CONFIG_VERSION_1;
  tealet_config_canonicalize(&requested);

  snapshot_required = tealet_snapshot_required_capacity(&requested);
  result = tealet_snapshot_ensure_capacity(g_main, snapshot_required);
  if (result)
    return result;

  g_main->g_cfg_flags = requested.flags;
  g_main->g_cfg_stack_integrity_bytes = requested.stack_integrity_bytes;
  g_main->g_cfg_stack_guard_mode = requested.stack_guard_mode;
  g_main->g_cfg_stack_integrity_fail_policy = requested.stack_integrity_fail_policy;
  g_main->g_cfg_stack_guard_limit = (char *)requested.stack_guard_limit;
  g_main->g_cfg_max_stack_size = requested.max_stack_size;

  memcpy(config, &requested, copy_size);
  return 0;
}

int tealet_configure_set_locking(tealet_t *_tealet, const tealet_lock_t *locking) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);

  if (locking == NULL) {
    g_main->g_locking.mode = TEALET_LOCK_OFF;
    g_main->g_locking.lock = NULL;
    g_main->g_locking.unlock = NULL;
    g_main->g_locking.arg = NULL;
  } else {
    if (locking->mode != TEALET_LOCK_OFF && locking->mode != TEALET_LOCK_SWITCH)
      return TEALET_ERR_INVAL;
    g_main->g_locking = *locking;
  }
  return 0;
}

/* Convenience API to enable stack checking with practical defaults.
 *
 * - Enables integrity + guard + snapshot flags.
 * - Uses NOACCESS guard mode and ERROR mismatch policy.
 * - If stack_integrity_bytes is zero, uses one Linux page when available,
 *   otherwise falls back to 4096 bytes.
 */
int tealet_configure_check_stack(tealet_t *_tealet, size_t stack_integrity_bytes) {
  tealet_config_t cfg = TEALET_CONFIG_INIT;
  size_t effective_bytes;
  char local_stack_marker;

  effective_bytes = stack_integrity_bytes;
  if (effective_bytes == 0) {
#if TEALET_GUARD_MPROTECT
    effective_bytes = tealet_guard_pagesize();
#else
    effective_bytes = 4096;
#endif
    if (effective_bytes == 0)
      effective_bytes = 4096;
  }

  cfg.flags = TEALET_CONFIGF_STACK_INTEGRITY | TEALET_CONFIGF_STACK_GUARD | TEALET_CONFIGF_STACK_SNAPSHOT;
  cfg.stack_integrity_bytes = effective_bytes;
  cfg.stack_guard_mode = TEALET_STACK_GUARD_MODE_NOACCESS;
  cfg.stack_integrity_fail_policy = TEALET_STACK_INTEGRITY_FAIL_ERROR;
  cfg.stack_guard_limit = &local_stack_marker;

  return tealet_configure_set(_tealet, &cfg);
}

/* ----------------------------------------------------------------
 * Public API - utility helpers
 */

void *tealet_malloc(tealet_t *tealet, size_t s) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  return tealet_int_malloc(g_main, s);
}

void tealet_free(tealet_t *tealet, void *p) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  tealet_int_free(g_main, p);
}

void tealet_lock(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  if (g_main->g_locking.lock)
    g_main->g_locking.lock(g_main->g_locking.arg);
}

void tealet_unlock(tealet_t *tealet) {
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);
  if (g_main->g_locking.unlock)
    g_main->g_locking.unlock(g_main->g_locking.arg);
}

ptrdiff_t tealet_stack_diff(void *a, void *b) { return STACKMAN_SP_DIFF((ptrdiff_t)a, (ptrdiff_t)(b)); }

void *tealet_stack_further(void *a, void *b) {
  if (STACKMAN_SP_LE(a, b))
    return b;
  return a;
}

#if __GNUC__ > 4
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"
#elif _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4172)
#endif
void *tealet_new_probe(tealet_t *d1, tealet_run_t d2, void **d3, void *d4, int d5) {
  tealet_sub_t *result;
  void *default_far;
  void *r;
  (void)d1;
  (void)d2;
  (void)d3;
  (void)d4;
  (void)d5;
  default_far = (void *)&result;
  r = tealet_pick_initial_far(default_far, d4);
  return r;
}
#if __GNUC__ > 4
#pragma GCC diagnostic pop
#elif _MSC_VER
#pragma warning(pop)
#endif

/* ----------------------------------------------------------------
 * Testing-only debug hooks
 */

#if TEALET_WITH_TESTING
/** Internal test hook: swap stack_far for a tealet.
 * This is used by tests to force caller-validation outcomes.
 * Not declared in public headers; test code can declare a matching prototype.
 */
int tealet_debug_swap_far(tealet_t *_tealet, void *new_far, void **old_far) {
  tealet_sub_t *tealet;

  if (_tealet == NULL || old_far == NULL)
    return TEALET_ERR_INVAL;

  tealet = (tealet_sub_t *)_tealet;
  *old_far = tealet->stack_far;
  tealet->stack_far = (char *)new_far;
  return 0;
}

/** Internal test hook: force a tealet into defunct state.
 * Not declared in public headers; test code can declare a matching prototype.
 */
int tealet_debug_force_defunct(tealet_t *_tealet) {
  tealet_sub_t *tealet = (tealet_sub_t *)_tealet;
  tealet_main_t *g_main = TEALET_GET_MAIN(tealet);

  if (TEALET_IS_MAIN(_tealet))
    return TEALET_ERR_INVAL;
  if (tealet == g_main->g_current)
    return TEALET_ERR_INVAL;
  if (tealet->flags & TEALET_TFLAGS_EXITED)
    return TEALET_ERR_INVAL;

  tealet_stack_decref(g_main, tealet->stack);
  tealet->stack = NULL;
  tealet->flags |= TEALET_TFLAGS_DEFUNCT;
  return 0;
}
#endif
