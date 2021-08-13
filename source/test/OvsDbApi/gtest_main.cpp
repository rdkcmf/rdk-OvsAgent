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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

extern "C" {
#include "common/OvsAgentLog.h"
}

#define GTEST_REPORT_FILEPATH   "/tmp/OvsDbApi_gtest_report.xml"
#define GTEST_REPORT_FILEPATH_SIZE 128

class OvsDbApiEnvironment : public ::testing::Environment
{
    public:
        virtual ~OvsDbApiEnvironment()
        {
        }
        virtual void SetUp()
        {
            printf("%s Environment\n", __func__);
            if (open_log(OVS_AGENT_API_LOG_FILE, "OvsDbApiTest"))
            {
                set_log_level(LOG_DEBUG_LEVEL);
            }
        }
        virtual void TearDown()
        {
            printf("%s Environment\n", __func__);
            close_log();
        }
};

GTEST_API_ int main(int argc, char *argv[])
{
    char filePath[GTEST_REPORT_FILEPATH_SIZE] = {0}; // Test Results Full File Path

    snprintf(filePath, GTEST_REPORT_FILEPATH_SIZE, "xml:%s",
        GTEST_REPORT_FILEPATH);
    ::testing::GTEST_FLAG(output) = filePath;

    ::testing::InitGoogleMock(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new OvsDbApiEnvironment());
    return RUN_ALL_TESTS();
}
