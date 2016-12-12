/*
 *  Copyright (c) 2009 Facebook
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef linux
/* To enable CPU_ZERO and CPU_SET, etc.     */
# define _GNU_SOURCE
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_xhprof.h"

#include "zend_compile.h"

#include <sys/time.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __FreeBSD__
# if __FreeBSD_version >= 700110
#   include <sys/resource.h>
#   include <sys/cpuset.h>
#   define cpu_set_t cpuset_t
#   define SET_AFFINITY(pid, size, mask) \
           cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
#   define GET_AFFINITY(pid, size, mask) \
           cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
# else
#   error "This version of FreeBSD does not support cpusets"
# endif /* __FreeBSD_version */
#elif __APPLE__
/*
 * Patch for compiling in Mac OS X Leopard
 * @author Svilen Spasov <s.spasov@gmail.com>
 */
#    include <mach/mach_init.h>
#    include <mach/thread_policy.h>
#    define cpu_set_t thread_affinity_policy_data_t
#    define CPU_SET(cpu_id, new_mask) \
        (*(new_mask)).affinity_tag = (cpu_id + 1)
#    define CPU_ZERO(new_mask)                 \
        (*(new_mask)).affinity_tag = THREAD_AFFINITY_TAG_NULL
#   define SET_AFFINITY(pid, size, mask)       \
        thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, mask, \
                          THREAD_AFFINITY_POLICY_COUNT)
#else
/* For sched_getaffinity, sched_setaffinity */
# include <sched.h>
# define SET_AFFINITY(pid, size, mask) sched_setaffinity(0, size, mask)
# define GET_AFFINITY(pid, size, mask) sched_getaffinity(0, size, mask)
#endif /* __FreeBSD__ */



/**
 * **********************
 * GLOBAL MACRO CONSTANTS
 * **********************
 */

/* XHProf version                           */
#define XHPROF_VERSION       "0.9.5"

/* Fictitious function name to represent top of the call tree. The paranthesis
 * in the name is to ensure we don't conflict with user function names.  */
#define ROOT_SYMBOL                "main()"

/* Size of a temp scratch buffer            */
#define SCRATCH_BUF_LEN            512

/* Various XHPROF modes. If you are adding a new mode, register the appropriate
 * callbacks in hp_begin() */
#define XHPROF_MODE_HIERARCHICAL            1
#define XHPROF_MODE_SAMPLED            620002      /* Rockfort's zip code */

/* Hierarchical profiling flags.
 *
 * Note: Function call counts and wall (elapsed) time are always profiled.
 * The following optional flags can be used to control other aspects of
 * profiling.
 */
#define XHPROF_FLAGS_NO_BUILTINS   0x0001         /* do not profile builtins */
#define XHPROF_FLAGS_CPU           0x0002      /* gather CPU times for funcs */
#define XHPROF_FLAGS_MEMORY        0x0004   /* gather memory usage for funcs */

/* Constants for XHPROF_MODE_SAMPLED        */
#define XHPROF_SAMPLING_INTERVAL       100000      /* In microsecs        */

/* Constant for ignoring functions, transparent to hierarchical profile */
#define XHPROF_MAX_IGNORED_FUNCTIONS  256

#if !defined(uint64)
typedef unsigned long long uint64;
#endif
#if !defined(uint32)
typedef unsigned int uint32;
#endif
#if !defined(uint8)
typedef unsigned char uint8;
#endif






/**
 * *****************************
 * GLOBAL DATATYPES AND TYPEDEFS
 * *****************************
 */

/* XHProf maintains a stack of entries being profiled. The memory for the entry
 * is passed by the layer that invokes BEGIN_PROFILING(), e.g. the hp_execute()
 * function. Often, this is just C-stack memory.
 *
 * This structure is a convenient place to track start time of a particular
 * profile operation, recursion depth, and the name of the function being
 * profiled. */
typedef struct hp_entry_t {
  zend_string             *name_hprof;                       /* function name */
  int                     rlvl_hprof;        /* recursion level for function */
  uint64                  tsc_start;         /* start value for TSC counter  */
  long int                mu_start_hprof;                    /* memory usage */
  long int                pmu_start_hprof;              /* peak memory usage */
  struct rusage           ru_start_hprof;             /* user/sys time start */
  struct hp_entry_t      *prev_hprof;    /* ptr to prev entry being profiled */
  uint8                   hash_code;     /* hash_code for the function name  */
} hp_entry_t;

/* Various types for XHPROF callbacks       */
typedef void (*hp_init_cb)           (TSRMLS_D);
typedef void (*hp_exit_cb)           (TSRMLS_D);
typedef void (*hp_begin_function_cb) (hp_entry_t **entries,
                                      hp_entry_t *current   TSRMLS_DC);
typedef void (*hp_end_function_cb)   (hp_entry_t **entries  TSRMLS_DC);

/* Struct to hold the various callbacks for a single xhprof mode */
typedef struct hp_mode_cb {
  hp_init_cb             init_cb;
  hp_exit_cb             exit_cb;
  hp_begin_function_cb   begin_fn_cb;
  hp_end_function_cb     end_fn_cb;
} hp_mode_cb;

/* Xhprof's global state.
 *
 * This structure is instantiated once.  Initialize defaults for attributes in
 * hp_init_profiler_state() Cleanup/free attributes in
 * hp_clean_profiler_state() */
typedef struct hp_global_t {

  /*       ----------   Global attributes:  -----------       */

  /* Indicates if xhprof is currently enabled */
  int              enabled;

  /* Indicates if xhprof was ever enabled during this request */
  int              ever_enabled;

  /* Holds all the xhprof statistics */
  zval            *stats_count;

  /* Indicates the current xhprof mode or level */
  int              profiler_level;

  /* Top of the profile stack */
  hp_entry_t      *entries;

  /* freelist of hp_entry_t chunks for reuse... */
  hp_entry_t      *entry_free_list;

  /* Callbacks for various xhprof modes */
  hp_mode_cb       mode_cb;

  /*       ----------   Mode specific attributes:  -----------       */

  /* Global to track the time of the last sample in time and ticks */
  struct timeval   last_sample_time;
  uint64           last_sample_tsc;
  /* XHPROF_SAMPLING_INTERVAL in ticks */
  uint64           sampling_interval_tsc;

  /* This array is used to store cpu frequencies for all available logical
   * cpus.  For now, we assume the cpu frequencies will not change for power
   * saving or other reasons. If we need to worry about that in the future, we
   * can use a periodical timer to re-calculate this arrary every once in a
   * while (for example, every 1 or 5 seconds). */
  double *cpu_frequencies;

  /* The number of logical CPUs this machine has. */
  uint32 cpu_num;

  /* The saved cpu affinity. */
  cpu_set_t prev_mask;

  /* The cpu id current process is bound to. (default 0) */
  uint32 cur_cpu_id;

  /* XHProf flags */
  uint32 xhprof_flags;

  /* counter table indexed by hash value of function names. */
  uint8  func_hash_counters[256];

  /* Table of ignored function names and their filter */
  HashTable *ignored_function_names;

} hp_global_t;


/**
 * ***********************
 * GLOBAL STATIC VARIABLES
 * ***********************
 */
/* XHProf global state */
static hp_global_t       hp_globals;

/**
 * ***************************
 * XHPROF DUMMY CALLBACKS
 * ***************************
 */
void hp_mode_dummy_init_cb(TSRMLS_D);

void hp_mode_dummy_exit_cb(TSRMLS_D);

void hp_mode_dummy_beginfn_cb(hp_entry_t **entries,
                              hp_entry_t *current  TSRMLS_DC);

void hp_mode_dummy_endfn_cb(hp_entry_t **entries   TSRMLS_DC);

/* Pointer to the original compile function */
static zend_op_array * (*_zend_compile_file) (zend_file_handle *file_handle, int type TSRMLS_DC);

/* Pointer to the original compile string function (used by eval) */
static zend_op_array * (*_zend_compile_string) (zval *source_string, char *filename TSRMLS_DC);


/* Pointer to the original execute function */
static void (*_zend_execute_ex) (zend_execute_data *execute_data TSRMLS_DC);

/* Pointer to the original execute_internal function */
static void (*_zend_execute_internal) (zend_execute_data *data, zval *ret TSRMLS_DC);


ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data, zval* ret TSRMLS_DC);

ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data TSRMLS_DC);

ZEND_DLEXPORT zend_op_array* hp_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC);

ZEND_DLEXPORT zend_op_array* hp_compile_string(zval *source_string, char *filename TSRMLS_DC);
/**
 * ****************************
 * STATIC FUNCTION DECLARATIONS
 * ****************************
 */
static void hp_begin(long level, long xhprof_flags TSRMLS_DC);
static void hp_stop(TSRMLS_D);
static void hp_end(TSRMLS_D);

static inline uint64 cycle_timer();
static double get_cpu_frequency();
static void clear_frequencies();

static void hp_free_the_free_list();
static hp_entry_t *hp_fast_alloc_hprof_entry();
static void hp_fast_free_hprof_entry(hp_entry_t *p);
static inline uint8 hp_inline_hash(char * str);
static void get_all_cpu_frequencies();
static long get_us_interval(struct timeval *start, struct timeval *end);
static void incr_us_interval(struct timeval *start, uint64 incr);

static void hp_get_ignored_functions_from_arg(zval *args);

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_xhprof_enable, 0, 0, 0)
  ZEND_ARG_INFO(0, flags)
  ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_disable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_enable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_disable, 0)
ZEND_END_ARG_INFO()

/* }}} */

/**
 * *********************
 * FUNCTION PROTOTYPES
 * *********************
 */
int restore_cpu_affinity(cpu_set_t * prev_mask);
int bind_to_cpu(uint32 cpu_id);

/**
 * *********************
 * PHP EXTENSION GLOBALS
 * *********************
 */
/* List of functions implemented/exposed by xhprof */
zend_function_entry xhprof_functions[] = {
  PHP_FE(xhprof_enable, arginfo_xhprof_enable)
  PHP_FE(xhprof_disable, arginfo_xhprof_disable)
  PHP_FE(xhprof_sample_enable, arginfo_xhprof_sample_enable)
  PHP_FE(xhprof_sample_disable, arginfo_xhprof_sample_disable)
  {NULL, NULL, NULL}
};

/* Callback functions for the xhprof extension */
zend_module_entry xhprof_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
  STANDARD_MODULE_HEADER,
#endif
  "xhprof",                        /* Name of the extension */
  xhprof_functions,                /* List of functions exposed */
  PHP_MINIT(xhprof),               /* Module init callback */
  PHP_MSHUTDOWN(xhprof),           /* Module shutdown callback */
  PHP_RINIT(xhprof),               /* Request init callback */
  PHP_RSHUTDOWN(xhprof),           /* Request shutdown callback */
  PHP_MINFO(xhprof),               /* Module info callback */
#if ZEND_MODULE_API_NO >= 20010901
  XHPROF_VERSION,
#endif
  STANDARD_MODULE_PROPERTIES
};

PHP_INI_BEGIN()

/* output directory:
 * Currently this is not used by the extension itself.
 * But some implementations of iXHProfRuns interface might
 * choose to save/restore XHProf profiler runs in the
 * directory specified by this ini setting.
 */
PHP_INI_ENTRY("xhprof.output_dir", "", PHP_INI_ALL, NULL)

PHP_INI_END()

/* Init module */
ZEND_GET_MODULE(xhprof)


/**
 * **********************************
 * PHP EXTENSION FUNCTION DEFINITIONS
 * **********************************
 */

/**
 * Start XHProf profiling in hierarchical mode.
 *
 * @param  long $flags  flags for hierarchical mode
 * @return void
 * @author kannan
 */
PHP_FUNCTION(xhprof_enable) {
  long  xhprof_flags = 0;                                    /* XHProf flags */
  zval *optional_array = NULL;         /* optional array arg: for future use */

#if 1 == 1
  ZEND_PARSE_PARAMETERS_START(0, 2);
    Z_PARAM_OPTIONAL;
    Z_PARAM_LONG(xhprof_flags);
    Z_PARAM_ZVAL(optional_array);
  ZEND_PARSE_PARAMETERS_END();
#else
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
                            "|lz", &xhprof_flags, &optional_array) == FAILURE) {
    return;
  }
#endif
  
  hp_get_ignored_functions_from_arg(optional_array);

  hp_begin(XHPROF_MODE_HIERARCHICAL, xhprof_flags TSRMLS_CC);
}

/**
 * Stops XHProf from profiling in hierarchical mode anymore and returns the
 * profile info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author kannan, hzhao
 */
PHP_FUNCTION(xhprof_disable) {
  if (hp_globals.enabled) {
    hp_stop(TSRMLS_C);
    RETURN_ZVAL(hp_globals.stats_count, 1, 0);
  }
  /* else null is returned */
}

/**
 * Start XHProf profiling in sampling mode.
 *
 * @return void
 * @author cjiang
 */
PHP_FUNCTION(xhprof_sample_enable) {
  long  xhprof_flags = 0;                                    /* XHProf flags */
  hp_get_ignored_functions_from_arg(NULL);
  hp_begin(XHPROF_MODE_SAMPLED, xhprof_flags TSRMLS_CC);
}
 
/**
 * Stops XHProf from profiling in sampling mode anymore and returns the profile
 * info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author cjiang
 */
PHP_FUNCTION(xhprof_sample_disable) {
  if (hp_globals.enabled) {
    hp_stop(TSRMLS_C);
    RETURN_ZVAL(hp_globals.stats_count, 1, 0);
  }
  /* else null is returned */
}

/**
 * Module init callback.
 *
 * @author cjiang
 */
PHP_MINIT_FUNCTION(xhprof) {
  int i;

  REGISTER_INI_ENTRIES();

  REGISTER_LONG_CONSTANT("XHPROF_FLAGS_NO_BUILTINS",
                         XHPROF_FLAGS_NO_BUILTINS,
                         CONST_CS | CONST_PERSISTENT);

  REGISTER_LONG_CONSTANT("XHPROF_FLAGS_CPU",
                         XHPROF_FLAGS_CPU,
                         CONST_CS | CONST_PERSISTENT);

  REGISTER_LONG_CONSTANT("XHPROF_FLAGS_MEMORY",
                         XHPROF_FLAGS_MEMORY,
                         CONST_CS | CONST_PERSISTENT);

  /* Get the number of available logical CPUs. */
  hp_globals.cpu_num = sysconf(_SC_NPROCESSORS_CONF);

  /* Get the cpu affinity mask. */
#ifndef __APPLE__
  if (GET_AFFINITY(0, sizeof(cpu_set_t), &hp_globals.prev_mask) < 0) {
    perror("getaffinity");
    return FAILURE;
  }
#else
  CPU_ZERO(&(hp_globals.prev_mask));
#endif

  /* Initialize cpu_frequencies and cur_cpu_id. */
  hp_globals.cpu_frequencies = NULL;
  hp_globals.cur_cpu_id = 0;

  hp_globals.stats_count = NULL;

  /* no free hp_entry_t structures to start with */
  hp_globals.entry_free_list = NULL;

  /* Initialize with the dummy mode first Having these dummy callbacks saves
   * us from checking if any of the callbacks are NULL everywhere. */
  hp_globals.mode_cb.init_cb     = hp_mode_dummy_init_cb;
  hp_globals.mode_cb.exit_cb     = hp_mode_dummy_exit_cb;
  hp_globals.mode_cb.begin_fn_cb = hp_mode_dummy_beginfn_cb;
  hp_globals.mode_cb.end_fn_cb   = hp_mode_dummy_endfn_cb;

  for (i = 0; i < 256; i++) {
    hp_globals.func_hash_counters[i] = 0;
  }

#if defined(DEBUG)
  /* To make it random number generator repeatable to ease testing. */
  srand(0);
#endif
  
  return SUCCESS;
}

/**
 * Module shutdown callback.
 */
PHP_MSHUTDOWN_FUNCTION(xhprof) {
  /* Make sure cpu_frequencies is free'ed. */
  clear_frequencies();

  /* free any remaining items in the free list */
  hp_free_the_free_list();

  UNREGISTER_INI_ENTRIES();

  return SUCCESS;
}

/**
 * Request init callback. Nothing to do yet!
 */
PHP_RINIT_FUNCTION(xhprof) {
 
  _zend_compile_file = zend_compile_file;
  zend_compile_file  = hp_compile_file;

  /* Replace zend_compile_string with our proxy */
  _zend_compile_string = zend_compile_string;
  zend_compile_string = hp_compile_string;

  /* Replace zend_execute with our proxy */
  _zend_execute_ex = zend_execute_ex;
  zend_execute_ex  = hp_execute_ex;

  /* Replace zend_execute_internal with our proxy */
  _zend_execute_internal = zend_execute_internal;
  zend_execute_internal = hp_execute_internal;
  
  return SUCCESS;
}

/**
 * Request shutdown callback. Stop profiling and return.
 */
PHP_RSHUTDOWN_FUNCTION(xhprof) {
  hp_end(TSRMLS_C);
  zend_execute_ex       = _zend_execute_ex;
  zend_execute_internal = _zend_execute_internal;
  
  zend_compile_file = _zend_compile_file;
  zend_compile_string = _zend_compile_string;
  return SUCCESS;
}

/**
 * Module info callback. Returns the xhprof version.
 */
PHP_MINFO_FUNCTION(xhprof)
{
  char buf[SCRATCH_BUF_LEN];
  char tmp[SCRATCH_BUF_LEN];
  int i;
  int len;

  php_info_print_table_start();
  php_info_print_table_header(2, "xhprof", XHPROF_VERSION);
  len = snprintf(buf, SCRATCH_BUF_LEN, "%d", hp_globals.cpu_num);
  buf[len] = 0;
  php_info_print_table_header(2, "CPU num", buf);

  if (hp_globals.cpu_frequencies) {
    /* Print available cpu frequencies here. */
    php_info_print_table_header(2, "CPU logical id", " Clock Rate (MHz) ");
    for (i = 0; i < hp_globals.cpu_num; ++i) {
      len = snprintf(buf, SCRATCH_BUF_LEN, " CPU %d ", i);
      buf[len] = 0;
      len = snprintf(tmp, SCRATCH_BUF_LEN, "%f", hp_globals.cpu_frequencies[i]);
      tmp[len] = 0;
      php_info_print_table_row(2, buf, tmp);
    }
  }

  php_info_print_table_end();
}


/**
 * ***************************************************
 * COMMON HELPER FUNCTION DEFINITIONS AND LOCAL MACROS
 * ***************************************************
 */

/**
 * A hash function to calculate a 8-bit hash code for a function name.
 * This is based on a small modification to 'zend_inline_hash_func' by summing
 * up all bytes of the ulong returned by 'zend_inline_hash_func'.
 *
 * @param str, char *, string to be calculated hash code for.
 *
 * @author cjiang
 */
static inline uint8 hp_inline_hash(char * str) {
  ulong h = 5381;
  uint i = 0;
  uint8 res = 0;

  while (*str) {
    h += (h << 5);
    h ^= (ulong) *str++;
  }

  for (i = 0; i < sizeof(ulong); i++) {
    res += ((uint8 *)&h)[i];
  }
  return res;
}

/**
 * Parse the list of ignored functions from the zval argument.
 *
 * @author mpal
 */
static void hp_get_ignored_functions_from_arg(zval *args) {
  if (args != NULL) {
    zval  *zresult = NULL;

    zend_string *ignored_functions = zend_string_init("ignored_functions", strlen("ignored_functions"), 0);
    
    if (Z_TYPE_P(args) == IS_ARRAY) {
      HashTable *ht;

      ht = Z_ARRVAL_P(args);
      zresult = zend_hash_find(ht, ignored_functions);
    }
    
    zend_string_release(ignored_functions);
    if (zresult && Z_TYPE_P(zresult) == IS_ARRAY) {
      hp_globals.ignored_function_names = Z_ARRVAL_P(zresult);
    } else if (zresult && Z_TYPE_P(zresult) == IS_STRING) {
        //zend_hash_init(hp_globals.ignored_function_names, 0, NULL, NULL, 0);
        //zend_hash_add(hp_globals.ignored_function_names, Z_STR_P(zresult), ZVAL_TRUE(IS_TRUE));
    } else {
      //hp_globals.ignored_function_names = NULL;
    }
  } else {
    //hp_globals.ignored_function_names = NULL;
  }
}

/**
 * Initialize profiler state
 *
 * @author kannan, veeve
 */
void hp_init_profiler_state(int level TSRMLS_DC) {
  /* Setup globals */
  if (!hp_globals.ever_enabled) {
    hp_globals.ever_enabled  = 1;
    hp_globals.entries = NULL;
  }
  hp_globals.profiler_level  = (int) level;

  /* Init stats_count */
  if (hp_globals.stats_count) {
    //zval_dtor(hp_globals.stats_count);
    efree(hp_globals.stats_count);
  }
  
  hp_globals.stats_count = (zval *)emalloc(sizeof(zval));
  array_init(hp_globals.stats_count);

  /* NOTE(cjiang): some fields such as cpu_frequencies take relatively longer
   * to initialize, (5 milisecond per logical cpu right now), therefore we
   * calculate them lazily. */
  if (hp_globals.cpu_frequencies == NULL) {
    get_all_cpu_frequencies();
    restore_cpu_affinity(&hp_globals.prev_mask);
  }

  /* bind to a random cpu so that we can use rdtsc instruction. */
  bind_to_cpu((int) (rand() % hp_globals.cpu_num));

  /* Call current mode's init cb */
  hp_globals.mode_cb.init_cb(TSRMLS_C);
}

/**
 * Cleanup profiler state
 *
 * @author kannan, veeve
 */
void hp_clean_profiler_state(TSRMLS_D) {
  /* Call current mode's exit cb */
  hp_globals.mode_cb.exit_cb(TSRMLS_C);

  /* Clear globals */
  if (hp_globals.stats_count) {
    //Z_DELREF_P(hp_globals.stats_count);
	zval_ptr_dtor(hp_globals.stats_count);
	//zend_array_destroy(Z_ARRVAL_P(hp_globals.stats_count));
    efree(hp_globals.stats_count);
    hp_globals.stats_count = NULL;
  }
  hp_globals.entries = NULL;
  hp_globals.profiler_level = 1;
  hp_globals.ever_enabled = 0;

  //hp_globals.ignored_function_names = NULL;
}

/*
 * Start profiling - called just before calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define BEGIN_PROFILING(entries, symbol, profile_curr)                  \
  do {                                                                  \
    /* Use a hash code to filter most of the string comparisons. */     \
    uint8 hash_code  = hp_inline_hash(symbol->val);                     \
    profile_curr = !hp_ignore_entry(symbol);                            \
    if (profile_curr) {                                                 \
      hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();              \
      (cur_entry)->hash_code = hash_code;                               \
      (cur_entry)->name_hprof = symbol;                                 \
      (cur_entry)->prev_hprof = (*(entries));                           \
      /* Call the universal callback */                                 \
      hp_mode_common_beginfn((entries), (cur_entry) TSRMLS_CC);         \
      /* Call the mode's beginfn callback */                            \
      hp_globals.mode_cb.begin_fn_cb((entries), (cur_entry) TSRMLS_CC); \
      /* Update entries linked list */                                  \
      (*(entries)) = (cur_entry);                                       \
    }                                                                   \
  } while (0)

/*
 * Stop profiling - called just after calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define END_PROFILING(entries, profile_curr)                            \
  do {                                                                  \
    if (profile_curr) {                                                 \
      hp_entry_t *cur_entry;                                            \
      /* Call the mode's endfn callback. */                             \
      /* NOTE(cjiang): we want to call this 'end_fn_cb' before */       \
      /* 'hp_mode_common_endfn' to avoid including the time in */       \
      /* 'hp_mode_common_endfn' in the profiling results.      */       \
      hp_globals.mode_cb.end_fn_cb((entries) TSRMLS_CC);                \
      cur_entry = (*(entries));                                         \
      /* Call the universal callback */                                 \
      hp_mode_common_endfn((entries), (cur_entry) TSRMLS_CC);           \
      /* Free top entry and update entries linked list */               \
      (*(entries)) = (*(entries))->prev_hprof;                          \
      hp_fast_free_hprof_entry(cur_entry);                              \
    }                                                                   \
  } while (0)


/**
 * Returns formatted function name
 *
 * @param  entry        hp_entry
 * @param  result_buf   ptr to result buf
 * @param  result_len   max size of result buf
 * @return total size of the function name returned in result_buf
 * @author veeve
 */
size_t hp_get_entry_name(hp_entry_t  *entry,
                         /*char           *result_buf,
                         size_t          result_len*/
                        zend_string *result
                        ) {

  /* Validate result_len */
  if (result->len <= 1) {
    /* Insufficient result_bug. Bail! */
    return 0;
  }

  /* Add '@recurse_level' if required */
  /* NOTE:  Dont use snprintf's return val as it is compiler dependent */
  if (entry->rlvl_hprof) {
    snprintf(result->val+strlen(result->val), result->len,
             "%s@%d",
             entry->name_hprof->val, entry->rlvl_hprof);
  }
  else {
    snprintf(result->val + strlen(result->val), result->len,
             "%s",
             entry->name_hprof->val);
  }

  /* Force null-termination at MAX */
//  result_buf[result_len - 1] = 0;
  result->val[result->len - 1] = 0;
  
  return strlen(result->val);
}

static inline int  hp_ignore_entry(zend_string *curr_func) {
  /* First check if ignoring functions is enabled */
  return hp_globals.ignored_function_names != NULL &&
         zend_hash_exists(hp_globals.ignored_function_names, curr_func);
}

/**
 * Build a caller qualified name for a callee.
 *
 * For example, if A() is caller for B(), then it returns "A==>B".
 * Recursive invokations are denoted with @<n> where n is the recursion
 * depth.
 *
 * For example, "foo==>foo@1", and "foo@2==>foo@3" are examples of direct
 * recursion. And  "bar==>foo@1" is an example of an indirect recursive
 * call to foo (implying the foo() is on the call stack some levels
 * above).
 *
 * @author kannan, veeve
 */
size_t hp_get_function_stack(hp_entry_t *entry,
                             int            level,
                             zend_string *result
                             ) {
  size_t         len = 0;
  
  /* End recursion if we dont need deeper levels or we dont have any deeper
   * levels */
  if (!entry->prev_hprof || (level <= 1)) {
    return hp_get_entry_name(entry, result);
  }

  /* Take care of all ancestors first */
  len = hp_get_function_stack(entry->prev_hprof,
                              level - 1,
                              result
                              );

  /* Append the delimiter */
# define    HP_STACK_DELIM        "==>"
# define    HP_STACK_DELIM_LEN    (sizeof(HP_STACK_DELIM) - 1)

  if (result->len < (len + HP_STACK_DELIM_LEN)) {
    /* Insufficient result_buf. Bail out! */
    return len;
  }

  /* Add delimiter only if entry had ancestors */
  if (len) {
    strncat(result->val + len,
            HP_STACK_DELIM,
            HP_STACK_DELIM_LEN);
  }

# undef     HP_STACK_DELIM_LEN
# undef     HP_STACK_DELIM
  
  /* Append the current function name */
  return hp_get_entry_name(entry, result);
}

/**
 * Takes an input of the form /a/b/c/d/foo.php and returns
 * a pointer to one-level directory and basefile name
 * (d/foo.php) in the same string.
 */
static const char *hp_get_base_filename(const char *filename) {
  const char *ptr;
  int   found = 0;

  if (!filename)
    return "";

  /* reverse search for "/" and return a ptr to the next char */
  for (ptr = filename + strlen(filename) - 1; ptr >= filename; ptr--) {
    if (*ptr == '/') {
      found++;
    }
    if (found == 2) {
      return ptr + 1;
    }
  }

  /* no "/" char found, so return the whole string */
  return filename;
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */
static char *hp_get_function_name(zend_op_array *ops TSRMLS_DC) {
  zend_execute_data *data;
  const char        *func = NULL;
  const char        *cls = NULL;
  char              *ret = NULL;
  int                len;
  zend_function      *curr_func;

  data = EG(current_execute_data);

  if (data) {
    /* shared meta data for function on the call stack */
    curr_func = data->func;

    /* extract function name from the meta info */
    func = curr_func->common.function_name->val;

    if (func) {
      /* previously, the order of the tests in the "if" below was
       * flipped, leading to incorrect function names in profiler
       * reports. When a method in a super-type is invoked the
       * profiler should qualify the function name with the super-type
       * class name (not the class name based on the run-time type
       * of the object.
       */
      if (curr_func->common.scope) {
        cls = curr_func->common.scope->name->val;
      } else if (data->called_scope) {
        cls = data->called_scope->name->val;
      }

      if (cls) {
        len = strlen(cls) + strlen(func) + 10;
        ret = (char*)emalloc(len);
        snprintf(ret, len, "%s::%s", cls, func);
      } else {
        ret = estrdup(func);
      }
    } else {
      long     curr_op;
      int      add_filename = 0;

      /* we are dealing with a special directive/function like
       * include, eval, etc.
       */
#if ZEND_EXTENSION_API_NO >= 220121212
      if (data->prev_execute_data) {
        curr_op = data->prev_execute_data->opline->extended_value;
      } else {
        curr_op = data->opline->extended_value;
      }
#elif ZEND_EXTENSION_API_NO >= 220100525
      curr_op = data->opline->extended_value;
#else
      //curr_op = data->opline->op2.u.constant.value.lval;
#endif

      switch (curr_op) {
        case ZEND_EVAL:
          func = "eval";
          break;
        case ZEND_INCLUDE:
          func = "include";
          add_filename = 1;
          break;
        case ZEND_REQUIRE:
          func = "require";
          add_filename = 1;
          break;
        case ZEND_INCLUDE_ONCE:
          func = "include_once";
          add_filename = 1;
          break;
        case ZEND_REQUIRE_ONCE:
          func = "require_once";
          add_filename = 1;
          break;
        default:
          func = "???_op";
          break;
      }

      /* For some operations, we'll add the filename as part of the function
       * name to make the reports more useful. So rather than just "include"
       * you'll see something like "run_init::foo.php" in your reports.
       */
      if (add_filename){
        const char *filename;
        int   len;
        filename = hp_get_base_filename((curr_func->op_array).filename->val);
        len      = strlen("run_init") + strlen(filename) + 3;
        ret      = (char *)emalloc(len);
        snprintf(ret, len, "run_init::%s", filename);
      } else {
        ret = estrdup(func);
      }
    }
  }
  return ret;
}


/**
 * Free any items in the free list.
 */
static void hp_free_the_free_list() {
  hp_entry_t *p = hp_globals.entry_free_list;
  hp_entry_t *cur;

  while (p) {
    cur = p;
    p = p->prev_hprof;
    zend_string_release(cur->name_hprof);
    free(cur);
  }
}

/**
 * Fast allocate a hp_entry_t structure. Picks one from the
 * free list if available, else does an actual allocate.
 *
 * Doesn't bother initializing allocated memory.
 *
 * @author kannan
 */
static hp_entry_t *hp_fast_alloc_hprof_entry() {
  hp_entry_t *p;

  p = hp_globals.entry_free_list;

  if (p) {
    hp_globals.entry_free_list = p->prev_hprof;
    return p;
  } else {
    hp_entry_t *tmp = malloc(sizeof(hp_entry_t));
    return tmp;
  }
}

/**
 * Fast free a hp_entry_t structure. Simply returns back
 * the hp_entry_t to a free list and doesn't actually
 * perform the free.
 *
 * @author kannan
 */
static void hp_fast_free_hprof_entry(hp_entry_t *p) {

  /* we use/overload the prev_hprof field in the structure to link entries in
   * the free list. */
  p->prev_hprof = hp_globals.entry_free_list;
  hp_globals.entry_free_list = p;
}

/**
 * Increment the count of the given stat with the given count
 * If the stat was not set before, inits the stat to the given count
 *
 * @param  zval *counts   Zend hash table pointer
 * @param  char *name     Name of the stat
 * @param  long  count    Value of the stat to incr by
 * @return void
 * @author kannan
 */
void hp_inc_count(zval *counts, zend_string *name, long count TSRMLS_DC) {
  HashTable *ht;
  zval *data;

  if (!counts) return;
  ht = HASH_OF(counts);
  if (!ht) return;

  if ((data = zend_hash_find(ht, name)) != NULL) {
    ZVAL_LONG(data, Z_LVAL_P(data) + count);
  } else {
     
    //ZVAL_LONG(data, count);
    //zend_hash_update(ht, name, data);
    
    add_assoc_long(counts, name->val, count);
  }
}


/**
 * Sample the stack. Add it to the stats_count global.
 *
 * @param  tv            current time
 * @param  entries       func stack as linked list of hp_entry_t
 * @return void
 * @author veeve
 */
void hp_sample_stack(hp_entry_t  **entries  TSRMLS_DC) {

  //char symbol[SCRATCH_BUF_LEN * 1000];
  zend_string *symbol;
  zend_string *key;
  symbol = zend_string_alloc(SCRATCH_BUF_LEN * 1000, 0);
  key = zend_string_alloc(20, 0);

  /* Build key */
  snprintf(key->val, key->len,
           "%d.%06d",
           hp_globals.last_sample_time.tv_sec,
           hp_globals.last_sample_time.tv_usec);

  /* Init stats in the global stats_count hashtable */
  hp_get_function_stack(*entries,
                        INT_MAX,
                        symbol
                        );

  add_assoc_string(hp_globals.stats_count,
                   key->val,
                   symbol->val);
  
  //zend_string_release(symbol);
  //zend_string_release(key);
  
  return;
}

/**
 * Checks to see if it is time to sample the stack.
 * Calls hp_sample_stack() if its time.
 *
 * @param  entries        func stack as linked list of hp_entry_t
 * @param  last_sample    time the last sample was taken
 * @param  sampling_intr  sampling interval in microsecs
 * @return void
 * @author veeve
 */
void hp_sample_check(hp_entry_t **entries  TSRMLS_DC) {
  /* Validate input */
  if (!entries || !(*entries)) {
    return;
  }

  /* See if its time to sample.  While loop is to handle a single function
   * taking a long time and passing several sampling intervals. */
  while ((cycle_timer() - hp_globals.last_sample_tsc)
         > hp_globals.sampling_interval_tsc) {

    /* bump last_sample_tsc */
    hp_globals.last_sample_tsc += hp_globals.sampling_interval_tsc;

    /* bump last_sample_time - HAS TO BE UPDATED BEFORE calling hp_sample_stack */
    incr_us_interval(&hp_globals.last_sample_time, XHPROF_SAMPLING_INTERVAL);

    /* sample the stack */
    hp_sample_stack(entries  TSRMLS_CC);
  }

  return;
}


/**
 * Truncates the given timeval to the nearest slot begin, where
 * the slot size is determined by intr
 *
 * @param  tv       Input timeval to be truncated in place
 * @param  intr     Time interval in microsecs - slot width
 * @return void
 * @author veeve
 */
void hp_trunc_time(struct timeval *tv,
                   uint64          intr) {
  uint64 time_in_micro;

  /* Convert to microsecs and trunc that first */
  time_in_micro = (tv->tv_sec * 1000000) + tv->tv_usec;
  time_in_micro /= intr;
  time_in_micro *= intr;

  /* Update tv */
  tv->tv_sec  = (time_in_micro / 1000000);
  tv->tv_usec = (time_in_micro % 1000000);
}



/**
 * ***********************
 * High precision timer related functions.
 * ***********************
 */

/**
 * Get time stamp counter (TSC) value via 'rdtsc' instruction.
 *
 * @return 64 bit unsigned integer
 * @author cjiang
 */
static inline uint64 cycle_timer() {
  uint32 __a,__d;
  uint64 val;
  asm volatile("rdtsc" : "=a" (__a), "=d" (__d));
  (val) = ((uint64)__a) | (((uint64)__d)<<32);
  return val;
}

/**
 * Bind the current process to a specified CPU. This function is to ensure that
 * the OS won't schedule the process to different processors, which would make
 * values read by rdtsc unreliable.
 *
 * @param uint32 cpu_id, the id of the logical cpu to be bound to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int bind_to_cpu(uint32 cpu_id) {
  cpu_set_t new_mask;

  CPU_ZERO(&new_mask);
  CPU_SET(cpu_id, &new_mask);

  if (SET_AFFINITY(0, sizeof(cpu_set_t), &new_mask) < 0) {
    perror("setaffinity");
    return -1;
  }

  /* record the cpu_id the process is bound to. */
  hp_globals.cur_cpu_id = cpu_id;

  return 0;
}

/**
 * Get time delta in microseconds.
 */
static long get_us_interval(struct timeval *start, struct timeval *end) {
  return (((end->tv_sec - start->tv_sec) * 1000000)
          + (end->tv_usec - start->tv_usec));
}

/**
 * Incr time with the given microseconds.
 */
static void incr_us_interval(struct timeval *start, uint64 incr) {
  incr += (start->tv_sec * 1000000 + start->tv_usec);
  start->tv_sec  = incr/1000000;
  start->tv_usec = incr%1000000;
  return;
}

/**
 * Convert from TSC counter values to equivalent microseconds.
 *
 * @param uint64 count, TSC count value
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author cjiang
 */
static inline double get_us_from_tsc(uint64 count, double cpu_frequency) {
  return count / cpu_frequency;
}

/**
 * Convert microseconds to equivalent TSC counter ticks
 *
 * @param uint64 microseconds
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author veeve
 */
static inline uint64 get_tsc_from_us(uint64 usecs, double cpu_frequency) {
  return (uint64) (usecs * cpu_frequency);
}

/**
 * This is a microbenchmark to get cpu frequency the process is running on. The
 * returned value is used to convert TSC counter values to microseconds.
 *
 * @return double.
 * @author cjiang
 */
static double get_cpu_frequency() {
  struct timeval start;
  struct timeval end;

  if (gettimeofday(&start, 0)) {
    perror("gettimeofday");
    return 0.0;
  }
  uint64 tsc_start = cycle_timer();
  /* Sleep for 5 miliseconds. Comparaing with gettimeofday's  few microseconds
   * execution time, this should be enough. */
  usleep(5000);
  if (gettimeofday(&end, 0)) {
    perror("gettimeofday");
    return 0.0;
  }
  uint64 tsc_end = cycle_timer();
  return (tsc_end - tsc_start) * 1.0 / (get_us_interval(&start, &end));
}

/**
 * Calculate frequencies for all available cpus.
 *
 * @author cjiang
 */
static void get_all_cpu_frequencies() {
  int id;
  double frequency;

  hp_globals.cpu_frequencies = malloc(sizeof(double) * hp_globals.cpu_num);
  if (hp_globals.cpu_frequencies == NULL) {
    return;
  }

  /* Iterate over all cpus found on the machine. */
  for (id = 0; id < hp_globals.cpu_num; ++id) {
    /* Only get the previous cpu affinity mask for the first call. */
    if (bind_to_cpu(id)) {
      clear_frequencies();
      return;
    }

    /* Make sure the current process gets scheduled to the target cpu. This
     * might not be necessary though. */
    usleep(0);

    frequency = get_cpu_frequency();
    if (frequency == 0.0) {
      clear_frequencies();
      return;
    }
    hp_globals.cpu_frequencies[id] = frequency;
  }
}

/**
 * Restore cpu affinity mask to a specified value. It returns 0 on success and
 * -1 on failure.
 *
 * @param cpu_set_t * prev_mask, previous cpu affinity mask to be restored to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int restore_cpu_affinity(cpu_set_t * prev_mask) {
  if (SET_AFFINITY(0, sizeof(cpu_set_t), prev_mask) < 0) {
    perror("restore setaffinity");
    return -1;
  }
  /* default value ofor cur_cpu_id is 0. */
  hp_globals.cur_cpu_id = 0;
  return 0;
}

/**
 * Reclaim the memory allocated for cpu_frequencies.
 *
 * @author cjiang
 */
static void clear_frequencies() {
  if (hp_globals.cpu_frequencies) {
    free(hp_globals.cpu_frequencies);
    hp_globals.cpu_frequencies = NULL;
  }
  restore_cpu_affinity(&hp_globals.prev_mask);
}


/**
 * ***************************
 * XHPROF DUMMY CALLBACKS
 * ***************************
 */
void hp_mode_dummy_init_cb(TSRMLS_D) { }


void hp_mode_dummy_exit_cb(TSRMLS_D) { }


void hp_mode_dummy_beginfn_cb(hp_entry_t **entries,
                              hp_entry_t *current  TSRMLS_DC) { }

void hp_mode_dummy_endfn_cb(hp_entry_t **entries   TSRMLS_DC) { }


/**
 * ****************************
 * XHPROF COMMON CALLBACKS
 * ****************************
 */
/**
 * XHPROF universal begin function.
 * This function is called for all modes before the
 * mode's specific begin_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack)
 *                                  of hprof entries
 * @param  hp_entry_t  *current  hprof entry for the current fn
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_beginfn(hp_entry_t **entries,
                            hp_entry_t  *current  TSRMLS_DC) {
  hp_entry_t   *p;

  /* This symbol's recursive level */
  int    recurse_level = 0;

  if (hp_globals.func_hash_counters[current->hash_code] > 0) {
    /* Find this symbols recurse level */
    for(p = (*entries); p; p = p->prev_hprof) {
      if (!strcmp(current->name_hprof->val, p->name_hprof->val)) {
        recurse_level = (p->rlvl_hprof) + 1;
        break;
      }
    }
  }
  hp_globals.func_hash_counters[current->hash_code]++;

  /* Init current function's recurse level */
  current->rlvl_hprof = recurse_level;
}

/**
 * XHPROF universal end function.  This function is called for all modes after
 * the mode's specific end_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack) of hprof entries
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_endfn(hp_entry_t **entries, hp_entry_t *current TSRMLS_DC) {
  hp_globals.func_hash_counters[current->hash_code]--;
}




/**
 * ************************************
 * XHPROF BEGIN FUNCTION CALLBACKS
 * ************************************
 */

/**
 * XHPROF_MODE_HIERARCHICAL's begin function callback
 *
 * @author kannan
 */
void hp_mode_hier_beginfn_cb(hp_entry_t **entries,
                             hp_entry_t  *current  TSRMLS_DC) {
  /* Get start tsc counter */
  current->tsc_start = cycle_timer();

  /* Get CPU usage */
  if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
    getrusage(RUSAGE_SELF, &(current->ru_start_hprof));
  }

  /* Get memory usage */
  if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
    current->mu_start_hprof  = zend_memory_usage(0 TSRMLS_CC);
    current->pmu_start_hprof = zend_memory_peak_usage(0 TSRMLS_CC);
  }
}

/**
 * XHPROF_MODE_SAMPLED's init callback
 *
 * @author veeve
 */
void hp_mode_sampled_init_cb(TSRMLS_D) {
  struct timeval  now;
  uint64 truncated_us;
  uint64 truncated_tsc;
  double cpu_freq = hp_globals.cpu_frequencies[hp_globals.cur_cpu_id];

  /* Init the last_sample in tsc */
  hp_globals.last_sample_tsc = cycle_timer();

  /* Find the microseconds that need to be truncated */
  gettimeofday(&hp_globals.last_sample_time, 0);
  now = hp_globals.last_sample_time;
  hp_trunc_time(&hp_globals.last_sample_time, XHPROF_SAMPLING_INTERVAL);

  /* Subtract truncated time from last_sample_tsc */
  truncated_us  = get_us_interval(&hp_globals.last_sample_time, &now);
  truncated_tsc = get_tsc_from_us(truncated_us, cpu_freq);
  if (hp_globals.last_sample_tsc > truncated_tsc) {
    /* just to be safe while subtracting unsigned ints */
    hp_globals.last_sample_tsc -= truncated_tsc;
  }

  /* Convert sampling interval to ticks */
  hp_globals.sampling_interval_tsc =
    get_tsc_from_us(XHPROF_SAMPLING_INTERVAL, cpu_freq);
}


/**
 * XHPROF_MODE_SAMPLED's begin function callback
 *
 * @author veeve
 */
void hp_mode_sampled_beginfn_cb(hp_entry_t **entries,
                                hp_entry_t  *current  TSRMLS_DC) {
  /* See if its time to take a sample */
  hp_sample_check(entries  TSRMLS_CC);
}

/**
 * **********************************
 * XHPROF END FUNCTION CALLBACKS
 * **********************************
 */

/**
 * XHPROF shared end function callback
 *
 * @author kannan
 */
zval * hp_mode_shared_endfn_cb(hp_entry_t *top,
                               zend_string          *symbol  TSRMLS_DC) {
  zval counts;
  zval *countsp;
  uint64   tsc_end;
  HashTable *ht;

  /* Get end tsc counter */
  tsc_end = cycle_timer();

  /* Get the stat array */
  /* Bail if something is goofy */
  if (!hp_globals.stats_count || !(ht = HASH_OF(hp_globals.stats_count))) {
    return (zval *) 0;
  }

  /* Lookup our hash table */
  if ((countsp = zend_hash_str_find(ht, symbol->val, strlen(symbol->val))) == NULL) {
    /* Add symbol to hash table */
    countsp = &counts;
    array_init(countsp);
    add_assoc_zval(hp_globals.stats_count, symbol->val, countsp);
  }

  /* Bump stats in the counts hashtable */
  zend_string *ct = zend_string_init("ct", sizeof("ct") - 1, 0);
  hp_inc_count(countsp, ct, 1  TSRMLS_CC);
  zend_string_release(ct);

  zend_string *wt = zend_string_init("wt", sizeof("wt") - 1, 0);
  hp_inc_count(countsp, wt, get_us_from_tsc(tsc_end - top->tsc_start,
        hp_globals.cpu_frequencies[hp_globals.cur_cpu_id]) TSRMLS_CC);
  zend_string_release(wt);
  
  // This is a dirty hack
  // TODO: replace with something more efficient
  ht = HASH_OF(hp_globals.stats_count);
  countsp = zend_hash_str_find(ht, symbol->val, strlen(symbol->val));
  
  return countsp;
}

/**
 * XHPROF_MODE_HIERARCHICAL's end function callback
 *
 * @author kannan
 */
void hp_mode_hier_endfn_cb(hp_entry_t **entries  TSRMLS_DC) {
  hp_entry_t   *top = (*entries);
  zval            *counts;
  struct rusage    ru_end;
  zend_string      *symbol;
  long int         mu_end;
  long int         pmu_end;

  /* Get the stat array */
  symbol = zend_string_alloc(SCRATCH_BUF_LEN, 0);
  ZSTR_VAL(symbol)[0] = '\000';
  hp_get_function_stack(top, 2, symbol);
  if (!(counts = hp_mode_shared_endfn_cb(top,
                                         symbol  TSRMLS_CC))) {
    efree(symbol);
    return;
  }
  zend_string_free(symbol);
  
  if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
    /* Get CPU usage */
    getrusage(RUSAGE_SELF, &ru_end);

    /* Bump CPU stats in the counts hashtable */
    zend_string *cpu = zend_string_init("cpu", sizeof("cpu") - 1, 0);
    hp_inc_count(counts, cpu, (get_us_interval(&(top->ru_start_hprof.ru_utime),
                                              &(ru_end.ru_utime)) +
                              get_us_interval(&(top->ru_start_hprof.ru_stime),
                                              &(ru_end.ru_stime)))
              TSRMLS_CC);
    zend_string_release(cpu);
  }

  if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
    /* Get Memory usage */
    mu_end  = zend_memory_usage(0 TSRMLS_CC);
    pmu_end = zend_memory_peak_usage(0 TSRMLS_CC);

    /* Bump Memory stats in the counts hashtable */
    zend_string *mu = zend_string_init("mu", sizeof("mu") - 1, 0);
    hp_inc_count(counts, mu,  mu_end - top->mu_start_hprof    TSRMLS_CC);
    zend_string_release(mu);
    
    zend_string *pmu = zend_string_init("pmu", sizeof("pmu") - 1, 0);
    hp_inc_count(counts, pmu, pmu_end - top->pmu_start_hprof  TSRMLS_CC);
    zend_string_release(pmu);
  }

}

/**
 * XHPROF_MODE_SAMPLED's end function callback
 *
 * @author veeve
 */
void hp_mode_sampled_endfn_cb(hp_entry_t **entries  TSRMLS_DC) {
  /* See if its time to take a sample */
  hp_sample_check(entries  TSRMLS_CC);
}


/**
 * ***************************
 * PHP EXECUTE/COMPILE PROXIES
 * ***************************
 */

/**
 * XHProf enable replaced the zend_execute function with this
 * new execute function. We can do whatever profiling we need to
 * before and after calling the actual zend_execute().
 *
 * @author hzhao, kannan, Jason
 */
ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data TSRMLS_DC) {
  zend_string   *func = NULL;
  int hp_profile_flag = 1;

  //func = hp_get_function_name(&current_data->func->op_array TSRMLS_CC);
  
  func = execute_data->func->internal_function.function_name;
  /* check if was in a class */ 
  if (execute_data ->called_scope != NULL && func != NULL) {
    //this is a class method;
    zend_string *class_name = execute_data->called_scope->name;
    zend_string *func_name = func;

    int class_name_len = class_name->len;
    func = zend_string_init(class_name->val, class_name_len + 2 + func_name->len, 0); 
    memcpy(func->val + class_name_len, "::", 2);
    memcpy(func->val + class_name_len + 2, func_name->val, func_name->len);
  } else if (func) {
    //just do the copy;
    func = zend_string_init(func->val, func->len, 0);
  } else if (execute_data->literals->u1.type_info == 6) {
    
    //could include, not sure others has the same value
    //This is fucking dam ugly
    zend_string *filename = execute_data->func->op_array.filename;
    
    const char *base_filename = hp_get_base_filename(filename->val);
    
    filename = zend_string_init(base_filename, strlen(base_filename), 0);
    
    int run_init_len = sizeof("run_init::") - 1;
    func = zend_string_init("run_init::", run_init_len + filename->len, 0); 
    memcpy(func->val + run_init_len, filename->val, filename->len);
    
    zend_string_release(filename);
  }

  if (!func || hp_globals.enabled == 0) {
    if (func) zend_string_free(func);
      _zend_execute_ex(execute_data TSRMLS_CC);
    return;
  }

  BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
  _zend_execute_ex(execute_data TSRMLS_CC);
  if (hp_globals.entries) {
    END_PROFILING(&hp_globals.entries, hp_profile_flag);
  }
  if (func) {
    zend_string_free(func);
  }
}

/**
 * Very similar to hp_execute. Proxy for zend_execute_internal().
 * Applies to zend builtin functions.
 *
 * @author hzhao, kannan, Jason
 */



ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data, zval *ret TSRMLS_DC) {
  zend_execute_data *current_data;
  zend_string   *func = NULL;
  int    hp_profile_flag = 1;

  current_data = EG(current_execute_data);
  
  if (!hp_globals.enabled
      || hp_globals.xhprof_flags & XHPROF_FLAGS_NO_BUILTINS) {
    /* if NO_BUILTINS is not set (i.e. user wants to profile builtins),
     * then we intercept internal (builtin) function calls.
     */
    if (!_zend_execute_internal) {
      /* no old override to begin with. so invoke the builtin's implementation  */

      //zend_op *opline = EX(opline);
      return execute_data ->func ->internal_function.handler(execute_data, ret);
    } else {
      /* call the old override */
      return _zend_execute_internal(execute_data, ret TSRMLS_CC);
    }
  }
  
  func = current_data->func->op_array.function_name ;
  
  //check is a class method
  if(current_data->func->op_array.scope != NULL) {
    zend_string *class_name = current_data->func->op_array.scope->name;
    zend_string *func_name = func;

    int class_name_len = class_name->len;
    func = zend_string_init(class_name->val, class_name_len + 2 + func_name->len, 0); 
    memcpy(func->val + class_name_len, "::", 2);
    memcpy(func->val + class_name_len + 2, func_name->val, func_name->len);
  } else {
    //just do the copy;
    func = zend_string_init(func->val, func->len, 0);
  }

  if (func && strcmp("xhprof_enable", func->val) != 0) {
    if (hp_globals.enabled == 1) {
      BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
    }
  }

  if (!_zend_execute_internal) {
    /* no old override to begin with. so invoke the builtin's implementation  */

    //zend_op *opline = EX(opline);
    execute_data ->func ->internal_function.handler(execute_data, ret);
  
  } else {
    /* call the old override */
    _zend_execute_internal(execute_data, ret TSRMLS_CC);
  }

  if (func && strcmp("xhprof_enable", func->val) != 0) {
    if (hp_globals.entries) {
      END_PROFILING(&hp_globals.entries, hp_profile_flag);
    }
    //zend_string_free(func);
  }

  if (func) {
    zend_string_release(func);
  }

}

/**
 * Proxy for zend_compile_file(). Used to profile PHP compilation time.
 *
 * @author kannan, hzhao
 */
ZEND_DLEXPORT zend_op_array* hp_compile_file(zend_file_handle *file_handle,
                                             int type TSRMLS_DC) {

  const char     *filename;
  int             len;
  zend_op_array  *ret;
  int             hp_profile_flag = 1;
  zend_string    *func_name;

  filename = hp_get_base_filename(file_handle->filename);
  len      = strlen("load::") + strlen(filename);

  func_name = zend_string_alloc(len, 0); 
  
  snprintf(func_name->val, len + 1, "load::%s", filename);

  if (hp_globals.enabled == 1) {
    BEGIN_PROFILING(&hp_globals.entries, func_name, hp_profile_flag);
  }
  ret = _zend_compile_file(file_handle, type TSRMLS_CC);
  if (hp_globals.entries) {
    END_PROFILING(&hp_globals.entries, hp_profile_flag);
  }

  zend_string_free(func_name);
  return ret;
}

/**
 * Proxy for zend_compile_string(). Used to profile PHP eval compilation time.
 */
ZEND_DLEXPORT zend_op_array* hp_compile_string(zval *source_string, char *filename TSRMLS_DC) {

    zend_string          *func;
    int            len;
    zend_op_array *ret;
    int            hp_profile_flag = 1;

    const char     *basename;
    
    basename = hp_get_base_filename(filename);
    
    len  = strlen("eval") + strlen(basename) + 3;
    func = zend_string_alloc(len, 0);
    snprintf(func->val, len, "eval::%s", basename);

    BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
    ret = _zend_compile_string(source_string, filename TSRMLS_CC);
    if (hp_globals.entries) {
        END_PROFILING(&hp_globals.entries, hp_profile_flag);
    }

    efree(func);
    return ret;
}

/**
 * **************************
 * MAIN XHPROF CALLBACKS
 * **************************
 */

/**
 * This function gets called once when xhprof gets enabled.
 * It replaces all the functions like zend_execute, zend_execute_internal,
 * etc that needs to be instrumented with their corresponding proxies.
 */
static void hp_begin(long level, long xhprof_flags TSRMLS_DC) {
  if (!hp_globals.enabled) {
    int hp_profile_flag = 1;

    hp_globals.enabled      = 1;
    hp_globals.xhprof_flags = (uint32)xhprof_flags;
    
    
    /* Register the appropriate callback functions Override just a subset of
     * all the callbacks is OK. */
    switch(level) {
      case XHPROF_MODE_HIERARCHICAL:
        hp_globals.mode_cb.begin_fn_cb = hp_mode_hier_beginfn_cb;
        hp_globals.mode_cb.end_fn_cb   = hp_mode_hier_endfn_cb;
        break;
      case XHPROF_MODE_SAMPLED:
        hp_globals.mode_cb.init_cb     = hp_mode_sampled_init_cb;
        hp_globals.mode_cb.begin_fn_cb = hp_mode_sampled_beginfn_cb;
        hp_globals.mode_cb.end_fn_cb   = hp_mode_sampled_endfn_cb;
        break;
    }
    
    hp_init_profiler_state(level TSRMLS_CC);
    
    zend_string *root_symbol = zend_string_init(ROOT_SYMBOL, sizeof(ROOT_SYMBOL) - 1, 1);
    BEGIN_PROFILING(&hp_globals.entries, root_symbol, hp_profile_flag);
    return;
  }
}

/**
 * Called at request shutdown time. Cleans the profiler's global state.
 */
static void hp_end(TSRMLS_D) {
  /* Bail if not ever enabled */
  if (!hp_globals.ever_enabled) {
    return;
  }

  /* Stop profiler if enabled */
  if (hp_globals.enabled) {
    hp_stop(TSRMLS_C);
  }

  /* Clean up state */
  hp_clean_profiler_state(TSRMLS_C);
}

/**
 * Called from xhprof_disable(). Removes all the proxies setup by
 * hp_begin() and restores the original values.
 */
static void hp_stop(TSRMLS_D) {
  int   hp_profile_flag = 1;

  /* End any unfinished calls */
  while (hp_globals.entries) {
    END_PROFILING(&hp_globals.entries, hp_profile_flag);
  }

  /* Resore cpu affinity. */
  restore_cpu_affinity(&hp_globals.prev_mask);

  /* Stop profiling */
  hp_globals.enabled = 0;
  hp_globals.ignored_function_names = NULL;
}
