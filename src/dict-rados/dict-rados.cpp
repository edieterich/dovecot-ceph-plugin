/* Copyright (c) 2017 Tallence AG and the authors, see the included COPYING file */

#include <limits.h>

#include <iostream>
#include <sstream>

#include <string>

#include <iterator>
#include <map>
#include <set>
#include <vector>
#include <list>
#include <algorithm>

#include <utility>
#include <cstdint>
#include <mutex>  // NOLINT

#include <rados/librados.hpp>

extern "C" {

#include "lib.h"
#include "array.h"
#include "fs-api.h"
#include "istream.h"
#include "str.h"
#include "dict-transaction-memory.h"
#include "dict-private.h"
#include "ostream.h"
#include "connection.h"
#include "module-dir.h"
#include "guid.h"

#include "dict-rados.h"
}

#include "rados-cluster.h"
#include "rados-dictionary.h"

#define FUNC_START() i_debug("[START] %s: %s at line %d", __FILE__, __func__, __LINE__)
#define FUNC_END() i_debug("[END] %s: %s at line %d\n", __FILE__, __func__, __LINE__)
#define FUNC_END_RET(ret) i_debug("[END] %s: %s at line %d, %s\n", __FILE__, __func__, __LINE__, ret)
#define FUNC_END_RET_INT(ret) i_debug("[END] %s: %s at line %d, ret==%d\n", __FILE__, __func__, __LINE__, ret)

using namespace librados;  // NOLINT

using std::string;
using std::stringstream;
using std::vector;
using std::list;
using std::map;
using std::pair;
using std::set;

#define DICT_USERNAME_SEPARATOR '/'
static const char CACHE_DELETED[] = "_DELETED_";
using namespace librmb;  // NOLINT

struct rados_dict {
  struct dict dict;
  RadosCluster cluster;
  RadosDictionary *d;
};

static const vector<string> explode(const string &str, const char &sep) {
  vector<string> v;
  stringstream ss(str);  // Turn the string into a stream.
  string tok;

  while (getline(ss, tok, sep)) {
    v.push_back(tok);
  }

  return v;
}

int rados_dict_init(struct dict *driver, const char *uri, const struct dict_settings *set, struct dict **dict_r,
                    const char **error_r) {
  FUNC_START();
  struct rados_dict *dict;
  const char *const *args;
  string oid = "";
  string pool = "mail_dictionaries";

  if (uri != nullptr) {
    i_debug("rados_dict_init(uri=%s)", uri);

    vector<string> props(explode(uri, ':'));

    for (vector<string>::iterator it = props.begin(); it != props.end(); ++it) {
      if (it->compare(0, 4, "oid=") == 0) {
        oid = it->substr(4);
      } else if (it->compare(0, 5, "pool=") == 0) {
        pool = it->substr(5);
      } else {
        *error_r = t_strdup_printf("Invalid URI!");
        FUNC_END_RET("ret == -1");
        return -1;
      }
    }
  }

  string username(set->username);
  if (username.find(DICT_USERNAME_SEPARATOR) != string::npos) {
    /* escape user name */
    username = dict_escape_string(username.c_str());
  }

  dict = i_new(struct rados_dict, 1);

  string error_msg;
  int ret = dict->cluster.init(&error_msg);

  if (ret < 0) {
    i_free(dict);
    *error_r = t_strdup_printf("%s", error_msg.c_str());
    FUNC_END_RET("ret == -1");
    return -1;
  }

  ret = dict->cluster.dictionary_create(pool, username, oid, &dict->d);

  if (ret < 0) {
    *error_r = t_strdup_printf("Error creating RadosDictionary()! %s", strerror(-ret));
    dict->cluster.deinit();
    FUNC_END_RET("ret == -1");
    return -1;
  }

  dict->dict = *driver;
  *dict_r = &dict->dict;

  FUNC_END();
  return 0;
}

void rados_dict_deinit(struct dict *_dict) {
  FUNC_START();
  struct rados_dict *dict = (struct rados_dict *)_dict;
  RadosDictionary *d = ((struct rados_dict *)_dict)->d;

  // wait for open operations
  rados_dict_wait(_dict);

  dict->cluster.deinit();
  delete dict->d;
  dict->d = nullptr;

  i_free(_dict);
  FUNC_END();
}

static void rados_lookup_complete_callback(rados_completion_t comp, void *arg);

int rados_dict_wait(struct dict *_dict) {
  FUNC_START();
  struct rados_dict *dict = (struct rados_dict *)_dict;

  dict->d->wait_for_completions();

  FUNC_END_RET_INT(0);
  return 0;
}

class rados_dict_lookup_context {
 public:
  RadosDictionary *dict;
  ObjectReadOperation read_op;
  map<string, bufferlist> result_map;
  int r_val = -1;
  bufferlist bl;

  AioCompletion *completion;
  string key;
  string value;
  void *context = nullptr;
  dict_lookup_callback_t *callback;

  explicit rados_dict_lookup_context(RadosDictionary *dict) : callback(nullptr) {
    FUNC_START();
    this->dict = dict;
    completion = librados::Rados::aio_create_completion(this, rados_lookup_complete_callback, nullptr);
    FUNC_END();
  }

  ~rados_dict_lookup_context() {}
};

static void rados_lookup_complete_callback(rados_completion_t comp, void *arg) {
  FUNC_START();
  rados_dict_lookup_context *lc = reinterpret_cast<rados_dict_lookup_context *>(arg);

  struct dict_lookup_result result;
  i_zero(&result);
  result.ret = DICT_COMMIT_RET_NOTFOUND;

  const char *values[2];
  int ret = lc->completion->get_return_value();

  if (lc->callback != nullptr) {
    if (ret == 0) {
      auto it = lc->result_map.find(lc->key);
      if (it != lc->result_map.end()) {
        lc->value = it->second.to_str();
        i_debug("rados_dict_lookup_complete_callback('%s')='%s'", it->first.c_str(), lc->value.c_str());
        result.value = lc->value.c_str();
        result.values = values;
        values[0] = lc->value.c_str();
        values[1] = nullptr;
        result.ret = DICT_COMMIT_RET_OK;
      } else {
        result.ret = DICT_COMMIT_RET_NOTFOUND;
      }
    } else {
      if (ret == -ENOENT) {
        result.ret = DICT_COMMIT_RET_NOTFOUND;
      } else {
        result.ret = DICT_COMMIT_RET_FAILED;
      }
    }

    i_debug("rados_dict_lookup_complete_callback(%s) call callback result=%d", lc->key.c_str(), result.ret);
    lc->callback(&result, lc->context);
  }

  delete lc;
  FUNC_END();
}

void rados_dict_lookup_async(struct dict *_dict, const char *key, dict_lookup_callback_t *callback, void *context) {
  FUNC_START();
  RadosDictionary *d = ((struct rados_dict *)_dict)->d;
  set<string> keys;
  keys.insert(key);
  auto lc = new rados_dict_lookup_context(d);

  i_debug("rados_dict_lookup_async(%s)", key);

  lc->key = key;
  lc->context = context;
  lc->callback = callback;
  lc->read_op.omap_get_vals_by_keys(keys, &lc->result_map, &lc->r_val);

  int err = d->get_io_ctx().aio_operate(d->get_full_oid(key), lc->completion, &lc->read_op, LIBRADOS_OPERATION_NOFLAG,
                                        &lc->bl);

  if (err < 0) {
    if (lc->callback != nullptr) {
      struct dict_lookup_result result;
      i_zero(&result);
      result.ret = DICT_COMMIT_RET_FAILED;
      lc->callback(&result, context);
    }
    lc->completion->release();
    delete lc;
  } else {
    d->push_back_completion(lc->completion);
  }

  FUNC_END();
}

int rados_dict_lookup(struct dict *_dict, pool_t pool, const char *key, const char **value_r) {
  FUNC_START();
  struct rados_dict *dict = (struct rados_dict *)_dict;
  RadosDictionary *d = dict->d;
  set<string> keys;
  keys.insert(key);
  map<string, bufferlist> result_map;
  *value_r = nullptr;

  int err = d->get_io_ctx().omap_get_vals_by_keys(d->get_full_oid(key), keys, &result_map);
  i_debug("rados_dict_lookup(%s), oid=%s, err=%d", key, d->get_full_oid(key).c_str(), err);

  if (err == 0) {
    auto value = result_map.find(key);
    if (value != result_map.end()) {
      *value_r = p_strdup(pool, value->second.to_str().c_str());
      i_debug("rados_dict_lookup(%s), err=%d, value_r=%s", key, err, *value_r);
      FUNC_END_RET_INT(DICT_COMMIT_RET_OK);
      return DICT_COMMIT_RET_OK;
    }
  } else if (err < 0 && err != -ENOENT) {
    i_error("rados_dict_lookup(%s), err=%d (%s)", key, err, strerror(-err));
    FUNC_END_RET_INT(DICT_COMMIT_RET_FAILED);
    return DICT_COMMIT_RET_FAILED;
  }

  i_debug("rados_dict_lookup(%s), NOT FOUND, err=%d (%s)", key, err, strerror(-err));
  FUNC_END_RET_INT(DICT_COMMIT_RET_NOTFOUND);
  return DICT_COMMIT_RET_NOTFOUND;
}

static void rados_dict_transaction_private_complete_callback(completion_t comp, void *arg);
static void rados_dict_transaction_shared_complete_callback(completion_t comp, void *arg);

#define ENORESULT 1000

class rados_dict_transaction_context {
 public:
  struct dict_transaction_context ctx;
  bool atomic_inc_not_found;

  guid_128_t guid;

  void *context = nullptr;
  dict_transaction_commit_callback_t *callback;

  std::map<std::string, string> cache;

  ObjectWriteOperation write_op_private;
  AioCompletion *completion_private;
  bool dirty_private;
  bool locked_private;
  int result_private;

  ObjectWriteOperation write_op_shared;
  AioCompletion *completion_shared;
  bool dirty_shared;
  bool locked_shared;
  int result_shared;

  explicit rados_dict_transaction_context(struct dict *_dict) {
    FUNC_START();
    dirty_private = false;
    dirty_shared = false;
    locked_private = false;
    locked_shared = false;
    result_private = -ENORESULT;
    result_shared = -ENORESULT;

    callback = nullptr;
    atomic_inc_not_found = false;

    ctx.dict = _dict;
    ctx.changed = 0;

    ctx.timestamp.tv_sec = 0;
    ctx.timestamp.tv_nsec = 0;

    completion_private = nullptr;
    completion_shared = nullptr;

    guid_128_generate(guid);
    FUNC_END();
  }
  ~rados_dict_transaction_context() {
    FUNC_START();
    FUNC_END();
  }

  ObjectWriteOperation &get_op(const std::string &key) {
    if (!key.compare(0, strlen(DICT_PATH_PRIVATE), DICT_PATH_PRIVATE)) {
      dirty_private = true;
      return write_op_private;
    } else if (!key.compare(0, strlen(DICT_PATH_SHARED), DICT_PATH_SHARED)) {
      dirty_shared = true;
      return write_op_shared;
    }
    i_unreached();
  }

  void set_locked(const std::string &key) {
    if (!key.compare(0, strlen(DICT_PATH_SHARED), DICT_PATH_SHARED)) {
      locked_shared = true;
    } else if (!key.compare(0, strlen(DICT_PATH_PRIVATE), DICT_PATH_PRIVATE)) {
      locked_private = true;
    }
  }

  bool is_locked(const std::string &key) {
    if (!key.compare(0, strlen(DICT_PATH_SHARED), DICT_PATH_SHARED)) {
      return locked_shared;
    } else if (!key.compare(0, strlen(DICT_PATH_PRIVATE), DICT_PATH_PRIVATE)) {
      return locked_private;
    }
    i_unreached();
  }
  int get_result(int result) {
    return result < 0 && result != -ENORESULT ? DICT_COMMIT_RET_FAILED : DICT_COMMIT_RET_OK;
  }
};

static std::mutex transaction_lock;

struct dict_transaction_context *rados_dict_transaction_init(struct dict *_dict) {
  FUNC_START();

  struct rados_dict_transaction_context *ctx = new rados_dict_transaction_context(_dict);

  FUNC_END();
  return &ctx->ctx;
}

void rados_dict_set_timestamp(struct dict_transaction_context *_ctx, const struct timespec *ts) {
  FUNC_START();
  struct rados_dict_transaction_context *ctx = (struct rados_dict_transaction_context *)_ctx;
  RadosDictionary *d = ((struct rados_dict *)_ctx->dict)->d;

  struct timespec t = {ts->tv_sec, ts->tv_nsec};

  if (ts != NULL) {
    _ctx->timestamp.tv_sec = t.tv_sec;
    _ctx->timestamp.tv_nsec = t.tv_nsec;
    ctx->write_op_private.mtime2(&t);
    ctx->write_op_shared.mtime2(&t);
  }
  FUNC_END();
}

static void rados_dict_transaction_private_complete_callback(completion_t comp, void *arg) {
  FUNC_START();
  rados_dict_transaction_context *ctx = reinterpret_cast<rados_dict_transaction_context *>(arg);
  RadosDictionary *d = ((struct rados_dict *)ctx->ctx.dict)->d;
  bool finished = true;

  std::lock_guard<std::mutex> lock(transaction_lock);

  i_debug("rados_dict_transaction_private_complete_callback() result=%d (%s)", ctx->result_private,
          strerror(-ctx->result_private));
  if (ctx->dirty_shared) {
    finished = ctx->result_private != -ENORESULT;
  }

  ctx->result_private = ctx->completion_private->get_return_value();

  if (ctx->locked_private) {
    int err = d->get_io_ctx().unlock(d->get_private_oid(), "ATOMIC_INC", guid_128_to_string(ctx->guid));
    i_debug("rados_dict_transaction_private_complete_callback(): unlock(%s) ret=%d (%s)", d->get_private_oid().c_str(),
            err, strerror(-err));
  }

  if (finished) {
    i_debug("rados_dict_transaction_private_complete_callback() finished...");
    if (ctx->callback != nullptr) {
      i_debug("rados_dict_transaction_private_complete_callback() call callback func...");
      ctx->callback(ctx->atomic_inc_not_found
                        ? DICT_COMMIT_RET_NOTFOUND
                        : (ctx->get_result(ctx->result_private) < 0 || ctx->get_result(ctx->result_shared) < 0
                               ? DICT_COMMIT_RET_FAILED
                               : DICT_COMMIT_RET_OK),
                    ctx->context);
    }
    delete ctx;
  }
  FUNC_END();
}

static void rados_dict_transaction_shared_complete_callback(completion_t comp, void *arg) {
  FUNC_START();
  rados_dict_transaction_context *ctx = reinterpret_cast<rados_dict_transaction_context *>(arg);
  RadosDictionary *d = ((struct rados_dict *)ctx->ctx.dict)->d;
  bool finished = true;

  std::lock_guard<std::mutex> lock(transaction_lock);

  if (ctx->dirty_private) {
    finished = ctx->result_private != -ENORESULT;
  }

  ctx->result_shared = ctx->completion_shared->get_return_value();

  if (ctx->locked_shared) {
    int err = d->get_io_ctx().unlock(d->get_shared_oid(), "ATOMIC_INC", guid_128_to_string(ctx->guid));
    i_debug("rados_dict_transaction_shared_complete_callback(): unlock(%s) ret=%d (%s)", d->get_shared_oid().c_str(),
            err, strerror(-err));
  }

  if (finished) {
    i_debug("rados_dict_transaction_shared_complete_callback() finished...");
    if (ctx->callback != nullptr) {
      i_debug("rados_dict_transaction_shared_complete_callback() call callback func...");
      ctx->callback(ctx->atomic_inc_not_found
                        ? DICT_COMMIT_RET_NOTFOUND
                        : (ctx->get_result(ctx->result_private) < 0 || ctx->get_result(ctx->result_shared) < 0
                               ? DICT_COMMIT_RET_FAILED
                               : DICT_COMMIT_RET_OK),
                    ctx->context);
    }
    delete ctx;
  }
  FUNC_END();
}

int rados_dict_transaction_commit(struct dict_transaction_context *_ctx, bool async,
                                  dict_transaction_commit_callback_t *callback, void *context) {
  FUNC_START();
  rados_dict_transaction_context *ctx = reinterpret_cast<rados_dict_transaction_context *>(_ctx);
  struct rados_dict *dict = (struct rados_dict *)ctx->ctx.dict;
  RadosDictionary *d = dict->d;

  i_debug("rados_dict_transaction_commit(): async=%d", async);

  bool failed = false;
  int ret = DICT_COMMIT_RET_OK;

  if (_ctx->changed) {
    ctx->context = context;
    ctx->callback = callback;

    if (ctx->dirty_private) {
      if (async) {
        ctx->completion_private =
            librados::Rados::aio_create_completion(ctx, rados_dict_transaction_private_complete_callback, nullptr);
      } else {
        ctx->completion_private = librados::Rados::aio_create_completion(ctx, nullptr, nullptr);
      }
      int ret = d->get_io_ctx().aio_operate(d->get_private_oid(), ctx->completion_private, &ctx->write_op_private);
      i_debug("rados_dict_transaction_commit(): aio_operate(%s) ret=%d (%s)", d->get_private_oid().c_str(), ret,
              strerror(-ret));
      failed = ret < 0;

      if (!failed && async) {
        d->push_back_completion(ctx->completion_private);
      }
    }

    if (ctx->dirty_shared) {
      if (async) {
        ctx->completion_shared =
            librados::Rados::aio_create_completion(ctx, rados_dict_transaction_shared_complete_callback, nullptr);
      } else {
        ctx->completion_shared = librados::Rados::aio_create_completion();
      }
      int ret = d->get_io_ctx().aio_operate(d->get_shared_oid(), ctx->completion_shared, &ctx->write_op_shared);
      i_debug("rados_dict_transaction_commit(): aio_operate(%s) ret=%d (%s)", d->get_shared_oid().c_str(), ret,
              strerror(-ret));
      failed |= ret < 0;

      if (!failed && async) {
        d->push_back_completion(ctx->completion_shared);
      }
    }

    if (!failed) {
      if (!async) {
        if (ctx->dirty_private) {
          ctx->completion_private->wait_for_complete();
          failed = ctx->completion_private->get_return_value() < 0;
          ctx->completion_private->release();
        }
        if (ctx->dirty_shared) {
          ctx->completion_shared->wait_for_complete();
          failed |= ctx->completion_shared->get_return_value() < 0;
          ctx->completion_shared->release();
        }
        if (callback != NULL) {
          callback(failed ? DICT_COMMIT_RET_FAILED : DICT_COMMIT_RET_OK, context);
        }
        ret = ctx->atomic_inc_not_found ? DICT_COMMIT_RET_NOTFOUND
                                        : (failed ? DICT_COMMIT_RET_FAILED : DICT_COMMIT_RET_OK);
        if (ctx->locked_private) {
          int err = d->get_io_ctx().unlock(d->get_private_oid(), "ATOMIC_INC", guid_128_to_string(ctx->guid));
          i_debug("rados_dict_transaction_commit(): unlock(%s) ret=%d (%s)", d->get_private_oid().c_str(), err,
                  strerror(-err));
        }
        if (ctx->locked_shared) {
          int err = d->get_io_ctx().unlock(d->get_shared_oid(), "ATOMIC_INC", guid_128_to_string(ctx->guid));
          i_debug("rados_dict_transaction_commit(): unlock(%s) ret=%d (%s)", d->get_shared_oid().c_str(), err,
                  strerror(-err));
        }
        delete ctx;
      }
    }
  } else {
    // nothing has been changed
    ret = ctx->atomic_inc_not_found ? DICT_COMMIT_RET_NOTFOUND : DICT_COMMIT_RET_OK;

    if (ctx->callback != nullptr) {
      ctx->callback(ret, ctx->context);
    }
    delete ctx;
  }

  FUNC_END();
  return ret;
}

void rados_dict_transaction_rollback(struct dict_transaction_context *_ctx) {
  FUNC_START();
  struct rados_dict_transaction_context *ctx = (struct rados_dict_transaction_context *)_ctx;
  struct rados_dict *dict = (struct rados_dict *)ctx->ctx.dict;
  RadosDictionary *d = dict->d;

  if (ctx->locked_private) {
    d->get_io_ctx().unlock(d->get_private_oid(), "ATOMIC_INC", guid_128_to_string(ctx->guid));
  }
  if (ctx->locked_shared) {
    d->get_io_ctx().unlock(d->get_shared_oid(), "ATOMIC_INC", guid_128_to_string(ctx->guid));
  }

  delete ctx;
  FUNC_END();
}

void rados_dict_set(struct dict_transaction_context *_ctx, const char *_key, const char *value) {
  FUNC_START();
  struct rados_dict_transaction_context *ctx = (struct rados_dict_transaction_context *)_ctx;
  RadosDictionary *d = ((struct rados_dict *)_ctx->dict)->d;
  const string key(_key);

  i_debug("rados_dict_set(%s,%s)", _key, value);

  _ctx->changed = TRUE;

  std::map<std::string, bufferlist> map;
  bufferlist bl;
  bl.append(value);
  map.insert(pair<string, bufferlist>(key, bl));
  ctx->get_op(key).omap_set(map);

  ctx->cache[key] = value;
  FUNC_END();
}

void rados_dict_unset(struct dict_transaction_context *_ctx, const char *_key) {
  FUNC_START();
  struct rados_dict_transaction_context *ctx = (struct rados_dict_transaction_context *)_ctx;
  RadosDictionary *d = ((struct rados_dict *)_ctx->dict)->d;
  const string key(_key);

  i_debug("rados_dict_unset(%s)", _key);

  _ctx->changed = TRUE;

  set<string> keys;
  keys.insert(key);
  ctx->get_op(key).omap_rm_keys(keys);

  ctx->cache[key] = CACHE_DELETED;
  FUNC_END();
}

void rados_dict_atomic_inc(struct dict_transaction_context *_ctx, const char *_key, long long diff) {  // NOLINT
  FUNC_START();
  struct rados_dict_transaction_context *ctx = (struct rados_dict_transaction_context *)_ctx;
  RadosDictionary *d = ((struct rados_dict *)_ctx->dict)->d;
  const string key(_key);
  string old_value = "0";

  i_debug("rados_atomic_inc(%s,%lld)", _key, diff);

  auto it = ctx->cache.find(key);
  if (it == ctx->cache.end()) {
    if (d->get(key, &old_value) == -ENOENT) {
      ctx->cache[key] = old_value = CACHE_DELETED;
      ctx->atomic_inc_not_found = true;
      i_debug("rados_dict_atomic_inc(%s,%lld) key not found!", _key, diff);
      FUNC_END();
      return;
    } else {
      RadosDictionary *d = ((struct rados_dict *)ctx->ctx.dict)->d;
      if (!ctx->is_locked(key)) {
        struct timeval tv = {30, 0};  // TODO(peter): config?
        int err = d->get_io_ctx().lock_exclusive(d->get_full_oid(key), "ATOMIC_INC", guid_128_to_string(ctx->guid),
                                                 "rados_atomic_inc(" + key + ")", &tv, 0);
        if (err == 0) {
          i_debug("rados_dict_atomic_inc(%s,%lld) lock acquired", _key, diff);
          ctx->set_locked(key);
        } else {
          i_error("rados_dict_atomic_inc(%s,%lld) lock not acquired err=%d", _key, diff, err);
          ctx->atomic_inc_not_found = true;
          FUNC_END();
          return;
        }
      }
    }
  } else {
    ctx->cache[key] = old_value = it->second;
  }

  i_debug("rados_dict_atomic_inc(%s,%lld) old_value=%s", _key, diff, old_value.c_str());

  if (old_value.compare(CACHE_DELETED) == 0) {
    ctx->atomic_inc_not_found = true;
    FUNC_END();
    return;
  }

  long long value;  // NOLINT
  if (str_to_llong(old_value.c_str(), &value) < 0)
    i_unreached();

  value += diff;
  string new_string_value = std::to_string(value);
  rados_dict_set(_ctx, _key, new_string_value.c_str());
  FUNC_END();
}

class kv_map {
 public:
  int rval = -1;
  std::string key;
  std::map<std::string, bufferlist> map;
  typename std::map<std::string, bufferlist>::iterator map_iter;
};

class rados_dict_iterate_context {
 public:
  struct dict_iterate_context ctx;
  enum dict_iterate_flags flags;
  bool failed;
  pool_t result_pool;

  std::vector<kv_map> results;
  typename std::vector<kv_map>::iterator results_iter;
  guid_128_t guid;

  rados_dict_iterate_context(struct dict *dict, enum dict_iterate_flags flags) {
    i_zero(&this->ctx);
    this->ctx.dict = dict;
    this->flags = flags;
    this->failed = FALSE;
    this->result_pool = pool_alloconly_create("iterate value pool", 256);
    guid_128_generate(this->guid);
  }

  void dump() {
    auto g = guid_128_to_string(guid);
    for (const auto &i : results) {
      for (const auto &j : i.map) {
        i_debug("rados_dict_iterate_context %s - %s=%s", g, j.first.c_str(), j.second.to_str().c_str());
      }
    }
  }
};

struct dict_iterate_context *rados_dict_iterate_init(struct dict *_dict, const char *const *paths,
                                                     const enum dict_iterate_flags flags) {
  FUNC_START();
  RadosDictionary *d = ((struct rados_dict *)_dict)->d;

  /* these flags are not supported for now */
  i_assert((flags & DICT_ITERATE_FLAG_SORT_BY_VALUE) == 0);
  i_assert((flags & DICT_ITERATE_FLAG_SORT_BY_KEY) == 0);
  i_assert((flags & DICT_ITERATE_FLAG_ASYNC) == 0);

  auto iter = new rados_dict_iterate_context(_dict, flags);

  set<string> private_keys;
  set<string> shared_keys;
  while (*paths) {
    string key = *paths++;
    i_debug("rados_dict_iterate_init(%s)", key.c_str());

    if (!key.compare(0, strlen(DICT_PATH_SHARED), DICT_PATH_SHARED)) {
      shared_keys.insert(key);
    } else if (!key.compare(0, strlen(DICT_PATH_PRIVATE), DICT_PATH_PRIVATE)) {
      private_keys.insert(key);
    }
  }

  if (private_keys.size() + shared_keys.size() > 0) {
    AioCompletion *private_read_completion = nullptr;
    ObjectReadOperation private_read_op;
    AioCompletion *shared_read_completion = nullptr;
    ObjectReadOperation shared_read_op;

    if (flags & DICT_ITERATE_FLAG_EXACT_KEY) {
      iter->results.reserve(2);
    } else {
      iter->results.reserve(private_keys.size() + shared_keys.size());
    }

    if (private_keys.size() > 0) {
      i_debug("rados_dict_iterate_init() private query");
      private_read_completion = librados::Rados::aio_create_completion();

      if (flags & DICT_ITERATE_FLAG_EXACT_KEY) {
        iter->results.emplace_back();
        private_read_op.omap_get_vals_by_keys(private_keys, &iter->results.back().map, &iter->results.back().rval);
      } else {
        int i = 0;
        for (auto k : private_keys) {
          iter->results.emplace_back();
          iter->results.back().key = k;
          private_read_op.omap_get_vals("", k, LONG_MAX, &iter->results.back().map, &iter->results.back().rval);
        }
      }

      bufferlist bl;
      int err = d->get_io_ctx().aio_operate(d->get_full_oid(DICT_PATH_PRIVATE), private_read_completion,
                                            &private_read_op, &bl);
      i_debug("rados_dict_iterate_init(): private err=%d(%s)", err, strerror(-err));
      iter->failed = err < 0;
    }

    if (!iter->failed && shared_keys.size() > 0) {
      i_debug("rados_dict_iterate_init() shared query");
      shared_read_completion = librados::Rados::aio_create_completion();

      if (flags & DICT_ITERATE_FLAG_EXACT_KEY) {
        iter->results.emplace_back();
        shared_read_op.omap_get_vals_by_keys(shared_keys, &iter->results.back().map, &iter->results.back().rval);
      } else {
        int i = 0;
        for (auto k : shared_keys) {
          iter->results.emplace_back();
          iter->results.back().key = k;
          shared_read_op.omap_get_vals("", k, LONG_MAX, &iter->results.back().map, &iter->results.back().rval);
        }
      }

      bufferlist bl;
      int err =
          d->get_io_ctx().aio_operate(d->get_full_oid(DICT_PATH_SHARED), shared_read_completion, &shared_read_op, &bl);
      i_debug("rados_dict_iterate_init(): shared err=%d(%s)", err, strerror(-err));
      iter->failed = err < 0;
    }

    if (!iter->failed && private_keys.size() > 0) {
      if (!private_read_completion->is_complete()) {
        int err = private_read_completion->wait_for_complete();
        i_debug("rados_dict_iterate_init(): priv wait_for_complete_and_cb() err=%d(%s)", err, strerror(-err));
        iter->failed = err < 0;
      }
      if (!iter->failed) {
        int err = private_read_completion->get_return_value();
        i_debug("rados_dict_iterate_init(): priv get_return_value() err=%d(%s)", err, strerror(-err));
        iter->failed |= err < 0;
      }
    }

    if (!iter->failed && shared_keys.size() > 0) {
      if (!shared_read_completion->is_complete()) {
        int err = shared_read_completion->wait_for_complete();
        i_debug("rados_dict_iterate_init(): shared wait_for_complete_and_cb() err=%d(%s)", err, strerror(-err));
        iter->failed = err < 0;
      }
      if (!iter->failed) {
        int err = shared_read_completion->get_return_value();
        i_debug("rados_dict_iterate_init(): shared get_return_value() err=%d(%s)", err, strerror(-err));
        iter->failed |= err < 0;
      }
    }

    if (!iter->failed) {
      for (auto r : iter->results) {
        i_debug("rados_dict_iterate_init(): r_val=%d(%s)", r.rval, strerror(-r.rval));
        iter->failed |= (r.rval < 0);
      }
    }

    if (!iter->failed) {
      auto ri = iter->results_iter = iter->results.begin();
      iter->results_iter->map_iter = iter->results_iter->map.begin();
      iter->dump();
    } else {
      i_debug("rados_dict_iterate_init() failed");
    }

    if (private_read_completion != nullptr) {
      private_read_completion->release();
    }
    if (shared_read_completion != nullptr) {
      shared_read_completion->release();
    }
  } else {
    i_debug("rados_dict_iterate_init() no keys");
    iter->failed = true;
  }

  FUNC_END();
  return &iter->ctx;
}

bool rados_dict_iterate(struct dict_iterate_context *ctx, const char **key_r, const char **value_r) {
  FUNC_START();
  struct rados_dict_iterate_context *iter = (struct rados_dict_iterate_context *)ctx;

  *key_r = NULL;
  *value_r = NULL;

  if (iter->failed) {
    FUNC_END_RET_INT(FALSE);
    return FALSE;
  }

  while (iter->results_iter->map_iter == iter->results_iter->map.end()) {
    if (++iter->results_iter == iter->results.end())
      return FALSE;
    iter->results_iter->map_iter = iter->results_iter->map.begin();
  }

  auto map_iter = iter->results_iter->map_iter++;

  if ((iter->flags & DICT_ITERATE_FLAG_RECURSE) != 0) {
    // match everything
  } else if ((iter->flags & DICT_ITERATE_FLAG_EXACT_KEY) != 0) {
    // prefiltered by query, match everything
  } else {
    if (map_iter->first.find('/', iter->results_iter->key.length()) != string::npos) {
      auto ret = rados_dict_iterate(ctx, key_r, value_r);
      FUNC_END_RET_INT(ret);
      return ret;
    }
  }

  i_debug("rados_dict_iterate() found key='%s', value='%s'", map_iter->first.c_str(),
          map_iter->second.to_str().c_str());

  p_clear(iter->result_pool);

  *key_r = p_strdup(iter->result_pool, map_iter->first.c_str());

  if ((iter->flags & DICT_ITERATE_FLAG_NO_VALUE) == 0) {
    *value_r = p_strdup(iter->result_pool, map_iter->second.to_str().c_str());
  }

  FUNC_END_RET_INT(TRUE);
  return TRUE;
}

int rados_dict_iterate_deinit(struct dict_iterate_context *ctx) {
  FUNC_START();
  struct rados_dict_iterate_context *iter = (struct rados_dict_iterate_context *)ctx;

  int ret = iter->failed ? -1 : 0;
  pool_unref(&iter->result_pool);
  delete iter;

  FUNC_END_RET_INT(ret);
  return ret;
}
