#!/bin/sh
#
# Copyright 2020 Comcast Cable Communications Management, LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
#


OPENSYNC_ENABLE=`syscfg get opensync_enable`
if [ $# -eq 0 ]; then
 echo "No arguments passed"
 exit 0
else
 if [ "$OPENSYNC_ENABLE" == "true" ];then
  /usr/opensync/scripts/managers.init $@
 else
  /usr/plume/scripts/managers.init $@
 fi
fi
