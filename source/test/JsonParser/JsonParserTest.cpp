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
#include <string>
#include "test/mocks/MockJsonParser.h"

extern "C" {
#include "OvsDbApi/OvsDbApi.h"
#include "common/OvsAgentLog.h"
#include "OvsDbApi/ovsdb_parser.h"
}

using ::testing::_;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Pointee;

JsonParserMock * g_jsonParserMock = NULL;

class JsonParserTestFixture : public ::testing::Test
{
    protected:
        JsonParserMock mockedJsonParser;

        JsonParserTestFixture()
        {
            g_jsonParserMock = &mockedJsonParser;
        }
        virtual ~JsonParserTestFixture()
        {
            g_jsonParserMock = NULL;
        }

        virtual void SetUp()
        {
            OvsDbApiInfo("%s %s %s\n", __func__,
                ::testing::UnitTest::GetInstance()->current_test_info()->test_case_name(),
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        }

        virtual void TearDown()
        {
            OvsDbApiInfo("%s %s %s\n", __func__,
                ::testing::UnitTest::GetInstance()->current_test_info()->test_case_name(),
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        }

        static void SetUpTestCase()
        {
            OvsDbApiInfo("%s %s\n", __func__,
                ::testing::UnitTest::GetInstance()->current_test_case()->name());

        }

        static void TearDownTestCase()
        {
            OvsDbApiInfo("%s %s\n", __func__,
                ::testing::UnitTest::GetInstance()->current_test_case()->name());
        }
};

MATCHER_P(JsonMatch, n, "")
{
    return json_equal(arg, n);
}

MATCHER_P(RdkbTableMatch, n, "")
{
    bool ret = true;
    if(n->table.id != arg->table.id)
    {
        return false;
    }

    switch(n->table.id)
    {
        case OVS_FEEDBACK_TABLE:
            {
                Feedback * n_fb = (Feedback*) n->config;
                Feedback * args_fb = (Feedback*) arg->config;
                if(n_fb->status != args_fb->status)
                {
                    ret = false;
                }
                else if(strcmp(n_fb->req_uuid, args_fb->req_uuid) != 0)
                {
                    ret = false;
                }
                break;
            }
        default:
            ret = false;
            break;
    }

    return ret;
}

TEST_F(JsonParserTestFixture, insert_receipt_test)
{
    const std::string example_receipt = "{\"id\":\"2003\",\"result\":[{\"uuid\":[\"uuid\",\"f8ecdbd4-0c07-42a9-91ff-819bbf2f4196\"]}],\"error\":null}";
    json_t* expected_result = json_loads("[{\"uuid\":[\"uuid\",\"f8ecdbd4-0c07-42a9-91ff-819bbf2f4196\"]}]", 0, NULL);
    const char* expected_id = "2003";

    EXPECT_CALL(*g_jsonParserMock, receipt_list_process(StrEq(expected_id), JsonMatch(expected_result)))
        .WillOnce(Return(OVS_SUCCESS_STATUS));

    EXPECT_EQ(OVS_SUCCESS_STATUS, ovsdb_parse_msg(example_receipt.c_str(), example_receipt.length()));
}

TEST_F(JsonParserTestFixture, monitor_update_test)
{
    const std::string example_update = "{\"id\":null,\"method\":\"update\",\"params\":[\"2\",{\"Feedback\":{\"e4bb63ed-988a-4951-848f-d8374f4972fd\":{\"new\":{\"_version\":[\"uuid\",\"570e2da2-2adb-4dd7-bc2d-944d065c53c0\"],\"req_uuid\":\"18ca5061-c9ef-42dc-9579-f9e3167a1ae7\",\"status\":0}}}}]}";
    const char * expected_uuid = "2";
    Feedback fb = {OVS_SUCCESS_STATUS, 0};
    strncpy(fb.req_uuid, "18ca5061-c9ef-42dc-9579-f9e3167a1ae7", sizeof(fb.req_uuid));

    Rdkb_Table_Config expected_table_config;
    expected_table_config.table.id = OVS_FEEDBACK_TABLE;
    expected_table_config.config = &fb;

    EXPECT_CALL(*g_jsonParserMock, mon_list_process(StrEq(expected_uuid), RdkbTableMatch(&expected_table_config)))
        .WillOnce(Return(OVS_SUCCESS_STATUS));

    EXPECT_EQ(OVS_SUCCESS_STATUS, ovsdb_parse_msg(example_update.c_str(), example_update.length()));
}
