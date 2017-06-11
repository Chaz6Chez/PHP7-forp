 /*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Anthony Terrien <forp@anthonyterrien.com>                    |
  | Author: ____Shies Gukai <gukai@bilibili.com>                         |
  +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"

#include "forp.h"
#include "php_forp.h"
#include "forp_log.h"

#include "forp_string.h"
#include "forp_annotation.h"
#include "ext/standard/php_var.h"
#include "Zend/zend_vm.h"

#if HAVE_SYS_TIME_H
#include <sys/time.h>
#elif defined(PHP_WIN32)
#include "win32/time.h"
#include <math.h>
static inline double round(double val) {
    return floor(val + 0.5);
}
#endif

#if HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

/* {{{ forp_populate_function
 */
static void forp_populate_function(
    forp_function_t *function,
    zend_execute_data *edata,
    zend_op_array *op_array TSRMLS_DC
) {

    // Inits function struct
    function->class = NULL;
    function->function = NULL;
    function->filename = NULL;

    // Retrieves class and function names
    if (edata->func->common.function_name) {
        if (Z_OBJ(edata->This)) {
            if (edata->func->common.scope) {
                function->class = strdup(ZSTR_VAL(edata->func->common.scope->name));
            }
        } else if (
                // EG(scope) &&
                edata->func->common.scope
                && edata->func->common.scope->name
                ) {
            function->class = strdup(ZSTR_VAL(edata->func->common.scope->name));
        }

        function->function = strdup(ZSTR_VAL(edata->func->common.function_name));
    } else {
#if PHP_VERSION_ID >= 50399
        switch (function->type = edata->opline->extended_value) {
#else
        switch (function->type = edata->opline->op2.u.constant.value.lval) {
#endif
            case ZEND_EVAL:
                function->function = "eval";
                break;
            case ZEND_INCLUDE:
                function->function = "include";
                break;
            case ZEND_REQUIRE:
                function->function = "require";
                break;
            case ZEND_INCLUDE_ONCE:
                function->function = "include_once";
                break;
            case ZEND_REQUIRE_ONCE:
                function->function = "require_once";
                break;
            default:
                function->function = "unknown";
                break;
        }
    }

    // Stores filename
    if (op_array && op_array->filename) {
        function->filename = strdup(ZSTR_VAL(op_array->filename));
    } else {
        zend_op_array *op_array = &edata->prev_execute_data->func->op_array;
        if (op_array && op_array->filename) {
            function->filename = strdup(ZSTR_VAL(op_array->filename));
        } else {
            function->filename = NULL;
        }
    }
}
/* }}} */

/* {{{ forp_open_node
 */
forp_node_t *forp_open_node(zend_execute_data *edata, zend_op_array *op_array TSRMLS_DC) {
    struct timeval tv;
    forp_node_t *n;
    int key;
    double start_time;

    // preparing current node
    // will be profiled after
    // forp_is_profiling_function test
    n = malloc(sizeof (forp_node_t));

    // getting function infos
    if (edata) {
        forp_populate_function(&(n->function), edata, op_array TSRMLS_CC);
        if (forp_is_profiling_function(n TSRMLS_CC)) {
            free(n);
            return NULL;
        }
    }

    // self duration on open
    gettimeofday(&tv, NULL);
    start_time = tv.tv_sec * 1000000.0 + tv.tv_usec;

    // continues node init
    n->state = 1; // opened
    n->level = FORP_G(nesting_level)++;
    n->parent = FORP_G(current_node);

    n->time_begin = 0;
    n->time_end = 0;
    n->time = 0;

    n->profiler_duration = 0;

    n->mem_begin = 0;
    n->mem_end = 0;
    n->mem = 0;

    // Call file and line number
    n->filename = NULL;
    n->lineno = 0;

    // Annotations
    n->alias = NULL;
    n->caption = NULL;
    n->function.groups = NULL;
    n->function.groups_len = 0;
    n->function.highlight = NULL;

    // Handles annotations
    if(op_array && op_array->doc_comment) {
        if(FORP_G(flags) & FORP_FLAG_ALIAS) {
            n->alias = forp_annotation_string(ZSTR_VAL(op_array->doc_comment), "ProfileAlias" TSRMLS_CC);
        }
        if(FORP_G(flags) & FORP_FLAG_CAPTION) {
            n->caption = forp_annotation_string(ZSTR_VAL(op_array->doc_comment), "ProfileCaption" TSRMLS_CC);
        }
        if(FORP_G(flags) & FORP_FLAG_GROUPS) {
            n->function.groups = malloc(sizeof(char*) * 10);
            n->function.groups_len = 0;
            forp_annotation_array(ZSTR_VAL(op_array->doc_comment), "ProfileGroup", &(n->function.groups), &(n->function.groups_len) TSRMLS_CC);
        }
        if(FORP_G(flags) & FORP_FLAG_HIGHLIGHT) {
            n->function.highlight = forp_annotation_string(ZSTR_VAL(op_array->doc_comment), "ProfileHighlight" TSRMLS_CC);
            if(n->function.highlight) php_printf(FORP_HIGHLIGHT_BEGIN);
        }
    }

    if (edata) {

        // Retrieves filename
        if(FORP_G(current_node) && FORP_G(current_node)->function.filename) {
            n->filename = strdup(FORP_G(current_node)->function.filename);
        }

        // Retrieves call lineno
        if(edata->opline && n->filename) {
            n->lineno = edata->opline->lineno;
        }

        // Collects params
        if(n->caption) {

#if PHP_VERSION_ID >= 50500
        /*
            if (ZEND_CALL_NUM_ARGS(edata) > 0) {
                char c[4];
                zval *arg;
                const char *nums = "123456789";
                char *result = NULL;
                char delims[] = "#";
                char *to;
                char *val;
                result = strtok( strdup(n->caption), delims );
                while( result != NULL ) {
                    if (strchr(nums, result[0])) {
                        to = strndup(result, 1);
                        arg = ZEND_CALL_ARG(edata, edata->func->op_array.num_args);
                        sprintf(c, "#%d", atoi(to));
                        switch(Z_TYPE_P(arg)) {
                            case IS_DOUBLE: case IS_LONG: case IS_TRUE:
                            case IS_FALSE: case IS_NULL: case IS_STRING:
                                convert_to_string(arg);
                                val = Z_STRVAL_P(arg);
                                break;
                            case IS_RESOURCE:
                                val = "Resource";
                                break;
                            case IS_OBJECT:
                                val = "Object";
                                break;
                            case IS_ARRAY:
                                val = "Array";
                                break;
                            default:
                                val = "";
                        }
                        n->caption = forp_str_replace(
                            c, val,
                            n->caption TSRMLS_CC
                        );
                    }
                    result = strtok( NULL, delims );
                }
            }
        */
#else
            void **params;
            int params_count;
            int i;

            // Retrieves params in zend_vm_stack
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION > 2)
            params = EG(argument_stack)->top-1;
#else
            params = EG(argument_stack).top_element-1;
#endif

            params_count = (ulong) *params;
            for(i = 1; i <= params_count; i++) {
                char c[4];
                char *v, *v_copy;
                zval *expr;
                zval expr_copy;
                int use_copy;

                sprintf(c, "#%d", params_count - i + 1);
                expr = *((zval **) (params - i));

                if(Z_TYPE_P(expr) == IS_OBJECT) {
                    // Object or Closure
                    // Closure throws a Recoverable Fatal in zend_make_printable_zval
                    v_copy = malloc(sizeof(char*) * (strlen(Z_OBJCE_P(expr)->name) + 2));
                    sprintf(v_copy, "(%s)", Z_OBJCE_P(expr)->name);
                    v = strdup(v_copy);
                    free(v_copy);
                } else {
                    // Uses zend_make_printable_zval
                    zend_make_printable_zval(expr, &expr_copy, &use_copy);
                    if(use_copy) {
                        v = strdup((char*)(expr_copy).value.str.val);
                        zval_dtor(&expr_copy);
                    } else {
                        v = strdup((char*)(*expr).value.str.val);
                    }
                }
                n->caption = forp_str_replace(
                    c, v,
                    n->caption TSRMLS_CC
                );
            }
#endif
        }
    } else {
        // Root node
        edata = EG(current_execute_data);
        n->function.class = NULL;
        n->function.function = "{main}";

        // zend_op_array *op_array = &edata->func->op_array;
        if (op_array && op_array->filename) {
            n->function.filename = strdup(ZSTR_VAL(op_array->filename));
            n->filename = strdup(n->function.filename);
        } else {
            n->function.filename = (char*) zend_get_executed_filename(TSRMLS_C);
            n->filename = n->function.filename;
        }
    }

    FORP_G(current_node) = n;
    key = FORP_G(stack_len);
    n->key = key;

    // realloc stack * FORP_STACK_REALLOC
    if (FORP_G(stack_len) % FORP_STACK_REALLOC == 0) {
        FORP_G(stack) = realloc(
            FORP_G(stack),
            ((FORP_G(stack_len)/FORP_STACK_REALLOC)+1) * FORP_STACK_REALLOC * sizeof(forp_node_t)
        );
    }

    FORP_G(stack_len)++;
    FORP_G(stack)[key] = n;

    if(FORP_G(flags) & FORP_FLAG_MEMORY) {
        n->mem_begin = zend_memory_usage(0 TSRMLS_CC);
    }

    if(FORP_G(flags) & FORP_FLAG_TIME) {
        gettimeofday(&tv, NULL);
        n->time_begin = tv.tv_sec * 1000000.0 + tv.tv_usec;
        n->profiler_duration = n->time_begin - start_time;
    }

    return n;
}
/* }}} */

/* {{{ forp_close_node
 */
void forp_close_node(forp_node_t *n TSRMLS_DC) {

    struct timeval tv;

    // closed state
    n->state = 2;

    // dump duration and memory before next steps
    if(FORP_G(flags) & FORP_FLAG_TIME) {
        gettimeofday(&tv, NULL);
        n->time_end = tv.tv_sec * 1000000.0 + tv.tv_usec;
        n->time = n->time_end - n->time_begin;
    }

    if(FORP_G(flags) & FORP_FLAG_MEMORY) {
        n->mem_end = zend_memory_usage(0 TSRMLS_CC);
        n->mem = n->mem_end - n->mem_begin;
    }

    if(n->function.highlight) {
        php_printf(FORP_HIGHLIGHT_END, n->caption != NULL ? n->caption : "", (n->time / 1000), (n->mem / 1024), n->level);
    }

    FORP_G(current_node) = n->parent;
    FORP_G(nesting_level)--;

    // self duration on exit
    if(FORP_G(flags) & FORP_FLAG_TIME) {
        gettimeofday(&tv, NULL);
        n->profiler_duration += (tv.tv_sec * 1000000.0 + tv.tv_usec) - n->time_end ;
    }
}
/* }}} */

/* {{{ forp_start
 */
void forp_start(TSRMLS_D) {
    if(FORP_G(started)) {
        php_error_docref(
            NULL TSRMLS_CC,
            E_NOTICE,
            "forp is already started."
            );
    } else {
        FORP_G(started) = 1;

#if HAVE_SYS_RESOURCE_H
        if(FORP_G(flags) & FORP_FLAG_CPU) {
            struct rusage ru;
            getrusage(RUSAGE_SELF, &ru);
            FORP_G(utime) = ru.ru_utime.tv_sec * 1000000.0 + ru.ru_utime.tv_usec;
            FORP_G(stime) = ru.ru_stime.tv_sec * 1000000.0 + ru.ru_stime.tv_usec;
        }
#endif

        // Proxying zend api methods
#if PHP_VERSION_ID < 50500
        old_execute = zend_execute;
        zend_execute = forp_execute;
#else
        /*init the execute pointer*/
        old_execute_ex = zend_execute_ex;
        zend_execute_ex = forp_execute_ex;
#endif

        if (!FORP_G(no_internals)) {
            old_execute_internal = zend_execute_internal;
            zend_execute_internal = forp_execute_internal;
        }

        FORP_G(main) = forp_open_node(NULL, NULL TSRMLS_CC);
    }
}
/* }}} */

/* {{{ forp_end
 */
void forp_end(TSRMLS_D) {

    if(FORP_G(started)) {

#if HAVE_SYS_RESOURCE_H
        if(FORP_G(flags) & FORP_FLAG_CPU) {
            struct rusage ru;
            getrusage(RUSAGE_SELF, &ru);
            FORP_G(utime) = (ru.ru_utime.tv_sec * 1000000.0 + ru.ru_utime.tv_usec) - FORP_G(utime);
            FORP_G(stime) = (ru.ru_stime.tv_sec * 1000000.0 + ru.ru_stime.tv_usec) - FORP_G(stime);
        }
#endif

        // Closing ancestors
        while(FORP_G(current_node)) {
            forp_close_node(FORP_G(current_node) TSRMLS_CC);
        }

        // Restores Zend API methods
#if PHP_VERSION_ID < 50500
        if (old_execute) {
            zend_execute = old_execute;
            old_execute = 0;
        }
#else
        if (old_execute_ex) {
            zend_execute_ex = old_execute_ex;
            old_execute_ex = 0;
        }
#endif
        if (!FORP_G(no_internals)) {
            zend_execute_internal = old_execute_internal;
        }
        // Stop
        FORP_G(started) = 0;
    }
}
/* }}} */


/* {{{ forp_stack_dump_var
 */
zval forp_stack_dump_var(forp_var_t *var TSRMLS_DC) {

    int i;
    zval zvar, zarr, entry;

    array_init(&zvar);

    if (var->level) add_assoc_string(&zvar, "level", var->level);
    if (var->type) add_assoc_string(&zvar, "type", var->type);
    if (var->is_ref) {
        add_assoc_long(&zvar, "is_ref", var->is_ref);
        if(var->refcount > 1) add_assoc_long(&zvar, "refcount", var->refcount);
    }
    if (var->stack_idx > -1) add_assoc_long(&zvar, "stack_idx", var->stack_idx);
    if (var->class) add_assoc_string(&zvar, "class", var->class);
    if (var->arr_len > 0) {
        add_assoc_long(&zvar, "size", var->arr_len);

        array_init(&zarr);

        i = 0;
        while (i < var->arr_len) {
            entry = forp_stack_dump_var(var->arr[i] TSRMLS_CC);
            add_assoc_zval(&zarr, var->arr[i]->key, &entry);
            i++;
        }
        if(strcmp(var->type, "object") == 0) add_assoc_zval(&zvar, "properties", &zarr);
        else add_assoc_zval(&zvar, "value", &zarr);

    } else {
        if(var->value) add_assoc_string(&zvar, "value", var->value);
    }

    return zvar;
}
/* }}} */

/* {{{ forp_stack_dump
 */
void forp_stack_dump(TSRMLS_D) {
    int i;
    int j = 0;
    zval entry, stack, groups, time, profiler_duration, inspect, var;
    forp_node_t *n;

    array_init(&FORP_G(dump));

    if(FORP_G(flags) & FORP_FLAG_CPU) {
        add_assoc_double(&FORP_G(dump), "utime", FORP_G(utime));
        add_assoc_double(&FORP_G(dump), "stime", FORP_G(stime));
    }

    if(FORP_G(stack_len)) {

        array_init(&stack);
        add_assoc_zval(&FORP_G(dump), "stack", &stack);

        for (i = 0; i < FORP_G(stack_len); ++i) {
            n = FORP_G(stack)[i];

            //if(forp_is_profiling_function(n TSRMLS_CC)) continue;

            // stack entry
            array_init(&entry);

            if (n->filename)
                add_assoc_string(&entry, FORP_DUMP_ASSOC_FILE, n->filename);

            if (n->function.class)
                add_assoc_string(&entry, FORP_DUMP_ASSOC_CLASS, n->function.class);

            if(n->alias) {
                add_assoc_string(&entry, FORP_DUMP_ASSOC_FUNCTION, n->alias);
            } else if (n->function.function) {
                add_assoc_string(&entry, FORP_DUMP_ASSOC_FUNCTION, n->function.function);
            }

            if (n->lineno)
                add_assoc_long(&entry, FORP_DUMP_ASSOC_LINENO, n->lineno);

            if (n->function.groups && n->function.groups_len > 0) {
                array_init(&groups);
                j = 0;
                while(j < n->function.groups_len) {
                    add_next_index_string(&groups, n->function.groups[j]);
                    j++;
                }
                add_assoc_zval(&entry, FORP_DUMP_ASSOC_GROUPS, &groups);
            }

            if (n->caption) {
                add_assoc_string(&entry, FORP_DUMP_ASSOC_CAPTION, n->caption);
            }

            if(FORP_G(flags) & FORP_FLAG_TIME) {

                ZVAL_DOUBLE(&time, round(n->time * 1000000.0) / 1000000.0);
                add_assoc_zval(&entry, FORP_DUMP_ASSOC_DURATION, &time);

                ZVAL_DOUBLE(&profiler_duration, round(n->profiler_duration * 1000000.0) / 1000000.0);
                add_assoc_zval(&entry, FORP_DUMP_ASSOC_PROFILERTIME, &profiler_duration);
            }

            if(FORP_G(flags) & FORP_FLAG_MEMORY) {
                add_assoc_long(&entry, FORP_DUMP_ASSOC_MEMORY, n->mem);
            }

            add_assoc_long(&entry, FORP_DUMP_ASSOC_LEVEL, n->level);

            // {main} don't have parent
            if (n->parent)
                add_assoc_long(&entry, FORP_DUMP_ASSOC_PARENT, n->parent->key);

            zend_hash_next_index_insert(Z_ARRVAL(stack), (zval *) &entry);
        }
    }

    if(FORP_G(inspect_len)) {

        array_init(&inspect);
        add_assoc_zval(&FORP_G(dump), "inspect", &inspect);

        for (i = 0; i < FORP_G(inspect_len); ++i) {
            var = forp_stack_dump_var(FORP_G(inspect)[i] TSRMLS_CC);
            add_assoc_zval(&inspect, FORP_G(inspect)[i]->name, &var);
        }
    }
}
/* }}} */


/* {{{ forp_stack_dump_cli_node
 */
void forp_stack_dump_cli_node(forp_node_t *n TSRMLS_DC) {
    int i;

    if(FORP_G(flags) & FORP_FLAG_TIME) {
        php_printf("[time:\x1B[36m%09.0f\x1B[37m] ", n->time);
    }
    if(FORP_G(flags) & FORP_FLAG_MEMORY) {
        php_printf("[mem:\x1B[36m%09d\x1B[37m] ", (int)n->mem);
    }
    for (i = 0; i < n->level; ++i) {
        if (i == n->level - 1) php_printf("\x1B[37m |--- \x1B[37m");
        else php_printf("\x1B[37m |   \x1B[37m");
    }
    if (n->function.class) php_printf("\x1B[1m%s\x1B[0m\x1B[37m::", n->function.class);
    php_printf("%s", n->function.function);
    if(n->filename) {
        php_printf(" (%s", n->filename);
        if(n->lineno) php_printf(":%d", n->lineno);
        php_printf(")");
    }
    php_printf("%s%s","\x1B[37m", PHP_EOL);
}
/* }}} */

/* {{{ forp_stack_dump_cli_var
 */
void forp_stack_dump_cli_var(forp_var_t *var, int depth TSRMLS_DC) {
    int i, indent;

    indent = depth*4;

    if(var->name) php_printf("%*s%s:\n", indent, "", var->name);
    if(var->key) php_printf("%*s%s:\n", indent, "", var->key);

    php_printf("%*stype: %s\n", indent+2, "", var->type);

    if(var->value) php_printf("%*svalue: %s\n", indent+2, "", var->value);

    if(var->class) php_printf("%*sclass: %s\n", indent+2, "", var->class);

    if(var->arr_len) {
        php_printf("%*ssize: %d\n", indent+2, "", var->arr_len);

        if(var->class) php_printf("%*sproperties:\n", indent+2, "");
        else php_printf("%*svalues:\n", indent+2, "");

        for (i = 0; i < var->arr_len; ++i) {
            forp_stack_dump_cli_var(var->arr[i], depth+1 TSRMLS_CC);
        }
    }
}
/* }}} */

/* {{{ forp_stack_dump_cli
 */
void forp_stack_dump_cli(TSRMLS_D) {
    int i;
    if(FORP_G(inspect_len) > 0) {
        php_printf("\n\x1B[37m-\x1B[36mdebug\x1B[37m--------------------------------------------------------------------------%s", PHP_EOL);
        for (i = 0; i < FORP_G(inspect_len); ++i) {
            forp_stack_dump_cli_var(FORP_G(inspect)[i], 0 TSRMLS_CC);
        }
    }
    php_printf("\n\x1B[37m-\x1B[36mprofile\x1B[37m------------------------------------------------------------------------%s", PHP_EOL);
    for (i = 0; i < FORP_G(stack_len); ++i) {
        //if(forp_is_profiling_function(FORP_G(stack)[i] TSRMLS_CC)) continue;
        forp_stack_dump_cli_node(FORP_G(stack)[i] TSRMLS_CC);
    }
    php_printf("--------------------------------------------------------------------------------%s", PHP_EOL);
    php_printf("forp %s%s", FORP_VERSION, PHP_EOL);
    php_printf("--------------------------------------------------------------------------------\x1B[0m%s", PHP_EOL);
}
/* }}} */

/* {{{ forp_is_profiling_function
 */
int forp_is_profiling_function(forp_node_t *n TSRMLS_DC) {
    if(!n->function.function) return 0;
    return (
        strstr(n->function.function, "forp_inspect")
        || strstr(n->function.function, "forp_start")
        || strstr(n->function.function, "forp_end")
        || strstr(n->function.function, "forp_dump")
        || strstr(n->function.function, "forp_print")
        || strstr(n->function.function, "forp_json")
        || strstr(n->function.function, "forp_json_google_tracer")
    );
}
