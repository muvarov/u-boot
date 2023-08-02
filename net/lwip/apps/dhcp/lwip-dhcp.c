// SPDX-License-Identifier: GPL-2.0

/*
 * (C) Copyright 2023 Linaro Ltd. <maxim.uvarov@linaro.org>
 */

#include <common.h>
#include <command.h>
#include <console.h>

#include <lwip/dhcp.h>
#include <lwip/prot/dhcp.h>
#include "lwip/timeouts.h"

#include <net/eth.h>
#include <net/ulwip.h>

#define DHCP_TMO_TIME 1000 /* poll for DHCP state change */
#define DHCP_TMO_NUM  20  /* number of tries */

typedef struct dhcp_priv {
	int num_tries;
	struct netif *netif;
} dhcp_priv;

static void dhcp_tmo(void *arg)
{
	struct dhcp_priv *dpriv = (struct dhcp_priv *)arg;
	struct netif *netif = dpriv->netif;
	struct dhcp *dhcp;

	log_err("%s %d/%d\n", __func__, dpriv->num_tries, DHCP_TMO_NUM);
	dhcp = netif_get_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_DHCP);
	if (!dhcp)
		return;

	if (dhcp->state == DHCP_STATE_BOUND) {
		int err = 0;

		err -= env_set("bootfile", dhcp->boot_file_name);
		err -= env_set("ipaddr", ip4addr_ntoa(&dhcp->offered_ip_addr));
		err -= env_set("netmask", ip4addr_ntoa(&dhcp->offered_sn_mask));
		err -= env_set("serverip", ip4addr_ntoa(&dhcp->server_ip_addr));
		if (err)
			log_err("error update envs\n");
		log_info("DHCP client bound to address %s\n", ip4addr_ntoa(&dhcp->offered_ip_addr));
		free(dpriv);
		ulwip_exit(err);
		return;
	}

	dpriv->num_tries--;
	if (dpriv->num_tries < 0) {
		log_err("DHCP client timeout\n");
		free(dpriv);
		ulwip_exit(-1);
		return;
	}

	sys_timeout(DHCP_TMO_TIME, dhcp_tmo, dpriv);
}

int ulwip_dhcp(void)
{
	struct netif *netif;
	int eth_idx;
	struct dhcp_priv *dpriv;

	dpriv = malloc(sizeof(struct dhcp_priv));
	if (!dpriv)
		return -EPERM;

	eth_idx = eth_get_dev_index();
	if (eth_idx < 0)
		return -EPERM;

	netif = netif_get_by_index(eth_idx + 1);
	if (!netif)
		return -ENOENT;

	dpriv->num_tries = DHCP_TMO_NUM;
	dpriv->netif = netif;
	sys_timeout(DHCP_TMO_TIME, dhcp_tmo, dpriv);

	return dhcp_start(netif) ? -EPERM : 0;
}
