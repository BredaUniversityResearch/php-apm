/*
 +----------------------------------------------------------------------+
 |  APM stands for Alternative PHP Monitor                              |
 +----------------------------------------------------------------------+
 | Copyright (c) 2008-2014  Davide Mendolia, Patrick Allaert            |
 +----------------------------------------------------------------------+
 | This source file is subject to version 3.01 of the PHP license,      |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.php.net/license/3_01.txt                                  |
 | If you did not receive a copy of the PHP license and are unable to   |
 | obtain it through the world-wide-web, please send a note to          |
 | license@php.net so we can mail you a copy immediately.               |
 +----------------------------------------------------------------------+
 | Authors: Patrick Allaert <patrickallaert@php.net>                    |
 +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* gettimeofday */
#ifdef PHP_WIN32
# include "win32/time.h"
#else
# include "main/php_config.h"
#endif

#ifdef HAVE_GETRUSAGE
# include <sys/resource.h>
#endif

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#include "php.h"
#include "php_ini.h"
#include "zend_exceptions.h"
#include "php_apm.h"
#include "backtrace.h"
#include "ext/standard/info.h"
#ifdef APM_DRIVER_SQLITE3
# include "driver_sqlite3.h"
#endif
#ifdef APM_DRIVER_MYSQL
# include "driver_mysql.h"
#endif
#ifdef APM_DRIVER_STATSD
# include "driver_statsd.h"
#endif
#ifdef APM_DRIVER_SOCKET
# include "driver_socket.h"
#endif

ZEND_DECLARE_MODULE_GLOBALS(apm);
static PHP_GINIT_FUNCTION(apm);
static PHP_GSHUTDOWN_FUNCTION(apm);

#define APM_DRIVER_BEGIN_LOOP driver_entry = APM_G(drivers); \
		while ((driver_entry = driver_entry->next) != NULL) {

static user_opcode_handler_t _orig_begin_silence_opcode_handler = NULL;
static user_opcode_handler_t _orig_end_silence_opcode_handler = NULL;

#if PHP_VERSION_ID >= 70000
# define ZEND_USER_OPCODE_HANDLER_ARGS zend_execute_data *execute_data
# define ZEND_USER_OPCODE_HANDLER_ARGS_PASSTHRU execute_data
#else
# define ZEND_USER_OPCODE_HANDLER_ARGS ZEND_OPCODE_HANDLER_ARGS
# define ZEND_USER_OPCODE_HANDLER_ARGS_PASSTHRU ZEND_OPCODE_HANDLER_ARGS_PASSTHRU
#endif

static int apm_begin_silence_opcode_handler(ZEND_USER_OPCODE_HANDLER_ARGS)
{
	APM_G(currently_silenced) = 1;

	if (_orig_begin_silence_opcode_handler) {
		_orig_begin_silence_opcode_handler(ZEND_USER_OPCODE_HANDLER_ARGS_PASSTHRU);
	}

	return ZEND_USER_OPCODE_DISPATCH;
}

static int apm_end_silence_opcode_handler(ZEND_USER_OPCODE_HANDLER_ARGS)
{
	APM_G(currently_silenced) = 0;

	if (_orig_end_silence_opcode_handler) {
		_orig_end_silence_opcode_handler(ZEND_USER_OPCODE_HANDLER_ARGS_PASSTHRU);
	}

	return ZEND_USER_OPCODE_DISPATCH;
}

#if PHP_VERSION_ID <  70100
static int apm_write(const char *str,
#if PHP_VERSION_ID >= 70000
size_t
#else
uint
#endif
length)
{
	smart_str_appendl(APM_G(buffer), str, length);
	smart_str_0(APM_G(buffer));
	return length;
}
#endif

// Function prototype for process_event
static void process_event(int event_type, int type, char * error_filename, uint error_lineno, char * msg);

// Update function pointer type for error callback
static void (*old_error_cb)(int, zend_string *, const uint32_t, zend_string *) = NULL;

// Update apm_error_cb signature for PHP 8.1+
void apm_error_cb(int type, zend_string *error_filename, const uint32_t error_lineno, zend_string *message)
{
    if (APM_G(event_enabled)) {
        process_event(APM_EVENT_ERROR, type, (char *) ZSTR_VAL(error_filename), error_lineno, (char *) ZSTR_VAL(message));
    }
    if (old_error_cb) {
        old_error_cb(type, error_filename, error_lineno, message);
    }
}

void apm_throw_exception_hook(zend_object *exception)
{
    zval rv;
    zval *message, *file, *line;
    zend_class_entry *default_ce;

    if (APM_G(event_enabled)) {
        if (!exception) {
            return;
        }
        default_ce = exception->ce;
        message = zend_read_property(default_ce, exception, "message", sizeof("message")-1, 0, &rv);
        file = zend_read_property(default_ce, exception, "file", sizeof("file")-1, 0, &rv);
        line = zend_read_property(default_ce, exception, "line", sizeof("line")-1, 0, &rv);
        process_event(APM_EVENT_EXCEPTION, E_EXCEPTION, Z_STRVAL_P(file), Z_LVAL_P(line), Z_STRVAL_P(message));
    }
}

static void process_event(int event_type, int type, char * error_filename, uint error_lineno, char * msg)
{
	smart_str trace_str = {0};
	apm_driver_entry * driver_entry;

	if (APM_G(store_stacktrace)) {
		append_backtrace(&trace_str);
		smart_str_0(&trace_str);
	}

	driver_entry = APM_G(drivers);
	APM_DEBUG("Direct processing process_event loop begin\n");
	while ((driver_entry = driver_entry->next) != NULL) {
		if (driver_entry->driver.want_event(event_type, type, msg)) {
			driver_entry->driver.process_event(
				type,
				error_filename,
				error_lineno,
				msg,
#if PHP_VERSION_ID >= 70000
				(APM_G(store_stacktrace) && trace_str.s && trace_str.s->val) ? trace_str.s->val : ""
#else
				(APM_G(store_stacktrace) && trace_str.c) ? trace_str.c : ""
#endif
			);
		}
	}
	APM_DEBUG("Direct processing process_event loop end\n");

	smart_str_free(&trace_str);
}

#if PHP_VERSION_ID >= 70000
#define REGISTER_INFO(name, dest, type) \
	if ((APM_RD(dest) = zend_hash_str_find(Z_ARRVAL_P(tmp), name, sizeof(name) - 1)) && (Z_TYPE_P(APM_RD(dest)) == (type))) { \
		APM_RD(dest##_found) = 1; \
	}
#else
#define REGISTER_INFO(name, dest, type) \
	if ((zend_hash_find(Z_ARRVAL_P(tmp), name, sizeof(name), (void**)&APM_RD(dest)) == SUCCESS) && (Z_TYPE_PP(APM_RD(dest)) == (type))) { \
		APM_RD(dest##_found) = 1; \
	}
#endif

#if PHP_VERSION_ID >= 70000
#define FETCH_HTTP_GLOBALS(name) (tmp = &PG(http_globals)[TRACK_VARS_##name])
#else
#define FETCH_HTTP_GLOBALS(name) (tmp = PG(http_globals)[TRACK_VARS_##name])
#endif

void extract_data()
{
	zval *tmp;

	APM_DEBUG("Extracting data\n");
	
	if (APM_RD(initialized)) {
		APM_DEBUG("Data already initialized\n");
		return;
	}

	APM_RD(initialized) = 1;
	
	zend_is_auto_global_compat("_SERVER");
	if (FETCH_HTTP_GLOBALS(SERVER)) {
		REGISTER_INFO("REQUEST_URI", uri, IS_STRING);
		REGISTER_INFO("HTTP_HOST", host, IS_STRING);
		REGISTER_INFO("HTTP_REFERER", referer, IS_STRING);
		REGISTER_INFO("REQUEST_TIME", ts, IS_LONG);
		REGISTER_INFO("SCRIPT_FILENAME", script, IS_STRING);
		REGISTER_INFO("REQUEST_METHOD", method, IS_STRING);
		
		if (APM_G(store_ip)) {
			REGISTER_INFO("REMOTE_ADDR", ip, IS_STRING);
		}
	}
	if (APM_G(store_cookies)) {
		zend_is_auto_global_compat("_COOKIE");
		if (FETCH_HTTP_GLOBALS(COOKIE)) {
			if (Z_ARRVAL_P(tmp)->nNumOfElements > 0) {
#if PHP_VERSION_ID >= 70100
				zend_string *tmpstr;
				tmpstr = zend_print_zval_r_to_str(tmp, 0);
				smart_str_append(&APM_RD(cookies), tmpstr);
				zend_string_release(tmpstr);
#else
				APM_G(buffer) = &APM_RD(cookies);
				zend_print_zval_r_ex(apm_write, tmp, 0);
#endif
				APM_RD(cookies_found) = 1;
			}
		}
	}
	if (APM_G(store_post)) {
		zend_is_auto_global_compat("_POST");
		if (FETCH_HTTP_GLOBALS(POST)) {
			if (Z_ARRVAL_P(tmp)->nNumOfElements > 0) {
#if PHP_VERSION_ID >= 70100
				zend_string *tmpstr;
				tmpstr = zend_print_zval_r_to_str(tmp, 0);
				smart_str_append(&APM_RD(post_vars), tmpstr);
				zend_string_release(tmpstr);
#else
				APM_G(buffer) = &APM_RD(post_vars);
				zend_print_zval_r_ex(apm_write, tmp, 0);
#endif
				APM_RD(post_vars_found) = 1;
			}
		}
	}
}
