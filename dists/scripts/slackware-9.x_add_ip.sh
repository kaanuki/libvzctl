#!/bin/bash
# Copyright (c) 1999-2017, Parallels International GmbH
#
# This file is part of OpenVZ libraries. OpenVZ is free software; you can
# redistribute it and/or modify it under the terms of the GNU Lesser General
# Public License as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.
#
# Our contact details: Parallels International GmbH, Vordergasse 59, 8200
# Schaffhausen, Switzerland.
#
# This script configure IP addrss inside VPS for Slackware like distros
# Slackware does not support ip aliases so multiple ip addresses not allowed
#
# Parameters are passed in environment variables.
# Required parameters:
#   IP_ADDR       - IP address(es) to add
#                   (several addresses should be divided by space)
# Optional parameters:
#   VE_STATE      - state of VPS; could be one of:
#                     starting | stopping | running | stopped
#   IPDELALL	  - delete all old interfaces
#
VENET_DEV=venet0

FAKEGATEWAY=191.255.255.1
FAKEGATEWAYNET=191.255.255.0

IFCFG_DIR=/etc/rc.d
IFCFG=${IFCFG_DIR}/rc.inet1
HOSTFILE=/etc/hosts

function fix_rcinet1()
{

	[ -f "${IFCFG}" ] || return 0
	cp -fp ${IFCFG} ${IFCFG}.$$ || error "Can't copy file ${IFCFG}" ${VZ_FS_NO_DISK_SPACE}
	sed -e "s/^GATEWAY=.*/GATEWAY=\"${FAKEGATEWAY}\"/" -e 's/^USE_DHCP=.*/\#USE_DHCP=\"\"/' -e 's/eth0/venet0/g' -e 's/^[\ \t]*\/sbin\/route add default gw .*/\t\/sbin\/route add -net 191.255.255.0\/24 dev venet0; \/sbin\/route add default gw \${GATEWAY} dev venet0/' < ${IFCFG} > ${IFCFG}.$$ && mv -f ${IFCFG}.$$ ${IFCFG}
	rm -f ${IFCFG}.$$ >/dev/null 2>&1
}

function setup_network()
{
	mkdir -p ${IFCFG_DIR}
	# Set up /etc/hosts
	if [ ! -f ${HOSTFILE} ]; then
		echo "127.0.0.1 localhost.localdomain localhost" > $HOSTFILE
	fi
	fix_rcinet1
}

function create_config()
{
	local ip=${1}

	put_param ${IFCFG} "IPADDR" ${ip}
	put_param ${IFCFG} "NETMASK" "255.255.255.255"
}

function add_ip()
{
	# In case we are starting VPS
	if [ "x${VE_STATE}" = "xstarting" ]; then
		setup_network
	fi
	for ip in ${IP_ADDR}; do
		if [ "${IPDELALL}" != "yes" ]; then
			if grep -qw "${ip}" ${IFCFG} 2>/dev/null; then
				break
			fi
		fi
		${IFCFG_DIR}/rc.inet1 stop >/dev/null 2>&1
		create_config ${ip}
		${IFCFG_DIR}/rc.inet1 start >/dev/null 2>&1
		break
	done
}

add_ip
exit 0
# end of script
