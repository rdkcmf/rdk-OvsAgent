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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "common/OvsAgentLog.h"
#include "OvsAction/ovs_action.h"
#include "OvsAction/syscfg.h"
#include "OvsAgentSsp/cosa_api.h"

#define MIN_IP_ADDR_STR_LEN 7  // 4+3
#define MAX_IP_ADDR_STR_LEN 15 // 12+3
#define MAC_ADDR_STR_LEN    17 // 12+5

typedef struct ovs_action_config
{
    OVS_DEVICE_MODEL modelNum; // RDKB Device Model Number
} ovs_action_config;

static ovs_action_config g_ovsActionConfig = {0};

static bool SetModelNum(const char * model_num, ovs_action_config * config)
{
    bool rtn = false;
    if (!model_num || !config)
    {
        return false;
    }
    if (strcmp(model_num, "CGM4140COM") == 0)
    {
        config->modelNum = OVS_CGM4140COM_MODEL;
        rtn = true;
    }
    else if (strcmp(model_num, "TG3482G") == 0)
    {
        config->modelNum = OVS_TG3482G_MODEL;
        rtn = true;
    }
    else if (strcmp(model_num, "CGM4981COM") == 0)
    {
        config->modelNum = OVS_CGM4981COM_MODEL;
        rtn = true;
    }
    else if (strcmp(model_num, "CGM4331COM") == 0)
    {
        config->modelNum = OVS_CGM4981COM_MODEL;
        rtn = true;
    }
    else if (strcmp(model_num, "SR300") == 0)
    {
        config->modelNum = OVS_SR300_MODEL;
        rtn = true;
    }
    else
    {
        config->modelNum = OVS_UNKNOWN_MODEL;
        OvsActionError("Failed to lookup Model Number!\n");
    }
    return rtn;
}

//WAR: Intel Puma7 Switch hal has a dependency in using the brctl to bringup
//the interfaces. As a temporary solution intermediate create dummy Linux bridge
//create ethernet ports using the dummy bridge and move it to required OVS bridge later
//TODO: Temporary solution, long term plan is to accomodate the brcompact ARRISXB6-12186
//TODO: Add unit tests

static OVS_STATUS ovs_setupEthSwitch(const char *ifname)
{
    char cmd[250] = {0};
    char ethpath[64] = {0};

    OvsActionDebug("%s Special handling of eth switch port %s\n",
        __func__, ifname);

    if (!ifname)
    {
        return OVS_FAILED_STATUS;
    }
    strcpy(ethpath, (strcmp(ifname, PUMA7_ETH1_NAME) == 0) ?
        PUMA7_ETH1_PATH : PUMA7_ETH2_PATH);

    OvsActionDebug("%s Cmd: /bin/vlan_util add_group dummy 100\n", __func__);
    system("/bin/vlan_util add_group dummy 100");

    OvsActionDebug("%s Cmd: /bin/vlan_util add_interface dummy eth_0\n", __func__);
    system("/bin/vlan_util add_interface dummy eth_0");

    OvsActionDebug("%s Cmd: /bin/vlan_util add_interface dummy eth_1\n", __func__);
    system("/bin/vlan_util add_interface dummy eth_1");

    memset(cmd, 0, sizeof (cmd));
    snprintf(cmd, 250, "brctl delif dummy %s", ifname);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    OvsActionDebug("%s Cmd: ifconfig dummy down; brctl delbr dummy\n", __func__);
    system("ifconfig dummy down; brctl delbr dummy");

    if ((access(ethpath, F_OK) == 0))
    {
       OvsActionDebug("%s: Ethernet Port %s exists\n", __func__, ifname);
    }
    else
    {
       OvsActionDebug("%s: Ethernet Port %s doesn't exist\n", __func__, ifname);
    }
    return OVS_SUCCESS_STATUS;
}

// TODO: Uses syscfg to get the IP Address for lan0 interface.
// In the future, remove utopia and syscfg dependency and have
// this info sent by the caller. i.e. bridgeUtils
static bool GetLan0IPAddess(char *ip_address, size_t size)
{
    size_t len = 0;
    if (!ip_address)
    {
        return false;
    }
    if (SyscfgGet(LAN0_IP_ADDR_SYSCFG_PARAM_NAME, ip_address, size) == 0)
    {
        len = strlen(ip_address);
        return ((len >= MIN_IP_ADDR_STR_LEN && len <= MAX_IP_ADDR_STR_LEN) ?
            true : false);
    }
    OvsActionError("%s failed to get Lan0 IP Address!\n", __func__);
    return false;
}

static bool ipv4_to_cidr_notation(char *ip, char *cidr, size_t cidr_size)
{
    if (!ip || !cidr)
    {
        return false;
    }

    const char * endptr = strrchr(ip, '.');
    if (!endptr)
    {
        OvsActionError("%s failed to find last '.' in IP %s!\n", __func__, ip);
        return false;
    }
    size_t size = (size_t) (endptr - ip);
    if (size==0 || size+5>=cidr_size)
    { // no space for current str and ".0/24" and null char
        OvsActionError("%s invalid size detected for CIDR string!\n", __func__);
        return false;
    }
    strncpy(cidr, ip, size);
    strcat(cidr, ".0/24");
    OvsActionInfo("%s IP Addr=%s, size=%zu, len=%zu, CIDR=%s, size=%zu, len=%zu\n",
        __func__, ip, size, strlen(ip), cidr, cidr_size, strlen(cidr));
    return true;
}

// RDKB-35546
static OVS_STATUS ovs_setup_gre_offloading(Gateway_Config * req)
{
    char cmd[250] = {0};
    char ip_address[16] = {0};
    char cidr[32] = {0};
    char associatedBridge[] = "brlan0";

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if (!GetLan0IPAddess(ip_address, sizeof(ip_address)))
    {
        return OVS_FAILED_STATUS;
    }

    if (!ipv4_to_cidr_notation(ip_address, cidr, sizeof(cidr)))
    {
        return OVS_FAILED_STATUS;
    }

    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s \"table=0, priority=50, ip_dst=%s, ct_state=-trk, tcp\"",
        associatedBridge, cidr);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    memset(cmd, 0, sizeof (cmd));
    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s \"table=0, priority=50, ip_dst=%s, ct_state=+trk+new, tcp\"",
        associatedBridge, cidr);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    memset(cmd, 0, sizeof (cmd));
    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s \"table=0, priority=50, ip_dst=%s, ct_state=-trk, udp\"",
        associatedBridge, cidr);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    memset(cmd, 0, sizeof (cmd));
    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s \"table=0, priority=50, ip_dst=%s, ct_state=+trk+new, udp\"",
        associatedBridge, cidr);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    if (req->if_cmd == OVS_IF_UP_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ovs-ofctl add-flow %s \"table=0, priority=50, ip_dst=%s, ct_state=-trk, tcp, actions=ct(table=0)\"",
            associatedBridge, cidr);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);

        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ovs-ofctl add-flow %s \"table=0, priority=50, ip_dst=%s, ct_state=+trk+new, tcp, actions=ct(commit),NORMAL\"",
            associatedBridge, cidr);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);

        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ovs-ofctl add-flow %s \"table=0, priority=50, ip_dst=%s, ct_state=-trk, udp, actions=ct(table=0)\"",
            associatedBridge, cidr);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);

        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ovs-ofctl add-flow %s \"table=0, priority=50, ip_dst=%s, ct_state=+trk+new, udp, actions=ct(commit),NORMAL\"",
            associatedBridge, cidr);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }
    return OVS_SUCCESS_STATUS;
}

// Fix for TCXB6-9125, ARRISXB6-12373, TCXB7-4051 and TCXB8-473
static OVS_STATUS ovs_setup_admin_gui_access(Gateway_Config * req)
{
    char cmd[250] = {0};
    size_t len = 0;
    char ip_address[16] = {0};
    char mac_address[18] = {0};
    FILE *fp = NULL;

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    /* Fix for TCXB6-9125. In Bridge Mode only, setup two static Open Flow flows for
       ARP & IP packets, so ethernet clients can access the http://10.0.0.1 webpage.*/
    if (!GetLan0IPAddess(ip_address, sizeof(ip_address)))
    {
        OvsActionDebug("%s lan0 IP Address=%s, len=%zu\n",
            __func__, ip_address, strlen(ip_address));
        return OVS_FAILED_STATUS;
    }

    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s arp,nw_dst=%s/32",
        req->parent_bridge, ip_address);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    memset(cmd, 0, sizeof (cmd));
    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s ip,nw_dst=%s/32",
        req->parent_bridge, ip_address);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    if (req->if_cmd != OVS_BR_REMOVE_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "cat /sys/class/net/lan0/address");
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        fp = popen(cmd, "r");
        if (!fp)
        {
            return OVS_FAILED_STATUS;
        }
        while (fgets(mac_address, sizeof(mac_address), fp) != NULL)
        {
            break;
        }
        pclose(fp);
        fp = NULL;

        len = strlen(mac_address);
        OvsActionDebug("%s lan0 Mac Address %s len: %zu\n", __func__,
            mac_address, len);
        if (len != MAC_ADDR_STR_LEN)
        {
            OvsActionError("%s Invalid Mac Address len %zu\n", __func__, len);
            return OVS_FAILED_STATUS;
        }
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250,
            "ovs-ofctl add-flow %s arp,nw_dst=%s/32,actions=mod_dl_dst:%s,output:%s",
            req->parent_bridge, ip_address, mac_address, req->if_name);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);

        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250,
            "ovs-ofctl add-flow %s ip,nw_dst=%s/32,actions=mod_dl_dst:%s,output:%s",
            req->parent_bridge, ip_address, mac_address, req->if_name);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }
    return OVS_SUCCESS_STATUS;
}

static bool GetCosaParamValues(char **paramNames, const int numParams,
    int * paramSize, parameterValStruct_t ***paramValues)
{
    char *componentName = NULL;
    char *componentPath = NULL;

    if (!paramNames || !paramSize | (numParams < 1))
    {
        return false;
    }

    OvsActionDebug("%s: Ovs Action Cosa Api Get Param '%s', numParams=%d\n",
        __func__, paramNames[0], numParams);

    if (!Cosa_FindDestComp(paramNames[0], &componentName, &componentPath) ||
        !componentName || !componentPath)
    {
        OvsActionError("Failed to find Dest CCSP component supporting '%s'!\n",
            paramNames[0]);
        return false;
    }
    OvsActionDebug(
        "%s: Ovs Action Cosa Api Found Dest CCSP component %s, Path: %s\n",
        __func__, componentName, componentPath);

    if (!Cosa_GetParamValues(componentName, componentPath, paramNames,
            numParams, paramSize, paramValues) ||
        (*paramSize != numParams) || !(*paramValues)[0])
    {
        OvsActionError(
            "Ovs Action Cosa Api Get Param Values failed for '%s' (size=%d)!\n",
            paramNames[0], *paramSize);
        Cosa_FreeParamValues(*paramSize, *paramValues);
        return false;
    }
    OvsActionDebug("%s: Ovs Action Cosa Api Get Param size=%d, %s=%s, len=%zu\n",
        __func__, *paramSize, ((*paramValues)[0])->parameterName,
        ((*paramValues)[0])->parameterValue,
        strlen(((*paramValues)[0])->parameterValue));
    return true;
}

// TCHXB6-9721 Fix to block MSO UI access from LAN clients in bridge mode.
// Fix for TCXB7-4051 and TCXB8-473
static OVS_STATUS ovs_block_mso_ui_access(Gateway_Config * req)
{
    char *paramNames[] = {"Device.DeviceInfo.X_COMCAST-COM_CM_IP"};
    const int numParams = (int)(sizeof(paramNames)/sizeof(*paramNames));
    parameterValStruct_t **paramValues = NULL;
    int paramSize = 0;
    char cmd[250] = {0};
    size_t paramValueLen = 0;

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if (!GetCosaParamValues(paramNames, numParams, &paramSize, &paramValues) ||
        !paramValues[0]->parameterValue)
    {
        OvsActionError("%s Ovs Action CCSP Get Param Values failed.\n", __func__);
        return OVS_FAILED_STATUS;
    }

    paramValueLen = strlen(paramValues[0]->parameterValue);
    if (paramValueLen == 0)
    {
        OvsActionError("%s failed due to empty Get Param Value length.\n",
        __func__);
        return OVS_FAILED_STATUS;
    }
    OvsActionDebug("%s: Ovs Action Cosa Api Get Param size=%d, %s=%s, len=%zu\n",
        __func__, paramSize, paramValues[0]->parameterName,
        paramValues[0]->parameterValue, paramValueLen);

    snprintf(cmd, 250, "ovs-ofctl --strict del-flows %s ipv6,ipv6_dst=%s/128",
        req->parent_bridge, paramValues[0]->parameterValue);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    if (req->if_cmd != OVS_BR_REMOVE_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250,
            "ovs-ofctl add-flow %s ipv6,ipv6_dst=%s/128,actions=drop",
            req->parent_bridge, paramValues[0]->parameterValue);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    // Free the parameter values
    Cosa_FreeParamValues(paramSize, paramValues);

    return OVS_SUCCESS_STATUS;
}

OVS_STATUS getExistingOvsParentBridge(Gateway_Config * req,
    char * existing_bridge, size_t size, bool * found)
{
    size_t len = 0;
    char cmd[250] = {0};
    char ports[128] = {0};
    FILE *fp = NULL;
    const char * ovsBridgeNotFoundPrefix = "ovs-vsctl:";

    if (!existing_bridge || !found || (size == 0))
    {
        return OVS_FAILED_STATUS;
    }
    *found = false;

    snprintf(cmd, 250, "ovs-vsctl iface-to-br %s", req->if_name);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    fp = popen(cmd, "r");
    if (!fp)
    {
        return OVS_FAILED_STATUS;
    }
    while (fgets(existing_bridge, size, fp) != NULL)
    {
        OvsActionDebug("%s existing parent bridge %s", __func__, existing_bridge);
    }
    pclose(fp);
    fp = NULL;

    /* Remove trailing newline character */
    len = strlen(existing_bridge);
    if (len > 0 && existing_bridge[len-1] == '\n')
    {
        existing_bridge[len-1] = '\0';
    }

    if ((strlen(existing_bridge) > 0) &&
        (strncmp(ovsBridgeNotFoundPrefix, existing_bridge,
            strlen(ovsBridgeNotFoundPrefix)) != 0))
    {
        OvsActionDebug("%s found existing bridge=%s, len=%zu\n",
            __func__, existing_bridge, strlen(existing_bridge));
        *found = true;
    }
    return OVS_SUCCESS_STATUS;
}

OVS_STATUS getExistingLinuxParentBridge(Gateway_Config * req,
    char * existing_bridge, size_t size, bool * found)
{
    size_t len = 0;
    char cmd[250] = {0};
    char line[64] = {0};
    //char ports[128] = {0};
    FILE *fp = NULL;
    char ifpath[128] = {0};
    const char * linuxBridgeFoundPrefix = LINUX_INTERFACE_PREFIX;
    char * bridge = NULL;

    if (!existing_bridge || !found || (size == 0))
    {
        return OVS_FAILED_STATUS;
    }
    *found = false;

    snprintf(ifpath, 128, "%s/%s%s", SYS_CLASS_NET_PATH, req->if_name, LINUX_BRPORT_POSTFIX_PATH);
    if ((access(ifpath, F_OK) != 0))
    {
       OvsActionDebug("%s: Interface Port path %s doesn't exist\n", __func__, ifpath);
       return OVS_SUCCESS_STATUS;
    }

    snprintf(cmd, 250, "cat %s", ifpath);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    fp = popen(cmd, "r");
    if (!fp)
    {
        return OVS_FAILED_STATUS;
    }
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if ((bridge = strstr(line, linuxBridgeFoundPrefix)) != NULL)
        {
            OvsActionDebug("%s found existing parent bridge %s",
                __func__, bridge);
            break;
        }
    }
    pclose(fp);
    fp = NULL;

    if (!bridge)
    {
        OvsActionDebug("%s didn't find existing parent bridge for %s\n",
            __func__, req->if_name);
        return OVS_SUCCESS_STATUS;
    }
    bridge += strlen(linuxBridgeFoundPrefix);

    /* Remove trailing newline character */
    len = strlen(bridge);
    if (len > 0 && bridge[len-1] == '\n')
    {
        bridge[len-1] = '\0';
    }

    strncpy(existing_bridge, bridge, size);
    len = strlen(existing_bridge);
    if (len > 0)
    {
        OvsActionDebug("%s found existing bridge=%s, len=%zu\n",
            __func__, existing_bridge, len);
        *found = true;
    }
    return OVS_SUCCESS_STATUS;
}

OVS_STATUS removeExistingInterfacePort(Gateway_Config * req, bool ovs_enabled)
{
    size_t len = 0;
    char cmd[250] = {0};
    char existingBridge[32] = {0};
    bool removePort = false;
    bool found = false;
    OVS_STATUS status = OVS_SUCCESS_STATUS;

    if (ovs_enabled)
    {
        status = getExistingOvsParentBridge(req, existingBridge,
            sizeof(existingBridge), &found);
    }
    else
    {
        status = getExistingLinuxParentBridge(req, existingBridge,
            sizeof(existingBridge), &found);
    }
    if (status != OVS_SUCCESS_STATUS)
    {
        return status;
    }
    if (!found)
    {
        OvsActionDebug("Port %s is not part of an existing bridge.\n",
            req->if_name);
        return status;
    }

    OvsActionDebug("Port %s already exists as part of bridge %s\n",
        req->if_name, existingBridge);
    if ((strcmp(req->parent_bridge, existingBridge) == 0) &&
        (req->if_cmd == OVS_BR_REMOVE_CMD))
    {   // parent bridge and existing bridge are the SAME and
        // command is to remove the bridge
        removePort = true;
    }
    else if ((strcmp(req->parent_bridge, existingBridge) != 0) &&
        ((req->if_cmd == OVS_IF_UP_CMD) || (req->if_cmd == OVS_BR_REMOVE_CMD)))
    {   // parent bridge and existing bridge are different and
        // command is to bring the interface up or to remove the bridge
        removePort = true;
    }

    if (!removePort)
    {
        return status;
    }

    /* remove the port from its existing bridge */
    memset(cmd, 0, sizeof (cmd));
    if (ovs_enabled)
    {
        snprintf(cmd, 250, "ovs-vsctl del-port %s %s", existingBridge,
            req->if_name);
    }
    else
    {
        snprintf(cmd, 250, "brctl delif %s %s", existingBridge,
            req->if_name);
    }
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    return status;
}

OVS_STATUS configureParentBridge(Gateway_Config * req, bool ovs_enabled)
{
    char cmd[250] = {0};
    char ports[128] = {0};
    FILE *fp = NULL;

    OvsActionDebug("Trying to add a port (that does not exist) to a bridge. Creating it...\n");
//TODO: No need of explcit addition of bridge, though its harmless, every port will retrigger this
    if (ovs_enabled)
    {
        snprintf(cmd, 250, "ovs-vsctl add-br %s", req->parent_bridge);
    }
    else
    {
        snprintf(cmd, 250, "brctl addbr %s", req->parent_bridge);
    }
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    if (req->if_cmd==OVS_IF_UP_CMD || req->if_cmd==OVS_IF_DOWN_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ifconfig %s %s", req->parent_bridge,
            (req->if_cmd==OVS_IF_UP_CMD ? "up" : "down"));
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    memset(cmd, 0, sizeof (cmd));
    if (ovs_enabled)
    {
        snprintf(cmd, 250, "ovs-vsctl add-port %s %s", req->parent_bridge,
            req->if_name);
    }
    else
    {
        snprintf(cmd, 250, "brctl addif %s %s", req->parent_bridge, req->if_name);
    }
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    memset(cmd, 0, sizeof (cmd));
    if (ovs_enabled)
    {
        snprintf(cmd, 250, "ovs-vsctl list-ports %s", req->parent_bridge);
    }
    else
    {
        snprintf(cmd, 250, "brctl show %s", req->parent_bridge);
    }
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    fp = popen(cmd, "r");
    if (!fp)
    {
        return OVS_FAILED_STATUS;
    }
    while (fgets(ports, sizeof(ports), fp) != NULL)
    {
        OvsActionDebug("existing port: %s", ports);
    }
    pclose(fp);
    fp = NULL;

    return OVS_SUCCESS_STATUS;
}

// TODO: replace error return value 1 with right enum value
// TODO: validate the action and then do a return

static OVS_STATUS ovs_modifyParentBridge(Gateway_Config * req)
{
    OVS_STATUS status = OVS_SUCCESS_STATUS;
    bool ovsEnabled = true;

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if ((strcmp(req->parent_bridge, "brlan2") == 0) ||
        (strcmp(req->parent_bridge, "brlan3") == 0) ||
        (strcmp(req->parent_bridge, "brlan4") == 0) ||
        (strcmp(req->parent_bridge, "brlan5") == 0))
    {   // RDKB-36101 Setup Xfinity Wifi bridges using Linux bridge utils and not OVS
        ovsEnabled = false;
    }

    if ((status = removeExistingInterfacePort(req, ovsEnabled)) !=
        OVS_SUCCESS_STATUS)
    {
        return status;
    }

    if (req->if_cmd != OVS_BR_REMOVE_CMD) // TODO: refactor this condition check
    {
        status = configureParentBridge(req, ovsEnabled);
    }
    return status;
}

static OVS_STATUS ovs_createBridge(Gateway_Config * req)
{
    char cmd[250] = {0};
    OVS_STATUS status = OVS_SUCCESS_STATUS;

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if (req->if_cmd == OVS_IF_DELETE_CMD)
    {
        snprintf(cmd, 250, "ovs-vsctl del-br %s", req->if_name);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
        return OVS_SUCCESS_STATUS;
    }

    snprintf(cmd, 250, "ovs-vsctl add-br %s", req->if_name);
    OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
    system(cmd);

    if (strlen(req->inet_addr))
    {
         memset(cmd, 0, sizeof (cmd));
         snprintf(cmd, 250, "ifconfig %s %s netmask %s", req->if_name,
             req->inet_addr, req->netmask);
         OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
         system(cmd);
    }

    if (req->if_cmd==OVS_IF_UP_CMD || req->if_cmd==OVS_IF_DOWN_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ifconfig %s %s", req->if_name,
            (req->if_cmd==OVS_IF_UP_CMD ? "up" : "down"));
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    if (req->mtu)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ifconfig %s mtu %d", req->if_name, req->mtu);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    // setup GRE offloading on TCHXB6
    if ((g_ovsActionConfig.modelNum == OVS_CGM4140COM_MODEL) &&
        (strcmp(req->parent_bridge, "br403") == 0) &&
        (strcmp(req->if_name, "br403") == 0) &&
        (req->if_cmd == OVS_IF_UP_CMD || req->if_cmd == OVS_BR_REMOVE_CMD))
    {
        if ((status = ovs_setup_gre_offloading(req)) != OVS_SUCCESS_STATUS)
        {
            OvsActionError(
                "%s failed to setup GRE Offloading for Bridge %s, Port %s.\n",
                __func__, req->parent_bridge, req->if_name);
            return status;
        }
    }
    return status;
}

static OVS_STATUS ovs_createVlan(Gateway_Config * req)
{
    char cmd[250] = {0};

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if (req->if_cmd == OVS_IF_DELETE_CMD)
    {
        snprintf(cmd, 250, "ip link del %s", req->if_name);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
        return OVS_SUCCESS_STATUS;
    }

    if (strlen(req->parent_ifname))
    {
        snprintf(cmd, 250, "vconfig add %s %d", req->parent_ifname, req->vlan_id);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    if (req->if_cmd==OVS_IF_UP_CMD || req->if_cmd==OVS_IF_DOWN_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ifconfig %s %s", req->if_name,
            (req->if_cmd==OVS_IF_UP_CMD ? "up" : "down"));
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    if (strlen(req->parent_bridge))
    {
        return ovs_modifyParentBridge(req);
    }
    return OVS_SUCCESS_STATUS;
}

static OVS_STATUS ovs_createGRE(Gateway_Config * req)
{
    char cmd[250] = {0};

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if (req->if_cmd == OVS_IF_DELETE_CMD)
    {
        snprintf(cmd, 250, "ip link del %s", req->if_name);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
        return OVS_SUCCESS_STATUS;
    }

    if (strlen(req->parent_ifname) && strlen(req->gre_local_inet_addr) &&
        strlen(req->gre_remote_inet_addr))
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250,
            "ip link add %s type gretap local %s remote %s dev %s tos 1",
            req->if_name, req->gre_local_inet_addr,
            req->gre_remote_inet_addr, req->parent_ifname);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    if (req->if_cmd==OVS_IF_UP_CMD || req->if_cmd==OVS_IF_DOWN_CMD)
    {
        memset(cmd, 0, sizeof (cmd));
        snprintf(cmd, 250, "ifconfig %s %s", req->if_name,
            (req->if_cmd==OVS_IF_UP_CMD ? "up" : "down"));
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    if (strlen(req->parent_bridge))
    {
        return ovs_modifyParentBridge(req);
    }
    return OVS_SUCCESS_STATUS;
}

static OVS_STATUS ovs_addPort(Gateway_Config * req)
{
    char cmd[250] = {0};
    OVS_STATUS status = OVS_SUCCESS_STATUS;

    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    if (req->if_cmd == OVS_IF_DELETE_CMD)
    {
        snprintf(cmd, 250, "ip link del %s", req->if_name);
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
        return OVS_SUCCESS_STATUS;
    }
    else if (req->if_cmd==OVS_IF_UP_CMD || req->if_cmd==OVS_IF_DOWN_CMD)
    {
        //WAR: Special handling in case of Intel Puma7 on Arris XB6
        if ((g_ovsActionConfig.modelNum == OVS_TG3482G_MODEL) &&
            ((strcmp(req->if_name, PUMA7_ETH1_NAME) == 0) ||
                (strcmp(req->if_name, PUMA7_ETH2_NAME) == 0)) &&
            (req->if_cmd == OVS_IF_UP_CMD))
        {
            if ((status = ovs_setupEthSwitch(req->if_name)) != OVS_SUCCESS_STATUS)
            {
                return status;
            }
        }

        snprintf(cmd, 250, "ifconfig %s %s", req->if_name,
            (req->if_cmd==OVS_IF_UP_CMD ? "up" : "down"));
        OvsActionDebug("%s Cmd: %s\n", __func__, cmd);
        system(cmd);
    }

    if (strlen(req->parent_bridge))
    {
        if ((status = ovs_modifyParentBridge(req)) != OVS_SUCCESS_STATUS)
        {
            OvsActionError("%s Error modifying Bridge %s, Port %s, Cmd %d\n",
                __func__, req->parent_bridge, req->if_name, req->if_cmd);
            return status;
        }

        // sets up OpenFlow flows for brlan0's llan0 interface port
        if (((g_ovsActionConfig.modelNum == OVS_CGM4140COM_MODEL) ||
            (g_ovsActionConfig.modelNum == OVS_TG3482G_MODEL) ||
            (g_ovsActionConfig.modelNum == OVS_CGM4331COM_MODEL) ||
            (g_ovsActionConfig.modelNum == OVS_CGM4981COM_MODEL)) &&
            (strcmp(req->parent_bridge, "brlan0") == 0) &&
            (strcmp(req->if_name, LLAN0_ETH_NAME) == 0))
        {
            if ((status = ovs_setup_admin_gui_access(req)) != OVS_SUCCESS_STATUS)
            {
                OvsActionError(
                    "%s failed to setup Admin GUI access flows for Bridge %s, Port %s.\n",
                    __func__, req->parent_bridge, req->if_name);
            }
            if ((status = ovs_block_mso_ui_access(req)) != OVS_SUCCESS_STATUS)
            {
                OvsActionError(
                    "%s failed to block MSO UI access flows for Bridge %s, Port %s.\n",
                    __func__, req->parent_bridge, req->if_name);
            }
        }
    }
    return status;
}

OVS_STATUS ovs_action_init()
{
    const char * model_num = getenv(MODEL_NUM);
    if (!SetModelNum(model_num, &g_ovsActionConfig))
    {
        OvsActionError("%s failed to set Model Number.\n", __func__);
        return OVS_FAILED_STATUS;
    }

    /* Initialize Sysconfig*/
    if (SyscfgInit() != 0)
    {
        OvsActionError("%s failed to initialize syscfg.\n", __func__);
        return OVS_FAILED_STATUS;
    }

    OvsActionInfo("%s successfully initialized for Model Number %d (%s)\n",
        __func__, g_ovsActionConfig.modelNum, model_num);
    return OVS_SUCCESS_STATUS;
}

OVS_STATUS ovs_action_gateway_config(Gateway_Config * req)
{
    OVS_STATUS status = OVS_SUCCESS_STATUS;
    if (!req)
    {
        return OVS_FAILED_STATUS;
    }

    switch (req->if_type)
    {
        case OVS_BRIDGE_IF_TYPE:
          OvsActionDebug("OvsAction: Bridge %s, Cmd: %d\n",
              req->if_name, req->if_cmd);
          status = ovs_createBridge(req);
          break;
        case OVS_VLAN_IF_TYPE:
          OvsActionDebug("OvsAction: VLAN %s, Cmd: %d\n",
              req->if_name, req->if_cmd);
          status = ovs_createVlan(req);
          break;
        case OVS_GRE_IF_TYPE:
          OvsActionDebug("OvsAction: GRE %s, Cmd: %d\n",
              req->if_name, req->if_cmd);
          status = ovs_createGRE(req);
          break;
        default:
          OvsActionDebug("OvsAction: If Type: %d for Port %s, Cmd: %d\n",
              req->if_type, req->if_name, req->if_cmd);
          status = ovs_addPort(req);
          break;
    }
    return status;
}

OVS_STATUS ovs_action_feedback(Feedback * req)
{
    if (!req)
    {
        return OVS_FAILED_STATUS;
    }
    return OVS_SUCCESS_STATUS;
}
