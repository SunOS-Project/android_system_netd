/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RouteController.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fib_rules.h>
#include <net/if.h>
#include <netdutils/InternetAddresses.h>
#include <private/android_filesystem_config.h>
#include <sys/stat.h>

#include <map>

#include "DummyNetwork.h"
#include "Fwmark.h"
#include "NetdConstants.h"
#include "NetlinkCommands.h"
#include "TcUtils.h"

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include "log/log.h"
#include "netid_client.h"
#include "netutils/ifc.h"

using android::base::StartsWith;
using android::base::StringPrintf;
using android::base::WriteStringToFile;
using android::netdutils::IPPrefix;

namespace android::net {

auto RouteController::iptablesRestoreCommandFunction = execIptablesRestoreCommand;
auto RouteController::ifNameToIndexFunction = if_nametoindex;
// BEGIN CONSTANTS --------------------------------------------------------------------------------

const uint32_t ROUTE_TABLE_LOCAL_NETWORK  = 97;
const uint32_t ROUTE_TABLE_LEGACY_NETWORK = 98;
const uint32_t ROUTE_TABLE_LEGACY_SYSTEM  = 99;

const char* const ROUTE_TABLE_NAME_LOCAL_NETWORK  = "local_network";
const char* const ROUTE_TABLE_NAME_LEGACY_NETWORK = "legacy_network";
const char* const ROUTE_TABLE_NAME_LEGACY_SYSTEM  = "legacy_system";

const char* const ROUTE_TABLE_NAME_LOCAL = "local";
const char* const ROUTE_TABLE_NAME_MAIN  = "main";

const char* const RouteController::LOCAL_MANGLE_INPUT = "routectrl_mangle_INPUT";

const IPPrefix V4_LOCAL_PREFIXES[] = {
        IPPrefix::forString("169.254.0.0/16"),  // Link Local
        IPPrefix::forString("100.64.0.0/10"),   // CGNAT
        IPPrefix::forString("10.0.0.0/8"),      // RFC1918
        IPPrefix::forString("172.16.0.0/12"),   // RFC1918
        IPPrefix::forString("192.168.0.0/16")   // RFC1918
};

const uint8_t AF_FAMILIES[] = {AF_INET, AF_INET6};

const uid_t UID_ROOT = 0;
const uint32_t FWMARK_NONE = 0;
const uint32_t MASK_NONE = 0;
const char* const IIF_LOOPBACK = "lo";
const char* const IIF_NONE = nullptr;
const char* const OIF_NONE = nullptr;
const bool ACTION_ADD = true;
const bool ACTION_DEL = false;
const bool MODIFY_NON_UID_BASED_RULES = true;

const mode_t RT_TABLES_MODE = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // mode 0644, rw-r--r--

// Avoids "non-constant-expression cannot be narrowed from type 'unsigned int' to 'unsigned short'"
// warnings when using RTA_LENGTH(x) inside static initializers (even when x is already uint16_t).
static constexpr uint16_t U16_RTA_LENGTH(uint16_t x) {
    return RTA_LENGTH(x);
}

// These are practically const, but can't be declared so, because they are used to initialize
// non-const pointers ("void* iov_base") in iovec arrays.
rtattr FRATTR_PRIORITY  = { U16_RTA_LENGTH(sizeof(uint32_t)),           FRA_PRIORITY };
rtattr FRATTR_TABLE     = { U16_RTA_LENGTH(sizeof(uint32_t)),           FRA_TABLE };
rtattr FRATTR_FWMARK    = { U16_RTA_LENGTH(sizeof(uint32_t)),           FRA_FWMARK };
rtattr FRATTR_FWMASK    = { U16_RTA_LENGTH(sizeof(uint32_t)),           FRA_FWMASK };
rtattr FRATTR_UID_RANGE = { U16_RTA_LENGTH(sizeof(fib_rule_uid_range)), FRA_UID_RANGE };

rtattr RTATTR_TABLE     = { U16_RTA_LENGTH(sizeof(uint32_t)),           RTA_TABLE };
rtattr RTATTR_OIF       = { U16_RTA_LENGTH(sizeof(uint32_t)),           RTA_OIF };
rtattr RTATTR_PRIO      = { U16_RTA_LENGTH(sizeof(uint32_t)),           RTA_PRIORITY };

// One or more nested attributes in the RTA_METRICS attribute.
rtattr RTATTRX_MTU      = { U16_RTA_LENGTH(sizeof(uint32_t)),           RTAX_MTU};
constexpr size_t RTATTRX_MTU_SIZE = RTA_SPACE(sizeof(uint32_t));

// The RTA_METRICS attribute itself.
constexpr size_t RTATTR_METRICS_SIZE = RTATTRX_MTU_SIZE;
rtattr RTATTR_METRICS   = { U16_RTA_LENGTH(RTATTR_METRICS_SIZE),         RTA_METRICS };

uint8_t PADDING_BUFFER[RTA_ALIGNTO] = {0, 0, 0, 0};

constexpr bool EXPLICIT = true;
constexpr bool IMPLICIT = false;

// END CONSTANTS ----------------------------------------------------------------------------------

static const char* actionName(uint16_t action) {
    static const char *ops[4] = {"adding", "deleting", "getting", "???"};
    return ops[action % 4];
}

static const char* familyName(uint8_t family) {
    switch (family) {
        case AF_INET: return "IPv4";
        case AF_INET6: return "IPv6";
        default: return "???";
    }
}

static void maybeModifyQdiscClsact(const char* interface, bool add);

static uint32_t getRouteTableIndexFromGlobalRouteTableIndex(uint32_t index, bool local) {
    // The local table is
    // "global table - ROUTE_TABLE_OFFSET_FROM_INDEX + ROUTE_TABLE_OFFSET_FROM_INDEX_FOR_LOCAL"
    const uint32_t localTableOffset = RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX_FOR_LOCAL -
                                      RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX;
    return local ? index + localTableOffset : index;
}

// Caller must hold sInterfaceToTableLock.
uint32_t RouteController::getRouteTableForInterfaceLocked(const char* interface, bool local) {
    // If we already know the routing table for this interface name, use it.
    // This ensures we can remove rules and routes for an interface that has been removed,
    // or has been removed and re-added with a different interface index.
    //
    // The caller is responsible for ensuring that an interface is never added to a network
    // until it has been removed from any network it was previously in. This ensures that
    // if the same interface disconnects and then reconnects with a different interface ID
    // when the reconnect happens the interface will not be in the map, and the code will
    // determine the new routing table from the interface ID, below.
    //
    // sInterfaceToTable stores the *global* routing table for the interface, and the local table is
    // "global table - ROUTE_TABLE_OFFSET_FROM_INDEX + ROUTE_TABLE_OFFSET_FROM_INDEX_FOR_LOCAL"
    auto iter = sInterfaceToTable.find(interface);
    if (iter != sInterfaceToTable.end()) {
        return getRouteTableIndexFromGlobalRouteTableIndex(iter->second, local);
    }

    uint32_t index = RouteController::ifNameToIndexFunction(interface);
    if (index == 0) {
        ALOGE("cannot find interface %s: %s", interface, strerror(errno));
        return RT_TABLE_UNSPEC;
    }
    index += RouteController::ROUTE_TABLE_OFFSET_FROM_INDEX;
    sInterfaceToTable[interface] = index;
    return getRouteTableIndexFromGlobalRouteTableIndex(index, local);
}

uint32_t RouteController::getIfIndex(const char* interface) {
    std::lock_guard lock(sInterfaceToTableLock);

    auto iter = sInterfaceToTable.find(interface);
    if (iter == sInterfaceToTable.end()) {
        ALOGE("getIfIndex: cannot find interface %s", interface);
        return 0;
    }

    // For interfaces that are not in the local network, the routing table is always the interface
    // index plus ROUTE_TABLE_OFFSET_FROM_INDEX. But for interfaces in the local network, there's no
    // way to know the interface index from this table. Return 0 here so callers of this method do
    // not get confused.
    // TODO: stop calling this method from any caller that only wants interfaces in client mode.
    int ifindex = iter->second;
    if (ifindex == ROUTE_TABLE_LOCAL_NETWORK) {
        return 0;
    }

    return ifindex - ROUTE_TABLE_OFFSET_FROM_INDEX;
}

uint32_t RouteController::getRouteTableForInterface(const char* interface, bool local) {
    std::lock_guard lock(sInterfaceToTableLock);
    return getRouteTableForInterfaceLocked(interface, local);
}

void addTableName(uint32_t table, const std::string& name, std::string* contents) {
    char tableString[UINT32_STRLEN];
    snprintf(tableString, sizeof(tableString), "%u", table);
    *contents += tableString;
    *contents += " ";
    *contents += name;
    *contents += "\n";
}

// Doesn't return success/failure as the file is optional; it's okay if we fail to update it.
void RouteController::updateTableNamesFile() {
    std::string contents;

    addTableName(RT_TABLE_LOCAL, ROUTE_TABLE_NAME_LOCAL, &contents);
    addTableName(RT_TABLE_MAIN,  ROUTE_TABLE_NAME_MAIN,  &contents);

    addTableName(ROUTE_TABLE_LOCAL_NETWORK,  ROUTE_TABLE_NAME_LOCAL_NETWORK,  &contents);
    addTableName(ROUTE_TABLE_LEGACY_NETWORK, ROUTE_TABLE_NAME_LEGACY_NETWORK, &contents);
    addTableName(ROUTE_TABLE_LEGACY_SYSTEM,  ROUTE_TABLE_NAME_LEGACY_SYSTEM,  &contents);

    std::lock_guard lock(sInterfaceToTableLock);
    for (const auto& [ifName, table] : sInterfaceToTable) {
        if (table <= ROUTE_TABLE_OFFSET_FROM_INDEX) {
            continue;
        }
        addTableName(table, ifName, &contents);
        // Add table for the local route of the network. It's expected to be used for excluding the
        // local traffic in the VPN network.
        // Start from ROUTE_TABLE_OFFSET_FROM_INDEX_FOR_LOCAL plus with the interface table index.
        uint32_t offset = ROUTE_TABLE_OFFSET_FROM_INDEX_FOR_LOCAL - ROUTE_TABLE_OFFSET_FROM_INDEX;
        addTableName(offset + table, ifName + INTERFACE_LOCAL_SUFFIX, &contents);
    }

    if (!WriteStringToFile(contents, RT_TABLES_PATH, RT_TABLES_MODE, AID_SYSTEM, AID_WIFI)) {
        ALOGE("failed to write to %s (%s)", RT_TABLES_PATH, strerror(errno));
        return;
    }
}

// Returns 0 on success or negative errno on failure.
int padInterfaceName(const char* input, char* name, size_t* length, uint16_t* padding) {
    if (!input) {
        *length = 0;
        *padding = 0;
        return 0;
    }
    *length = strlcpy(name, input, IFNAMSIZ) + 1;
    if (*length > IFNAMSIZ) {
        ALOGE("interface name too long (%zu > %u)", *length, IFNAMSIZ);
        return -ENAMETOOLONG;
    }
    *padding = RTA_SPACE(*length) - RTA_LENGTH(*length);
    return 0;
}

// Adds or removes a routing rule for IPv4 and IPv6.
//
// + If |table| is non-zero, the rule points at the specified routing table. Otherwise, the table is
//   unspecified. An unspecified table is not allowed when creating an FR_ACT_TO_TBL rule.
// + If |mask| is non-zero, the rule matches the specified fwmark and mask. Otherwise, |fwmark| is
//   ignored.
// + If |iif| is non-NULL, the rule matches the specified incoming interface.
// + If |oif| is non-NULL, the rule matches the specified outgoing interface.
// + If |uidStart| and |uidEnd| are not INVALID_UID, the rule matches packets from UIDs in that
//   range (inclusive). Otherwise, the rule matches packets from all UIDs.
//
// Returns 0 on success or negative errno on failure.
[[nodiscard]] static int modifyIpRule(uint16_t action, int32_t priority, uint8_t ruleType,
                                      uint32_t table, uint32_t fwmark, uint32_t mask,
                                      const char* iif, const char* oif, uid_t uidStart,
                                      uid_t uidEnd) {
    if (priority < 0) {
        ALOGE("invalid IP-rule priority %d", priority);
        return -ERANGE;
    }

    // Ensure that if you set a bit in the fwmark, it's not being ignored by the mask.
    if (fwmark & ~mask) {
        ALOGE("mask 0x%x does not select all the bits set in fwmark 0x%x", mask, fwmark);
        return -ERANGE;
    }

    // Interface names must include exactly one terminating NULL and be properly padded, or older
    // kernels will refuse to delete rules.
    char iifName[IFNAMSIZ], oifName[IFNAMSIZ];
    size_t iifLength, oifLength;
    uint16_t iifPadding, oifPadding;
    if (int ret = padInterfaceName(iif, iifName, &iifLength, &iifPadding)) {
        return ret;
    }
    if (int ret = padInterfaceName(oif, oifName, &oifLength, &oifPadding)) {
        return ret;
    }

    // Either both start and end UID must be specified, or neither.
    if ((uidStart == INVALID_UID) != (uidEnd == INVALID_UID)) {
        ALOGE("incompatible start and end UIDs (%u vs %u)", uidStart, uidEnd);
        return -EUSERS;
    }

    bool isUidRule = (uidStart != INVALID_UID);

    // Assemble a rule request and put it in an array of iovec structures.
    fib_rule_hdr rule = {
        .action = ruleType,
        // Note that here we're implicitly setting rule.table to 0. When we want to specify a
        // non-zero table, we do this via the FRATTR_TABLE attribute.
    };

    // Don't ever create a rule that looks up table 0, because table 0 is the local table.
    // It's OK to specify a table ID of 0 when deleting a rule, because that doesn't actually select
    // table 0, it's a wildcard that matches anything.
    if (table == RT_TABLE_UNSPEC && rule.action == FR_ACT_TO_TBL && action != RTM_DELRULE) {
        ALOGE("RT_TABLE_UNSPEC only allowed when deleting rules");
        return -ENOTUNIQ;
    }

    rtattr fraIifName = { U16_RTA_LENGTH(iifLength), FRA_IIFNAME };
    rtattr fraOifName = { U16_RTA_LENGTH(oifLength), FRA_OIFNAME };
    struct fib_rule_uid_range uidRange = { uidStart, uidEnd };

    iovec iov[] = {
        { nullptr,              0 },
        { &rule,             sizeof(rule) },
        { &FRATTR_PRIORITY,  sizeof(FRATTR_PRIORITY) },
        { &priority,         sizeof(priority) },
        { &FRATTR_TABLE,     table != RT_TABLE_UNSPEC ? sizeof(FRATTR_TABLE) : 0 },
        { &table,            table != RT_TABLE_UNSPEC ? sizeof(table) : 0 },
        { &FRATTR_FWMARK,    mask ? sizeof(FRATTR_FWMARK) : 0 },
        { &fwmark,           mask ? sizeof(fwmark) : 0 },
        { &FRATTR_FWMASK,    mask ? sizeof(FRATTR_FWMASK) : 0 },
        { &mask,             mask ? sizeof(mask) : 0 },
        { &FRATTR_UID_RANGE, isUidRule ? sizeof(FRATTR_UID_RANGE) : 0 },
        { &uidRange,         isUidRule ? sizeof(uidRange) : 0 },
        { &fraIifName,       iif != IIF_NONE ? sizeof(fraIifName) : 0 },
        { iifName,           iifLength },
        { PADDING_BUFFER,    iifPadding },
        { &fraOifName,       oif != OIF_NONE ? sizeof(fraOifName) : 0 },
        { oifName,           oifLength },
        { PADDING_BUFFER,    oifPadding },
    };

    uint16_t flags = (action == RTM_NEWRULE) ? NETLINK_RULE_CREATE_FLAGS : NETLINK_REQUEST_FLAGS;
    for (size_t i = 0; i < ARRAY_SIZE(AF_FAMILIES); ++i) {
        rule.family = AF_FAMILIES[i];
        if (int ret = sendNetlinkRequest(action, flags, iov, ARRAY_SIZE(iov), nullptr)) {
            if (!(action == RTM_DELRULE && ret == -ENOENT && priority == RULE_PRIORITY_TETHERING)) {
                // Don't log when deleting a tethering rule that's not there. This matches the
                // behaviour of clearTetheringRules, which ignores ENOENT in this case.
                ALOGE("Error %s %s rule: %s", actionName(action), familyName(rule.family),
                      strerror(-ret));
            }
            return ret;
        }
    }

    return 0;
}

[[nodiscard]] static int modifyIpRule(uint16_t action, int32_t priority, uint32_t table,
                                      uint32_t fwmark, uint32_t mask, const char* iif,
                                      const char* oif, uid_t uidStart, uid_t uidEnd) {
    return modifyIpRule(action, priority, FR_ACT_TO_TBL, table, fwmark, mask, iif, oif, uidStart,
                        uidEnd);
}

[[nodiscard]] static int modifyIpRule(uint16_t action, int32_t priority, uint32_t table,
                                      uint32_t fwmark, uint32_t mask) {
    return modifyIpRule(action, priority, table, fwmark, mask, IIF_NONE, OIF_NONE, INVALID_UID,
                        INVALID_UID);
}

// Adds or deletes an IPv4 or IPv6 route.
// Returns 0 on success or negative errno on failure.
int modifyIpRoute(uint16_t action, uint16_t flags, uint32_t table, const char* interface,
                  const char* destination, const char* nexthop, uint32_t mtu, uint32_t priority) {
    // At least the destination must be non-null.
    if (!destination) {
        ALOGE("null destination");
        return -EFAULT;
    }

    // Parse the prefix.
    uint8_t rawAddress[sizeof(in6_addr)];
    uint8_t family;
    uint8_t prefixLength;
    int rawLength = parsePrefix(destination, &family, rawAddress, sizeof(rawAddress),
                                &prefixLength);
    if (rawLength < 0) {
        ALOGE("parsePrefix failed for destination %s (%s)", destination, strerror(-rawLength));
        return rawLength;
    }

    if (static_cast<size_t>(rawLength) > sizeof(rawAddress)) {
        ALOGE("impossible! address too long (%d vs %zu)", rawLength, sizeof(rawAddress));
        return -ENOBUFS;  // Cannot happen; parsePrefix only supports IPv4 and IPv6.
    }

    uint8_t type = RTN_UNICAST;
    uint32_t ifindex;
    uint8_t rawNexthop[sizeof(in6_addr)];

    if (nexthop && !strcmp(nexthop, "unreachable")) {
        type = RTN_UNREACHABLE;
        // 'interface' is likely non-NULL, as the caller (modifyRoute()) likely used it to lookup
        // the table number. But it's an error to specify an interface ("dev ...") or a nexthop for
        // unreachable routes, so nuke them. (IPv6 allows them to be specified; IPv4 doesn't.)
        interface = OIF_NONE;
        nexthop = nullptr;
    } else if (nexthop && !strcmp(nexthop, "throw")) {
        type = RTN_THROW;
        interface = OIF_NONE;
        nexthop = nullptr;
    } else {
        // If an interface was specified, find the ifindex.
        if (interface != OIF_NONE) {
            ifindex = RouteController::ifNameToIndexFunction(interface);

            if (!ifindex) {
                ALOGE("cannot find interface %s", interface);
                return -ENODEV;
            }
        }

        // If a nexthop was specified, parse it as the same family as the prefix.
        if (nexthop && inet_pton(family, nexthop, rawNexthop) <= 0) {
            ALOGE("inet_pton failed for nexthop %s", nexthop);
            return -EINVAL;
        }
    }

    // Assemble a rtmsg and put it in an array of iovec structures.
    rtmsg route = {
            .rtm_family = family,
            .rtm_dst_len = prefixLength,
            .rtm_protocol = RTPROT_STATIC,
            .rtm_scope = static_cast<uint8_t>(nexthop ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK),
            .rtm_type = type,
    };

    rtattr rtaDst     = { U16_RTA_LENGTH(rawLength), RTA_DST };
    rtattr rtaGateway = { U16_RTA_LENGTH(rawLength), RTA_GATEWAY };

    iovec iov[] = {
            {nullptr, 0},
            {&route, sizeof(route)},
            {&RTATTR_TABLE, sizeof(RTATTR_TABLE)},
            {&table, sizeof(table)},
            {&rtaDst, sizeof(rtaDst)},
            {rawAddress, static_cast<size_t>(rawLength)},
            {&RTATTR_OIF, interface != OIF_NONE ? sizeof(RTATTR_OIF) : 0},
            {&ifindex, interface != OIF_NONE ? sizeof(ifindex) : 0},
            {&rtaGateway, nexthop ? sizeof(rtaGateway) : 0},
            {rawNexthop, nexthop ? static_cast<size_t>(rawLength) : 0},
            {&RTATTR_METRICS, mtu != 0 ? sizeof(RTATTR_METRICS) : 0},
            {&RTATTRX_MTU, mtu != 0 ? sizeof(RTATTRX_MTU) : 0},
            {&mtu, mtu != 0 ? sizeof(mtu) : 0},
            {&RTATTR_PRIO, priority != 0 ? sizeof(RTATTR_PRIO) : 0},
            {&priority, priority != 0 ? sizeof(priority) : 0},
    };

    // Allow creating multiple link-local routes in the same table, so we can make IPv6
    // work on all interfaces in the local_network table.
    if (family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(reinterpret_cast<in6_addr*>(rawAddress))) {
        flags &= ~NLM_F_EXCL;
    }

    int ret = sendNetlinkRequest(action, flags, iov, ARRAY_SIZE(iov), nullptr);
    if (ret) {
        ALOGE("Error %s route %s -> %s %s to table %u: %s",
              actionName(action), destination, nexthop, interface, table, strerror(-ret));
    }
    return ret;
}

// An iptables rule to mark incoming packets on a network with the netId of the network.
//
// This is so that the kernel can:
// + Use the right fwmark for (and thus correctly route) replies (e.g.: TCP RST, ICMP errors, ping
//   replies, SYN-ACKs, etc).
// + Mark sockets that accept connections from this interface so that the connection stays on the
//   same interface.
int modifyIncomingPacketMark(unsigned netId, const char* interface, Permission permission,
                             bool add) {
    Fwmark fwmark;

    fwmark.netId = netId;
    fwmark.explicitlySelected = true;
    fwmark.protectedFromVpn = true;
    fwmark.permission = permission;

    const uint32_t mask = Fwmark::getUidBillingMask() | Fwmark::getIngressCpuWakeupMask();

    std::string cmd = StringPrintf(
        "%s %s -i %s -j MARK --set-mark 0x%x/0x%x", add ? "-A" : "-D",
        RouteController::LOCAL_MANGLE_INPUT, interface, fwmark.intValue, ~mask);
    if (RouteController::iptablesRestoreCommandFunction(V4V6, "mangle", cmd, nullptr) != 0) {
        ALOGE("failed to change iptables rule that sets incoming packet mark");
        return -EREMOTEIO;
    }

    return 0;
}

// A rule to route responses to the local network forwarded via the VPN.
//
// When a VPN is in effect, packets from the local network to upstream networks are forwarded into
// the VPN's tunnel interface. When the VPN forwards the responses, they emerge out of the tunnel.
[[nodiscard]] static int modifyVpnOutputToLocalRule(const char* vpnInterface, bool add) {
    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_VPN_OUTPUT_TO_LOCAL,
                        ROUTE_TABLE_LOCAL_NETWORK, MARK_UNSET, MARK_UNSET, vpnInterface, OIF_NONE,
                        INVALID_UID, INVALID_UID);
}

// A rule to route all traffic from a given set of UIDs to go over the VPN.
//
// Notice that this rule doesn't use the netId. I.e., no matter what netId the user's socket may
// have, if they are subject to this VPN, their traffic has to go through it. Allows the traffic to
// bypass the VPN if the protectedFromVpn bit is set.
[[nodiscard]] static int modifyVpnUidRangeRule(uint32_t table, uid_t uidStart, uid_t uidEnd,
                                               int32_t subPriority, bool secure, bool add,
                                               bool excludeLocalRoutes) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.protectedFromVpn = false;
    mask.protectedFromVpn = true;

    int32_t priority;

    if (secure) {
        priority = RULE_PRIORITY_SECURE_VPN;
    } else {
        priority = excludeLocalRoutes ? RULE_PRIORITY_BYPASSABLE_VPN_LOCAL_EXCLUSION
                                      : RULE_PRIORITY_BYPASSABLE_VPN_NO_LOCAL_EXCLUSION;

        fwmark.explicitlySelected = false;
        mask.explicitlySelected = true;
    }

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, priority + subPriority, table,
                        fwmark.intValue, mask.intValue, IIF_LOOPBACK, OIF_NONE, uidStart, uidEnd);
}

// A rule to allow system apps to send traffic over this VPN even if they are not part of the target
// set of UIDs.
//
// This is needed for DnsProxyListener to correctly resolve a request for a user who is in the
// target set, but where the DnsProxyListener itself is not.
[[nodiscard]] static int modifyVpnSystemPermissionRule(unsigned netId, uint32_t table, bool secure,
                                                       bool add, bool excludeLocalRoutes) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.permission = PERMISSION_SYSTEM;
    mask.permission = PERMISSION_SYSTEM;

    uint32_t priority;

    if (secure) {
        priority = RULE_PRIORITY_SECURE_VPN;
    } else {
        priority = excludeLocalRoutes ? RULE_PRIORITY_BYPASSABLE_VPN_LOCAL_EXCLUSION
                                      : RULE_PRIORITY_BYPASSABLE_VPN_NO_LOCAL_EXCLUSION;
    }

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, priority, table, fwmark.intValue,
                        mask.intValue);
}

// A rule to route traffic based on an explicitly chosen network.
//
// Supports apps that use the multinetwork APIs to restrict their traffic to a network.
//
// Even though we check permissions at the time we set a netId into the fwmark of a socket, we need
// to check it again in the rules here, because a network's permissions may have been updated via
// modifyNetworkPermission().
[[nodiscard]] static int modifyExplicitNetworkRule(unsigned netId, uint32_t table,
                                                   Permission permission, uid_t uidStart,
                                                   uid_t uidEnd, int32_t subPriority, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.explicitlySelected = true;
    mask.explicitlySelected = true;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE,
                        RULE_PRIORITY_EXPLICIT_NETWORK + subPriority, table, fwmark.intValue,
                        mask.intValue, IIF_LOOPBACK, OIF_NONE, uidStart, uidEnd);
}

// A rule to route traffic based on an local network.
//
// Supports apps that send traffic to local IPs without binding to a particular network.
//
[[nodiscard]] static int modifyLocalNetworkRule(uint32_t table, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    if (const int ret = modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_LOCAL_NETWORK,
                                     table, fwmark.intValue, mask.intValue, IIF_NONE, OIF_NONE,
                                     INVALID_UID, INVALID_UID)) {
        return ret;
    }

    fwmark.explicitlySelected = true;
    mask.explicitlySelected = true;

    fwmark.netId = INetd::LOCAL_NET_ID;
    mask.netId = FWMARK_NET_ID_MASK;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_EXPLICIT_NETWORK, table,
                        fwmark.intValue, mask.intValue, IIF_LOOPBACK, OIF_NONE, INVALID_UID,
                        INVALID_UID);
}

// A rule to route traffic based on a chosen outgoing interface.
//
// Supports apps that use SO_BINDTODEVICE or IP_PKTINFO options and the kernel that already knows
// the outgoing interface (typically for link-local communications).
[[nodiscard]] static int modifyOutputInterfaceRules(const char* interface, uint32_t table,
                                                    Permission permission, uid_t uidStart,
                                                    uid_t uidEnd, int32_t subPriority, bool add) {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.permission = permission;
    mask.permission = permission;

    // If this rule does not specify a UID range, then also add a corresponding high-priority rule
    // for root. This covers kernel-originated packets, TEEd packets and any local daemons that open
    // sockets as root.
    if (uidStart == INVALID_UID && uidEnd == INVALID_UID) {
        if (int ret = modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_VPN_OVERRIDE_OIF,
                                   table, FWMARK_NONE, MASK_NONE, IIF_LOOPBACK, interface,
                                   UID_ROOT, UID_ROOT)) {
            return ret;
        }
    }

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE,
                        RULE_PRIORITY_OUTPUT_INTERFACE + subPriority, table, fwmark.intValue,
                        mask.intValue, IIF_LOOPBACK, interface, uidStart, uidEnd);
}

int RouteController::modifyVpnLocalExclusionRule(bool add, const char* physicalInterface) {
    uint32_t table = getRouteTableForInterface(physicalInterface, true /* local */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    fwmark.permission = PERMISSION_NONE;
    mask.permission = PERMISSION_NONE;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_LOCAL_ROUTES, table,
                        fwmark.intValue, mask.intValue, IIF_LOOPBACK, OIF_NONE, INVALID_UID,
                        INVALID_UID);
}

int RouteController::addFixedLocalRoutes(const char* interface) {
    for (size_t i = 0; i < ARRAY_SIZE(V4_FIXED_LOCAL_PREFIXES); ++i) {
        if (int ret = modifyRoute(RTM_NEWROUTE, NETLINK_ROUTE_CREATE_FLAGS, interface,
                                  V4_FIXED_LOCAL_PREFIXES[i], nullptr /* nexthop */,
                                  RouteController::INTERFACE, 0 /* mtu */, 0 /* priority */,
                                  true /* isLocal */)) {
            return ret;
        }
    }

    return 0;
}

// A rule to enable split tunnel VPNs.
//
// If a packet with a VPN's netId doesn't find a route in the VPN's routing table, it's allowed to
// go over the default network, provided it has the permissions required by the default network.
int RouteController::modifyVpnFallthroughRule(uint16_t action, unsigned vpnNetId,
                                              const char* physicalInterface,
                                              Permission permission) {
    uint32_t table = getRouteTableForInterface(physicalInterface, false /* local */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = vpnNetId;
    mask.netId = FWMARK_NET_ID_MASK;

    // This fallthrough rule does not consider allowed UIDs at all, so limit it to system perm.
    permission = PERMISSION_SYSTEM;

    // Do not allow fallthrough when the VPN is explicitly selected. This ensures that DNS queries
    // will not escape the VPN if it is unavailable and in the process of being brought down.
    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(action, RULE_PRIORITY_VPN_FALLTHROUGH, table, fwmark.intValue,
                        mask.intValue);
}

// Add rules to allow legacy routes added through the requestRouteToHost() API.
[[nodiscard]] static int addLegacyRouteRules() {
    Fwmark fwmark;
    Fwmark mask;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    // Rules to allow legacy routes to override the default network.
    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_LEGACY_SYSTEM, ROUTE_TABLE_LEGACY_SYSTEM,
                               fwmark.intValue, mask.intValue)) {
        return ret;
    }
    if (int ret = modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_LEGACY_NETWORK,
                               ROUTE_TABLE_LEGACY_NETWORK, fwmark.intValue, mask.intValue)) {
        return ret;
    }

    fwmark.permission = PERMISSION_SYSTEM;
    mask.permission = PERMISSION_SYSTEM;

    // A rule to allow legacy routes from system apps to override VPNs.
    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_VPN_OVERRIDE_SYSTEM, ROUTE_TABLE_LEGACY_SYSTEM,
                        fwmark.intValue, mask.intValue);
}

// Add rules to lookup the local network when specified explicitly or otherwise.
[[nodiscard]] static int addLocalNetworkRules(unsigned localNetId) {
    if (int ret = modifyExplicitNetworkRule(localNetId, ROUTE_TABLE_LOCAL_NETWORK, PERMISSION_NONE,
                                            INVALID_UID, INVALID_UID,
                                            UidRanges::SUB_PRIORITY_HIGHEST, ACTION_ADD)) {
        return ret;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_LOCAL_NETWORK, ROUTE_TABLE_LOCAL_NETWORK,
                        fwmark.intValue, mask.intValue);
}

/* static */
int RouteController::configureDummyNetwork() {
    const char *interface = DummyNetwork::INTERFACE_NAME;
    uint32_t table = getRouteTableForInterface(interface, false /* local */);
    if (table == RT_TABLE_UNSPEC) {
        // getRouteTableForInterface has already logged an error.
        return -ESRCH;
    }

    ifc_init();
    int ret = ifc_up(interface);
    ifc_close();
    if (ret) {
        ALOGE("Can't bring up %s: %s", interface, strerror(errno));
        return -errno;
    }

    if ((ret = modifyOutputInterfaceRules(interface, table, PERMISSION_NONE, INVALID_UID,
                                          INVALID_UID, UidRanges::SUB_PRIORITY_HIGHEST,
                                          ACTION_ADD))) {
        ALOGE("Can't create oif rules for %s: %s", interface, strerror(-ret));
        return ret;
    }

    if ((ret = modifyIpRoute(RTM_NEWROUTE, NETLINK_ROUTE_CREATE_FLAGS, table, interface,
                             "0.0.0.0/0", nullptr, 0 /* mtu */, 0 /* priority */))) {
        return ret;
    }

    if ((ret = modifyIpRoute(RTM_NEWROUTE, NETLINK_ROUTE_CREATE_FLAGS, table, interface, "::/0",
                             nullptr, 0 /* mtu */, 0 /* priority */))) {
        return ret;
    }

    return 0;
}

// Add an explicit unreachable rule close to the end of the prioriy list to make it clear that
// relying on the kernel-default "from all lookup main" rule at priority 32766 is not intended
// behaviour. We do flush the kernel-default rules at startup, but having an explicit unreachable
// rule will hopefully make things even clearer.
[[nodiscard]] static int addUnreachableRule() {
    return modifyIpRule(RTM_NEWRULE, RULE_PRIORITY_UNREACHABLE, FR_ACT_UNREACHABLE, RT_TABLE_UNSPEC,
                        MARK_UNSET, MARK_UNSET, IIF_NONE, OIF_NONE, INVALID_UID, INVALID_UID);
}

[[nodiscard]] static int modifyLocalNetwork(unsigned netId, const char* interface, bool add) {
    if (int ret = modifyIncomingPacketMark(netId, interface, PERMISSION_NONE, add)) {
        return ret;
    }
    maybeModifyQdiscClsact(interface, add);
    return modifyOutputInterfaceRules(interface, ROUTE_TABLE_LOCAL_NETWORK, PERMISSION_NONE,
                                      INVALID_UID, INVALID_UID, UidRanges::SUB_PRIORITY_HIGHEST,
                                      add);
}

[[nodiscard]] static int modifyUidNetworkRule(unsigned netId, uint32_t table, uid_t uidStart,
                                              uid_t uidEnd, int32_t subPriority, bool add,
                                              bool explicitSelect) {
    if ((uidStart == INVALID_UID) || (uidEnd == INVALID_UID)) {
        ALOGE("modifyUidNetworkRule, invalid UIDs (%u, %u)", uidStart, uidEnd);
        return -EUSERS;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.explicitlySelected = explicitSelect;
    mask.explicitlySelected = true;

    // Access to this network is controlled by UID rules, not permission bits.
    fwmark.permission = PERMISSION_NONE;
    mask.permission = PERMISSION_NONE;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE,
                        explicitSelect ? (RULE_PRIORITY_UID_EXPLICIT_NETWORK + subPriority)
                                       : (RULE_PRIORITY_UID_IMPLICIT_NETWORK + subPriority),
                        table, fwmark.intValue, mask.intValue, IIF_LOOPBACK, OIF_NONE, uidStart,
                        uidEnd);
}

[[nodiscard]] static int modifyUidDefaultNetworkRule(uint32_t table, uid_t uidStart, uid_t uidEnd,
                                                     int32_t subPriority, bool add) {
    if ((uidStart == INVALID_UID) || (uidEnd == INVALID_UID)) {
        ALOGE("modifyUidDefaultNetworkRule, invalid UIDs (%u, %u)", uidStart, uidEnd);
        return -EUSERS;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = NETID_UNSET;
    mask.netId = FWMARK_NET_ID_MASK;

    // Access to this network is controlled by UID rules, not permission bits.
    fwmark.permission = PERMISSION_NONE;
    mask.permission = PERMISSION_NONE;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE,
                        RULE_PRIORITY_UID_DEFAULT_NETWORK + subPriority, table, fwmark.intValue,
                        mask.intValue, IIF_LOOPBACK, OIF_NONE, uidStart, uidEnd);
}

/* static */
int RouteController::modifyPhysicalNetwork(unsigned netId, const char* interface,
                                           const UidRangeMap& uidRangeMap, Permission permission,
                                           bool add, bool modifyNonUidBasedRules, bool local) {
    uint32_t table = getRouteTableForInterface(interface, false /* local */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    for (const auto& [subPriority, uidRanges] : uidRangeMap) {
        for (const UidRangeParcel& range : uidRanges.getRanges()) {
            if (int ret = modifyUidNetworkRule(netId, table, range.start, range.stop, subPriority,
                                               add, EXPLICIT)) {
                return ret;
            }
            if (int ret = modifyUidNetworkRule(netId, table, range.start, range.stop, subPriority,
                                               add, IMPLICIT)) {
                return ret;
            }
            // SUB_PRIORITY_NO_DEFAULT is "special" and does not require a
            // default network rule, see UidRanges.h.
            if (subPriority != UidRanges::SUB_PRIORITY_NO_DEFAULT) {
                if (int ret = modifyUidDefaultNetworkRule(table, range.start, range.stop,
                                                          subPriority, add)) {
                    return ret;
                }

                // Per-UID local network rules must always match per-app default network rules,
                // because their purpose is to allow the UIDs to use the default network for
                // local destinations within it.
                if (int ret = modifyUidLocalNetworkRule(interface, range.start, range.stop, add)) {
                    return ret;
                }
            }
        }
    }

    if (!modifyNonUidBasedRules) {
        // we are done.
        return 0;
    }

    if (int ret = modifyIncomingPacketMark(netId, interface, permission, add)) {
        return ret;
    }
    if (int ret = modifyExplicitNetworkRule(netId, table, permission, INVALID_UID, INVALID_UID,
                                            UidRanges::SUB_PRIORITY_HIGHEST, add)) {
        return ret;
    }
    if (local) {
        if (const int ret = modifyLocalNetworkRule(table, add)) {
            return ret;
        }
    }
    if (int ret = modifyOutputInterfaceRules(interface, table, PERMISSION_SYSTEM, INVALID_UID,
                                             INVALID_UID,
                                             UidRanges::SUB_PRIORITY_HIGHEST, add)) {
        return ret;
    }

    return 0;
}

int RouteController::modifyUidLocalNetworkRule(const char* interface, uid_t uidStart, uid_t uidEnd,
                                               bool add) {
    uint32_t table = getRouteTableForInterface(interface, true /* local */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    if ((uidStart == INVALID_UID) || (uidEnd == INVALID_UID)) {
        ALOGE("modifyUidLocalNetworkRule, invalid UIDs (%u, %u)", uidStart, uidEnd);
        return -EUSERS;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.explicitlySelected = false;
    mask.explicitlySelected = true;

    // Access to this network is controlled by UID rules, not permission bits.
    fwmark.permission = PERMISSION_NONE;
    mask.permission = PERMISSION_NONE;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_UID_LOCAL_ROUTES, table,
                        fwmark.intValue, mask.intValue, IIF_LOOPBACK, OIF_NONE, uidStart, uidEnd);
}

[[nodiscard]] static int modifyUidUnreachableRule(unsigned netId, uid_t uidStart, uid_t uidEnd,
                                                  int32_t subPriority, bool add,
                                                  bool explicitSelect) {
    if ((uidStart == INVALID_UID) || (uidEnd == INVALID_UID)) {
        ALOGE("modifyUidUnreachableRule, invalid UIDs (%u, %u)", uidStart, uidEnd);
        return -EUSERS;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = netId;
    mask.netId = FWMARK_NET_ID_MASK;

    fwmark.explicitlySelected = explicitSelect;
    mask.explicitlySelected = true;

    // Access to this network is controlled by UID rules, not permission bits.
    fwmark.permission = PERMISSION_NONE;
    mask.permission = PERMISSION_NONE;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE,
                        explicitSelect ? (RULE_PRIORITY_UID_EXPLICIT_NETWORK + subPriority)
                                       : (RULE_PRIORITY_UID_IMPLICIT_NETWORK + subPriority),
                        FR_ACT_UNREACHABLE, RT_TABLE_UNSPEC, fwmark.intValue, mask.intValue,
                        IIF_LOOPBACK, OIF_NONE, uidStart, uidEnd);
}

[[nodiscard]] static int modifyUidDefaultUnreachableRule(uid_t uidStart, uid_t uidEnd,
                                                         int32_t subPriority, bool add) {
    if ((uidStart == INVALID_UID) || (uidEnd == INVALID_UID)) {
        ALOGE("modifyUidDefaultUnreachableRule, invalid UIDs (%u, %u)", uidStart, uidEnd);
        return -EUSERS;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = NETID_UNSET;
    mask.netId = FWMARK_NET_ID_MASK;

    // Access to this network is controlled by UID rules, not permission bits.
    fwmark.permission = PERMISSION_NONE;
    mask.permission = PERMISSION_NONE;

    return modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE,
                        RULE_PRIORITY_UID_DEFAULT_UNREACHABLE + subPriority, FR_ACT_UNREACHABLE,
                        RT_TABLE_UNSPEC, fwmark.intValue, mask.intValue, IIF_LOOPBACK, OIF_NONE,
                        uidStart, uidEnd);
}

int RouteController::modifyUnreachableNetwork(unsigned netId, const UidRangeMap& uidRangeMap,
                                              bool add) {
    for (const auto& [subPriority, uidRanges] : uidRangeMap) {
        for (const UidRangeParcel& range : uidRanges.getRanges()) {
            if (int ret = modifyUidUnreachableRule(netId, range.start, range.stop, subPriority, add,
                                                   EXPLICIT)) {
                return ret;
            }
            if (int ret = modifyUidUnreachableRule(netId, range.start, range.stop, subPriority, add,
                                                   IMPLICIT)) {
                return ret;
            }
            if (int ret = modifyUidDefaultUnreachableRule(range.start, range.stop, subPriority,
                                                          add)) {
                return ret;
            }
        }
    }

    return 0;
}

[[nodiscard]] static int modifyRejectNonSecureNetworkRule(const UidRanges& uidRanges, bool add) {
    Fwmark fwmark;
    Fwmark mask;
    fwmark.protectedFromVpn = false;
    mask.protectedFromVpn = true;

    for (const UidRangeParcel& range : uidRanges.getRanges()) {
        if (int ret = modifyIpRule(add ? RTM_NEWRULE : RTM_DELRULE, RULE_PRIORITY_PROHIBIT_NON_VPN,
                                   FR_ACT_PROHIBIT, RT_TABLE_UNSPEC, fwmark.intValue, mask.intValue,
                                   IIF_LOOPBACK, OIF_NONE, range.start, range.stop)) {
            return ret;
        }
    }

    return 0;
}

int RouteController::modifyVirtualNetwork(unsigned netId, const char* interface,
                                          const UidRangeMap& uidRangeMap, bool secure, bool add,
                                          bool modifyNonUidBasedRules, bool excludeLocalRoutes) {
    uint32_t table = getRouteTableForInterface(interface, false /* false */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    for (const auto& [subPriority, uidRanges] : uidRangeMap) {
        for (const UidRangeParcel& range : uidRanges.getRanges()) {
            if (int ret = modifyVpnUidRangeRule(table, range.start, range.stop, subPriority, secure,
                                                add, excludeLocalRoutes)) {
                return ret;
            }
            if (int ret = modifyExplicitNetworkRule(netId, table, PERMISSION_NONE, range.start,
                                                    range.stop, subPriority, add)) {
                return ret;
            }
            if (int ret = modifyOutputInterfaceRules(interface, table, PERMISSION_NONE, range.start,
                                                     range.stop, subPriority, add)) {
                return ret;
            }
        }
    }

    if (modifyNonUidBasedRules) {
        if (int ret = modifyIncomingPacketMark(netId, interface, PERMISSION_NONE, add)) {
            return ret;
        }
        if (int ret = modifyVpnOutputToLocalRule(interface, add)) {
            return ret;
        }
        if (int ret =
                    modifyVpnSystemPermissionRule(netId, table, secure, add, excludeLocalRoutes)) {
            return ret;
        }
        return modifyExplicitNetworkRule(netId, table, PERMISSION_NONE, UID_ROOT, UID_ROOT,
                                         UidRanges::SUB_PRIORITY_HIGHEST, add);
    }

    return 0;
}

int RouteController::modifyDefaultNetwork(uint16_t action, const char* interface,
                                          Permission permission) {
    uint32_t table = getRouteTableForInterface(interface, false /* local */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    Fwmark fwmark;
    Fwmark mask;

    fwmark.netId = NETID_UNSET;
    mask.netId = FWMARK_NET_ID_MASK;

    // This default network rule does not consider allowed UIDs at all, so limit it to system perm.
    permission = PERMISSION_SYSTEM;

    fwmark.permission = permission;
    mask.permission = permission;

    return modifyIpRule(action, RULE_PRIORITY_DEFAULT_NETWORK, table, fwmark.intValue,
                        mask.intValue, IIF_LOOPBACK, OIF_NONE, INVALID_UID, INVALID_UID);
}

int RouteController::modifyTetheredNetwork(uint16_t action, const char* inputInterface,
                                           const char* outputInterface) {
    uint32_t table = getRouteTableForInterface(outputInterface, false /* local */);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    return modifyIpRule(action, RULE_PRIORITY_TETHERING, table, MARK_UNSET, MARK_UNSET,
                        inputInterface, OIF_NONE, INVALID_UID, INVALID_UID);
}

// Adds or removes an IPv4 or IPv6 route to the specified table.
// Returns 0 on success or negative errno on failure.
int RouteController::modifyRoute(uint16_t action, uint16_t flags, const char* interface,
                                 const char* destination, const char* nexthop, TableType tableType,
                                 int mtu, int priority, bool isLocal) {
    uint32_t table;
    switch (tableType) {
        case RouteController::INTERFACE: {
            table = getRouteTableForInterface(interface, isLocal);
            if (table == RT_TABLE_UNSPEC) {
                return -ESRCH;
            }
            break;
        }
        case RouteController::LOCAL_NETWORK: {
            table = ROUTE_TABLE_LOCAL_NETWORK;
            break;
        }
        case RouteController::LEGACY_NETWORK: {
            table = ROUTE_TABLE_LEGACY_NETWORK;
            break;
        }
        case RouteController::LEGACY_SYSTEM: {
            table = ROUTE_TABLE_LEGACY_SYSTEM;
            break;
        }
    }

    int ret = modifyIpRoute(action, flags, table, interface, destination, nexthop, mtu, priority);
    // Trying to add a route that already exists shouldn't cause an error.
    if (ret && !(action == RTM_NEWROUTE && ret == -EEXIST)) {
        return ret;
    }

    return 0;
}

static void maybeModifyQdiscClsact(const char* interface, bool add) {
    // The clsact attaching of v4- tun interface is triggered by ClatdController::maybeStartBpf
    // because the clat is started before the v4- interface is added to the network and the
    // clat startup needs to add {in, e}gress filters.
    // TODO: remove this workaround once v4- tun interface clsact attaching is moved out from
    // ClatdController::maybeStartBpf.
    if (StartsWith(interface, "v4-") && add) return;

    // The interface may have already gone away in the delete case.
    uint32_t ifindex = RouteController::ifNameToIndexFunction(interface);
    if (!ifindex) {
        ALOGE("cannot find interface %s", interface);
        return;
    }

    if (add) {
        if (int ret = tcQdiscAddDevClsact(ifindex)) {
            ALOGE("tcQdiscAddDevClsact(%d[%s]) failure: %s", ifindex, interface, strerror(-ret));
            return;
        }
    } else {
        if (int ret = tcQdiscDelDevClsact(ifindex)) {
            ALOGE("tcQdiscDelDevClsact(%d[%s]) failure: %s", ifindex, interface, strerror(-ret));
            return;
        }
    }

    return;
}

[[nodiscard]] static int clearTetheringRules(const char* inputInterface) {
    int ret = 0;
    while (ret == 0) {
        ret = modifyIpRule(RTM_DELRULE, RULE_PRIORITY_TETHERING, 0, MARK_UNSET, MARK_UNSET,
                           inputInterface, OIF_NONE, INVALID_UID, INVALID_UID);
    }

    if (ret == -ENOENT) {
        return 0;
    } else {
        return ret;
    }
}

uint32_t getRulePriority(const nlmsghdr *nlh) {
    return getRtmU32Attribute(nlh, FRA_PRIORITY);
}

uint32_t getRouteTable(const nlmsghdr *nlh) {
    return getRtmU32Attribute(nlh, RTA_TABLE);
}

[[nodiscard]] static int flushRules() {
    NetlinkDumpFilter shouldDelete = [] (nlmsghdr *nlh) {
        // Don't touch rules at priority 0 because by default they are used for local input.
        return getRulePriority(nlh) != 0;
    };
    return rtNetlinkFlush(RTM_GETRULE, RTM_DELRULE, "rules", shouldDelete);
}

int RouteController::flushRoutes(uint32_t table) {
    NetlinkDumpFilter shouldDelete = [table] (nlmsghdr *nlh) {
        return getRouteTable(nlh) == table;
    };

    return rtNetlinkFlush(RTM_GETROUTE, RTM_DELROUTE, "routes", shouldDelete);
}

int RouteController::flushRoutes(const char* interface) {
    // Try to flush both local and global routing tables.
    //
    // Flush local first because flush global routing tables may erase the sInterfaceToTable map.
    // Then the fake <iface>_local interface will be unable to find the index because the local
    // interface depends physical interface to find the correct index.
    int ret = flushRoutes(interface, true);
    ret |= flushRoutes(interface, false);
    return ret;
}

// Returns 0 on success or negative errno on failure.
int RouteController::flushRoutes(const char* interface, bool local) {
    std::lock_guard lock(sInterfaceToTableLock);

    uint32_t table = getRouteTableForInterfaceLocked(interface, local);
    if (table == RT_TABLE_UNSPEC) {
        return -ESRCH;
    }

    int ret = flushRoutes(table);

    // If we failed to flush routes, the caller may elect to keep this interface around, so keep
    // track of its name.
    // Skip erasing local fake interface since it does not exist in sInterfaceToTable.
    if (ret == 0 && !local) {
        sInterfaceToTable.erase(interface);
    }

    return ret;
}

int RouteController::Init(unsigned localNetId) {
    if (int ret = flushRules()) {
        return ret;
    }
    if (int ret = addLegacyRouteRules()) {
        return ret;
    }
    if (int ret = addLocalNetworkRules(localNetId)) {
        return ret;
    }
    if (int ret = addUnreachableRule()) {
        return ret;
    }
    // Don't complain if we can't add the dummy network, since not all devices support it.
    configureDummyNetwork();

    updateTableNamesFile();
    return 0;
}

int RouteController::addInterfaceToLocalNetwork(unsigned netId, const char* interface) {
    if (int ret = modifyLocalNetwork(netId, interface, ACTION_ADD)) {
        return ret;
    }
    std::lock_guard lock(sInterfaceToTableLock);
    sInterfaceToTable[interface] = ROUTE_TABLE_LOCAL_NETWORK;
    return 0;
}

int RouteController::removeInterfaceFromLocalNetwork(unsigned netId, const char* interface) {
    if (int ret = modifyLocalNetwork(netId, interface, ACTION_DEL)) {
        return ret;
    }
    std::lock_guard lock(sInterfaceToTableLock);
    sInterfaceToTable.erase(interface);
    return 0;
}

int RouteController::addInterfaceToPhysicalNetwork(unsigned netId, const char* interface,
                                                   Permission permission,
                                                   const UidRangeMap& uidRangeMap, bool local) {
    if (int ret = modifyPhysicalNetwork(netId, interface, uidRangeMap, permission, ACTION_ADD,
                                        MODIFY_NON_UID_BASED_RULES, local)) {
        return ret;
    }

    maybeModifyQdiscClsact(interface, ACTION_ADD);
    updateTableNamesFile();

    if (int ret = addFixedLocalRoutes(interface)) {
        return ret;
    }

    return 0;
}

int RouteController::removeInterfaceFromPhysicalNetwork(unsigned netId, const char* interface,
                                                        Permission permission,
                                                        const UidRangeMap& uidRangeMap,
                                                        bool local) {
    if (int ret = modifyPhysicalNetwork(netId, interface, uidRangeMap, permission, ACTION_DEL,
                                        MODIFY_NON_UID_BASED_RULES, local)) {
        return ret;
    }

    if (int ret = flushRoutes(interface)) {
        return ret;
    }

    if (int ret = clearTetheringRules(interface)) {
        return ret;
    }

    maybeModifyQdiscClsact(interface, ACTION_DEL);
    updateTableNamesFile();
    return 0;
}

int RouteController::addInterfaceToVirtualNetwork(unsigned netId, const char* interface,
                                                  bool secure, const UidRangeMap& uidRangeMap,
                                                  bool excludeLocalRoutes) {
    if (int ret = modifyVirtualNetwork(netId, interface, uidRangeMap, secure, ACTION_ADD,
                                       MODIFY_NON_UID_BASED_RULES, excludeLocalRoutes)) {
        return ret;
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::removeInterfaceFromVirtualNetwork(unsigned netId, const char* interface,
                                                       bool secure, const UidRangeMap& uidRangeMap,
                                                       bool excludeLocalRoutes) {
    if (int ret = modifyVirtualNetwork(netId, interface, uidRangeMap, secure, ACTION_DEL,
                                       MODIFY_NON_UID_BASED_RULES, excludeLocalRoutes)) {
        return ret;
    }
    if (int ret = flushRoutes(interface)) {
        return ret;
    }
    updateTableNamesFile();
    return 0;
}

int RouteController::modifyPhysicalNetworkPermission(unsigned netId, const char* interface,
                                                     Permission oldPermission,
                                                     Permission newPermission, bool local) {
    // Physical network rules either use permission bits or UIDs, but not both.
    // So permission changes don't affect any UID-based rules.
    UidRangeMap emptyUidRangeMap;
    // Add the new rules before deleting the old ones, to avoid race conditions.
    if (int ret = modifyPhysicalNetwork(netId, interface, emptyUidRangeMap, newPermission,
                                        ACTION_ADD, MODIFY_NON_UID_BASED_RULES, local)) {
        return ret;
    }
    return modifyPhysicalNetwork(netId, interface, emptyUidRangeMap, oldPermission, ACTION_DEL,
                                 MODIFY_NON_UID_BASED_RULES, local);
}

int RouteController::addUsersToRejectNonSecureNetworkRule(const UidRanges& uidRanges) {
    return modifyRejectNonSecureNetworkRule(uidRanges, true);
}

int RouteController::removeUsersFromRejectNonSecureNetworkRule(const UidRanges& uidRanges) {
    return modifyRejectNonSecureNetworkRule(uidRanges, false);
}

int RouteController::addUsersToVirtualNetwork(unsigned netId, const char* interface, bool secure,
                                              const UidRangeMap& uidRangeMap,
                                              bool excludeLocalRoutes) {
    return modifyVirtualNetwork(netId, interface, uidRangeMap, secure, ACTION_ADD,
                                !MODIFY_NON_UID_BASED_RULES, excludeLocalRoutes);
}

int RouteController::removeUsersFromVirtualNetwork(unsigned netId, const char* interface,
                                                   bool secure, const UidRangeMap& uidRangeMap,
                                                   bool excludeLocalRoutes) {
    return modifyVirtualNetwork(netId, interface, uidRangeMap, secure, ACTION_DEL,
                                !MODIFY_NON_UID_BASED_RULES, excludeLocalRoutes);
}

int RouteController::addInterfaceToDefaultNetwork(const char* interface, Permission permission) {
    return modifyDefaultNetwork(RTM_NEWRULE, interface, permission);
}

int RouteController::removeInterfaceFromDefaultNetwork(const char* interface,
                                                       Permission permission) {
    return modifyDefaultNetwork(RTM_DELRULE, interface, permission);
}

bool RouteController::isWithinIpv4LocalPrefix(const char* dst) {
    for (IPPrefix addr : V4_LOCAL_PREFIXES) {
        if (addr.contains(IPPrefix::forString(dst))) {
            return true;
        }
    }
    return false;
}

bool RouteController::isLocalRoute(TableType tableType, const char* destination,
                                   const char* nexthop) {
    IPPrefix prefix = IPPrefix::forString(destination);
    return nexthop == nullptr && tableType == RouteController::INTERFACE &&
           // Skip default route to prevent network being modeled as point-to-point interfaces.
           ((prefix.family() == AF_INET6 && prefix != IPPrefix::forString("::/0")) ||
            // Skip adding non-target local network range.
            (prefix.family() == AF_INET && isWithinIpv4LocalPrefix(destination)));
}

int RouteController::addRoute(const char* interface, const char* destination, const char* nexthop,
                              TableType tableType, int mtu, int priority) {
    if (int ret = modifyRoute(RTM_NEWROUTE, NETLINK_ROUTE_CREATE_FLAGS, interface, destination,
                              nexthop, tableType, mtu, priority, false /* isLocal */)) {
        return ret;
    }

    if (isLocalRoute(tableType, destination, nexthop)) {
        return modifyRoute(RTM_NEWROUTE, NETLINK_ROUTE_CREATE_FLAGS, interface, destination,
                           nexthop, tableType, mtu, priority, true /* isLocal */);
    }

    return 0;
}

int RouteController::removeRoute(const char* interface, const char* destination,
                                 const char* nexthop, TableType tableType, int priority) {
    if (int ret = modifyRoute(RTM_DELROUTE, NETLINK_REQUEST_FLAGS, interface, destination, nexthop,
                              tableType, 0 /* mtu */, priority, false /* isLocal */)) {
        return ret;
    }

    if (isLocalRoute(tableType, destination, nexthop)) {
        return modifyRoute(RTM_DELROUTE, NETLINK_REQUEST_FLAGS, interface, destination, nexthop,
                           tableType, 0 /* mtu */, priority, true /* isLocal */);
    }
    return 0;
}

int RouteController::updateRoute(const char* interface, const char* destination,
                                 const char* nexthop, TableType tableType, int mtu) {
    if (int ret = modifyRoute(RTM_NEWROUTE, NETLINK_ROUTE_REPLACE_FLAGS, interface, destination,
                              nexthop, tableType, mtu, 0 /* priority */, false /* isLocal */)) {
        return ret;
    }

    if (isLocalRoute(tableType, destination, nexthop)) {
        return modifyRoute(RTM_NEWROUTE, NETLINK_ROUTE_REPLACE_FLAGS, interface, destination,
                           nexthop, tableType, mtu, 0 /* priority */, true /* isLocal */);
    }
    return 0;
}

int RouteController::enableTethering(const char* inputInterface, const char* outputInterface) {
    return modifyTetheredNetwork(RTM_NEWRULE, inputInterface, outputInterface);
}

int RouteController::disableTethering(const char* inputInterface, const char* outputInterface) {
    return modifyTetheredNetwork(RTM_DELRULE, inputInterface, outputInterface);
}

int RouteController::addVirtualNetworkFallthrough(unsigned vpnNetId, const char* physicalInterface,
                                                  Permission permission) {
    if (int ret = modifyVpnFallthroughRule(RTM_NEWRULE, vpnNetId, physicalInterface, permission)) {
        return ret;
    }

    return 0;
}

int RouteController::removeVirtualNetworkFallthrough(unsigned vpnNetId,
                                                     const char* physicalInterface,
                                                     Permission permission) {
    if (int ret = modifyVpnFallthroughRule(RTM_DELRULE, vpnNetId, physicalInterface, permission)) {
        return ret;
    }

    return 0;
}

int RouteController::addUsersToPhysicalNetwork(unsigned netId, const char* interface,
                                               const UidRangeMap& uidRangeMap, bool local) {
    return modifyPhysicalNetwork(netId, interface, uidRangeMap, PERMISSION_NONE, ACTION_ADD,
                                 !MODIFY_NON_UID_BASED_RULES, local);
}

int RouteController::removeUsersFromPhysicalNetwork(unsigned netId, const char* interface,
                                                    const UidRangeMap& uidRangeMap, bool local) {
    return modifyPhysicalNetwork(netId, interface, uidRangeMap, PERMISSION_NONE, ACTION_DEL,
                                 !MODIFY_NON_UID_BASED_RULES, local);
}

int RouteController::addUsersToUnreachableNetwork(unsigned netId, const UidRangeMap& uidRangeMap) {
    return modifyUnreachableNetwork(netId, uidRangeMap, ACTION_ADD);
}

int RouteController::removeUsersFromUnreachableNetwork(unsigned netId,
                                                       const UidRangeMap& uidRangeMap) {
    return modifyUnreachableNetwork(netId, uidRangeMap, ACTION_DEL);
}

// Protects sInterfaceToTable.
std::mutex RouteController::sInterfaceToTableLock;
std::map<std::string, uint32_t> RouteController::sInterfaceToTable;

}  // namespace android::net
