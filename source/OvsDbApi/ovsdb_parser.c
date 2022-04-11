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

#include <string.h>
#include <jansson.h>
#include "OvsDataTypes.h"
#include "common/OvsAgentLog.h"
#include "OvsDbApi/OvsDbDefs.h"
#include "OvsDbApi/ovsdb_parser.h"
#include "OvsDbApi/json_parser/table_parser.h"
#include "OvsDbApi/json_parser/json_parser.h"
#include "OvsDbApi/mon_update_list.h"
#include "OvsDbApi/receipt_list.h"

static OVS_STATUS ovsdb_parse_monitor_update(const char * uuid, json_t* update);
static OVS_STATUS ovsdb_parse_params(json_t* params);

char * ovsdb_insert_to_json(Rdkb_Table_Config * table_config, const char * unique_id)
{
    char * str_json = NULL;

    switch(table_config->table.id)
    {
        case OVS_GW_CONFIG_TABLE:
            str_json = gc_insert_to_json( (Gateway_Config*) table_config->config, unique_id);
            break;

        case OVS_FEEDBACK_TABLE:
            str_json = fb_insert_to_json( (Feedback*) table_config->config, unique_id);
            break;

         default:
            OvsDbApiError("Failed to identify config table, corrupted configuration or this table is not implemented.\n");
            break;
    }

    return str_json;
}

char * ovsdb_monitor_to_json(OVS_TABLE ovsdb_table, const char * rID, const char * unique_id)
{
  char * table = NULL;

  if(ovsdb_table == OVS_GW_CONFIG_TABLE)
  {
      table = GATEWAY_CONFIG_TABLE_NAME;
  }
  else if(ovsdb_table == OVS_FEEDBACK_TABLE)
  {
      table = FEEDBACK_TABLE_NAME;
  }
  else
  {
      printf("Only Gateway_Config is implemented in monitor_to_json!\n");
      return NULL;
  }

  int ret;
  json_t *js = NULL;
  json_t *jparams;
  json_t *jtblval;
  json_t *jtbl;
  json_t *jo_col;

  jparams = json_array ();
  json_array_append_new (jparams, json_string (OVSDB_DEF_DB));
  json_array_append_new (jparams, json_string (unique_id));

  jtbl = json_object ();
  jo_col = json_object ();

  json_object_set_new (jtbl, table, jo_col);

  json_array_append_new (jparams, jtbl);

  js = json_object ();

  ret = json_object_set_new (js, "method", json_string("monitor"));
  if(ret < 0)
  {
      printf("Error adding method key.\n");
      return NULL;
  }

  ret = json_object_set_new(js, "params", jparams);
  if(ret < 0)
  {
      printf("Error adding params array.\n");
      return NULL;
  }

  ret = json_object_set_new (js, "id", json_string(rID));
  if(ret < 0)
  {
      printf("Error adding ID key.\n");
      return NULL;
  }

  char * str_out = json_dumps(js, JSON_COMPACT);
  json_decref(js);
  return str_out;

  //TODO: Check to make sure we are releasing all JSON objects and this isn't memory leak.
}

char * ovsdb_monitor_cancel_to_json(const char * old_id, const char * rID)
{
  int ret;
  json_t *js = NULL;
  json_t *jparams;

  jparams = json_array ();
  json_array_append_new (jparams, json_string (old_id));

  js = json_object ();

  ret = json_object_set_new (js, "method", json_string("monitor_cancel"));
  if(ret < 0)
  {
      printf("Error adding method key.\n");
      return NULL;
  }

  ret = json_object_set_new(js, "params", jparams);
  if(ret < 0)
  {
      printf("Error adding params array.\n");
      return NULL;
  }

  ret = json_object_set_new (js, "id", json_string(rID));
  if(ret < 0)
  {
      printf("Error adding ID key.\n");
      return NULL;
  }

  char * str_out = json_dumps(js, JSON_COMPACT);
  json_decref(js);
  return str_out;
}

OVS_STATUS ovsdb_parse_msg(const char* json_str, size_t size)
{
    OVS_STATUS status = OVS_FAILED_STATUS;
    json_error_t error;
    json_t* msg = NULL;
    int bytes_read = 0;

    if(json_str == NULL)
    {
        OvsDbApiError("JSON message is NULL.\n");
        return OVS_FAILED_STATUS;
    }

    do
    {
        msg = json_loads(json_str, JSON_DISABLE_EOF_CHECK, &error);
        if(msg == NULL)
        {
            OvsDbApiError("Failed to parse JRPC: %s\n", error.text);
            return OVS_FAILED_STATUS;
        }

        bytes_read += error.position;
        json_str += error.position;

        json_t* id = json_object_get(msg, "id");
        if(id == NULL)
        {
            OvsDbApiError("Cannot fetch ID from the JRPC, must be malformed message.\n");
            return OVS_FAILED_STATUS; // TODO: Use more meaningful OVS_STATUS code
        }

        if(json_is_null(id))
        {
            //TODO: Check 'method' to make sure it's monitor update
            OvsDbApiDebug("JSON monitor update\n");
            json_t* method = json_object_get(msg, "method");
            if(method == NULL || json_is_string(method) == 0)
            {
                OvsDbApiError("The value of 'method' within JSON string is invalid.\n");
                return OVS_FAILED_STATUS;
            }

            json_t* params = json_object_get(msg, "params");
            if(params == NULL || json_is_array(params) == 0)
            {
                OvsDbApiError("The value of 'params' with monitor update is invalid.\n");
                return OVS_FAILED_STATUS;
            }

            status = ovsdb_parse_params(params);
        }
        else
        {
            status = receipt_list_process(json_string_value(id), json_object_get(msg, "result"));
        }
    } while(bytes_read != size);

    return status;
}

static OVS_STATUS ovsdb_parse_params(json_t* params)
{
    size_t index;
    json_t* value;
    const char * uuid = NULL;
    OVS_STATUS status = OVS_SUCCESS_STATUS;

    if(params == NULL)
    {
        OvsDbApiError("params passed to ovsdb_parse_params is NULL.\n");
        return OVS_FAILED_STATUS;
    }

    //TODO: Add check to make sure params is an array

    json_array_foreach(params, index, value)
    {
        if(index == 0)
        {
            if(value == NULL || json_is_string(value) == 0)
            {
                OvsDbApiError("Unable to get the UUID from the monitor update.\n");
                return OVS_FAILED_STATUS;
            }

            uuid = json_string_value(value);
        }
        else
        {
            if(value == NULL || json_is_object(value) == 0)
            {
                OvsDbApiError("Unable to get the object from the monitor update.\n");
                return OVS_FAILED_STATUS;
            }

            status = ovsdb_parse_monitor_update(uuid, value);
            if(status != OVS_SUCCESS_STATUS)
            {
                OvsDbApiError("Failed to parse monitor update.\n");
            }
        }
    }

    return status;
}

static OVS_STATUS ovsdb_parse_monitor_update(const char * uuid, json_t* update)
{
    OVS_STATUS status = OVS_FAILED_STATUS;
    OvsAgent_Table_Config table_config = { 0 };
    char* str_json = NULL;

    str_json = json_dumps(update, JSON_COMPACT);
    OvsDbApiDebug("%s UUID: %s, update: %s\n", __func__, uuid, str_json);
    free(str_json);

    if(json_object_size(update) != 1)
    {
        OvsDbApiError("The size of the update object isn't 1?\n");
        return OVS_FAILED_STATUS;
    }

    //TODO: Make new abstraction to handle 'old' updates and things, not just 'new'

    void* iter = json_object_iter(update);
    const char * table_name = json_object_iter_key(iter);
    json_t* table_value = json_object_iter_value(iter);

    void* iter2 = json_object_iter(table_value);
    const char * table_uuid = json_object_iter_key(iter2);
    json_t* outer_uuid = json_object_iter_value(iter2);

    json_t* new = json_object_get(outer_uuid, "new");
    if (!new)
    {   // handles and discards an update of type 'old' i.e. delete requests
        OvsDbApiWarning("%s Ignoring monitor update!\n", __func__);
        return OVS_SUCCESS_STATUS;
    }

    strncpy(table_config.uuid, table_uuid, sizeof(table_config.uuid));
    table_config.uuid[ sizeof(table_config.uuid)-1 ] = '\0';

    status = parse_table(table_name, new, (Rdkb_Table_Config*) &table_config);
    if(status != OVS_SUCCESS_STATUS)
    {
        OvsDbApiError("Failed to parse local structure.\n");
        return status;
    }

    status = mon_list_process(uuid, (Rdkb_Table_Config*)&table_config);
    if(status != OVS_SUCCESS_STATUS)
    {
        OvsDbApiError("Failed to process monitor update.\n");
        goto cleanup;
    }

cleanup:
    free(table_config.config);
    return status;
}
