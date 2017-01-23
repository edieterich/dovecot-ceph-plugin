/* Copyright (c) 2015-2017 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "test-common.h"
#include "ioloop.h"
#include "lib-signals.h"
#include "master-service.h"
#include "settings-parser.h"
#include "safe-mkstemp.h"
#include "safe-mkdir.h"
#include "str.h"
#include "unlink-directory.h"
#include "randgen.h"
#include "dcrypt.h"
#include "hex-binary.h"
#include "dict.h"
#include "dict-private.h"

#include "librados-plugin.h"

static struct ioloop *test_ioloop = NULL;
static pool_t test_pool;

static char * uri = "oid=metadata:pool=librmb-index:config=/home/peter/dovecot/etc/ceph/ceph.conf";

extern struct dict dict_driver_rados;

struct dict *test_dict_r = NULL;

static void dict_transaction_commit_sync_callback(const struct dict_commit_result *result, void *context) {
	struct dict_commit_result *sync_result = context;

	sync_result->ret = result->ret;
	sync_result->error = i_strdup(result->error);
}

static void test_setup(void) {
	test_pool = pool_alloconly_create(MEMPOOL_GROWING "mcp test pool", 128);
	test_ioloop = io_loop_create();
	rados_plugin_init(NULL);
}

static void test_dict_init(void) {
	const char *error_r;

	struct dict_settings *set = i_new(struct dict_settings, 1);
	set->username = "t";

	int err = dict_driver_rados.v.init(&dict_driver_rados, uri, set, &test_dict_r, &error_r);
	test_assert(err == 0);

}

static void test_dict_set_get_delete(void) {
	struct dict_transaction_context * ctx;

	ctx = dict_driver_rados.v.transaction_init(test_dict_r);
	test_dict_r->v.set(ctx, "key", "Artemis");

	struct dict_commit_result result;

	i_zero(&result);
	ctx->dict->v.transaction_commit(ctx, FALSE, dict_transaction_commit_sync_callback, &result);
	test_assert(result.ret == DICT_COMMIT_RET_OK);
	i_free(result.error);

	const char *value_r;
	const char *error_r;
	int err = dict_driver_rados.v.lookup(test_dict_r, test_pool, "key", &value_r, &error_r);
	i_debug("error=%s", error_r);
	i_debug("value=%s", value_r);

	test_assert(err == 0);

	ctx = dict_driver_rados.v.transaction_init(test_dict_r);
	test_dict_r->v.unset(ctx, "key");
	i_zero(&result);
	ctx->dict->v.transaction_commit(ctx, FALSE, dict_transaction_commit_sync_callback, &result);
	test_assert(result.ret == DICT_COMMIT_RET_OK);
	i_free(result.error);

}

static void test_dict_deinit(void) {
	dict_driver_rados.v.deinit(test_dict_r);
}

static void test_teardown(void) {
	rados_plugin_deinit();
	io_loop_destroy(&test_ioloop);
	pool_unref(&test_pool);
}

int main(int argc, char **argv) {
	void (*tests[])(void) = {
		test_setup,
		test_dict_init,
		test_dict_set_get_delete,
		test_dict_deinit,
		test_teardown,
		NULL
	};

	master_service = master_service_init("test-rados",
			MASTER_SERVICE_FLAG_STANDALONE | MASTER_SERVICE_FLAG_NO_CONFIG_SETTINGS | MASTER_SERVICE_FLAG_NO_SSL_INIT
					| MASTER_SERVICE_FLAG_NO_INIT_DATASTACK_FRAME, &argc, &argv, "");
	random_init();
	int ret = test_run(tests);
	master_service_deinit(&master_service);
	return ret;
}
