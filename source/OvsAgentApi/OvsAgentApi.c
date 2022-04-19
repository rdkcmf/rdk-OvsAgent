/*
* Copyright 2020 Comcast Cable Communications Management, LLC
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "OvsAgentApi.h"
#include "OvsAgentDefs.h"
#include "common/OvsAgentLog.h"
#include "OvsDbApi/OvsDbApi.h"
#include "OvsAgentApi/transaction_interface.h"

#define MAX_RID_LEN                 12
#define OVS_BLOCK_MODE_TIMEOUT_SECS 3    // wait for OvsDbApi to respond back
#define OVS_STARTING_ID_MULTIPLIER  1000 // multiplier used to generate range of rId's

// Context structure used to generate a handle to this context
typedef struct ovs_agent_api_context
{
    OVS_COMPONENT_ID cid; // Component ID enum.
    pthread_cond_t  condition;
    pthread_mutex_t mutex;
    bool            blocked; // true if in a blocked state
    bool            monitorFeedback; // true if successfully sent a monitor Feedback request
} ovs_agent_api_context;

static ovs_agent_api_context * g_handle = NULL;
static const char* const g_cids[] = {"Unknown", "TestApp", "OvsAgent", "BridgeUtils", "MeshAgent"};

static void signal_callback_completion()
{
    if (g_handle->blocked)
    {
        pthread_mutex_lock(&g_handle->mutex);
        g_handle->blocked = false;
        pthread_cond_signal(&g_handle->condition);
        pthread_mutex_unlock(&g_handle->mutex);
    }
}

// wait for both callbacks or timeout to occur
static OVS_STATUS wait_for_callback_completion(int timeoutSecs)
{
    OVS_STATUS status = OVS_FAILED_STATUS;
    struct timespec ts;
    struct timespec now;
    int result;

    memset(&ts, 0, sizeof(struct timespec));
    memset(&now, 0, sizeof(struct timespec));

    pthread_mutex_lock(&g_handle->mutex);

    clock_gettime(CLOCK_REALTIME, &now);
    ts.tv_sec = now.tv_sec + timeoutSecs;

    OvsAgentApiDebug("%s waiting for %d secs...\n", __FUNCTION__, timeoutSecs);
    while (g_handle->blocked)
    {
        result = pthread_cond_timedwait(&g_handle->condition, &g_handle->mutex, &ts);
        if (result == ETIMEDOUT)
        {
            OvsAgentApiError("%s pthread_cond_timedwait TIMED OUT!!!\n", __FUNCTION__);
            g_handle->blocked = false;
            status = OVS_TIMED_OUT_STATUS;
        }
        else if (result == 0)
        {
            OvsAgentApiDebug("%s pthread_cond_timedwait SIGNALLED OK!!!\n", __FUNCTION__);
            status = OVS_SUCCESS_STATUS;
        }
        else
        {
            OvsAgentApiError("%s pthread_cond_timedwait ERROR!!!\n", __FUNCTION__);
            g_handle->blocked = false;
            status = OVS_TIMED_WAIT_ERROR_STATUS;
        }
    }

    pthread_mutex_unlock(&g_handle->mutex);
    return status;
}

// TODO: in non-blocked scenario, the DB Abstraction layer needs to call this callback in case of a timeout or no response from the OVS DB.
static void ovs_agent_api_monitor_feedback_callback(OVS_STATUS status,
    Rdkb_Table_Config * table_config)
{
    Feedback * feedback = NULL;
    OvsAgentApiDebug("%s Rcvd monitor callback with Status: %d, Table Config: %p\n",
        __func__, status, table_config);

    // TODO: delete a completed transaction or keep a completed transaction entry and instead set its transaction id to unused value
    if (!table_config || !table_config->config)
    {
        OvsAgentApiError("%s failed as table config is NULL!\n", __func__);
        //callback(OVS_FAILED_STATUS); // TODO Call callback to notify RDKB Component and update transaction as failed and free transaction
        return;
    }

    if (table_config->table.id != OVS_FEEDBACK_TABLE)
    {
        OvsAgentApiError("%s failed as table config is NOT Feedback table!\n", __func__);
        //callback(OVS_FAILED_STATUS); // TODO Call callback to notify RDKB Component and update transaction as failed and free transaction
        return;
    }

    feedback = (Feedback *)table_config->config;
    OvsAgentApiDebug("%s Rcvd monitor callback Config: %p, Table: %d, Status: %d, Uuid: %s\n",
        __func__, feedback, table_config->table.id, feedback->status, feedback->req_uuid);

    if (!complete_transaction(feedback->req_uuid, feedback->status))
    {
        OvsAgentApiError("%s failed to complete transaction for Table: %d, Status: %d, Uuid: %s\n",
            __func__, table_config->table.id, feedback->status, feedback->req_uuid);
        //callback(OVS_FAILED_STATUS); // TODO Call callback to notify RDKB Component and update transaction as failed and free transaction
        return;
    }

    OvsAgentApiDebug("%s: succeeded for Table: %d, Status: %d, Uuid: %s\n", __func__,
        table_config->table.id, feedback->status, feedback->req_uuid);

    // delete entry in Gateway_Config table using _uuid field as key
    if (ovsdb_delete(OVS_GW_CONFIG_TABLE, OVSDB_TABLE_UUID, feedback->req_uuid) !=
        OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to delete _uuid: %s in Table: %d\n",
            __func__, feedback->req_uuid, OVS_GW_CONFIG_TABLE);
        return;
    }

    // delete entry in Feedback table using req_uuid field as key
    if (ovsdb_delete(table_config->table.id, FEEDBACK_REQ_UUID,
        feedback->req_uuid) != OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to delete %s: %s in Table: %d\n",
            __func__, FEEDBACK_REQ_UUID, feedback->req_uuid,
            table_config->table.id);
        return;
    }

    signal_callback_completion();
}

static bool send_monitor_feedback_request()
{
    OVS_STATUS status = ovsdb_monitor(OVS_FEEDBACK_TABLE,
        ovs_agent_api_monitor_feedback_callback, NULL);
    if (status != OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to do a OVS DB monitor request for Feedback table!\n",
            __func__);
        return false;
    }

    OvsAgentApiDebug("%s Successfully sent monitor request for Feedback table\n",
        __func__);
    return true;
}

static bool send_monitor_feedback_cancel_request()
{
    /*OVS_STATUS status = ovsdb_monitor_cancel(OVS_FEEDBACK_TABLE, NULL);
    if (status != OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to do a OVS DB monitor cancel request for Feedback table!\n",
            __func__);
        return false;
    }

    OvsAgentApiDebug("%s Successfully sent monitor cancel request for Feedback table\n",
        __func__);*/
    return true;
}

// TODO: in non-blocked scenario, the DB Abstraction layer needs to call this callback in case of a timeout or no response from the OVS DB.
static void ovs_agent_api_write_callback(const char * rid, const OvsDb_Base_Receipt* receipt_result)
{
    OvsDb_Insert_Receipt * receipt = (OvsDb_Insert_Receipt *)receipt_result;

    OvsAgentApiDebug("%s Rcvd write callback for rId: %s, Receipt Id: %d\n", __func__,
        (rid ? rid : "NULL"), (receipt ? receipt->receipt_id : -1));

    if (!receipt || receipt->receipt_id != OVSDB_INSERT_RECEIPT_ID)
    {
        OvsAgentApiWarning("%s received invalid receipt\n", __func__);
        //callback(OVS_FAILED_STATUS); // TODO Call callback to notify RDKB Component and update transaction as failed and free transaction
        return;
    }
    if (!update_transaction(rid, receipt->uuid))
    {
        OvsAgentApiError("%s failed to update transaction for rId: %s, Uuid: %s\n",
            __func__, (rid ? rid : "NULL"), receipt->uuid);
        //callback(OVS_FAILED_STATUS); // TODO Call callback to notify RDKB Component and update transaction as failed and free transaction
        return;
    }

    if (!g_handle->monitorFeedback)
    {
        g_handle->monitorFeedback = send_monitor_feedback_request();
    }
}

static const char * component_id_enum_to_string(OVS_COMPONENT_ID cid)
{
    if (cid > 0 && cid < OVS_MAX_COMPONENT_ID)
    {
        return g_cids[cid];
    }
    return g_cids[0];
}

bool ovs_agent_api_init(OVS_COMPONENT_ID cid)
{
    OVS_STATUS status;
    pthread_condattr_t condition_attr;

    if (g_handle)
    {
        OvsAgentApiDebug("%s already initialized.\n", __func__);
        return true;
    }

    if ((g_handle = (ovs_agent_api_context *)malloc(sizeof(ovs_agent_api_context))) == NULL)
    {
        fprintf(stderr, "%s failed to intialize context\n", __func__);
        return false;
    }

    if (!open_log(OVS_AGENT_API_LOG_FILE, component_id_enum_to_string(cid)))
    {
        fprintf(stderr, "%s failed to open log file %s\n", __func__,
            OVS_AGENT_API_LOG_FILE);
        free(g_handle);
        g_handle = NULL;
        return false;
    }

    g_handle->cid = cid;
    g_handle->blocked = OVS_DISABLE_BLOCK_MODE;
    g_handle->monitorFeedback = false;
    if ((access(OVSAGENT_DEBUG_ENABLE, F_OK) != -1) &&
        set_log_level(LOG_DEBUG_LEVEL))
    {
        OvsAgentApiInfo("%s successfully set debug log level.\n", __func__);
    }

    status = ovsdb_init(cid * OVS_STARTING_ID_MULTIPLIER);
    if (status != OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to initialize db with status %d\n", __func__, status);
        close_log();
        free(g_handle);
        g_handle = NULL;
        return false;
    }

    if (pthread_mutex_init(&g_handle->mutex, NULL) != 0) {
        OvsAgentApiError("%s failed to initialize mutex\n", __func__);
        // TODO: deinit database connection
        close_log();
        free(g_handle);
        g_handle = NULL;
        return false;
    }

    pthread_condattr_init(&condition_attr);
    if (pthread_cond_init (&g_handle->condition, &condition_attr) != 0) {
       OvsAgentApiError("%s failed to create intialize condition\n",__func__);

       pthread_mutex_destroy(&g_handle->mutex);
        // TODO: deinit database connection
       close_log();
       free(g_handle);
       g_handle = NULL;
       return false;
    }

    // TODO: Store ptr to transaction table within handle
    if (!init_transaction_manager())
    {
        OvsAgentApiError("%s failed to initialize transaction manager!\n", __func__);

        pthread_cond_destroy(&g_handle->condition);
        pthread_mutex_destroy(&g_handle->mutex);
        // TODO: deinit database connection
        close_log();
        free(g_handle);
        g_handle = false;
        return false;
    }

    OvsAgentApiInfo("%s successfully initialized for cid %u.\n", __func__, cid);
    return true;
}

bool ovs_agent_api_deinit(void)
{
    if (!g_handle)
    {
        fprintf(stderr, "%s already de-initialized.\n", __func__);
        return true;
    }

    OvsAgentApiInfo("%s de-initializing cid %u\n", __func__, g_handle->cid);

    if (g_handle->monitorFeedback)
    {
        g_handle->monitorFeedback = !send_monitor_feedback_cancel_request();
        // TODO: how to handle a failure in sending a monitor feedback cancel request
    }

    // Deinit database socket
    if (ovsdb_deinit() == OVS_FAILED_STATUS)
    {
        OvsAgentApiError("%s failed to gracefully shutdown database connection\n",
            __func__);
    }

    deinit_transaction_manager();

    pthread_cond_destroy(&g_handle->condition);
    pthread_mutex_destroy(&g_handle->mutex);

    if (!close_log())
    {
        fprintf(stderr, "%s failed to close log file\n", __func__);
    }

    free(g_handle);
    g_handle = NULL;

    return true;
}

void print_gateway_config(Gateway_Config * config)
{
    if (!config)
    {
        OvsAgentApiWarning("%s Gateway Config is NULL\n", __func__);
        return;
    }

    // TODO: print all members of structure, not just some
    OvsAgentApiDebug(
        "%s Gateway Config %p, If Name: %s len: %zu, Parent Bridge: %s len: %zu, If Name: %s len: %zu, IP: %s len: %zu, Netmask: %s len: %zu, Mtu: %d, Vlan Id: %d, If Type: %d, If Cmd: %d\n",
        __func__, config, config->if_name, strlen(config->if_name),
        config->parent_bridge, strlen(config->parent_bridge),
        config->parent_ifname, strlen(config->parent_ifname),
        config->inet_addr, strlen(config->inet_addr),
        config->netmask, strlen(config->netmask), config->mtu,
        config->vlan_id, config->if_type, config->if_cmd);
}

void print_feedback_config(Feedback * config)
{
    if (!config)
    {
        OvsAgentApiError("%s Feedback Config is NULL\n", __func__);
        return;
    }

    OvsAgentApiDebug("%s Feedback Config %p, Req Uuid: %s len: %zu, Status: %d\n",
        __func__, config, config->req_uuid, strlen(config->req_uuid),
        config->status);
}

void print_rdkb_table_config(Rdkb_Table_Config * table_config)
{
    if (!table_config)
    {
        return;
    }

    OvsAgentApiDebug("%s Set Table: %d, Config: %p at %p=%p\n", __func__,
        table_config->table.id, table_config->config,
        ((Union_Table_Config *) table_config)->table_configs.gw_config.config,
        ((Union_Table_Config *) table_config)->table_configs.fb_config.config);

    switch (table_config->table.id)
    {
        case OVS_GW_CONFIG_TABLE:
            print_gateway_config((Gateway_Config *)table_config->config);
            break;
        case OVS_FEEDBACK_TABLE:
            print_feedback_config((Feedback *)table_config->config);
            break;
        default:
            break;
    }
}

static bool handle_transact_insert_request(ovs_interact_request * request, ovs_interact_cb callback)
{
    bool rtn = false;
    char rid[MAX_RID_LEN] = "";
    int len = 0;
    OVS_STATUS status;
    ovsdb_receipt_cb cb = NULL;

    if (!request)
    {
        return false;
    }

    len = snprintf(rid, MAX_RID_LEN, "%u", id_generate());
    if (len <= 0 && len >= MAX_RID_LEN)
    {
        return false;
    }

    if (callback || request->block_mode == OVS_ENABLE_BLOCK_MODE)
    {   // RDKB Component case only
        g_handle->blocked = (request->block_mode == OVS_ENABLE_BLOCK_MODE) ? true : false;
        cb = ovs_agent_api_write_callback;

        if (!insert_transaction(rid, callback, &request->table_config))
        {
            OvsAgentApiError("%s failed to create transaction with rId %s!\n",
                __func__, rid);
            return false;
        }
    }

    OvsAgentApiDebug("%s Doing a %s ovsdb_write with rId: %s, Table: %d, Callback: %s\n",
        __func__, (request->block_mode ? "BLOCKED" : "UNBLOCKED"), rid,
        request->table_config.table.id, (cb ? "NON-NULL" : "NULL"));
    status = ovsdb_write(rid, &request->table_config, cb);
    if (status != OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to do a OVS DB write operation for rId %s!\n",
            __func__, rid);
        // TODO: Update failed transaction status, complete and free transaction, return
        if (g_handle->blocked)
        {
            g_handle->blocked = false;
        }
        goto cleanup;
    }

    if (request->block_mode != OVS_ENABLE_BLOCK_MODE)
    {
        return true;
    } // else block mode is enabled
    else
    {
        status = wait_for_callback_completion(OVS_BLOCK_MODE_TIMEOUT_SECS);
    }

cleanup:
    if (status == OVS_SUCCESS_STATUS)
    {
        rtn = true;
    }
    else if (!delete_transaction(rid))
    {
        OvsAgentApiError("%s failed to delete transaction with rId: %s!\n",
            __func__, rid);
    }

    OvsAgentApiInfo("%s rId: %s, status: %d, rtn: %s\n", __func__, rid, status,
        (rtn?"SUCCESS":"ERROR"));
    return rtn;
}

// TODO: have ovs_interact_cb and ovsdb_mon_cb as one definition instead of two
static bool handle_monitor_insert_request(ovs_interact_request * request, ovs_interact_cb callback)
{
    bool rtn = true;
    OVS_STATUS status;

    if (!request)
    {
        return false;
    }

    OvsAgentApiDebug("%s Monitor Table: %d\n", __func__, request->table_config.table.id);
    status = ovsdb_monitor(request->table_config.table.id, callback, NULL);
    if (status != OVS_SUCCESS_STATUS)
    {
        OvsAgentApiError("%s failed to Monitor Table: %d!\n", __func__,
            request->table_config.table.id);
        rtn = false;
    }

    OvsAgentApiInfo("%s status: %d, rtn: %s\n", __func__, status, (rtn?"SUCCESS":"ERROR"));
    return rtn;
}

// TODO: pass param as pointer and delete it
bool ovs_agent_api_interact(ovs_interact_request * request, ovs_interact_cb callback)
{
    if (!request)
    {
        return false;
    }

    if (!g_handle)
    {
        OvsAgentApiError("%s failed as the Ovs Agent Api was not initialized!\n",
            __func__);
        return false;
    }

    print_rdkb_table_config(&request->table_config);

    if (request->operation == OVS_INSERT_OPERATION)
    {
        if (request->method == OVS_TRANSACT_METHOD)
        {
            return handle_transact_insert_request(request, callback);
        }
        else if (request->method == OVS_MONITOR_METHOD)
        { // OVS Agent case
            return handle_monitor_insert_request(request, callback);
        }
    }
    OvsAgentApiDebug("%s Unsupported request operation=%d, method=%d\n",
        __func__, request->operation, request->method);
    return false;
}

static bool initialize_gateway_config(Gateway_Config * config)
{
    if (!config)
    {
        return false;
    }

    config->if_type = OVS_OTHER_IF_TYPE;
    config->mtu = DEFAULT_MTU;
    config->vlan_id = DEFAULT_VLAN_ID;
    config->if_cmd = OVS_IF_UP_CMD;
    return true;
}

bool ovs_agent_api_get_config(OVS_TABLE table, void ** ppConfig)
{
    bool rtn = false;
    if (!ppConfig)
    {
        return false;
    }

    switch (table)
    {
        case OVS_GW_CONFIG_TABLE:
            {
                Gateway_Config * config = NULL;
                if ((config = (Gateway_Config *)malloc(sizeof(Gateway_Config))) != NULL)
                {
                   memset(config, 0, sizeof(Gateway_Config));
                   rtn = initialize_gateway_config(config);
                   *ppConfig = config;
                }
            }
            break;
        default:
            break;
    }
    return rtn;
}
