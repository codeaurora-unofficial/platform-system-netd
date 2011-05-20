/*
 * Copyright (C) 2008 The Android Open Source Project
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

// #define LOG_NDEBUG 0

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <linux/if.h>

#define LOG_TAG "CommandListener"

#include <cutils/log.h>
#include <netutils/ifc.h>
#include <sysutils/SocketClient.h>

#include "CommandListener.h"
#include "ResponseCode.h"
#include "ThrottleController.h"
#include "BandwidthController.h"

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#include "SecondaryTableController.h"

#ifdef QCOM_WLAN
#include "qsap_api.h"
#endif

TetherController *CommandListener::sTetherCtrl = NULL;
NatController *CommandListener::sNatCtrl = NULL;
PppController *CommandListener::sPppCtrl = NULL;
PanController *CommandListener::sPanCtrl = NULL;
SoftapController *CommandListener::sSoftapCtrl = NULL;
BandwidthController * CommandListener::sBandwidthCtrl = NULL;
ResolverController *CommandListener::sResolverCtrl = NULL;
RouteController *CommandListener::sRouteCtrl = NULL;
SecondaryTableController *CommandListener::sSecondaryTableCtrl = NULL;

CommandListener::CommandListener() :
                 FrameworkListener("netd") {
    registerCmd(new InterfaceCmd());
    registerCmd(new IpFwdCmd());
    registerCmd(new TetherCmd());
    registerCmd(new NatCmd());
    registerCmd(new ListTtysCmd());
    registerCmd(new PppdCmd());
    registerCmd(new PanCmd());
    registerCmd(new SoftapCmd());
    registerCmd(new BandwidthControlCmd());
    registerCmd(new ResolverCmd());
    registerCmd(new RouteCmd());

    if (!sSecondaryTableCtrl)
        sSecondaryTableCtrl = new SecondaryTableController();
    if (!sTetherCtrl)
        sTetherCtrl = new TetherController();
    if (!sNatCtrl)
        sNatCtrl = new NatController(sSecondaryTableCtrl);
    if (!sPppCtrl)
        sPppCtrl = new PppController();
    if (!sPanCtrl)
        sPanCtrl = new PanController();
    if (!sSoftapCtrl)
        sSoftapCtrl = new SoftapController();
    if (!sBandwidthCtrl)
        sBandwidthCtrl = new BandwidthController();
    if (!sResolverCtrl)
        sResolverCtrl = new ResolverController();
    if (!sRouteCtrl)
        sRouteCtrl = new RouteController();
}

CommandListener::InterfaceCmd::InterfaceCmd() :
                 NetdCommand("interface") {
}

int CommandListener::writeFile(const char *path, const char *value, int size) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOGE("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }

    if (write(fd, value, size) != size) {
        LOGE("Failed to write %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int CommandListener::InterfaceCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "list")) {
        DIR *d;
        struct dirent *de;

        if (!(d = opendir("/sys/class/net"))) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to open sysfs dir", true);
            return 0;
        }

        while((de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            cli->sendMsg(ResponseCode::InterfaceListResult, de->d_name, false);
        }
        closedir(d);
        cli->sendMsg(ResponseCode::CommandOkay, "Interface list completed", false);
        return 0;
    } else if (!strcmp(argv[1], "readrxcounter")) {
        if (argc != 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: interface readrxcounter <interface>", false);
            return 0;
        }
        unsigned long rx = 0, tx = 0;
        if (readInterfaceCounters(argv[2], &rx, &tx)) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to read counters", true);
            return 0;
        }

        char *msg;
        asprintf(&msg, "%lu", rx);
        cli->sendMsg(ResponseCode::InterfaceRxCounterResult, msg, false);
        free(msg);

        return 0;
    } else if (!strcmp(argv[1], "readtxcounter")) {
        if (argc != 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: interface readtxcounter <interface>", false);
            return 0;
        }
        unsigned long rx = 0, tx = 0;
        if (readInterfaceCounters(argv[2], &rx, &tx)) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to read counters", true);
            return 0;
        }

        char *msg = NULL;
        asprintf(&msg, "%lu", tx);
        cli->sendMsg(ResponseCode::InterfaceTxCounterResult, msg, false);
        free(msg);
        return 0;
    } else if (!strcmp(argv[1], "getthrottle")) {
        if (argc != 4 || (argc == 4 && (strcmp(argv[3], "rx") && (strcmp(argv[3], "tx"))))) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: interface getthrottle <interface> <rx|tx>", false);
            return 0;
        }
        int val = 0;
        int rc = 0;
        int voldRc = ResponseCode::InterfaceRxThrottleResult;

        if (!strcmp(argv[3], "rx")) {
            rc = ThrottleController::getInterfaceRxThrottle(argv[2], &val);
        } else {
            rc = ThrottleController::getInterfaceTxThrottle(argv[2], &val);
            voldRc = ResponseCode::InterfaceTxThrottleResult;
        }
        if (rc) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to get throttle", true);
        } else {
            char *msg = NULL;
            asprintf(&msg, "%u", val);
            cli->sendMsg(voldRc, msg, false);
            free(msg);
            return 0;
        }
        return 0;
    } else if (!strcmp(argv[1], "setthrottle")) {
        if (argc != 5) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: interface setthrottle <interface> <rx_kbps> <tx_kbps>", false);
            return 0;
        }
        if (ThrottleController::setInterfaceThrottle(argv[2], atoi(argv[3]), atoi(argv[4]))) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to set throttle", true);
        } else {
            cli->sendMsg(ResponseCode::CommandOkay, "Interface throttling set", false);
        }
        return 0;
    } else {
        /*
         * These commands take a minimum of 3 arguments
         */
        if (argc < 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
            return 0;
        }

        //     0       1       2        3          4           5     6      7
        // interface route add/remove iface default/secondary dest prefix gateway
        if (!strcmp(argv[1], "route")) {
            int prefix_length = 0;
            if (argc < 8) {
                cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
                return 0;
            }
            if (sscanf(argv[6], "%d", &prefix_length) != 1) {
                cli->sendMsg(ResponseCode::CommandParameterError, "Invalid route prefix", false);
                return 0;
            }
            if (!strcmp(argv[2], "add")) {
                if (!strcmp(argv[4], "default")) {
                    if (ifc_add_route(argv[3], argv[5], prefix_length, argv[7])) {
                        cli->sendMsg(ResponseCode::OperationFailed,
                                "Failed to add route to default table", true);
                    } else {
                        cli->sendMsg(ResponseCode::CommandOkay,
                                "Route added to default table", false);
                    }
                } else if (!strcmp(argv[4], "secondary")) {
                    return sSecondaryTableCtrl->addRoute(cli, argv[3], argv[5],
                            prefix_length, argv[7]);
                } else {
                    cli->sendMsg(ResponseCode::CommandParameterError,
                            "Invalid route type, expecting 'default' or 'secondary'", false);
                    return 0;
                }
            } else if (!strcmp(argv[2], "remove")) {
                if (!strcmp(argv[4], "default")) {
                    if (ifc_remove_route(argv[3], argv[5], prefix_length, argv[7])) {
                        cli->sendMsg(ResponseCode::OperationFailed,
                                "Failed to remove route from default table", true);
                    } else {
                        cli->sendMsg(ResponseCode::CommandOkay,
                                "Route removed from default table", false);
                    }
                } else if (!strcmp(argv[4], "secondary")) {
                    return sSecondaryTableCtrl->removeRoute(cli, argv[3], argv[5],
                            prefix_length, argv[7]);
                } else {
                    cli->sendMsg(ResponseCode::CommandParameterError,
                            "Invalid route type, expecting 'default' or 'secondary'", false);
                    return 0;
                }
            } else {
                cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown interface cmd", false);
            }
            return 0;
        }

        if (!strcmp(argv[1], "getcfg")) {
            struct in_addr addr;
            int prefixLength;
            unsigned char hwaddr[6];
            unsigned flags = 0;

            ifc_init();
            memset(hwaddr, 0, sizeof(hwaddr));

            if (ifc_get_info(argv[2], &addr.s_addr, &prefixLength, &flags)) {
                cli->sendMsg(ResponseCode::OperationFailed, "Interface not found", true);
                ifc_close();
                return 0;
            }

            if (ifc_get_hwaddr(argv[2], (void *) hwaddr)) {
                LOGW("Failed to retrieve HW addr for %s (%s)", argv[2], strerror(errno));
            }

            char *addr_s = strdup(inet_ntoa(addr));
            const char *updown, *brdcst, *loopbk, *ppp, *running, *multi;

            updown =  (flags & IFF_UP)           ? "up" : "down";
            brdcst =  (flags & IFF_BROADCAST)    ? " broadcast" : "";
            loopbk =  (flags & IFF_LOOPBACK)     ? " loopback" : "";
            ppp =     (flags & IFF_POINTOPOINT)  ? " point-to-point" : "";
            running = (flags & IFF_RUNNING)      ? " running" : "";
            multi =   (flags & IFF_MULTICAST)    ? " multicast" : "";

            char *flag_s;

            asprintf(&flag_s, "[%s%s%s%s%s%s]", updown, brdcst, loopbk, ppp, running, multi);

            char *msg = NULL;
            asprintf(&msg, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x %s %d %s",
                     hwaddr[0], hwaddr[1], hwaddr[2], hwaddr[3], hwaddr[4], hwaddr[5],
                     addr_s, prefixLength, flag_s);

            cli->sendMsg(ResponseCode::InterfaceGetCfgResult, msg, false);

            free(addr_s);
            free(flag_s);
            free(msg);

            ifc_close();
            return 0;
        } else if (!strcmp(argv[1], "setcfg")) {
            // arglist: iface addr prefixLength [flags]
            if (argc < 5) {
                cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
                return 0;
            }
            LOGD("Setting iface cfg");

            struct in_addr addr;
            unsigned flags = 0;

            if (!inet_aton(argv[3], &addr)) {
                cli->sendMsg(ResponseCode::CommandParameterError, "Invalid address", false);
                return 0;
            }

            ifc_init();
            if (ifc_set_addr(argv[2], addr.s_addr)) {
                cli->sendMsg(ResponseCode::OperationFailed, "Failed to set address", true);
                ifc_close();
                return 0;
            }

            //Set prefix length on a non zero address
            if (addr.s_addr != 0 && ifc_set_prefixLength(argv[2], atoi(argv[4]))) {
                cli->sendMsg(ResponseCode::OperationFailed, "Failed to set prefixLength", true);
                ifc_close();
                return 0;
            }

            /* Process flags */
            /* read from "[XX" arg to "YY]" arg */
            bool bStarted = false;
            for (int i = 5; i < argc; i++) {
                char *flag = argv[i];
                if (!bStarted) {
                    if (*flag == '[') {
                        flag++;
                        bStarted = true;
                    } else {
                        continue;
                    }
                }
                int len = strlen(flag);
                if (flag[len-1] == ']') {
                    i = argc;  // stop after this loop
                    flag[len-1] = 0;
                }
                if (!strcmp(flag, "up")) {
                    LOGD("Trying to bring up %s", argv[2]);
                    if (ifc_up(argv[2])) {
                        LOGE("Error upping interface");
                        cli->sendMsg(ResponseCode::OperationFailed, "Failed to up interface", true);
                        ifc_close();
                        return 0;
                    }
                } else if (!strcmp(flag, "down")) {
                    LOGD("Trying to bring down %s", argv[2]);
                    if (ifc_down(argv[2])) {
                        LOGE("Error downing interface");
                        cli->sendMsg(ResponseCode::OperationFailed, "Failed to down interface", true);
                        ifc_close();
                        return 0;
                    }
                } else if (!strcmp(flag, "broadcast")) {
                    LOGD("broadcast flag ignored");
                } else if (!strcmp(flag, "multicast")) {
                    LOGD("multicast flag ignored");
                } else {
                    cli->sendMsg(ResponseCode::CommandParameterError, "Flag unsupported", false);
                    ifc_close();
                    return 0;
                }
            }

            cli->sendMsg(ResponseCode::CommandOkay, "Interface configuration set", false);
            ifc_close();
            return 0;
        } else if (!strcmp(argv[1], "clearaddrs")) {
            // arglist: iface
            LOGD("Clearing all IP addresses on %s", argv[2]);

            ifc_clear_addresses(argv[2]);

            cli->sendMsg(ResponseCode::CommandOkay, "Interface IP addresses cleared", false);
            return 0;
        } else if (!strcmp(argv[1], "ipv6privacyextensions")) {
            if (argc != 4) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                        "Usage: interface ipv6privacyextensions <interface> <enable|disable>",
                        false);
                return 0;
            }

            char *tmp;
            asprintf(&tmp, "/proc/sys/net/ipv6/conf/%s/use_tempaddr", argv[2]);

            if (writeFile(tmp, !strncmp(argv[3], "enable", 7) ? "2" : "0", 1) < 0) {
                free(tmp);
                cli->sendMsg(ResponseCode::OperationFailed,
                        "Failed to set ipv6 privacy extensions", true);
                return 0;
            }

            free(tmp);
            cli->sendMsg(ResponseCode::CommandOkay, "IPv6 privacy extensions changed", false);
            return 0;
        } else if (!strcmp(argv[1], "ipv6")) {
            if (argc != 4) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                        "Usage: interface ipv6 <interface> <enable|disable>",
                        false);
                return 0;
            }

            char *tmp;
            asprintf(&tmp, "/proc/sys/net/ipv6/conf/%s/disable_ipv6", argv[2]);

            if (writeFile(tmp, !strncmp(argv[3], "enable", 7) ? "0" : "1", 1) < 0) {
                free(tmp);
                cli->sendMsg(ResponseCode::OperationFailed,
                        "Failed to change IPv6 state", true);
                return 0;
            }

            free(tmp);
            cli->sendMsg(ResponseCode::CommandOkay, "IPv6 state changed", false);
            return 0;
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown interface cmd", false);
            return 0;
        }
    }
    return 0;
}


CommandListener::ListTtysCmd::ListTtysCmd() :
                 NetdCommand("list_ttys") {
}

int CommandListener::ListTtysCmd::runCommand(SocketClient *cli,
                                             int argc, char **argv) {
    TtyCollection *tlist = sPppCtrl->getTtyList();
    TtyCollection::iterator it;

    for (it = tlist->begin(); it != tlist->end(); ++it) {
        cli->sendMsg(ResponseCode::TtyListResult, *it, false);
    }

    cli->sendMsg(ResponseCode::CommandOkay, "Ttys listed.", false);
    return 0;
}

CommandListener::IpFwdCmd::IpFwdCmd() :
                 NetdCommand("ipfwd") {
}

int CommandListener::IpFwdCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    int rc = 0;

    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "status")) {
        char *tmp = NULL;

        asprintf(&tmp, "Forwarding %s", (sTetherCtrl->getIpFwdEnabled() ? "enabled" : "disabled"));
        cli->sendMsg(ResponseCode::IpFwdStatusResult, tmp, false);
        free(tmp);
        return 0;
    } else if (!strcmp(argv[1], "enable")) {
        rc = sTetherCtrl->setIpFwdEnabled(true);
    } else if (!strcmp(argv[1], "disable")) {
        rc = sTetherCtrl->setIpFwdEnabled(false);
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown ipfwd cmd", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "ipfwd operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "ipfwd operation failed", true);
    }

    return 0;
}

CommandListener::TetherCmd::TetherCmd() :
                 NetdCommand("tether") {
}

int CommandListener::TetherCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    int rc = 0;

    LOGD("TetherCmd::runCommand. argc: %d. argv[0]: %s", argc, argv[0]);
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "stop")) {
        rc = sTetherCtrl->stopTethering();
    } else if (!strcmp(argv[1], "status")) {
        char *tmp = NULL;

        asprintf(&tmp, "Tethering services %s",
                 (sTetherCtrl->isTetheringStarted() ? "started" : "stopped"));
        cli->sendMsg(ResponseCode::TetherStatusResult, tmp, false);
        free(tmp);
        return 0;
    } else {
        /*
         * These commands take a minimum of 4 arguments
         */
        if (argc < 4) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
            return 0;
        }

        if (!strcmp(argv[1], "start")) {
            if (argc % 2 == 1) {
                cli->sendMsg(ResponseCode::CommandSyntaxError, "Bad number of arguments", false);
                return 0;
            }

            int num_addrs = argc - 2;
            int arg_index = 2;
            int array_index = 0;
            in_addr *addrs = (in_addr *)malloc(sizeof(in_addr) * num_addrs);
            while (array_index < num_addrs) {
                if (!inet_aton(argv[arg_index++], &(addrs[array_index++]))) {
                    cli->sendMsg(ResponseCode::CommandParameterError, "Invalid address", false);
                    free(addrs);
                    return 0;
                }
            }
            rc = sTetherCtrl->startTethering(num_addrs, addrs);
            free(addrs);
        } else if (!strcmp(argv[1], "interface")) {
            if (!strcmp(argv[2], "add")) {
                rc = sTetherCtrl->tetherInterface(argv[3]);
            } else if (!strcmp(argv[2], "remove")) {
                rc = sTetherCtrl->untetherInterface(argv[3]);
            } else if (!strcmp(argv[2], "list")) {
                InterfaceCollection *ilist = sTetherCtrl->getTetheredInterfaceList();
                InterfaceCollection::iterator it;

                for (it = ilist->begin(); it != ilist->end(); ++it) {
                    cli->sendMsg(ResponseCode::TetherInterfaceListResult, *it, false);
                }
            } else if (!strcmp(argv[2], "add_upstream")) {
                LOGD("command %s %s %s %s", argv[0], argv[1], argv[2], argv[3]);
                rc = sTetherCtrl->addUpstreamInterface(argv[3]);
            } else if (!strcmp(argv[2], "remove_upstream")) {
                rc = sTetherCtrl->removeUpstreamInterface(argv[3]);
            } else {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Unknown tether interface operation", false);
                return 0;
            }
        } else if (!strcmp(argv[1], "dns")) {
            if (!strcmp(argv[2], "set")) {
                rc = sTetherCtrl->setDnsForwarders(&argv[3], argc - 3);
            } else if (!strcmp(argv[2], "list")) {
                NetAddressCollection *dlist = sTetherCtrl->getDnsForwarders();
                NetAddressCollection::iterator it;

                for (it = dlist->begin(); it != dlist->end(); ++it) {
                    cli->sendMsg(ResponseCode::TetherDnsFwdTgtListResult, inet_ntoa(*it), false);
                }
            } else {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Unknown tether interface operation", false);
                return 0;
            }
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown tether cmd", false);
            return 0;
        }
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "Tether operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Tether operation failed", true);
    }

    return 0;
}

CommandListener::V6RtrAdvCmd::V6RtrAdvCmd() :
                 NetdCommand("v6rtradv") {
}

int CommandListener::V6RtrAdvCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    int rc = 0;

    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "stop")) {
        rc = sTetherCtrl->stopV6RtrAdv();
    } else if (!strcmp(argv[1], "status")) {
        char *tmp = NULL;

        asprintf(&tmp, "IPv6 Router Advertisement service %s",
                 (sTetherCtrl->isV6RtrAdvStarted() ? "started" : "stopped"));
        cli->sendMsg(ResponseCode::V6RtrAdvResult, tmp, false);
        free(tmp);
        return 0;
    } else {
        /*
         * These commands take a minimum of 4 arguments
         */
        if (argc < 4) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
            return 0;
        }

        if (!strcmp(argv[1], "start")) {

            int num_ifaces = argc - 2;
            int arg_index = 2;
            rc = sTetherCtrl->startV6RtrAdv(num_ifaces, &argv[arg_index]);
        } else if (!strcmp(argv[1], "interface")) {
            if (!strcmp(argv[2], "add")) {
                rc = sTetherCtrl->tetherInterface(argv[3]);
            } else if (!strcmp(argv[2], "remove")) {
                rc = sTetherCtrl->untetherInterface(argv[3]);
            } else if (!strcmp(argv[2], "list")) {
                InterfaceCollection *ilist = sTetherCtrl->getTetheredInterfaceList();
                InterfaceCollection::iterator it;

                for (it = ilist->begin(); it != ilist->end(); ++it) {
                    cli->sendMsg(ResponseCode::TetherInterfaceListResult, *it, false);
                }
            } else {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Unknown tether interface operation", false);
                return 0;
            }
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown v6rtradv cmd", false);
            return 0;
        }
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "V6RtrAdv operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "V6RtrAdv operation failed", true);
    }

    return 0;
}

CommandListener::NatCmd::NatCmd() :
                 NetdCommand("nat") {
}

int CommandListener::NatCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    int rc = 0;

    if (argc < 5) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "enable")) {
        rc = sNatCtrl->enableNat(argc, argv);
        if(!rc) {
            /* Ignore ifaces for now. */
            rc = sBandwidthCtrl->setGlobalAlertInForwardChain();
        }
    } else if (!strcmp(argv[1], "disable")) {
        /* Ignore ifaces for now. */
        rc = sBandwidthCtrl->removeGlobalAlertInForwardChain();
        rc |= sNatCtrl->disableNat(argc, argv);
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown nat cmd", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "Nat operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Nat operation failed", true);
    }

    return 0;
}

CommandListener::PppdCmd::PppdCmd() :
                 NetdCommand("pppd") {
}

int CommandListener::PppdCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    int rc = 0;

    if (argc < 3) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "attach")) {
        struct in_addr l, r, dns1, dns2;

        memset(&dns1, sizeof(struct in_addr), 0);
        memset(&dns2, sizeof(struct in_addr), 0);

        if (!inet_aton(argv[3], &l)) {
            cli->sendMsg(ResponseCode::CommandParameterError, "Invalid local address", false);
            return 0;
        }
        if (!inet_aton(argv[4], &r)) {
            cli->sendMsg(ResponseCode::CommandParameterError, "Invalid remote address", false);
            return 0;
        }
        if ((argc > 3) && (!inet_aton(argv[5], &dns1))) {
            cli->sendMsg(ResponseCode::CommandParameterError, "Invalid dns1 address", false);
            return 0;
        }
        if ((argc > 4) && (!inet_aton(argv[6], &dns2))) {
            cli->sendMsg(ResponseCode::CommandParameterError, "Invalid dns2 address", false);
            return 0;
        }
        rc = sPppCtrl->attachPppd(argv[2], l, r, dns1, dns2);
    } else if (!strcmp(argv[1], "detach")) {
        rc = sPppCtrl->detachPppd(argv[2]);
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown pppd cmd", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "Pppd operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Pppd operation failed", true);
    }

    return 0;
}

CommandListener::PanCmd::PanCmd() :
                 NetdCommand("pan") {
}

int CommandListener::PanCmd::runCommand(SocketClient *cli,
                                        int argc, char **argv) {
    int rc = 0;

    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "start")) {
        rc = sPanCtrl->startPan();
    } else if (!strcmp(argv[1], "stop")) {
        rc = sPanCtrl->stopPan();
    } else if (!strcmp(argv[1], "status")) {
        char *tmp = NULL;

        asprintf(&tmp, "Pan services %s",
                 (sPanCtrl->isPanStarted() ? "started" : "stopped"));
        cli->sendMsg(ResponseCode::PanStatusResult, tmp, false);
        free(tmp);
        return 0;
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown pan cmd", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "Pan operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Pan operation failed", true);
    }

    return 0;
}

CommandListener::SoftapCmd::SoftapCmd() :
                 NetdCommand("softap") {
}

int CommandListener::SoftapCmd::runCommand(SocketClient *cli,
                                        int argc, char **argv) {
    int rc = 0, flag = 0;
    char *retbuf = NULL;

    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Softap Missing argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "start")) {
        rc = sSoftapCtrl->startDriver(argv[2]);
    } else if (!strcmp(argv[1], "stop")) {
        rc = sSoftapCtrl->stopDriver(argv[2]);
    } else if (!strcmp(argv[1], "startap")) {
        rc = sSoftapCtrl->startSoftap();
    } else if (!strcmp(argv[1], "stopap")) {
        rc = sSoftapCtrl->stopSoftap();
    } else if (!strcmp(argv[1], "fwreload")) {
        rc = sSoftapCtrl->fwReloadSoftap(argc, argv);
    } else if (!strcmp(argv[1], "clients")) {
        rc = sSoftapCtrl->clientsSoftap(&retbuf);
        if (!rc) {
            cli->sendMsg(ResponseCode::CommandOkay, retbuf, false);
            free(retbuf);
            return 0;
        }
    } else if (!strcmp(argv[1], "status")) {
        asprintf(&retbuf, "Softap service %s",
                 (sSoftapCtrl->isSoftapStarted() ? "started" : "stopped"));
        cli->sendMsg(ResponseCode::SoftapStatusResult, retbuf, false);
        free(retbuf);
        return 0;
    } else if (!strcmp(argv[1], "set")) {
        rc = sSoftapCtrl->setSoftap(argc, argv);
    }
#ifdef QCOM_WLAN
    else if (!strcmp(argv[1], "qccmd")) {
#define MAX_CMD_SIZE 256
    char qccmdbuf[MAX_CMD_SIZE], *pcmdbuf;
    int len = MAX_CMD_SIZE, i=2, ret;

    if ( argc < 4 ) {
        cli->sendMsg("failure: invalid arguments");
        return 0;
    }

    argc -= 2;
    pcmdbuf = qccmdbuf;

    while(argc--) {
        ret = snprintf(pcmdbuf, len, " %s", argv[i]);
        if ( ret == len ) {
        /* Error case */
        /* TODO: Command too long send the error message */
            *pcmdbuf = '\0';
             break;
        }
        pcmdbuf += ret;
        len -= ret;
        i++;
        }

        len = MAX_CMD_SIZE;
        qsap_hostd_exec_cmd(qccmdbuf, qccmdbuf, (u32*)&len);
        cli->sendMsg(qccmdbuf);
        return 0;
    }
#endif
    else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Softap Unknown cmd", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "Softap operation succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Softap operation failed", true);
    }

    return 0;
}

CommandListener::ResolverCmd::ResolverCmd() :
        NetdCommand("resolver") {
}

int CommandListener::ResolverCmd::runCommand(SocketClient *cli, int argc, char **argv) {
    int rc = 0;
    struct in_addr addr;

    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Resolver missing arguments", false);
        return 0;
    }

    if (!strcmp(argv[1], "setdefaultif")) { // "resolver setdefaultif <iface>"
        if (argc == 3) {
            rc = sResolverCtrl->setDefaultInterface(argv[2]);
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Wrong number of arguments to resolver setdefaultif", false);
            return 0;
        }
    } else if (!strcmp(argv[1], "setifdns")) { // "resolver setifdns <iface> <dns1> <dns2> ..."
        if (argc >= 4) {
            rc = sResolverCtrl->setInterfaceDnsServers(argv[2], &argv[3], argc - 3);
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Wrong number of arguments to resolver setifdns", false);
            return 0;
        }

        // set the address of the interface to which the name servers
        // are bound. Required in order to bind to right interface when
        // doing the dns query.
        if (!rc) {
            ifc_init();
            ifc_get_info(argv[2], &addr.s_addr, NULL, 0);

            rc = sResolverCtrl->setInterfaceAddress(argv[2], &addr);
        }
    } else if (!strcmp(argv[1], "flushdefaultif")) { // "resolver flushdefaultif"
        if (argc == 2) {
            rc = sResolverCtrl->flushDefaultDnsCache();
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Wrong number of arguments to resolver flushdefaultif", false);
            return 0;
        }
    } else if (!strcmp(argv[1], "flushif")) { // "resolver flushif <iface>"
        if (argc == 3) {
            rc = sResolverCtrl->flushInterfaceDnsCache(argv[2]);
        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Wrong number of arguments to resolver setdefaultif", false);
            return 0;
        }
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError,"Resolver unknown command", false);
        return 0;
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "Resolver command succeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Resolver command failed", true);
    }

    return 0;
}

int CommandListener::readInterfaceCounters(const char *iface, unsigned long *rx, unsigned long *tx) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        LOGE("Failed to open /proc/net/dev (%s)", strerror(errno));
        return -1;
    }

    char buffer[512];

    fgets(buffer, sizeof(buffer), fp); // Header 1
    fgets(buffer, sizeof(buffer), fp); // Header 2
    while(fgets(buffer, sizeof(buffer), fp)) {
        buffer[strlen(buffer)-1] = '\0';

        char name[31];
        unsigned long d;
        sscanf(buffer, "%30s %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                name, rx, &d, &d, &d, &d, &d, &d, &d, tx);
        char *rxString = strchr(name, ':');
        *rxString = '\0';
        rxString++;
        // when the rx count gets too big it changes from "name: 999" to "name:1000"
        // and the sscanf munge the two together.  Detect that and fix
        // note that all the %lu will be off by one and the real tx value will be in d
        if (*rxString != '\0') {
            *tx = d;
            sscanf(rxString, "%20lu", rx);
        }
        if (strcmp(name, iface)) {
            continue;
        }
        fclose(fp);
        return 0;
    }

    fclose(fp);
    *rx = 0;
    *tx = 0;
    return 0;
}

CommandListener::BandwidthControlCmd::BandwidthControlCmd() :
    NetdCommand("bandwidth") {
}

void CommandListener::BandwidthControlCmd::sendGenericSyntaxError(SocketClient *cli, const char *usageMsg) {
    char *msg;
    asprintf(&msg, "Usage: bandwidth %s", usageMsg);
    cli->sendMsg(ResponseCode::CommandSyntaxError, msg, false);
    free(msg);
}

void CommandListener::BandwidthControlCmd::sendGenericOkFail(SocketClient *cli, int cond) {
    if (!cond) {
        cli->sendMsg(ResponseCode::CommandOkay, "Bandwidth command succeeeded", false);
    } else {
        cli->sendMsg(ResponseCode::OperationFailed, "Bandwidth command failed", false);
    }
}

void CommandListener::BandwidthControlCmd::sendGenericOpFailed(SocketClient *cli, const char *errMsg) {
    cli->sendMsg(ResponseCode::OperationFailed, errMsg, false);
}

int CommandListener::BandwidthControlCmd::runCommand(SocketClient *cli, int argc, char **argv) {
    if (argc < 2) {
        sendGenericSyntaxError(cli, "<cmds> <args...>");
        return 0;
    }

    LOGV("bwctrlcmd: argc=%d %s %s ...", argc, argv[0], argv[1]);

    if (!strcmp(argv[1], "enable")) {
        int rc = sBandwidthCtrl->enableBandwidthControl();
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "disable")) {
        int rc = sBandwidthCtrl->disableBandwidthControl();
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "removequota") || !strcmp(argv[1], "rq")) {
        if (argc != 3) {
            sendGenericSyntaxError(cli, "removequota <interface>");
            return 0;
        }
        int rc = sBandwidthCtrl->removeInterfaceSharedQuota(argv[2]);
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "getquota") || !strcmp(argv[1], "gq")) {
        int64_t bytes;
        if (argc != 2) {
            sendGenericSyntaxError(cli, "getquota");
            return 0;
        }
        int rc = sBandwidthCtrl->getInterfaceSharedQuota(&bytes);
        if (rc) {
            sendGenericOpFailed(cli, "Failed to get quota");
            return 0;
        }

        char *msg;
        asprintf(&msg, "%lld", bytes);
        cli->sendMsg(ResponseCode::QuotaCounterResult, msg, false);
        free(msg);
        return 0;

    }
    if (!strcmp(argv[1], "getiquota") || !strcmp(argv[1], "giq")) {
        int64_t bytes;
        if (argc != 3) {
            sendGenericSyntaxError(cli, "getiquota <iface>");
            return 0;
        }

        int rc = sBandwidthCtrl->getInterfaceQuota(argv[2], &bytes);
        if (rc) {
            sendGenericOpFailed(cli, "Failed to get quota");
            return 0;
        }
        char *msg;
        asprintf(&msg, "%lld", bytes);
        cli->sendMsg(ResponseCode::QuotaCounterResult, msg, false);
        free(msg);
        return 0;

    }
    if (!strcmp(argv[1], "setquota") || !strcmp(argv[1], "sq")) {
        if (argc != 4) {
            sendGenericSyntaxError(cli, "setquota <interface> <bytes>");
            return 0;
        }
        int rc = sBandwidthCtrl->setInterfaceSharedQuota(argv[2], atoll(argv[3]));
        sendGenericOkFail(cli, rc);
        return 0;
    }
    if (!strcmp(argv[1], "setquotas") || !strcmp(argv[1], "sqs")) {
        int rc;
        if (argc < 4) {
            sendGenericSyntaxError(cli, "setquotas <bytes> <interface> ...");
            return 0;
        }

        for (int q = 3; argc >= 4; q++, argc--) {
            rc = sBandwidthCtrl->setInterfaceSharedQuota(argv[q], atoll(argv[2]));
            if (rc) {
                char *msg;
                asprintf(&msg, "bandwidth setquotas %s %s failed", argv[2], argv[q]);
                cli->sendMsg(ResponseCode::OperationFailed,
                             msg, false);
                free(msg);
                return 0;
            }
        }
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "removequotas") || !strcmp(argv[1], "rqs")) {
        int rc;
        if (argc < 3) {
            sendGenericSyntaxError(cli, "removequotas <interface> ...");
            return 0;
        }

        for (int q = 2; argc >= 3; q++, argc--) {
            rc = sBandwidthCtrl->removeInterfaceSharedQuota(argv[q]);
            if (rc) {
                char *msg;
                asprintf(&msg, "bandwidth removequotas %s failed", argv[q]);
                cli->sendMsg(ResponseCode::OperationFailed,
                             msg, false);
                free(msg);
                return 0;
            }
        }
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "removeiquota") || !strcmp(argv[1], "riq")) {
        if (argc != 3) {
            sendGenericSyntaxError(cli, "removeiquota <interface>");
            return 0;
        }
        int rc = sBandwidthCtrl->removeInterfaceQuota(argv[2]);
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "setiquota") || !strcmp(argv[1], "siq")) {
        if (argc != 4) {
            sendGenericSyntaxError(cli, "setiquota <interface> <bytes>");
            return 0;
        }
        int rc = sBandwidthCtrl->setInterfaceQuota(argv[2], atoll(argv[3]));
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "addnaughtyapps") || !strcmp(argv[1], "ana")) {
        if (argc < 3) {
            sendGenericSyntaxError(cli, "addnaughtyapps <appUid> ...");
            return 0;
        }
        int rc = sBandwidthCtrl->addNaughtyApps(argc - 2, argv + 2);
        sendGenericOkFail(cli, rc);
        return 0;


    }
    if (!strcmp(argv[1], "removenaughtyapps") || !strcmp(argv[1], "rna")) {
        if (argc < 3) {
            sendGenericSyntaxError(cli, "removenaughtyapps <appUid> ...");
            return 0;
        }
        int rc = sBandwidthCtrl->removeNaughtyApps(argc - 2, argv + 2);
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "setglobalalert") || !strcmp(argv[1], "sga")) {
        if (argc != 3) {
            sendGenericSyntaxError(cli, "setglobalalert <bytes>");
            return 0;
        }
        int rc = sBandwidthCtrl->setGlobalAlert(atoll(argv[2]));
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "debugsettetherglobalalert") || !strcmp(argv[1], "dstga")) {
        if (argc != 4) {
            sendGenericSyntaxError(cli, "debugsettetherglobalalert <interface0> <interface1>");
            return 0;
        }
        /* We ignore the interfaces for now. */
        int rc = sBandwidthCtrl->setGlobalAlertInForwardChain();
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "removeglobalalert") || !strcmp(argv[1], "rga")) {
        if (argc != 2) {
            sendGenericSyntaxError(cli, "removeglobalalert");
            return 0;
        }
        int rc = sBandwidthCtrl->removeGlobalAlert();
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "debugremovetetherglobalalert") || !strcmp(argv[1], "drtga")) {
        if (argc != 4) {
            sendGenericSyntaxError(cli, "debugremovetetherglobalalert <interface0> <interface1>");
            return 0;
        }
        /* We ignore the interfaces for now. */
        int rc = sBandwidthCtrl->removeGlobalAlertInForwardChain();
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "setsharedalert") || !strcmp(argv[1], "ssa")) {
        if (argc != 3) {
            sendGenericSyntaxError(cli, "setsharedalert <bytes>");
            return 0;
        }
        int rc = sBandwidthCtrl->setSharedAlert(atoll(argv[2]));
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "removesharedalert") || !strcmp(argv[1], "rsa")) {
        if (argc != 2) {
            sendGenericSyntaxError(cli, "removesharedalert");
            return 0;
        }
        int rc = sBandwidthCtrl->removeSharedAlert();
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "setinterfacealert") || !strcmp(argv[1], "sia")) {
        if (argc != 4) {
            sendGenericSyntaxError(cli, "setinterfacealert <interface> <bytes>");
            return 0;
        }
        int rc = sBandwidthCtrl->setInterfaceAlert(argv[2], atoll(argv[3]));
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "removeinterfacealert") || !strcmp(argv[1], "ria")) {
        if (argc != 3) {
            sendGenericSyntaxError(cli, "removeinterfacealert <interface>");
            return 0;
        }
        int rc = sBandwidthCtrl->removeInterfaceAlert(argv[2]);
        sendGenericOkFail(cli, rc);
        return 0;

    }
    if (!strcmp(argv[1], "gettetherstats") || !strcmp(argv[1], "gts")) {
        BandwidthController::TetherStats tetherStats;
        if (argc != 4) {
            sendGenericSyntaxError(cli, "gettetherstats <interface0> <interface1>");
            return 0;
        }

        tetherStats.ifaceIn = argv[2];
        tetherStats.ifaceOut = argv[3];
        int rc = sBandwidthCtrl->getTetherStats(tetherStats);
        if (rc) {
            sendGenericOpFailed(cli, "Failed to get tethering stats");
            return 0;
        }

        char *msg = tetherStats.getStatsLine();
        cli->sendMsg(ResponseCode::TetheringStatsResult, msg, false);
        free(msg);
        return 0;

    }

    cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown bandwidth cmd", false);
    return 0;
}

CommandListener::RouteCmd::RouteCmd() :
                 NetdCommand("route") {
}

int CommandListener::RouteCmd::runCommand(SocketClient *cli,
                                          int argc, char **argv) {
    if (argc < 5) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing argument", false);
        return 0;
    }

    const char *ipVer = NULL;
    int domain;

    if (!strcmp(argv[3], "v4")) {
        ipVer = "-4";
        domain = AF_INET;
    } else if (!strcmp(argv[3], "v6")) {
        ipVer = "-6";
        domain = AF_INET6;
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError,
                     "Supported family v4|v6",false);
        return 0;
    }

    if (!strcmp(argv[2], "src")) {
        /* source based routing */
        if (!strcmp(argv[1], "replace")) {
            if (argc != 7 && argc != 8) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                   "Usage: route replace src family <interface>"
                   " <ipaddr> <routeId> [<gateway>]", false);
                return 0;
            }

            int rid = atoi(argv[6]);
            if ((rid < 1) || (rid > 252)) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                                "0 < RouteID < 253", false);
                return 0;
            }

            struct in_addr addr;
            int prefix_length;
            int structsz = (domain==AF_INET) ?
                    sizeof(struct in_addr) : sizeof(struct in6_addr);
            unsigned char s[structsz], g[structsz];
            unsigned flags = 0;

            /* verify if interface is UP */
            ifc_init();
            if (ifc_get_info(argv[4], &addr.s_addr, &prefix_length, &flags) ||
                            !(flags & IFF_UP)) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Interface is down|not found", false);
                ifc_close();
                return 0;
            }
            ifc_close();

            if (inet_pton(domain, argv[5], s) < 1) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Invalid src address", false);
                return 0;
            }

            char *iface = argv[4],
                 *srcPrefix = argv[5],
                 *routeId = argv[6],
                 *network = NULL,
                 *gateway = NULL;

            if (argc > 7) {
                if (inet_pton(domain, argv[7], g) < 1) {
                    cli->sendMsg(ResponseCode::CommandParameterError,
                                 "Invalid gateway address", false);
                    return 0;
                } else {
                    gateway = argv[7];
                }
            }

            // compute the network block in CIDR notation (for IPv4 only)
            if (domain == AF_INET) {
                struct in_addr net;
                in_addr_t mask = prefixLengthToIpv4Netmask(prefix_length);
                net.s_addr = (addr.s_addr & mask);


                char net_s[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(net.s_addr), net_s, INET_ADDRSTRLEN);
                asprintf(&network, "%s/%d", net_s, prefix_length);
            }

            char *tmp = NULL;
            if (sRouteCtrl->repSrcRoute(iface, srcPrefix,
                                    gateway, routeId, ipVer) != 0) {
                asprintf(&tmp,"failed to set route for route id [%s]",routeId);
                cli->sendMsg(ResponseCode::OperationFailed,tmp,true);
            } else {
                if (network != NULL) {
                     // gateway should be null as traffic is on same subnet
                    if (sRouteCtrl->addDstRoute(iface, network, NULL, routeId) != 0) {
                        asprintf(&tmp,
                             "source route replace succeeded for route id [%s]"
                             ", but local destination routing failed", routeId);
                    } else {
                        asprintf(&tmp,
                             "source route replace succeeded for route id [%s]"
                             ", and local destination routing succeeded", routeId);
                    }
                    cli->sendMsg(ResponseCode::CommandOkay,tmp,false);
                } else {
                    asprintf(&tmp,
                         "source route replace succeeded for route id [%s]", routeId);
                    cli->sendMsg(ResponseCode::CommandOkay,tmp,false);
                }
            }
            free(network);
            free(tmp);
        } else if (!strcmp(argv[1], "del")) {
            if (argc != 5) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                             "Usage: route src del v[4|6] <routeId>", false);
                return 0;
            }

            int rid = atoi(argv[4]);

            if ((rid < 1) || (rid > 252)) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "RouteID: between 0 and 253", false);
                return 0;
            }

            char *tmp = NULL;

            if (sRouteCtrl->delSrcRoute(argv[4], ipVer) != 0) {
                asprintf(&tmp,
                    "failed to delete source route for route id [%s]",argv[4]);
                cli->sendMsg(ResponseCode::OperationFailed,tmp, true);
            } else {
                asprintf(&tmp,
                    "source route delete succeeded for route id [%s]",argv[4]);
                cli->sendMsg(ResponseCode::CommandOkay,tmp,false);
            }
            free(tmp);

        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                         "permitted operation: <replace|del>", false);
        }

        return 0;

    } else if (!strcmp(argv[2], "def")) {
        /* default route configuration */
        if (!strcmp(argv[1], "replace")) {
            if ((argc != 5) && (argc != 6)) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                        "Usage: route replace def v[4|6]"
                        " <interface> [<gateway>]", false);
                return 0;
            }

            struct in_addr addr;
            int prefixLength;
            int structsz = (domain==AF_INET) ?
                    sizeof(struct in_addr) : sizeof(struct in6_addr);
            unsigned char g[structsz];
            unsigned flags = 0;

            /* verify if interface is UP */
            ifc_init();
            if (ifc_get_info(argv[4], &addr.s_addr, &prefixLength, &flags) ||
                            !(flags & IFF_UP)) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Interface is down|not found", false);
                return 0;
            }
	    ifc_close();

            char *iface = argv[4],
                 *gateway = NULL;

            if (argc > 5) {
                if (inet_pton(domain, argv[5], g) < 1) {
                    cli->sendMsg(ResponseCode::CommandParameterError,
                                 "Invalid gateway address", false);
                    return 0;
                } else {
                    gateway = argv[5];
                }
            }

            char *tmp = NULL;

            if (sRouteCtrl->replaceDefRoute(iface, gateway, ipVer) != 0) {
                asprintf(&tmp,"failed to replace default route"
                                " for [%s %s]", iface, ipVer);
                cli->sendMsg(ResponseCode::OperationFailed,tmp,true);
            } else {
                asprintf(&tmp,"default route replace succeeded "
                                "for [%s %s]", iface, ipVer);
                cli->sendMsg(ResponseCode::CommandOkay,tmp,false);
            }
            free(tmp);

        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                         "Permitted action for def routes <replace>", false);
        }

    } else if (!strcmp(argv[2], "dst")) {
        /* destination based route configuration */
        if (!strcmp(argv[1], "add")) {
            if (argc != 6 && argc != 7) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                   "Usage: route dst add v[4|6]"
                   " <interface> <ipaddr> [<gateway>]", false);
                return 0;
            }

            struct in_addr addr;
            int prefixLength;
            int structsz = (domain==AF_INET) ?
                    sizeof(struct in_addr) : sizeof(struct in6_addr);
            unsigned char d[structsz], g[structsz];
            unsigned flags = 0;

            /* verify if interface is UP */
            ifc_init();
            if (ifc_get_info(argv[4], &addr.s_addr, &prefixLength, &flags) ||
                            !(flags & IFF_UP)) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Interface is down|not found", false);
                return 0;
            }
            ifc_close();

            if (inet_pton(domain, argv[5], d) < 1) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Invalid dst address", false);
                return 0;
            }

            char *iface = argv[4],
                 *dstPrefix = argv[5],
                 *gateway = NULL;

            if (argc > 6) {
                if (inet_pton(domain, argv[6], g) < 1) {
                    cli->sendMsg(ResponseCode::CommandParameterError,
                                 "Invalid gateway address", false);
                    return 0;
                } else {
                    gateway = argv[6];
                }
            }

            char *tmp = NULL;

            //FIXME remove comments once IPV6 kernel issues for default routes are
            //resolved. Until then ignore preexisting route addition error.Ignore
            //the error here, rather than in RouteController since the validity of
            //arguments is performed here.
            /*if (*/sRouteCtrl->addDstRoute(iface, dstPrefix, gateway);/* != 0) {
                asprintf(&tmp,"failed to set route for destination "
                              "[%s %s]", iface, dstPrefix);
                cli->sendMsg(ResponseCode::OperationFailed,tmp,true);
            } else {*/
                asprintf(&tmp,"destination route add succeeded "
                              "for [%s %s]",iface, dstPrefix);
                cli->sendMsg(ResponseCode::CommandOkay,tmp,false);
            /*}*/
            free(tmp);
        } else if (!strcmp(argv[1], "del")) {
            if (argc != 5) {
                cli->sendMsg(ResponseCode::CommandSyntaxError,
                             "Usage: route dst del v[4|6] <ipaddr>", false);
                return 0;
            }

            int structsz = (domain==AF_INET) ?
                    sizeof(struct in_addr) : sizeof(struct in6_addr);
            unsigned char d[structsz];

            if (inet_pton(domain, argv[4], d) < 1) {
                cli->sendMsg(ResponseCode::CommandParameterError,
                             "Invalid destination address", false);
                return 0;
            }

            char *tmp = NULL;

            if (sRouteCtrl->delDstRoute(argv[4]) != 0) {
                asprintf(&tmp, "failed to delete destination "
                               "route for [%s]", argv[4]);
                cli->sendMsg(ResponseCode::OperationFailed,tmp, true);
            } else {
                asprintf(&tmp,
                    "destination route delete succeeded for [%s]", argv[4]);
                cli->sendMsg(ResponseCode::CommandOkay,tmp,false);
            }
            free(tmp);

        } else {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                         "permitted operation: <add|del>", false);
        }

        return 0;

    } else {
        cli->sendMsg(ResponseCode::CommandParameterError,
                     "host routes: <src|dst|def>",false);
    }

    return 0;

}
