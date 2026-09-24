#!/bin/sh
# fw4 builds the flowtable from each network's physdev, which leaves out the
# 5G modem netdev (proto xmm's device is /dev/ttyUSB4) and, on an IPv6-only
# APN, the 464xlat CLAT device. Add them. The CLAT device must be in it too:
# with only one side of a translated flow offloaded, conntrack sees half of
# each TCP connection and the wan zone drops the rest as invalid.
# Run by the fw4 include (after every reload) and the net hotplug hook.
. /lib/functions/network.sh
DEVS=$1
[ -n "$DEVS" ] || {
	network_get_device DEVS wwan
	DEVS="$DEVS $(ls /sys/class/net | grep '^464-')"
}
FT=$(nft list flowtable inet fw4 ft 2>/dev/null) || exit 0
for DEV in $DEVS; do
	[ -e "/sys/class/net/$DEV" ] || continue
	case "$FT" in *" $DEV,"*|*" $DEV "*) continue ;; esac
	nft add flowtable inet fw4 ft "{ hook ingress priority 0; devices = { \"$DEV\" }; }"
done
