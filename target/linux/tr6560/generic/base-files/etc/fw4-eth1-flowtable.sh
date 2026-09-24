#!/bin/sh
# Put the 5G modem's netdev into fw4's software flowtable. fw4 takes flowtable
# devices from each network's physdev; for network wwan (proto xmm) that is the
# AT tty, so the modem netdev (eth1) is left out and downloads, which arrive on
# it, never take the fast path.
# Run by fw4 after every (re)load as a firewall include (no argument: the L3
# device of network wwan) and by /etc/hotplug.d/net/21-rps-tuning with the
# newly created netdev. Idempotent.
. /lib/functions/network.sh
DEV=$1
[ -n "$DEV" ] || network_get_device DEV wwan
[ -n "$DEV" ] && [ -e "/sys/class/net/$DEV" ] || exit 0
FT=$(nft list flowtable inet fw4 ft 2>/dev/null) || exit 0
case "$FT" in *" $DEV,"*|*" $DEV "*) exit 0 ;; esac
nft add flowtable inet fw4 ft "{ hook ingress priority 0; devices = { $DEV }; }"
