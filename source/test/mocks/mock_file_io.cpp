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

#include "test/mocks/mock_file_io.h"

extern FileIOMock * g_fileIOMock;   /* This is just a declaration! The actual mock
                                       obj is defined globally in the test file. */

// Mock Method
extern "C" char * fgets(char * str, int n, FILE * stream)
{
    if (!g_fileIOMock)
    {
        return NULL;
    }
    return g_fileIOMock->fgets(str, n, stream);
}

extern "C" FILE * popen(const char * command, const char * type)
{
    if (!g_fileIOMock)
    {
        return NULL;
    }
    return g_fileIOMock->popen(command, type);
}

extern "C" int pclose(FILE * stream)
{
    if (!g_fileIOMock)
    {
        return 0;
    }
    return g_fileIOMock->pclose(stream);
}
