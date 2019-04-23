/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Martin Schröder <m.schroeder2007@gmail.com>                 |
  +----------------------------------------------------------------------+
*/

#include "php_async.h"

#include "SAPI.h"
#include "php_main.h"

#include "thread/copy.h"

zend_string* php_parallel_runtime_main;

ASYNC_API zend_class_entry *async_thread_pool_ce;

static zend_object_handlers async_thread_pool_handlers;

#define ASYNC_THREAD_POOL_JOB_FLAG_CANCELLED 1

typedef struct _async_thread_job async_thread_job;

struct _async_thread_job {
	uint16_t flags;

	uv_async_t *handle;
	zend_execute_data *exec;
	zval *retval;
	
	async_thread_job *prev;
	async_thread_job *next;
};

typedef struct {
	async_deferred_custom_awaitable base;
	uv_async_t handle;
	async_thread_job *job;
	zend_bool failed;
	zval result;
} async_thread_awaitable;

#define ASYNC_THREAD_POOL_FLAG_CLOSED 1

typedef struct {
	uint16_t flags;

	uv_mutex_t mutex;
	uv_cond_t cond;
	
	struct {
		async_thread_job *first;
		async_thread_job *last;
	} jobs;
	
	async_thread_job *first;
	async_thread_job *last;
} async_thread_data;

typedef struct _async_thread async_thread;

struct _async_thread {
	uv_thread_t handle;
	async_thread_data *data;
	async_thread *prev;
	async_thread *next;
};

typedef struct {
	zend_object std;
	
	uv_thread_t worker;
	async_thread_data *data;
} async_thread_pool;


static void run_job(async_thread_data *data, async_thread_job *job, zend_execute_data *frame)
{
	zval retval;
	
	ZVAL_UNDEF(&retval);
	
	zend_init_func_execute_data(frame, &frame->func->op_array, &retval);
	
	zend_first_try {
		zend_try {
			zend_execute_ex(frame);
			
			if (UNEXPECTED(EG(exception))) {
			} else {
				if (Z_REFCOUNTED(retval)) {
					php_parallel_copy_zval(job->retval, &retval, 1);
				} else {
					ZVAL_COPY(job->retval, &retval);
				}
				
				zval_ptr_dtor(&retval);
			}
		} zend_catch {
			
		} zend_end_try();
		
		if (job->handle == NULL) {
			pefree(job, 1);
		} else {
			uv_async_send(job->handle);
		}
	
		php_parallel_copy_free(frame->func, 0);
		zend_vm_stack_free_call_frame(frame);
	} zend_end_try();
}

ASYNC_CALLBACK run_thread(void *arg)
{
	async_thread_data *data;
	async_thread_job *job;
	
	zend_execute_data *frame;
	
	int argc;
	
	data = (async_thread_data *) arg;
	
	ts_resource(0);

	TSRMLS_CACHE_UPDATE();
	
	PG(expose_php) = 0;
	PG(auto_globals_jit) = 1;
	
	php_request_startup();
	
	PG(during_request_startup) = 0;
	SG(sapi_started) = 0;
	SG(headers_sent) = 1;
	SG(request_info).no_headers = 1;
	
	do {
		uv_mutex_lock(&data->mutex);
		
		while (data->first == NULL) {
			if (data->flags & ASYNC_THREAD_POOL_FLAG_CLOSED) {
				uv_mutex_unlock(&data->mutex);
				
				goto cleanup;
			}
			
			uv_cond_wait(&data->cond, &data->mutex);
		}
		
		ASYNC_LIST_EXTRACT_FIRST(data, job);
		
		uv_mutex_unlock(&data->mutex);
		
		argc = ZEND_CALL_NUM_ARGS(job->exec);
		frame = zend_vm_stack_push_call_frame(ZEND_CALL_TOP_FUNCTION, php_parallel_copy(job->exec->func, 0), argc, NULL, NULL);
		
		run_job(data, job, frame);
	} while (1);
	
cleanup:

	php_request_shutdown(NULL);

	ts_free_thread();
}

static zend_object *async_thread_pool_object_create(zend_class_entry *ce)
{
	async_thread_pool *pool;
	
	pool = ecalloc(1, sizeof(async_thread_pool));
	
	zend_object_std_init(&pool->std, ce);
	pool->std.handlers = &async_thread_pool_handlers;
	
	pool->data = pecalloc(1, sizeof(async_thread_data), 1);
	
	uv_mutex_init_recursive(&pool->data->mutex);
	uv_cond_init(&pool->data->cond);
	
	uv_thread_create(&pool->worker, run_thread, pool->data);
	
	return &pool->std;
}

static void async_thread_pool_object_destroy(zend_object *object)
{
	async_thread_pool *pool;
	
	pool = (async_thread_pool *) object;
	
	ASYNC_DEBUG_LOG("=> JOIN\n");
	uv_thread_join(&pool->worker);
	ASYNC_DEBUG_LOG("DONE!\n");
	
	uv_cond_destroy(&pool->data->cond);
	uv_mutex_destroy(&pool->data->mutex);
	
	pefree(pool->data, 1);
	
	zend_object_std_dtor(&pool->std);
}

static ZEND_METHOD(ThreadPool, __construct)
{
}

static ZEND_METHOD(ThreadPool, close)
{
	async_thread_pool *pool;
	
	ZEND_PARSE_PARAMETERS_NONE();
	
	pool = (async_thread_pool *) Z_OBJ_P(getThis());
	
	uv_mutex_lock(&pool->data->mutex);
	
	pool->data->flags |= ASYNC_THREAD_POOL_FLAG_CLOSED;
	
	uv_cond_signal(&pool->data->cond);
	uv_mutex_unlock(&pool->data->mutex);
}

ASYNC_CALLBACK job_close_cb(uv_handle_t *handle)
{
	async_thread_awaitable *awaitable;
	
	awaitable = (async_thread_awaitable *) handle->data;
	
	zval_ptr_dtor(&awaitable->result);
	
	pefree(awaitable->job, 1);
	
	ASYNC_DELREF(&((async_deferred_awaitable *) awaitable)->std);
}

ASYNC_CALLBACK job_done_cb(uv_async_t *handle)
{
	async_thread_awaitable *awaitable;
	
	awaitable = (async_thread_awaitable *) handle->data;
	
	ASYNC_DEBUG_LOG("=> JOB COMPLETED!\n");
	
	async_resolve_awaitable((async_deferred_awaitable *) awaitable, &awaitable->result);
	
	if (!(uv_is_closing((uv_handle_t *) handle))) {
		uv_close((uv_handle_t *) handle, job_close_cb);
	}
}

ASYNC_CALLBACK job_dispose_cb(async_deferred_awaitable *obj)
{
	async_thread_awaitable *awaitable;
	
	awaitable = (async_thread_awaitable *) obj;
	
	awaitable->job->flags |= ASYNC_THREAD_POOL_JOB_FLAG_CANCELLED;
	
	ASYNC_DEBUG_LOG("=> JOB CANCELLED!\n");
}

static ZEND_METHOD(ThreadPool, submit)
{
	async_thread_pool *pool;
	async_thread_job *job;
	async_thread_awaitable *awaitable;
	async_context *context;
	
	zend_execute_data *frame;
	zend_function *func;
	
	zval *closure;
	
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_OBJECT_OF_CLASS(closure, zend_ce_closure)
	ZEND_PARSE_PARAMETERS_END();
	
	pool = (async_thread_pool *) Z_OBJ_P(getThis());
	func = (zend_function *) zend_get_closure_method_def(closure);
	
	frame = pecalloc(1, zend_vm_calc_used_stack(0, func), 1);
	frame->func = php_parallel_copy(func, 1);
	
	frame->return_value = NULL;	
	ZEND_CALL_NUM_ARGS(frame) = 0;
	
	awaitable = ecalloc(1, sizeof(async_thread_awaitable));
	context = async_context_get();
	
	async_init_awaitable((async_deferred_custom_awaitable *) awaitable, job_dispose_cb, context);
	
	uv_async_init(&awaitable->base.base.state->scheduler->loop, &awaitable->handle, job_done_cb);
	
	awaitable->handle.data = awaitable;
	
	job = pecalloc(1, sizeof(async_thread_job), 1);
	job->handle = &awaitable->handle;
	job->exec = frame;
	job->retval = &awaitable->result;
	
	awaitable->job = job;
	
	uv_mutex_lock(&pool->data->mutex);
	
	ASYNC_LIST_APPEND(pool->data, job);
	
	uv_cond_signal(&pool->data->cond);
	uv_mutex_unlock(&pool->data->mutex);
	
	ASYNC_ADDREF(&awaitable->base.base.std);
	
	RETURN_OBJ(&awaitable->base.base.std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_thread_pool_ctor, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, size, IS_LONG, 1)
	ZEND_ARG_TYPE_INFO(0, bootstrap, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_thread_pool_close, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_thread_pool_submit, 0, 1, Concurrent\\Awaitable, 0)
	ZEND_ARG_OBJ_INFO(0, work, Closure, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry thread_pool_funcs[] = {
	ZEND_ME(ThreadPool, __construct, arginfo_thread_pool_ctor, ZEND_ACC_PUBLIC)
	ZEND_ME(ThreadPool, close, arginfo_thread_pool_close, ZEND_ACC_PUBLIC)
	ZEND_ME(ThreadPool, submit, arginfo_thread_pool_submit, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};


void async_thread_ce_register()
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Concurrent\\ThreadPool", thread_pool_funcs);
	async_thread_pool_ce = zend_register_internal_class(&ce);
	async_thread_pool_ce->ce_flags |= ZEND_ACC_FINAL;
	async_thread_pool_ce->create_object = async_thread_pool_object_create;
	async_thread_pool_ce->serialize = zend_class_serialize_deny;
	async_thread_pool_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&async_thread_pool_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_thread_pool_handlers.free_obj = async_thread_pool_object_destroy;
	async_thread_pool_handlers.dtor_obj = NULL;
	async_thread_pool_handlers.clone_obj = NULL;
	
	php_parallel_runtime_main = zend_string_init("main", sizeof("main")-1 , 1);
}
