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

#include <jansson.h>
#include <string.h>
#include "common/OvsAgentLog.h"
#include "OvsDbApi/OvsDbDefs.h"

typedef OvsDb_Base_Receipt*(*receipt_parser)(json_t* receipt);
static OvsDb_Base_Receipt* insert_receipt_parser(json_t* receipt);
static OvsDb_Base_Receipt* monitor_receipt_parser(json_t* receipt);
static OvsDb_Base_Receipt* monitor_cancel_receipt_parser(json_t* receipt);

const static receipt_parser parser_lkup_tbl[] = {
    [OVSDB_INSERT_RECEIPT_ID] = insert_receipt_parser,
    [OVSDB_MONITOR_RECEIPT_ID] = monitor_receipt_parser,
    [OVSDB_MONITOR_CANCEL_RECEIPT_ID] = monitor_cancel_receipt_parser
};

OvsDb_Base_Receipt* ovsdb_parse_result(OVSDB_RECEIPT_ID type, json_t* receipt)
{
    //TODO: Add a check to make sure type is within range
    //if(type >

    if(receipt == NULL){
        OvsDbApiError("Cannot call ovsdb_parse_result with NULL result.\n");
        return NULL;
    }

    return parser_lkup_tbl[type](receipt);
}

OvsDb_Base_Receipt* insert_receipt_parser(json_t* receipt)
{
    if(receipt == NULL){
        OvsDbApiError("Cannot call insert_receipt_parser with NULL receipt.\n");
        return NULL;
    }

    if(json_is_array(receipt) == 0)
    {
        OvsDbApiError("receipt object is not an array\n");
        return NULL;
    }

    json_t* json_obj = json_array_get(receipt, 0);
    json_t* json_uuid = json_object_get(json_obj, "uuid");

    if(json_is_array(json_uuid) == 0)
    {
        OvsDbApiError("json_uuid is not an array\n");
        return NULL;
    }

    json_t* json_uuid2 = json_array_get(json_uuid, 0);
    if( json_uuid2 == NULL || json_is_string(json_uuid2) == 0)
    {
        OvsDbApiError("json_uuid2 is not correct.\n");
        return NULL;
    }

    if( strcmp( json_string_value(json_uuid2), "uuid") != 0)
    {
        OvsDbApiError("json_uuid2 is not 'uuid'.\n");
        return NULL;
    }

    OvsDb_Insert_Receipt* insert_receipt = (OvsDb_Insert_Receipt*) malloc(sizeof(OvsDb_Insert_Receipt));
    if (!insert_receipt)
    {
        OvsDbApiError("%s memory allocation failed!\n", __func__);
        return NULL;
    }

    memset(insert_receipt, 0, sizeof(OvsDb_Insert_Receipt));
    insert_receipt->receipt_id = OVSDB_INSERT_RECEIPT_ID;
    strcpy(insert_receipt->uuid, json_string_value( json_array_get(json_uuid,1)));

    return (OvsDb_Base_Receipt*)insert_receipt;
}

OvsDb_Base_Receipt* monitor_receipt_parser(json_t* receipt)
{
    if(receipt == NULL){
        OvsDbApiError("Cannot call monitor_receipt_parser with NULL result.\n");
        return NULL;
    }

    //TODO: Implement this to actually parse monitor receipts
    OvsDb_Monitor_Receipt* update_receipt = (OvsDb_Monitor_Receipt*) malloc(sizeof(OvsDb_Monitor_Receipt));
    if (!update_receipt)
    {
        OvsDbApiError("%s memory allocation failed!\n", __func__);
        return NULL;
    }

    memset(update_receipt, 0, sizeof(OvsDb_Monitor_Receipt));
    update_receipt->receipt_id = OVSDB_MONITOR_RECEIPT_ID;
    update_receipt->update_count = 0;
    return (OvsDb_Base_Receipt*)update_receipt;
}

OvsDb_Base_Receipt* monitor_cancel_receipt_parser(json_t* receipt)
{
    if(receipt == NULL){
        OvsDbApiError("Cannot call monitor_cancel_receipt_parser with NULL receipt.\n");
        return NULL;
    }

    OvsDb_Monitor_Cancel_Receipt* mon_cancel_receipt = (OvsDb_Monitor_Cancel_Receipt*) malloc(sizeof(OvsDb_Monitor_Cancel_Receipt));
    if (!mon_cancel_receipt)
    {
        OvsDbApiError("%s memory allocation failed!\n", __func__);
        return NULL;
    }

    memset(mon_cancel_receipt, 0, sizeof(OvsDb_Monitor_Cancel_Receipt));
    mon_cancel_receipt->is_successful = true;
    mon_cancel_receipt->receipt_id = OVSDB_MONITOR_CANCEL_RECEIPT_ID;
    return (OvsDb_Base_Receipt*)mon_cancel_receipt;
}
