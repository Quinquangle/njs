
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_auto_config.h>
#include <nxt_types.h>
#include <nxt_clang.h>
#include <nxt_alignment.h>
#include <nxt_string.h>
#include <nxt_stub.h>
#include <nxt_utf8.h>
#include <nxt_djb_hash.h>
#include <nxt_array.h>
#include <nxt_lvlhsh.h>
#include <nxt_random.h>
#include <nxt_mem_cache_pool.h>
#include <njscript.h>
#include <njs_vm.h>
#include <njs_extern.h>
#include <njs_variable.h>
#include <njs_parser.h>
#include <string.h>
#include <stdio.h>


typedef struct njs_extern_part_s  njs_extern_part_t;

struct njs_extern_part_s {
    njs_extern_part_t  *next;
    nxt_str_t          str;
};


static nxt_int_t
njs_extern_hash_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    njs_extern_t  *ext;

    ext = (njs_extern_t *) data;

    if (nxt_strstr_eq(&lhq->key, &ext->name)) {
        return NXT_OK;
    }

    return NXT_DECLINED;
}


static nxt_int_t
njs_extern_value_hash_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    njs_extern_value_t  *ev;

    ev = (njs_extern_value_t *) data;

    if (nxt_strstr_eq(&lhq->key, &ev->name)) {
        return NXT_OK;
    }

    return NXT_DECLINED;
}


const nxt_lvlhsh_proto_t  njs_extern_hash_proto
    nxt_aligned(64) =
{
    NXT_LVLHSH_DEFAULT,
    NXT_LVLHSH_BATCH_ALLOC,
    njs_extern_hash_test,
    njs_lvlhsh_alloc,
    njs_lvlhsh_free,
};


const nxt_lvlhsh_proto_t  njs_extern_value_hash_proto
    nxt_aligned(64) =
{
    NXT_LVLHSH_DEFAULT,
    NXT_LVLHSH_BATCH_ALLOC,
    njs_extern_value_hash_test,
    njs_lvlhsh_alloc,
    njs_lvlhsh_free,
};


static njs_extern_t *
njs_vm_external_add(njs_vm_t *vm, nxt_lvlhsh_t *hash, njs_external_t *external,
    nxt_uint_t n)
{
    nxt_int_t           ret;
    njs_extern_t        *ext, *child;
    nxt_lvlhsh_query_t  lhq;

    do {
        ext = nxt_mem_cache_alloc(vm->mem_cache_pool, sizeof(njs_extern_t));
        if (nxt_slow_path(ext == NULL)) {
            return NULL;
        }

        ext->name = external->name;

        nxt_lvlhsh_init(&ext->hash);

        ext->type = external->type;
        ext->get = external->get;
        ext->set = external->set;
        ext->find = external->find;
        ext->foreach = external->foreach;
        ext->next = external->next;
        ext->data = external->data;

        if (external->method != NULL) {
            ext->function = nxt_mem_cache_zalloc(vm->mem_cache_pool,
                                                 sizeof(njs_function_t));
            if (nxt_slow_path(ext->function == NULL)) {
                return NULL;
            }

            ext->function->native = 1;
            ext->function->args_offset = 1;
            ext->function->u.native = external->method;

        } else {
            ext->function = NULL;
        }

        if (external->properties != NULL) {
            child = njs_vm_external_add(vm, &ext->hash, external->properties,
                                        external->nproperties);
            if (nxt_slow_path(child == NULL)) {
                return NULL;
            }
        }

        if (hash != NULL) {
            lhq.key_hash = nxt_djb_hash(external->name.start,
                                        external->name.length);
            lhq.key = ext->name;
            lhq.replace = 0;
            lhq.value = ext;
            lhq.pool = vm->mem_cache_pool;
            lhq.proto = &njs_extern_hash_proto;

            ret = nxt_lvlhsh_insert(hash, &lhq);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NULL;
            }
        }

        external++;
        n--;

    } while (n != 0);

    return ext;
}


const njs_extern_t *
njs_vm_external_prototype(njs_vm_t *vm, njs_external_t *external)
{
    return njs_vm_external_add(vm, &vm->external_prototypes_hash, external, 1);
}


nxt_int_t
njs_vm_external_create(njs_vm_t *vm, njs_opaque_value_t *value,
    const njs_extern_t *proto,  void *object)
{
    void         *obj;
    njs_value_t  *ext_val;

    if (nxt_slow_path(proto == NULL)) {
        return NXT_ERROR;
    }

    obj = nxt_array_add(vm->external_objects, &njs_array_mem_proto,
                        vm->mem_cache_pool);
    if (nxt_slow_path(obj == NULL)) {
        return NXT_ERROR;
    }

    memcpy(obj, &object, sizeof(void *));

    ext_val = (njs_value_t *) value;

    ext_val->type = NJS_EXTERNAL;
    ext_val->data.truth = 1;
    ext_val->external.proto = proto;
    ext_val->external.index = vm->external_objects->items - 1;

    return NXT_OK;
}


nxt_int_t
njs_vm_external_bind(njs_vm_t *vm, const nxt_str_t *var_name,
    njs_opaque_value_t *val)
{
    nxt_int_t           ret;
    njs_value_t         *value;
    njs_extern_value_t  *ev;
    nxt_lvlhsh_query_t  lhq;

    value = (njs_value_t *) val;

    if (nxt_slow_path(!njs_is_external(value))) {
        return NXT_ERROR;
    }

    ev = nxt_mem_cache_align(vm->mem_cache_pool, sizeof(njs_value_t),
                             sizeof(njs_extern_value_t));
    if (nxt_slow_path(ev == NULL)) {
        return NXT_ERROR;
    }

    ev->value = *value;
    ev->name = *var_name;

    lhq.key = *var_name;
    lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);
    lhq.proto = &njs_extern_value_hash_proto;
    lhq.value = ev;
    lhq.replace = 0;
    lhq.pool = vm->mem_cache_pool;

    ret = nxt_lvlhsh_insert(&vm->externals_hash, &lhq);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    return NXT_OK;
}


njs_value_t *
njs_parser_external(njs_vm_t *vm, njs_parser_t *parser)
{
    nxt_lvlhsh_query_t  lhq;
    njs_extern_value_t  *ev;

    lhq.key_hash = parser->lexer->key_hash;
    lhq.key = parser->lexer->text;
    lhq.proto = &njs_extern_value_hash_proto;

    if (nxt_lvlhsh_find(&vm->externals_hash, &lhq) == NXT_OK) {
        ev = (njs_extern_value_t *) lhq.value;
        return &ev->value;
    }

    return NULL;
}


static nxt_int_t
njs_external_match(njs_vm_t *vm, njs_function_native_t func, njs_extern_t *ext,
    nxt_str_t *name, njs_extern_part_t *head, njs_extern_part_t *ppart)
{
    char               *buf, *p;
    size_t             len;
    nxt_int_t          ret;
    njs_extern_t       *prop;
    njs_extern_part_t  part, *pr;
    nxt_lvlhsh_each_t  lhe;

    ppart->next = &part;

    nxt_lvlhsh_each_init(&lhe, &njs_extern_hash_proto);

    for ( ;; ) {
        prop = nxt_lvlhsh_each(&ext->hash, &lhe);
        if (prop == NULL) {
            break;
        }

        part.next = NULL;
        part.str = prop->name;

        if (prop->function && prop->function->u.native == func) {
            goto found;
        }

        ret = njs_external_match(vm, func, prop, name, head, &part);
        if (ret != NXT_DECLINED) {
            return ret;
        }
    }

    return NXT_DECLINED;

found:

    len = 0;

    for (pr = head; pr != NULL; pr = pr->next) {
        len += pr->str.length + sizeof(".") - 1;
    }

    buf = nxt_mem_cache_zalloc(vm->mem_cache_pool, len);
    if (buf == NULL) {
        return NXT_ERROR;
    }

    p = buf;

    for (pr = head; pr != NULL; pr = pr->next) {
        p += snprintf(p, buf + len - p, "%.*s.", (int) pr->str.length,
                      pr->str.start);
    }

    name->start = (u_char *) buf;
    name->length = len;

    return NXT_OK;
}


nxt_int_t
njs_external_match_native_function(njs_vm_t *vm, njs_function_native_t func,
    nxt_str_t *name)
{
    nxt_int_t          ret;
    njs_extern_t       *ext;
    njs_extern_part_t  part;
    nxt_lvlhsh_each_t  lhe;

    nxt_lvlhsh_each_init(&lhe, &njs_extern_hash_proto);

    for ( ;; ) {
        ext = nxt_lvlhsh_each(&vm->external_prototypes_hash, &lhe);
        if (ext == NULL) {
            break;
        }

        part.next = NULL;
        part.str = ext->name;

        ret = njs_external_match(vm, func, ext, name, &part, &part);
        if (ret != NXT_DECLINED) {
            return ret;
        }
    }

    return NXT_DECLINED;
}
