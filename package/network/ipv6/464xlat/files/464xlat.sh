#!/bin/sh
# 464xlat.sh - 464xlat CLAT
#
# Copyright (c) 2015 Steven Barth <cyrus@openwrt.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2
# as published by the Free Software Foundation
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

[ -n "$INCLUDE_ONLY" ] || {
	. /lib/functions.sh
	. /lib/functions/network.sh
	. ../netifd-proto.sh
	init_proto "$@"
}

proto_464xlat_setup() {
	local cfg="$1"
	local iface="$2"
	local link="464-$cfg"

	local ip6addr ip6prefix tunlink zone mtu
	json_get_vars ip6addr ip6prefix tunlink zone mtu

	[ "$zone" = "-" ] && zone=""

	( proto_add_host_dependency "$cfg" "::" "$tunlink" )

	if [ -z "$tunlink" ] && ! network_find_wan6 tunlink; then
		proto_notify_error "$cfg" "NO_WAN_LINK"
		return
	fi
	network_get_device tundev "$tunlink"

	ip6addr=$(464xlatcfg "$link" "$tundev" "$ip6prefix" 192.0.0.1 $ip6addr)
	if [ -z "$ip6addr" ]; then
		proto_notify_error "$cfg" "CLAT_CONFIG_FAILED"
		return
	fi

	ip -6 rule del from all lookup local
	ip -6 rule add from all lookup local pref 1
	ip -6 rule add to $ip6addr lookup prelocal pref 0
	echo "$ip6addr" > /tmp/464-$cfg-anycast

	proto_init_update "$link" 1
	proto_add_ipv4_route "0.0.0.0" 0 "" "" 2048
	proto_add_ipv6_route $ip6addr 128 "" "" "" "" 128

	proto_add_data
	[ -n "$zone" ] && json_add_string zone "$zone"

	json_add_array firewall
		[ -z "$zone" ] && zone=$(fw3 -q network $iface 2>/dev/null)

		json_add_object ""
			json_add_string type nat
			json_add_string target SNAT
			json_add_string family inet
			json_add_string snat_ip 192.0.0.1
		json_close_object
		[ -n "$zone" ] && {
			json_add_object ""
				json_add_string type rule
				json_add_string family inet6
				json_add_string proto all
				json_add_string direction in
				json_add_string dest "$zone"
				json_add_string src "$zone"
				json_add_string src_ip $ip6addr
				json_add_string target ACCEPT
			json_close_object
		}
	json_close_array
	proto_close_data

	proto_send_update "$cfg"

	# Translated packets grow by 20 bytes (IPv6 vs IPv4 header). The uplink's
	# IPv6 MTU can be below 1500 (an RA may lower it: the FM350 advertises
	# 1280), and the nat46 device can't go below 1280 itself without losing
	# IPv6. So the IPv4 MTU goes on a default route preferred over netifd's
	# (metric 2048): fw4 MSS clamping and IPv4 PMTUD use the route MTU. It goes
	# away with the device on teardown.
	[ -n "$mtu" ] || mtu=$(( $(cat /proc/sys/net/ipv6/conf/$tundev/mtu 2>/dev/null || echo 1500) - 20 ))
	ip link set dev "$link" up
	ip route replace default dev "$link" metric 2047 mtu "$mtu"
}

proto_464xlat_teardown() {
	local cfg="$1"
	local link="464-$cfg"

	[ -f /tmp/464-$cfg-anycast ] || return
	local ip6addr=$(cat /tmp/464-$cfg-anycast)

	464xlatcfg "$link"

	rm -rf /tmp/464-$cfg-anycast
	[ -n "$ip6addr" ] && ip -6 rule del to $ip6addr lookup prelocal

	if [ -z "$(ls /tmp/464-*-anycast 2>&-)" ]; then
		ip -6 rule del from all lookup local
		ip -6 rule add from all lookup local pref 0
	fi

	# Kill conntracks SNATed to 192.0.0.1
	echo 192.0.0.1 > /proc/net/nf_conntrack
}

proto_464xlat_init_config() {
	no_device=1
	available=1

	proto_config_add_string "ip6prefix"
	proto_config_add_string "ip6addr"
	proto_config_add_string "tunlink"
	proto_config_add_string "zone"
	proto_config_add_int "mtu"
}

[ -n "$INCLUDE_ONLY" ] || {
        add_protocol 464xlat
}
