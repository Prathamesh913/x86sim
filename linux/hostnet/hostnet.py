#!/usr/bin/env python3
"""x86Sim: the host's network configuration, as macOS's configd would publish it in its dynamic store.

On a Mac, configd_sim's SimulatorSupport plugin mirrors the host configd's Setup:/Network/Global/*,
Setup:+State:/Network/Service/*/* and Setup:+State:/Network/Interface/*/* keys into the simulator's store
(configd's Plugins/SimulatorSupport/simulator_support.c); the simulator's IPMonitor then derives State:/Network/Global/
IPv4, DNS, the nwi state (nw_path) and the DNS configuration (dnsinfo) from them. Here com.x86sim.host-configd plays
the host configd and host/netconfig/netconfig (a simulator tool) loads these keys into it.

Usage: hostnet.py <out.plist>   Writes {key: value} for the Linux interface of the IPv4 default route (lowest metric).
"""
import ipaddress
import plistlib
import re
import subprocess
import sys
import uuid


def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout


def main():
    routes = []
    for line in run("ip", "-4", "route", "show", "default").splitlines():
        m = re.search(r"via (\S+) dev (\S+)(?:.* metric (\d+))?", line)
        if m:
            routes.append((int(m.group(3) or 0), m.group(2), m.group(1)))
    keys = {}
    if routes:
        routes.sort()
        _, ifname, router = routes[0]
        addrs, masks = [], []
        for line in run("ip", "-4", "-o", "addr", "show", "dev", ifname).splitlines():
            m = re.search(r"inet (\S+)", line)
            if m:
                n = ipaddress.ip_interface(m.group(1))
                addrs.append(str(n.ip)); masks.append(str(n.netmask))
        a6, p6 = [], []
        for line in run("ip", "-6", "-o", "addr", "show", "dev", ifname, "scope", "global").splitlines():
            m = re.search(r"inet6 (\S+)", line)
            if m:
                n = ipaddress.ip_interface(m.group(1))
                a6.append(str(n.ip)); p6.append(n.network.prefixlen)
        dns, search = [], []
        try:
            for line in open("/etc/resolv.conf"):
                p = line.split()
                if p[:1] == ["nameserver"]:
                    dns.append(p[1])
                elif p[:1] in (["search"], ["domain"]):
                    search += p[1:]
        except OSError:
            pass
        sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "x86sim-net-" + ifname)).upper()
        svc, st = "Setup:/Network/Service/" + sid, "State:/Network/Service/" + sid
        keys.update({
            "Setup:/Network/Global/IPv4": {"ServiceOrder": [sid]},
            svc: {"UserDefinedName": "Ethernet"},
            svc + "/Interface": {"DeviceName": ifname, "Hardware": "Ethernet", "Type": "Ethernet",
                                 "UserDefinedName": "Ethernet"},
            svc + "/IPv4": {"ConfigMethod": "DHCP"},
            svc + "/IPv6": {"ConfigMethod": "Automatic"},
            svc + "/DNS": {},
            st + "/IPv4": {"Addresses": addrs, "SubnetMasks": masks, "Router": router, "InterfaceName": ifname},
            st + "/DNS": {"ServerAddresses": dns, **({"SearchDomains": search} if search else {})},
            "State:/Network/Interface/%s/Link" % ifname: {"Active": True},
            "State:/Network/Interface/%s/IPv4" % ifname: {"Addresses": addrs, "SubnetMasks": masks},
        })
        if a6:
            keys[st + "/IPv6"] = {"Addresses": a6, "PrefixLength": p6, "InterfaceName": ifname}
            keys["State:/Network/Interface/%s/IPv6" % ifname] = {"Addresses": a6, "PrefixLength": p6}
    with open(sys.argv[1], "wb") as f:
        plistlib.dump(keys, f)


if __name__ == "__main__":
    main()
