/****************************************************************
 *
 *        Copyright 2013, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

/**
 * @file
 * @brief OpenFlow message handlers for group messages
 *
 * See detailed documentation in the Indigo architecture headers.
 */

#include "ofstatemanager_log.h"

#include <OFStateManager/ofstatemanager_config.h>
#include <OFConnectionManager/ofconnectionmanager.h>
#include <indigo/indigo.h>
#include <indigo/of_state_manager.h>
#include <indigo/forwarding.h>
#include <loci/loci.h>
#include "ofstatemanager_decs.h"
#include "ofstatemanager_int.h"
#include "handlers.h"
#include <BigHash/bighash.h>

typedef struct ind_core_group_s {
    bighash_entry_t hash_entry;
    uint32_t id;
    uint32_t type;
    of_list_bucket_t *buckets;
    indigo_time_t creation_time;
} ind_core_group_t;

#define TEMPLATE_NAME group_hashtable
#define TEMPLATE_OBJ_TYPE ind_core_group_t
#define TEMPLATE_KEY_FIELD id
#define TEMPLATE_ENTRY_FIELD hash_entry
#include <BigHash/bighash_template.h>

static bighash_table_t *ind_core_group_hashtable;

static ind_core_group_t *
ind_core_group_lookup(uint32_t id)
{
    return group_hashtable_first(ind_core_group_hashtable, &id);
}

static void
ind_core_group_delete_one(ind_core_group_t *group)
{
    indigo_fwd_group_delete(group->id);
    of_object_delete(group->buckets);
    bighash_remove(ind_core_group_hashtable, &group->hash_entry);
    INDIGO_MEM_FREE(group);
}

void
ind_core_group_add_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_group_add_t *obj = _obj;
    uint32_t xid;
    uint8_t type;
    uint32_t id;
    of_list_bucket_t buckets;
    ind_core_group_t *group = NULL;
    uint16_t err_type = OF_ERROR_TYPE_GROUP_MOD_FAILED;
    uint16_t err_code = OF_GROUP_MOD_FAILED_EPERM;
    indigo_error_t result;

    of_group_add_xid_get(obj, &xid);
    of_group_add_group_type_get(obj, &type);
    of_group_add_group_id_get(obj, &id);
    of_group_add_buckets_bind(obj, &buckets);

    if (id <= OF_GROUP_MAX) {
        group = ind_core_group_lookup(id);
    }

    if (group != NULL) {
        err_code = OF_GROUP_MOD_FAILED_GROUP_EXISTS;
        goto error;
    } else if (id > OF_GROUP_MAX) {
        err_code = OF_GROUP_MOD_FAILED_INVALID_GROUP;
        goto error;
    }

    result = indigo_fwd_group_add(id, type, &buckets);
    if (result < 0) {
        err_code = OF_GROUP_MOD_FAILED_INVALID_GROUP;
        goto error;
    }

    group = INDIGO_MEM_ALLOC(sizeof(*group));
    AIM_TRUE_OR_DIE(group != NULL);
    group->id = id;
    group->type = type;
    group->buckets = of_object_dup(&buckets);
    AIM_TRUE_OR_DIE(group->buckets != NULL);
    group->creation_time = INDIGO_CURRENT_TIME;

    group_hashtable_insert(ind_core_group_hashtable, group);

    of_object_delete(obj);
    return;

error:
    indigo_cxn_send_error_reply(cxn_id, obj, err_type, err_code);
    of_object_delete(obj);
}

void
ind_core_group_modify_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_group_mod_t *obj = _obj;
    uint32_t xid;
    uint8_t type;
    uint32_t id;
    of_list_bucket_t buckets;
    ind_core_group_t *group = NULL;
    uint16_t err_type = OF_ERROR_TYPE_GROUP_MOD_FAILED;
    uint16_t err_code = OF_GROUP_MOD_FAILED_EPERM;
    indigo_error_t result;

    of_group_modify_xid_get(obj, &xid);
    of_group_modify_group_type_get(obj, &type);
    of_group_modify_group_id_get(obj, &id);
    of_group_modify_buckets_bind(obj, &buckets);

    if (id <= OF_GROUP_MAX) {
        group = ind_core_group_lookup(id);
    }

    if (group == NULL) {
        err_code = OF_GROUP_MOD_FAILED_UNKNOWN_GROUP;
        goto error;
    }

    if (group->type == type) {
        result = indigo_fwd_group_modify(id, &buckets);
    } else {
        indigo_fwd_group_delete(id);
        result = indigo_fwd_group_add(id, type, &buckets);
    }

    if (result < 0) {
        err_code = OF_GROUP_MOD_FAILED_INVALID_GROUP;
        goto error;
    }

    group->type = type;
    of_object_delete(group->buckets);
    group->buckets = of_object_dup(&buckets);
    AIM_TRUE_OR_DIE(group->buckets != NULL);

    of_object_delete(obj);
    return;

error:
    indigo_cxn_send_error_reply(cxn_id, obj, err_type, err_code);
    of_object_delete(obj);
}

void
ind_core_group_delete_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_group_delete_t *obj = _obj;
    uint32_t xid;
    uint32_t id;
    ind_core_group_t *group = NULL;
    uint16_t err_type = OF_ERROR_TYPE_GROUP_MOD_FAILED;
    uint16_t err_code = OF_GROUP_MOD_FAILED_EPERM;

    of_group_delete_xid_get(obj, &xid);
    of_group_delete_group_id_get(obj, &id);

    if (id <= OF_GROUP_MAX) {
        group = ind_core_group_lookup(id);
    }

    if (id == OF_GROUP_ALL) {
        bighash_iter_t iter;
        for (group = bighash_iter_start(ind_core_group_hashtable, &iter);
                group; group = bighash_iter_next(&iter)) {
            ind_core_group_delete_one(group);
        }
    } else if (group != NULL) {
        ind_core_group_delete_one(group);
    } else if (id > OF_GROUP_MAX) {
        err_code = OF_GROUP_MOD_FAILED_INVALID_GROUP;
        goto error;
    }

    of_object_delete(obj);
    return;

error:
    indigo_cxn_send_error_reply(cxn_id, obj, err_type, err_code);
    of_object_delete(obj);
}

static void
ind_core_group_stats_entry_populate(of_group_stats_entry_t *entry,
                                    ind_core_group_t *group,
                                    indigo_time_t current_time)
{
    uint32_t duration_sec, duration_nsec;

    of_group_stats_entry_group_id_set(entry, group->id);

    calc_duration(current_time, group->creation_time, &duration_sec, &duration_nsec);
    of_group_stats_entry_duration_sec_set(entry, duration_sec);
    of_group_stats_entry_duration_nsec_set(entry, duration_nsec);

    indigo_fwd_group_stats_get(group->id, entry);
}

/* TODO segment long replies */
void
ind_core_group_stats_request_handler(of_object_t *_obj,
                                     indigo_cxn_id_t cxn_id)
{
    of_group_stats_request_t *obj = _obj;
    of_group_stats_reply_t *reply;
    of_list_group_stats_entry_t entries;
    of_group_stats_entry_t *entry;
    uint32_t xid;
    uint32_t id;
    indigo_time_t current_time = INDIGO_CURRENT_TIME;

    of_group_stats_request_group_id_get(obj, &id);

    reply = of_group_stats_reply_new(obj->version);
    AIM_TRUE_OR_DIE(reply != NULL);

    of_group_stats_request_xid_get(obj, &xid);
    of_group_stats_reply_xid_set(reply, xid);
    of_group_stats_reply_entries_bind(reply, &entries);

    entry = of_group_stats_entry_new(entries.version);
    AIM_TRUE_OR_DIE(entry != NULL);

    if (id == OF_GROUP_ALL) {
        bighash_iter_t iter;
        ind_core_group_t *group;
        for (group = bighash_iter_start(ind_core_group_hashtable, &iter);
                group; group = bighash_iter_next(&iter)) {
            ind_core_group_stats_entry_populate(entry, group, current_time);

            if (of_list_append(&entries, entry) < 0) {
                break;
            }

            /* HACK unable to truncate existing object */
            of_object_delete(entry);
            entry = of_group_stats_entry_new(entries.version);
            AIM_TRUE_OR_DIE(entry != NULL);
        }
    } else if (id <= OF_GROUP_MAX) {
        ind_core_group_t *group = ind_core_group_lookup(id);
        if (group != NULL) {
            ind_core_group_stats_entry_populate(entry, group, current_time);

            if (of_list_append(&entries, entry) < 0) {
                AIM_DIE("unexpected failure appending single group stats entry");
            }
        }
    }

    of_object_delete(entry);
    of_object_delete(obj);

    indigo_cxn_send_controller_message(cxn_id, reply);
}

/* TODO segment long replies */
void
ind_core_group_desc_stats_request_handler(of_object_t *_obj,
                                          indigo_cxn_id_t cxn_id)
{
    of_group_desc_stats_request_t *obj = _obj;
    of_group_desc_stats_reply_t *reply;
    of_list_group_desc_stats_entry_t entries;
    of_group_desc_stats_entry_t *entry;
    uint32_t xid;
    ind_core_group_t *group;
    bighash_iter_t iter;

    reply = of_group_desc_stats_reply_new(obj->version);
    AIM_TRUE_OR_DIE(reply != NULL);

    of_group_desc_stats_request_xid_get(obj, &xid);
    of_group_desc_stats_reply_xid_set(reply, xid);
    of_group_desc_stats_reply_entries_bind(reply, &entries);

    entry = of_group_desc_stats_entry_new(entries.version);
    AIM_TRUE_OR_DIE(entry != NULL);

    for (group = bighash_iter_start(ind_core_group_hashtable, &iter);
            group; group = bighash_iter_next(&iter)) {
        of_group_desc_stats_entry_group_type_set(entry, group->type);
        of_group_desc_stats_entry_group_id_set(entry, group->id);
        if (of_group_desc_stats_entry_buckets_set(entry, group->buckets) < 0) {
            AIM_DIE("unexpected failure setting group desc stats entry buckets");
        }

        if (of_list_append(&entries, entry) < 0) {
            break;
        }
    }

    of_object_delete(entry);
    of_object_delete(obj);

    indigo_cxn_send_controller_message(cxn_id, reply);
}

void
ind_core_group_features_stats_request_handler(of_object_t *_obj,
                                              indigo_cxn_id_t cxn_id)
{
    of_group_features_stats_request_t *obj = _obj;
    ind_core_unhandled_message(obj, cxn_id);
}

void
ind_core_group_init(void)
{
    ind_core_group_hashtable = bighash_table_create(1024);
    AIM_TRUE_OR_DIE(ind_core_group_hashtable != NULL);
}
