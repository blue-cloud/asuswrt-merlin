/*

	Copyright 2005, Broadcom Corporation
	All Rights Reserved.

	THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
	KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
	SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
	FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.

*/
/*

	wificonf, OpenWRT
	Copyright (C) 2005 Felix Fietkau <nbd@vd-s.ath.cx>

*/
/*

	Modified for Tomato Firmware
	Portions, Copyright (C) 2006-2009 Jonathan Zarate

*/

#include <rc.h>

#ifndef UU_INT
typedef u_int64_t u64;
typedef u_int32_t u32;
typedef u_int16_t u16;
typedef u_int8_t u8;
#endif

#include <linux/types.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <sys/ioctl.h>
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <dirent.h>

#include <wlutils.h>
#ifdef CONFIG_BCMWL5
#include <bcmparams.h>
#include <wlioctl.h>
#include <security_ipc.h>

#ifndef WL_BSS_INFO_VERSION
#error WL_BSS_INFO_VERSION
#endif
#if WL_BSS_INFO_VERSION >= 108
#include <etioctl.h>
#else
#include <etsockio.h>
#endif
#endif /* CONFIG_BCMWL5 */

#ifdef RTCONFIG_RALINK
#include <ralink.h>
#endif

#ifdef RTCONFIG_QCA
#include <qca.h>
#endif

#ifdef RTCONFIG_USB_MODEM
#include <usb_info.h>
#endif
#ifdef RTCONFIG_DPSTA
#include <dpsta_linux.h>
#endif

const int ifup_vap =
#if defined(RTCONFIG_QCA)
	0;
#else
	1;
#endif

#define sin_addr(s) (((struct sockaddr_in *)(s))->sin_addr)

static time_t s_last_lan_port_stopped_ts = 0;

void update_lan_state(int state, int reason)
{
	char prefix[32];
	char tmp[100], tmp1[100], *ptr;

	snprintf(prefix, sizeof(prefix), "lan_");

	_dprintf("%s(%s, %d, %d)\n", __FUNCTION__, prefix, state, reason);

#ifdef RTCONFIG_QTN	/* AP and Repeater mode, workaround to infosvr, RT-AC87U bug#38, bug#44, bug#46 */
	_dprintf("%s(workaround to infosvr)\n", __FUNCTION__);
	eval("ifconfig", "br0:0", "169.254.39.3", "netmask", "255.255.255.0");
	if(*nvram_safe_get("QTN_RPC_CLIENT"))
		eval("ifconfig", "br0:0", nvram_safe_get("QTN_RPC_CLIENT"), "netmask", "255.255.255.0");
	else
		eval("ifconfig", "br0:0", "169.254.39.1", "netmask", "255.255.255.0");
#endif

	nvram_set_int(strcat_r(prefix, "state_t", tmp), state);
	nvram_set_int(strcat_r(prefix, "sbstate_t", tmp), 0);

	if(state==LAN_STATE_INITIALIZING)
	{
		if(nvram_match(strcat_r(prefix, "proto", tmp), "dhcp")) {
			// always keep in default ip before getting ip
			nvram_set(strcat_r(prefix, "dns", tmp), nvram_default_get("lan_ipaddr"));
		}

		if (!nvram_get_int(strcat_r(prefix, "dnsenable_x", tmp))) {
			strcpy(tmp1, "");
			ptr = nvram_safe_get(strcat_r(prefix, "dns1_x", tmp));
			if (*ptr && inet_addr_(ptr) != INADDR_ANY)
				sprintf(tmp1, "%s", ptr);
			ptr = nvram_safe_get(strcat_r(prefix, "dns2_x", tmp));
			if (*ptr && inet_addr_(ptr) != INADDR_ANY)
				sprintf(tmp1 + strlen(tmp1), "%s%s", *tmp1 ? " " : "", ptr);
			nvram_set(strcat_r(prefix, "dns", tmp), tmp1);
		}
		// why not keeping default ip set above?
		else nvram_set(strcat_r(prefix, "dns", tmp), "");
	}
	else if(state==LAN_STATE_STOPPED) {
		// Save Stopped Reason
		// keep ip info if it is stopped from connected
		nvram_set_int(strcat_r(prefix, "sbstate_t", tmp), reason);
	}

}

#ifdef RTCONFIG_BCMWL6
/* workaround for BCMWL6 only */
static void set_mrate(const char* ifname, const char* prefix)
{
	float mrate = 0;
	char tmp[100];

	switch (nvram_get_int(strcat_r(prefix, "mrate_x", tmp))) {
	case 0: /* Auto */
		mrate = 0;
		break;
	case 1: /* Legacy CCK 1Mbps */
		mrate = 1;
		break;
	case 2: /* Legacy CCK 2Mbps */
		mrate = 2;
		break;
	case 3: /* Legacy CCK 5.5Mbps */
		mrate = 5.5;
		break;
	case 4: /* Legacy OFDM 6Mbps */
		mrate = 6;
		break;
	case 5: /* Legacy OFDM 9Mbps */
		mrate = 9;
		break;
	case 6: /* Legacy CCK 11Mbps */
		mrate = 11;
		break;
	case 7: /* Legacy OFDM 12Mbps */
		mrate = 12;
		break;
	case 8: /* Legacy OFDM 18Mbps */
		mrate = 18;
		break;
	case 9: /* Legacy OFDM 24Mbps */
		mrate = 24;
		break;
	case 10: /* Legacy OFDM 36Mbps */
		mrate = 36;
		break;
	case 11: /* Legacy OFDM 48Mbps */
		mrate = 48;
		break;
	case 12: /* Legacy OFDM 54Mbps */
		mrate = 54;
		break;
	default: /* Auto */
		mrate = 0;
		break;
	}

	sprintf(tmp, "wl -i %s mrate %.1f", ifname, mrate);
	system(tmp);
}
#endif

#ifdef CONFIG_BCMWL5
static int wlconf(char *ifname, int unit, int subunit)
{
	int r;
	char wl[24];
	int txpower;
	int model = get_model();
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXX";

#ifdef RTCONFIG_QTN
	if (!strcmp(ifname, "wifi0"))
		unit = 1;
#endif
	if (unit < 0) return -1;

	if (subunit < 0)
	{
#ifdef RTCONFIG_QTN
		if (unit == 1)
			goto GEN_CONF;
#endif
		snprintf(prefix, sizeof(prefix), "wl%d_", unit);

#if 0
#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
		if (psta_exist_except(unit) || psr_exist_except(unit))
		{
			eval("wlconf", ifname, "down");
			eval("wl", "-i", ifname, "radio", "off");
			return -1;
		}
#endif
#endif

#ifdef RTCONFIG_QTN
GEN_CONF:
#endif
#ifdef RTCONFIG_BCMWL6
		wl_check_chanspec();
#endif
		generate_wl_para(unit, subunit);

		for (r = 1; r < MAX_NO_MSSID; r++)	// early convert for wlx.y
			generate_wl_para(unit, r);

		if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
		{
			eval("wlconf", ifname, "down");
			eval("wl", "-i", ifname, "radio", "off");
			return -1;
		}
	}

#if 0
	if (/* !wl_probe(ifname) && */ unit >= 0) {
		// validate nvram settings foa wireless i/f
		snprintf(wl, sizeof(wl), "--wl%d", unit);
		eval("nvram", "validate", wl);
	}
#endif

#ifdef RTCONFIG_QTN
	if (unit == 1)
		return -1;
#endif

	if (unit >= 0 && subunit < 0)
	{
#ifdef RTCONFIG_OPTIMIZE_XBOX
		if (nvram_match(strcat_r(prefix, "optimizexbox", tmp), "1"))
			eval("wl", "-i", ifname, "ldpc_cap", "0");
		else
			eval("wl", "-i", ifname, "ldpc_cap", "1");	// driver default setting
#endif
#ifdef RTCONFIG_BCMWL6
#if !defined(RTCONFIG_BCM7) && !defined(RTCONFIG_BCM_7114)
		if (nvram_match(strcat_r(prefix, "ack_ratio", tmp), "1"))
			eval("wl", "-i", ifname, "ack_ratio", "4");
		else
			eval("wl", "-i", ifname, "ack_ratio", "2");	// driver default setting
#endif
		if (nvram_match(strcat_r(prefix, "ampdu_mpdu", tmp), "1"))
			eval("wl", "-i", ifname, "ampdu_mpdu", "64");
		else
#if !defined(RTCONFIG_BCM7)
			eval("wl", "-i", ifname, "ampdu_mpdu", "-1");	// driver default setting
#else
			eval("wl", "-i", ifname, "ampdu_mpdu", "32");	// driver default setting
#endif
#ifdef RTCONFIG_BCMARM
		if (nvram_match(strcat_r(prefix, "ampdu_rts", tmp), "1"))
			eval("wl", "-i", ifname, "ampdu_rts", "1");	// driver default setting
		else
			eval("wl", "-i", ifname, "ampdu_rts", "0");
#if 0
		if (nvram_match(strcat_r(prefix, "itxbf", tmp), "1"))
			eval("wl", "-i", ifname, "txbf_imp", "1");	// driver default setting
		else
			eval("wl", "-i", ifname, "txbf_imp", "0");
#endif
#endif /* RTCONFIG_BCMARM */
#else
		eval("wl", "-i", ifname, "ampdu_density", "6");		// resolve IOT with Intel STA for BRCM SDK 5.110.27.20012
#endif /* RTCONFIG_BCMWL6 */
	}

	r = eval("wlconf", ifname, "up");
	if (r == 0) {
		if (unit >= 0 && subunit < 0) {
#ifdef REMOVE
			// setup primary wl interface
			nvram_set("rrules_radio", "-1");
			eval("wl", "-i", ifname, "antdiv", nvram_safe_get(wl_nvname("antdiv", unit, 0)));
			eval("wl", "-i", ifname, "txant", nvram_safe_get(wl_nvname("txant", unit, 0)));
			eval("wl", "-i", ifname, "txpwr1", "-o", "-m", nvram_get_int(wl_nvname("txpwr", unit, 0)) ? nvram_safe_get(wl_nvname("txpwr", unit, 0)) : "-1");
			eval("wl", "-i", ifname, "interference", nvram_safe_get(wl_nvname("interfmode", unit, 0)));
#endif
#ifndef RTCONFIG_BCMWL6
			switch (model) {
				default:
					if ((unit == 0) &&
						nvram_match(strcat_r(prefix, "noisemitigation", tmp), "1"))
					{
						eval("wl", "-i", ifname, "interference_override", "4");
						eval("wl", "-i", ifname, "phyreg", "0x547", "0x4444");
						eval("wl", "-i", ifname, "phyreg", "0xc33", "0x280");
					}
					break;
			}
#else
#ifdef RTCONFIG_PROXYSTA
			if (psta_exist_except(unit)/* || psr_exist_except(unit)*/)
			{
				eval("wl", "-i", ifname, "closed", "1");
				eval("wl", "-i", ifname, "maxassoc", "0");
			}
#endif
			set_mrate(ifname, prefix);
#ifdef RTCONFIG_BCMARM
			if (nvram_match(strcat_r(prefix, "ampdu_rts", tmp), "0") &&
				nvram_match(strcat_r(prefix, "nmode", tmp), "-1"))
				eval("wl", "-i", ifname, "rtsthresh", "65535");
#endif

			wl_dfs_radarthrs_config(ifname, unit);

#endif /* RTCONFIG_BCMWL6 */
			txpower = nvram_get_int(wl_nvname("txpower", unit, 0));

			dbG("unit: %d, txpower: %d%\n", unit, txpower);

			switch (model) {
				default:

					eval("wl", "-i", ifname, "txpwr1", "-1");

					break;
			}
		}

		if (wl_client(unit, subunit)) {
			if (nvram_match(wl_nvname("mode", unit, subunit), "wet")) {
				ifconfig(ifname, IFUP | IFF_ALLMULTI, NULL, NULL);
			}
			if (nvram_get_int(wl_nvname("radio", unit, 0))) {
				snprintf(wl, sizeof(wl), "%d", unit);
				xstart("radio", "join", wl);
			}
		}
	}
	return r;
}

// -----------------------------------------------------------------------------

/*
 * Carry out a socket request including openning and closing the socket
 * Return -1 if failed to open socket (and perror); otherwise return
 * result of ioctl
 */
static int
soc_req(const char *name, int action, struct ifreq *ifr)
{
	int s;
	int rv = 0;

	if (name == NULL) return -1;

	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
		perror("socket");
		return -1;
	}
	strncpy(ifr->ifr_name, name, IFNAMSIZ);
	ifr->ifr_name[IFNAMSIZ-1] = '\0';
	rv = ioctl(s, action, ifr);
	close(s);

	return rv;
}
#endif

/* Check NVRam to see if "name" is explicitly enabled */
static inline int
wl_vif_enabled(const char *name, char *tmp)
{
	if (name == NULL) return 0;

	return (nvram_get_int(strcat_r(name, "_bss_enabled", tmp)));
}

#if defined(CONFIG_BCMWL5)
/* Set the HW address for interface "name" if present in NVRam */
static void
wl_vif_hwaddr_set(const char *name)
{
	int rc;
	char *ea;
	char hwaddr[20];
	struct ifreq ifr;
	int retry = 0;
	unsigned char comp_mac_address[ETHER_ADDR_LEN];
	snprintf(hwaddr, sizeof(hwaddr), "%s_hwaddr", name);
	ea = nvram_get(hwaddr);
	if (ea == NULL) {
		fprintf(stderr, "NET: No hw addr found for %s\n", name);
		return;
	}

#ifdef RTCONFIG_QTN
	if(strcmp(name, "wl1.1") == 0 ||
		strcmp(name, "wl1.2") == 0 ||
		strcmp(name, "wl1.3") == 0)
		return;
#endif
	fprintf(stderr, "NET: Setting %s hw addr to %s\n", name, ea);
	ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	ether_atoe(ea, (unsigned char *)ifr.ifr_hwaddr.sa_data);
	ether_atoe(ea, comp_mac_address);
	if ((rc = soc_req(name, SIOCSIFHWADDR, &ifr)) < 0) {
		fprintf(stderr, "NET: Error setting hw for %s; returned %d\n", name, rc);
	}
	memset(&ifr, 0, sizeof(ifr));
	while (retry < 100) { /* maximum 100 millisecond waiting */
		usleep(1000); /* 1 ms sleep */
		if ((rc = soc_req(name, SIOCGIFHWADDR, &ifr)) < 0) {
			fprintf(stderr, "NET: Error Getting hw for %s; returned %d\n", name, rc);
		}
		if (memcmp(comp_mac_address, (unsigned char *)ifr.ifr_hwaddr.sa_data,
			ETHER_ADDR_LEN) == 0) {
			break;
		}
		retry++;
	}
	if (retry >= 100) {
		fprintf(stderr, "Unable to check if mac was set properly for %s\n", name);
	}
}
#endif

#ifdef RTCONFIG_EMF
void
emf_mfdb_update(char *lan_ifname, char *lan_port_ifname, bool add)
{
	char word[256], *next;
	char *mgrp, *ifname;

	/* Add/Delete MFDB entries corresponding to new interface */
	foreach(word, nvram_safe_get("emf_entry"), next) {
		ifname = word;
		mgrp = strsep(&ifname, ":");

		if ((mgrp == 0) || (ifname == 0))
			continue;

		/* Add/Delete MFDB entry using the group addr and interface */
		if (strcmp(lan_port_ifname, ifname) == 0) {
			eval("emf", ((add) ? "add" : "del"),
			     "mfdb", lan_ifname, mgrp, ifname);
		}
	}

	return;
}

void
emf_uffp_update(char *lan_ifname, char *lan_port_ifname, bool add)
{
	char word[256], *next;
	char *ifname;

	/* Add/Delete UFFP entries corresponding to new interface */
	foreach(word, nvram_safe_get("emf_uffp_entry"), next) {
		ifname = word;

		if (ifname == 0)
			continue;

		/* Add/Delete UFFP entry for the interface */
		if (strcmp(lan_port_ifname, ifname) == 0) {
			eval("emf", ((add) ? "add" : "del"),
			     "uffp", lan_ifname, ifname);
		}
	}

	return;
}

void
emf_rtport_update(char *lan_ifname, char *lan_port_ifname, bool add)
{
	char word[256], *next;
	char *ifname;

	/* Add/Delete RTPORT entries corresponding to new interface */
	foreach(word, nvram_safe_get("emf_rtport_entry"), next) {
		ifname = word;

		if (ifname == 0)
			continue;

		/* Add/Delete RTPORT entry for the interface */
		if (strcmp(lan_port_ifname, ifname) == 0) {
			eval("emf", ((add) ? "add" : "del"),
			     "rtport", lan_ifname, ifname);
		}
	}

	return;
}

void
start_emf(char *lan_ifname)
{
	char word[256], *next;
	char *mgrp, *ifname;

	if (!nvram_match("emf_enable", "1"))
		return;

	/* Start EMF */
	eval("emf", "start", lan_ifname);

	/* Add the static MFDB entries */
	foreach(word, nvram_safe_get("emf_entry"), next) {
		ifname = word;
		mgrp = strsep(&ifname, ":");

		if ((mgrp == 0) || (ifname == 0))
			continue;

		/* Add MFDB entry using the group addr and interface */
		eval("emf", "add", "mfdb", lan_ifname, mgrp, ifname);
	}

	/* Add the UFFP entries */
	foreach(word, nvram_safe_get("emf_uffp_entry"), next) {
		ifname = word;
		if (ifname == 0)
			continue;

		/* Add UFFP entry for the interface */
		eval("emf", "add", "uffp", lan_ifname, ifname);
	}

	/* Add the RTPORT entries */
	foreach(word, nvram_safe_get("emf_rtport_entry"), next) {
		ifname = word;
		if (ifname == 0)
			continue;

		/* Add RTPORT entry for the interface */
		eval("emf", "add", "rtport", lan_ifname, ifname);
	}

	return;
}

static void stop_emf(char *lan_ifname)
{
	/* Stop the EMF for this LAN */
	eval("emf", "stop", lan_ifname);
	/* Remove Bridge from igs */
	eval("igs", "del", "bridge", lan_ifname);
	eval("emf", "del", "bridge", lan_ifname);
}
#endif

static void start_snooper(char *lan_ifname)
{
#ifdef CONFIG_BCMWL5
	char word[64], *next;

	if (!nvram_match("emf_enable", "1"))
		return;

	foreach (word, nvram_safe_get("lan_ifnames"), next) {
		if (eval("/usr/sbin/snooper", "-b", lan_ifname, "-s", word) == 0);
			break;
	}
#endif
}

static void stop_snooper(void)
{
	killall_tk("snooper");
}

// -----------------------------------------------------------------------------

/* Set initial QoS mode for all et interfaces that are up. */
#ifdef CONFIG_BCMWL5
void
set_et_qos_mode(void)
{
	int i, s, qos;
	struct ifreq ifr;
	struct ethtool_drvinfo info;

	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
		return;

	qos = (strcmp(nvram_safe_get("wl_wme"), "off") != 0);

	for (i = 1; i <= DEV_NUMIFS; i ++) {
		ifr.ifr_ifindex = i;
		if (ioctl(s, SIOCGIFNAME, &ifr))
			continue;
		if (ioctl(s, SIOCGIFHWADDR, &ifr))
			continue;
		if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
			continue;
		if (ioctl(s, SIOCGIFFLAGS, &ifr))
			continue;
		if (!(ifr.ifr_flags & IFF_UP))
			continue;
		/* Set QoS for et & bcm57xx devices */
		memset(&info, 0, sizeof(info));
		info.cmd = ETHTOOL_GDRVINFO;
		ifr.ifr_data = (caddr_t)&info;
		if (ioctl(s, SIOCETHTOOL, &ifr) < 0)
			continue;
		if ((strncmp(info.driver, "et", 2) != 0) &&
		    (strncmp(info.driver, "bcm57", 5) != 0))
			continue;
		ifr.ifr_data = (caddr_t)&qos;
		ioctl(s, SIOCSETCQOS, &ifr);
	}

	close(s);
}
#endif /* CONFIG_BCMWL5 */

#if defined(RTCONFIG_QCA)
void stavap_start(void)
{
	dbG("qca sta start\n");
}
#endif

#if defined(RTCONFIG_RALINK) && defined(RTCONFIG_WIRELESSREPEATER)
void apcli_start(void)
{
//repeater mode :sitesurvey channel and apclienable=1
	int ch;
	char *aif;
	int ht_ext;

	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER)
	{
		int wlc_band = nvram_get_int("wlc_band");
		if (wlc_band == 0)
			aif=nvram_safe_get("wl0_ifname");
		else
			aif=nvram_safe_get("wl1_ifname");
		ch = site_survey_for_channel(0,aif, &ht_ext);
		if(ch!=-1)
		{
			if (wlc_band == 1)
			{
				doSystem("iwpriv %s set Channel=%d", APCLI_5G, ch);
				doSystem("iwpriv %s set ApCliEnable=1", APCLI_5G);
			}
			else
			{
				doSystem("iwpriv %s set Channel=%d", APCLI_2G, ch);
				doSystem("iwpriv %s set ApCliEnable=1", APCLI_2G);
			}
			fprintf(stderr,"##set channel=%d, enable apcli ..#\n",ch);
		}
		else
			fprintf(stderr,"## Can not find pap's ssid ##\n");
	}
}
#endif	/* RTCONFIG_RALINK && RTCONFIG_WIRELESSREPEATER */

void start_wl(void)
{
#ifdef CONFIG_BCMWL5
	char *lan_ifname, *lan_ifnames, *ifname, *p;
	int unit, subunit;
	int is_client = 0;
	char tmp[100], tmp2[100], prefix[] = "wlXXXXXXXXXXXXXX";

	lan_ifname = nvram_safe_get("lan_ifname");
	if (strncmp(lan_ifname, "br", 2) == 0) {
		if ((lan_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
			p = lan_ifnames;
			while ((ifname = strsep(&p, " ")) != NULL) {
				while (*ifname == ' ') ++ifname;
				if (*ifname == 0) break;

				unit = -1; subunit = -1;

				// ignore disabled wl vifs
				if (strncmp(ifname, "wl", 2) == 0 && strchr(ifname, '.')) {
					char nv[40];
					snprintf(nv, sizeof(nv) - 1, "%s_bss_enabled", wif_to_vif(ifname));
					if (!nvram_get_int(nv))
						continue;
					if (get_ifname_unit(ifname, &unit, &subunit) < 0)
						continue;
				}
				// get the instance number of the wl i/f
				else if (wl_ioctl(ifname, WLC_GET_INSTANCE, &unit, sizeof(unit)))
					continue;

				is_client |= wl_client(unit, subunit) && nvram_get_int(wl_nvname("radio", unit, 0));

#ifdef CONFIG_BCMWL5
				snprintf(prefix, sizeof(prefix), "wl%d_", unit);
				if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
				{
					nvram_set_int(strcat_r(prefix, "timesched", tmp2), 0);	// disable wifi time-scheduler
#ifdef RTCONFIG_QTN
					if (strcmp(ifname, "wifi0"))
#endif
					{
						eval("wlconf", ifname, "down");
						eval("wl", "-i", ifname, "radio", "off");
					}
				}
				else
#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
				if (!psta_exist_except(unit)/* && !psr_exist_except(unit)*/)
#endif
				{
#ifdef RTCONFIG_QTN
					if (strcmp(ifname, "wifi0"))
#endif
					eval("wlconf", ifname, "start"); /* start wl iface */
				}
				wlconf_post(ifname);
#endif	// CONFIG_BCMWL5
			}
			free(lan_ifnames);
		}
	}
#ifdef CONFIG_BCMWL5
	else if (strcmp(lan_ifname, "")) {
		/* specific non-bridged lan iface */
		eval("wlconf", lan_ifname, "start");
	}
#endif	// CONFIG_BCMWL5

#if 0
	killall("wldist", SIGTERM);
	eval("wldist");
#endif

	if (is_client)
		xstart("radio", "join");

#ifdef RTCONFIG_PORT_BASED_VLAN
	start_vlan_wl();
#endif

	/* manual turn on 5g led for some gpio-ctrl-5g-led */
#ifdef RTCONFIG_BCMWL6
#if defined(RTAC66U) || defined(BCM4352)
	if (nvram_match("wl1_radio", "1"))
	{
#ifndef RTCONFIG_LED_BTN
		if (!(nvram_get_int("sw_mode")==SW_MODE_AP && nvram_get_int("wlc_psta") && nvram_get_int("wlc_band")==0)) {
			nvram_set("led_5g", "1");
			led_control(LED_5G, LED_ON);
		}
#else
		nvram_set("led_5g", "1");
		if (nvram_get_int("AllLED"))
			led_control(LED_5G, LED_ON);
#endif
	}
	else
	{
		nvram_set("led_5g", "0");
		led_control(LED_5G, LED_OFF);
	}

#ifdef RTCONFIG_TURBO
	if ((nvram_match("wl0_radio", "1") || nvram_match("wl1_radio", "1")
#if defined(RTAC3200) || defined(RTAC5300) || defined(RTAC5300R)
		|| nvram_match("wl2_radio", "1")
#endif
	)
#ifdef RTCONFIG_LED_BTN
		&& nvram_get_int("AllLED")
#endif
	)
		led_control(LED_TURBO, LED_ON);
	else
		led_control(LED_TURBO, LED_OFF);
#endif
#endif
#endif
#endif /* CONFIG_BCMWL5 */


#if defined(RTCONFIG_USER_LOW_RSSI) && !defined(RTCONFIG_BCMARM)
	init_wllc();
#endif


#ifdef RTCONFIG_QTN
	start_qtn_monitor();
#endif
}

void stop_wl(void)
{
}

#ifdef CONFIG_BCMWL5
static int set_wlmac(int idx, int unit, int subunit, void *param)
{
	char *ifname;

	ifname = nvram_safe_get(wl_nvname("ifname", unit, subunit));

	// skip disabled wl vifs
	if (strncmp(ifname, "wl", 2) == 0 && strchr(ifname, '.') &&
		!nvram_get_int(wl_nvname("bss_enabled", unit, subunit)))
		return 0;

	set_mac(ifname, wl_nvname("macaddr", unit, subunit),
		2 + unit + ((subunit > 0) ? ((unit + 1) * 0x10 + subunit) : 0));

	return 1;
}
#endif

static int
add_lan_routes(char *lan_ifname)
{
	return add_routes("lan_", "route", lan_ifname);
}

static int
del_lan_routes(char *lan_ifname)
{
	return del_routes("lan_", "route", lan_ifname);
}

#if defined(RTCONFIG_QCA)||defined(RTCONFIG_RALINK)
char *get_hwaddr(const char *ifname)
{
	int s = -1;
	struct ifreq ifr;
	char eabuf[32];
	char *p = NULL;

	if (ifname == NULL) return NULL;

	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) return NULL;

	strcpy(ifr.ifr_name, ifname);
	if (ioctl(s, SIOCGIFHWADDR, &ifr)) goto error;

	p = strdup(ether_etoa((const unsigned char *)ifr.ifr_hwaddr.sa_data, eabuf));

error:
	close(s);
	return p;
}
#endif

#ifdef RTCONFIG_QCA
void
qca_wif_up(const char* wif)
{
#ifdef RTCONFIG_WIRELESSREPEATER
	if ((nvram_get_int("sw_mode") == SW_MODE_REPEATER && !strncmp(wif,"sta",3)))
		ifconfig(wif, IFUP, NULL, NULL);
#endif
}


void
gen_qca_wifi_cfgs(void)
{
	pid_t pid;
	char *argv[]={"/sbin/delay_exec","1","/tmp/postwifi.sh",NULL};
	char wi2_gn[10],wi5_gn[10];
	char *p1, path2[50];
	char wif[256], *next;
	FILE *fp,*fp2;
	char tmp[100], prefix[]="wlXXXXXXX_";
	int led_onoff[2] = { LED_ON, LED_ON }, unit = -1, sunit = 0;
	int sidx = 0;
	unsigned int m, wl_mask = 0;	/* bit0~3: 2G, bit4~7: 5G */
	char conf_path[] = "/etc/Wireless/conf/hostapd_athXXX.confYYYYYY";
	char pid_path[] = "/var/run/hostapd_athXXX.pidYYYYYY";
	char entropy_path[] = "/var/run/entropy_athXXX.binYYYYYY";
	char path[] = "/sys/class/net/ath001XXXXXX";
	char path1[sizeof(NAWDS_SH_FMT) + 6];
	int i;
#ifdef RTCONFIG_WIRELESSREPEATER
	char cmd[200];
#if 0 // ARPNAT is obsolete
	char ebtable[3][40]={"PREROUTING --in-interface","POSTROUTING --out-interface","ebtables -t nat -F"};
#endif
	memset(cmd,0,sizeof(cmd));
#endif
	sprintf(wi2_gn,"%s0",WIF_2G);
	sprintf(wi5_gn,"%s0",WIF_5G);
	if (!(fp = fopen("/tmp/prewifi.sh", "w+")))
		return;
	if (!(fp2 = fopen("/tmp/postwifi.sh", "w+")))
		return;

	foreach (wif, nvram_safe_get("lan_ifnames"), next) {

		extern int get_wlsubnet(int band, const char *ifname);
		if (!guest_wlif(wif) && (unit = get_wifi_unit(wif)) >= 0 && unit < sizeof(led_onoff)) {
			snprintf(prefix, sizeof(prefix), "wl%d_", unit);
			if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
				led_onoff[unit] = LED_OFF;
		}
		if (!strcmp(wif, WIF_2G)) {			// 2.4G
			wl_mask |= 1;
			gen_ath_config(0, 0, 0);
		}
		else if (!strncmp(wif,wi2_gn,strlen(wi2_gn)))	// 2.4G guest
		{
			p1=strstr(wif,wi2_gn);
			sunit = get_wlsubnet(0, wif);
			if (p1 && sunit > 0) {
				sidx = atoi(p1 + strlen(wi2_gn));
				wl_mask |= 1 << sunit;
				gen_ath_config(0, 0, sidx);
			}
		}
		else if (!strcmp(wif, WIF_5G)) {		// 5G
			wl_mask |= (1 << 4);
			gen_ath_config(1, 1, 0);
		}
		else if (!strncmp(wif,wi5_gn,strlen(wi5_gn)))	// 5G guest
		{
			p1=strstr(wif,wi5_gn);
			sunit = get_wlsubnet(1, wif);
			if (p1 && sunit > 0) {
				sidx = atoi(p1 + strlen(wi5_gn));
				wl_mask |= 1 << (sunit + 4);
				gen_ath_config(1, 1, sidx);
			}
		}

#ifdef RTCONFIG_WIRELESSREPEATER
		else if (!strcmp(wif, STA_2G) || !strcmp(wif, STA_5G))
		{
			if(nvram_get_int("sw_mode") == SW_MODE_REPEATER){
				sprintf(cmd,"wpa_supplicant -B -P /var/run/wifi-%s.pid -D athr -i %s -b br0 -c /etc/Wireless/conf/wpa_supplicant-%s.conf",wif,wif,wif);
			}
		}
#endif
		else
			continue;

#ifdef RTCONFIG_WIRELESSREPEATER
		if (strcmp(wif,"sta0")&&strcmp(wif,"sta1"))
#endif
		{
			fprintf(fp, "/etc/Wireless/sh/prewifi_%s.sh\n",wif);
			fprintf(fp2, "/etc/Wireless/sh/postwifi_%s.sh\n",wif);
		}
	}

#if defined(PLN12)
	//fprintf(fp2, "led_ctrl %d %d\n", LED_2G_GREEN, led_onoff[0]);
	//fprintf(fp2, "led_ctrl %d %d\n", LED_2G_ORANGE, led_onoff[0]);
	fprintf(fp2, "led_ctrl %d %d\n", LED_2G_RED, led_onoff[0]);
	set_wifiled(1);
#elif defined(PLAC56)
	fprintf(fp2, "led_ctrl %d %d\n", LED_2G_GREEN, led_onoff[0]);
	//fprintf(fp2, "led_ctrl %d %d\n", LED_2G_RED, led_onoff[0]);
	fprintf(fp2, "led_ctrl %d %d\n", LED_5G_GREEN, led_onoff[0]);
	//fprintf(fp2, "led_ctrl %d %d\n", LED_5G_RED, led_onoff[0]);
	set_wifiled(1);
#else
	fprintf(fp2, "led_ctrl %d %d\n", LED_2G, led_onoff[0]);
#endif
#if defined(RTCONFIG_HAS_5G)
	fprintf(fp2, "led_ctrl %d %d\n", LED_5G, led_onoff[1]);
#endif
	fprintf(fp2, "nvram set wlready=1\n");

	fclose(fp);
	fclose(fp2);
	chmod("/tmp/prewifi.sh",0777);
	chmod("/tmp/postwifi.sh",0777);

	for (i = 0, unit = 0, sunit = 0, sidx = 0, m = 0xFF; m > 0; ++i, ++sunit, ++sidx, m >>= 1) {
		if (i == 4) {
			unit = 1;
			sunit = 0;
			sidx = 0;
		}
		__get_wlifname(unit, sidx, wif);
		sprintf(path, "/sys/class/net/%s", wif);
		if (d_exists(path))
			ifconfig(wif, 0, NULL, NULL);
		sprintf(pid_path, "/var/run/hostapd_%s.pid", wif);
		if (!f_exists(pid_path))
			continue;

		kill_pidfile_tk(pid_path);
	}

	doSystem("/tmp/prewifi.sh");
#ifdef RTCONFIG_WIRELESSREPEATER
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER){
		doSystem(cmd);
#if 0 // ARPNAT is obsolete
		sleep(1);
		doSystem(ebtable[2]);
		for(i=0;i<2;i++)
		{
			if(nvram_get_int("wlc_band")==0)
				sprintf(cmd,"ebtables -t nat -A %s %s -j arpnat --arpnat-target ACCEPT",ebtable[i], STA_2G);
			else
				sprintf(cmd,"ebtables -t nat -A %s %s -j arpnat --arpnat-target ACCEPT",ebtable[i], STA_5G);
			doSystem(cmd);
		}
#else
		doSystem("iwpriv %s extap 1", get_staifname(nvram_get_int("wlc_band")));
#endif
		doSystem("ifconfig %s up", get_staifname(nvram_get_int("wlc_band")));
	}
#endif
#ifdef RTCONFIG_PROXYSTA
	if (nvram_get_int("wlc_psta") == 0) {
#endif
	m = wl_mask & 0xFF;
	for (i = 0, unit = 0, sunit = 0, sidx = 0; m > 0; ++i, ++sunit, ++sidx, m >>= 1) {
		char *str;
		if (i == 4) {
			unit = 1;
			sunit = 0;
			sidx = 0;
		}
		if (!nvram_match(wl_nvname("bss_enabled", unit, sunit), "1"))
		{
			sidx--;
			continue;
		}

		__get_wlifname(unit, sidx, wif);
		tweak_wifi_ps(wif);

#ifdef RTCONFIG_WIRELESSREPEATER
		if(nvram_get_int("sw_mode") == SW_MODE_REPEATER)
			doSystem("iwpriv %s athnewind 1", wif);
#endif

		sprintf(path2, "/etc/Wireless/sh/postwifi_%s.sh", wif);
		if (!(fp = fopen(path2, "a")))
			continue;

		/* hostapd is not required if
		 * 1. Open system and WPS is disabled.
		 *    a. primary 2G/5G and WPS is disabled
		 *    b. guest 2G/5G
		 * 2. WEP
		 */
		if (!strcmp(nvram_safe_get(wl_nvname("auth_mode_x", unit, sunit)), "open") &&
		    ((!sunit && !nvram_get_int("wps_enable")) || sunit)) {
			fclose(fp);
			continue;
		} else if (!strcmp(nvram_safe_get(wl_nvname("auth_mode_x", unit, sunit)), "shared")) {
			fclose(fp);
			continue;
		}

		sprintf(conf_path, "/etc/Wireless/conf/hostapd_%s.conf", wif);
		sprintf(pid_path, "/var/run/hostapd_%s.pid", wif);
		sprintf(entropy_path, "/var/run/entropy_%s.bin", wif);
		fprintf(fp, "hostapd -d -B %s -P %s -e %s\n", conf_path, pid_path, entropy_path);

		str = nvram_safe_get(wl_nvname("radio", unit, 0));
		if (str && strlen(str)) {
			char vphy[10];
			char *updown = atoi(str)? "up" : "down";
			strcpy(vphy, get_vphyifname(unit));
			fprintf(fp, "ifconfig %s %s\n", vphy, updown);
			fprintf(fp, "ifconfig %s %s\n", wif, updown);

			/* Connect to peer WDS AP after VAP up */
			sprintf(path1, NAWDS_SH_FMT, wif);
			if (!sunit && atoi(str) && f_exists(path1)) {
				fprintf(fp, "%s\n", path1);
			}
		}

		fclose(fp);
	}

	argv[1]="4";
	_eval(argv, NULL, 0, &pid);
	//system("/tmp/postwifi.sh");
	//sleep(1);
#ifdef RTCONFIG_PROXYSTA
	}
#endif
}

static void
set_wlpara_qca(const char* wif, int band)
{
/* TBD.fill some eraly init here
	char tmp[100], prefix[]="wlXXXXXXX_";

	snprintf(prefix, sizeof(prefix), "wl%d_", band);

	if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
		radio_ra(wif, band, 0);
	else
	{
		int txpower = atoi(nvram_safe_get(strcat_r(prefix, "TxPower", tmp)));
		if ((txpower >= 0) && (txpower <= 100))
			doSystem("iwpriv %s set TxPower=%d",wif, txpower);
	}
	eval("iwpriv", (char *)wif, "set", "IgmpAdd=01:00:5e:7f:ff:fa");
	eval("iwpriv", (char *)wif, "set", "IgmpAdd=01:00:5e:00:00:09");
	eval("iwpriv", (char *)wif, "set", "IgmpAdd=01:00:5e:00:00:fb");
*/
}


static int
wlconf_qca(const char* wif)
{
	int unit = 0;
	char word[256], *next;
	char tmp[128], prefix[] = "wlXXXXXXXXXX_";
	char *p;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		snprintf(prefix, sizeof(prefix), "wl%d_", unit);

		if (!strcmp(word, wif))
		{
			p = get_hwaddr(wif);
			if (p)
			{
				nvram_set(strcat_r(prefix, "hwaddr", tmp), p);
				free(p);
			}
			if (!strcmp(word, WIF_2G))
				set_wlpara_qca(wif, 0);
			else if (!strcmp(word, WIF_5G))
				set_wlpara_qca(wif, 1);
		}

		unit++;
	}
	return 0;
}

#endif
#ifdef RTCONFIG_RALINK
static void
gen_ra_config(const char* wif)
{
	char word[256], *next;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		if (!strcmp(word, wif))
		{
			if (!strcmp(word, nvram_safe_get("wl0_ifname"))) // 2.4G
			{
				if (!strncmp(word, "rai", 3))	// iNIC
					gen_ralink_config(0, 1);
				else
					gen_ralink_config(0, 0);
			}
			else if (!strcmp(word, nvram_safe_get("wl1_ifname"))) // 5G
			{
				if (!strncmp(word, "rai", 3))	// iNIC
					gen_ralink_config(1, 1);
				else
					gen_ralink_config(1, 0);
			}
		}
	}
}

static int
radio_ra(const char *wif, int band, int ctrl)
{
	char tmp[100], prefix[]="wlXXXXXXX_";

	snprintf(prefix, sizeof(prefix), "wl%d_", band);

	if (wif == NULL) return -1;

	if (!ctrl)
	{
		doSystem("iwpriv %s set RadioOn=0", wif);
	}
	else
	{
		if (nvram_match(strcat_r(prefix, "radio", tmp), "1"))
			doSystem("iwpriv %s set RadioOn=1", wif);
	}

	return 0;
}

static void
set_wlpara_ra(const char* wif, int band)
{
	char tmp[100], prefix[]="wlXXXXXXX_";

	snprintf(prefix, sizeof(prefix), "wl%d_", band);

	if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
		radio_ra(wif, band, 0);
	else
	{
		int txpower = nvram_get_int(strcat_r(prefix, "txpower", tmp));
		if ((txpower >= 0) && (txpower <= 100))
			doSystem("iwpriv %s set TxPower=%d",wif, txpower);
	}
#if 0
	if (nvram_match(strcat_r(prefix, "bw", tmp), "2"))
	{
		int channel = get_channel(band);

		if (channel)
			eval("iwpriv", (char *)wif, "set", "HtBw=1");
	}
#endif
#if 0 //defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)	/* set RT3352 iNIC in kernel driver to avoid loss setting after reset */
	if(strcmp(wif, "rai0") == 0)
	{
		int i;
		char buf[32];
		eval("iwpriv", (char *)wif, "set", "asiccheck=1");
		for(i = 0; i < 3; i++)
		{
			sprintf(buf, "setVlanId=%d,%d", i + INIC_VLAN_IDX_START, i + INIC_VLAN_ID_START);
			eval("iwpriv", (char *)wif, "switch", buf);	//set vlan id can pass through the switch insided the RT3352.
								//set this before wl connection linu up. Or the traffic would be blocked by siwtch and need to reconnect.
		}
		for(i = 0; i < 5; i++)
		{
			sprintf(buf, "setPortPowerDown=%d,%d", i, 1);
			eval("iwpriv", (char *)wif, "switch", buf);	//power down the Ethernet PHY of RT3352 internal switch.
		}
	}
#endif // RTCONFIG_WLMODULE_RT3352_INIC_MII
	eval("iwpriv", (char *)wif, "set", "IgmpAdd=01:00:5e:7f:ff:fa");
	eval("iwpriv", (char *)wif, "set", "IgmpAdd=01:00:5e:00:00:09");
	eval("iwpriv", (char *)wif, "set", "IgmpAdd=01:00:5e:00:00:fb");
}

static int
wlconf_ra(const char* wif)
{
	int unit = 0;
	char word[256], *next;
	char tmp[128], prefix[] = "wlXXXXXXXXXX_";
	char *p;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		snprintf(prefix, sizeof(prefix), "wl%d_", unit);

		if (!strcmp(word, wif))
		{
			p = get_hwaddr(wif);
			if (p)
			{
				nvram_set(strcat_r(prefix, "hwaddr", tmp), p);
				free(p);
			}

			if (!strcmp(word, WIF_2G))
				set_wlpara_ra(wif, 0);
#if defined(RTCONFIG_HAS_5G)
			else if (!strcmp(word, WIF_5G))
				set_wlpara_ra(wif, 1);
#endif	/* RTCONFIG_HAS_5G */
		}

		unit++;
	}
	return 0;
}
#endif

#ifdef RTCONFIG_IPV6
void ipv6_sysconf(const char *ifname, const char *name, int value)
{
	char path[PATH_MAX], sval[16];

	if (ifname == NULL || name == NULL)
		return;

	snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/%s", ifname, name);
	snprintf(sval, sizeof(sval), "%d", value);
	f_write_string(path, sval, 0, 0);
}

int ipv6_getconf(const char *ifname, const char *name)
{
	char path[PATH_MAX], sval[16];

	if (ifname == NULL || name == NULL)
		return 0;

	snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/%s", ifname, name);
	if (f_read_string(path, sval, sizeof(sval)) <= 0)
		return 0;

	return atoi(sval);
}

void set_default_accept_ra(int flag)
{
	ipv6_sysconf("all", "accept_ra", flag ? 1 : 0);
	ipv6_sysconf("default", "accept_ra", flag ? 1 : 0);
}

void set_default_forwarding(int flag)
{
	ipv6_sysconf("all", "forwarding", flag ? 1 : 0);
	ipv6_sysconf("default", "forwarding", flag ? 1 : 0);
}

void set_intf_ipv6_dad(const char *ifname, int addbr, int flag)
{
	if (ifname == NULL) return;

	ipv6_sysconf(ifname, "accept_dad", flag ? 2 : 0);
	if (flag)
		ipv6_sysconf(ifname, "dad_transmits", addbr ? 2 : 1);
}

void enable_ipv6(const char *ifname)
{
	if (ifname == NULL) return;

	ipv6_sysconf(ifname, "disable_ipv6", 0);
}

void disable_ipv6(const char *ifname)
{
	if (ifname == NULL) return;

	ipv6_sysconf(ifname, "disable_ipv6", 1);
}

void config_ipv6(int enable, int incl_wan)
{
	DIR *dir;
	struct dirent *dirent;
	int service;
	int match;
	char word[256], *next;

	if ((dir = opendir("/proc/sys/net/ipv6/conf")) != NULL) {
		while ((dirent = readdir(dir)) != NULL) {
			if (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))
				continue;
			if (incl_wan)
				goto ALL;
			match = 0;
			foreach (word, nvram_safe_get("wan_ifnames"), next) {
				if (!strcmp(dirent->d_name, word))
				{
					match = 1;
					break;
				}
			}
			if (match) continue;
ALL:
			if (enable)
				enable_ipv6(dirent->d_name);
			else
				disable_ipv6(dirent->d_name);

			if (enable && strcmp(dirent->d_name, "all") &&
				strcmp(dirent->d_name, "default") &&
				!with_ipv6_linklocal_addr(dirent->d_name))
				reset_ipv6_linklocal_addr(dirent->d_name, 0);
		}
		closedir(dir);
	}

	if (is_routing_enabled())
	{
		service = get_ipv6_service();
		switch (service) {
		case IPV6_NATIVE_DHCP:
			set_default_accept_ra(1);
		case IPV6_6IN4:
		case IPV6_6TO4:
		case IPV6_6RD:
		case IPV6_MANUAL:
		default:
			set_default_accept_ra(0);
			break;
		}

		set_default_forwarding(1);
	}
	else set_default_accept_ra(0);
}

#ifdef RTCONFIG_DUALWAN
void start_lan_ipv6(void)
{
	char *lan_ifname = strdup(nvram_safe_get("lan_ifname"));
	char *dualwan_wans = nvram_safe_get("wans_dualwan");
	char *dualwan_mode = nvram_safe_get("wans_mode");

	if (!(!strstr(dualwan_mode, "none") &&
		(!strcmp(dualwan_mode, "fo") || !strcmp(dualwan_mode, "fb"))))
		return;

	set_intf_ipv6_dad(lan_ifname, 0, 1);
	config_ipv6(ipv6_enabled() && is_routing_enabled(), 0);
	start_ipv6();
}

void stop_lan_ipv6(void)
{
	char *lan_ifname = strdup(nvram_safe_get("lan_ifname"));
	char *dualwan_wans = nvram_safe_get("wans_dualwan");
	char *dualwan_mode = nvram_safe_get("wans_mode");

	if (!(!strstr(dualwan_mode, "none") &&
		(!strcmp(dualwan_mode, "fo") || !strcmp(dualwan_mode, "fb"))))
		return;

	stop_ipv6();
	set_intf_ipv6_dad(lan_ifname, 0, 0);
	config_ipv6(ipv6_enabled() && is_routing_enabled(), 0);
}

void restart_dnsmasq_ipv6(void)
{
	char *dualwan_wans = nvram_safe_get("wans_dualwan");
	char *dualwan_mode = nvram_safe_get("wans_mode");

	if (!(!strstr(dualwan_mode, "none") &&
		(!strcmp(dualwan_mode, "fo") || !strcmp(dualwan_mode, "fb"))))
		return;

	if (!ipv6_enabled())
		return;

	start_dnsmasq();
}
#endif
#endif

#ifdef RTCONFIG_DPSTA
static int
dpsta_ioctl(char *name, void *buf, int len)
{
	struct ifreq ifr;
	int ret = 0;
	int s;

	/* open socket to kernel */
	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("socket");
		return errno;
	}

	strncpy(ifr.ifr_name, name, IFNAMSIZ);
	ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
	ifr.ifr_data = (caddr_t)buf;
	if ((ret = ioctl(s, SIOCDEVPRIVATE, &ifr)) < 0)
		perror(ifr.ifr_name);

	/* cleanup */
	close(s);
	return ret;
}
#endif

void start_lan(void)
{
	char *lan_ifname;
	struct ifreq ifr;
	char *lan_ifnames, *ifname, *p;
	char *hostname;
	int sfd;
	uint32 ip;
	int hwaddrset;
	char eabuf[32];
	char word[256], *next;
	int match;
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXX";
	int i;
#ifdef RTCONFIG_WIRELESSREPEATER
	char domain_mapping[64];
#endif
#ifdef CONFIG_BCMWL5
	int unit, subunit, sta = 0;
#endif
#ifdef RTCONFIG_DPSTA
	char hwaddr[ETHER_ADDR_LEN];
	char macaddr[18];
	int s=0;
	int dpsta=0;
	dpsta_enable_info_t info = { 0 };
#endif
#ifdef RTCONFIG_GMAC3
	char name[80];
#endif
#ifdef __CONFIG_DHDAP__
	int is_dhd;
#endif /* __CONFIG_DHDAP__ */

	update_lan_state(LAN_STATE_INITIALIZING, 0);

	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER) {
		nvram_set("wlc_mode", "0");
		nvram_set("btn_ez_radiotoggle", "0"); // reset to default
	}

	set_hostname();

	convert_routes();

#if defined(RTCONFIG_RALINK) || defined(RTCONFIG_QCA)
	init_wl();
#endif

#ifdef RTCONFIG_LED_ALL
	led_control(LED_ALL, LED_ON);
#if defined(RTAC1200HP) || defined(RTN56UB1) || defined(RTN56UB2)
	led_control(LED_5G, LED_ON);
	led_control(LED_2G, LED_ON);
#endif
#endif

#ifdef CONFIG_BCMWL5
	init_wl_compact();
	wlconf_pre();

	if (0)
	{
		foreach_wif(1, NULL, set_wlmac);
		check_afterburner();
	}
#endif

	if (no_need_to_start_wps() ||
	    wps_band_radio_off(get_radio_band(nvram_get_int("wps_band"))) ||
	    wps_band_ssid_broadcast_off(get_radio_band(nvram_get_int("wps_band"))))
		nvram_set("wps_enable", "0");
	/* recover wps enable option back to original state */
	else if(!nvram_match("wps_enable", nvram_safe_get("wps_enable_old"))){
		nvram_set("wps_enable", nvram_safe_get("wps_enable_old"));
	}

	if ((sfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) return;

#ifdef RTCONFIG_GMAC3
	/* In 3GMAC mode, bring up the GMAC forwarding interfaces.
	 * Then bringup the primary wl interfaces that avail of hw switching.
	 */
	if (nvram_match("gmac3_enable", "1")) { /* __CONFIG_GMAC3__ */

		/* Bring up GMAC#0 and GMAC#1 forwarder(s) */
		foreach(name, nvram_safe_get("fwddevs"), next) {
			ifconfig(name, 0, NULL, NULL);
			ifconfig(name, IFUP | IFF_ALLMULTI | IFF_PROMISC, NULL, NULL);
		}
	}
#endif
	lan_ifname = strdup(nvram_safe_get("lan_ifname"));
	if (strncmp(lan_ifname, "br", 2) == 0) {
		_dprintf("%s: setting up the bridge %s\n", __FUNCTION__, lan_ifname);

		eval("brctl", "addbr", lan_ifname);
		eval("brctl", "setfd", lan_ifname, "0");
#ifdef RTAC87U
		eval("brctl", "stp", lan_ifname, nvram_safe_get("lan_stp"));
#else
		if (is_routing_enabled())
			eval("brctl", "stp", lan_ifname, nvram_safe_get("lan_stp"));
		else
			eval("brctl", "stp", lan_ifname, "0");
#endif

#ifdef RTCONFIG_IPV6
		if (get_ipv6_service() != IPV6_DISABLED) {
			ipv6_sysconf(lan_ifname, "accept_ra", 0);
			ipv6_sysconf(lan_ifname, "forwarding", 0);
		}
		set_intf_ipv6_dad(lan_ifname, 1, 1);
#endif
#ifdef RTCONFIG_EMF
		if (nvram_match("emf_enable", "1")) {
			eval("emf", "add", "bridge", lan_ifname);
			eval("igs", "add", "bridge", lan_ifname);
		}
#endif
		inet_aton(nvram_safe_get("lan_ipaddr"), (struct in_addr *)&ip);

		hwaddrset = 0;
		if ((lan_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
			p = lan_ifnames;

			while ((ifname = strsep(&p, " ")) != NULL) {
				while (*ifname == ' ') ++ifname;
				if (*ifname == 0) break;
#ifdef CONFIG_BCMWL5
				if (strncmp(ifname, "wl", 2) == 0) {
					if (!wl_vif_enabled(ifname, tmp)) {
						continue; /* Ignore disabled WL VIF */
					}
					wl_vif_hwaddr_set(ifname);
				}
				unit = -1; subunit = -1;
#endif

				// ignore disabled wl vifs
				if (guest_wlif(ifname)) {
					char nv[40];
					char nv2[40];
					snprintf(nv, sizeof(nv) - 1, "%s_bss_enabled", wif_to_vif(ifname));
					snprintf(nv2, sizeof(nv2) - 1, "%s_expire", wif_to_vif(ifname));
					if(nvram_get_int("sw_mode")==SW_MODE_REPEATER)
					{
						nvram_set(nv,"1");	/*wl0.x_bss_enable=1*/
						nvram_unset(nv2);	/*remove wl0.x_expire*/
					}
					if (nvram_get_int(nv2) && nvram_get_int("success_start_service")==0)
					{
						nvram_set(nv, "0");
					}

					if (!nvram_get_int(nv))
						continue;
#ifdef CONFIG_BCMWL5
					if (get_ifname_unit(ifname, &unit, &subunit) < 0)
						continue;
#endif
				}
#ifdef CONFIG_BCMWL5
				else
					wl_ioctl(ifname, WLC_GET_INSTANCE, &unit, sizeof(unit));
#endif

#ifdef RTCONFIG_QCA
				qca_wif_up(ifname);
#endif
#ifdef RTCONFIG_RALINK
				gen_ra_config(ifname);
#endif
#if defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)
				{ // add interface for iNIC packets
					char *nic_if, *nic_ifs, *nic_lan_ifnames;
					if((nic_lan_ifnames = strdup(nvram_safe_get("nic_lan_ifnames"))))
					{
						nic_ifs = nic_lan_ifnames;
						while ((nic_if = strsep(&nic_ifs, " ")) != NULL) {
							while (*nic_if == ' ')
								nic_if++;
							if (*nic_if == 0)
								break;
							if(strcmp(ifname, nic_if) == 0)
							{
								int vlan_id;
								if((vlan_id = atoi(ifname + 4)) > 0)
								{
									char id[8];
									sprintf(id, "%d", vlan_id);
									eval("vconfig", "add", "eth2", id);
								}
								break;
							}
						}
						free(nic_lan_ifnames);
					}
				}
#endif
#if defined(RTCONFIG_QCA)
				if (!ifup_vap &&
				    (!strncmp(ifname, WIF_2G, strlen(WIF_2G)) ||
				     !strncmp(ifname, WIF_5G, strlen(WIF_5G))))
				{
					/* Don't up VAP interface here. */
				} else {
#endif
					// bring up interface
					if (ifconfig(ifname, IFUP | IFF_ALLMULTI, NULL, NULL) != 0) {
#if defined(RTAC56U) || defined(RTAC56S)
						if(strncmp(ifname, "eth2", 4)==0)
							nvram_set("5g_fail", "1");	// gpio led
#endif
#ifdef RTCONFIG_QTN
						if (strcmp(ifname, "wifi0"))
#endif
						continue;
					}
#if defined(RTAC56U) || defined(RTAC56S)
					else if(strncmp(ifname, "eth2", 4)==0)
						nvram_unset("5g_fail");			// gpio led
#endif
#if defined(RTCONFIG_QCA)
				}
#endif

#ifdef RTCONFIG_RALINK
				wlconf_ra(ifname);
#elif defined(RTCONFIG_QCA)
				wlconf_qca(ifname);
#endif

				// set the logical bridge address to that of the first interface
				strlcpy(ifr.ifr_name, lan_ifname, IFNAMSIZ);
				if ((!hwaddrset) ||
				    (ioctl(sfd, SIOCGIFHWADDR, &ifr) == 0 &&
				    memcmp(ifr.ifr_hwaddr.sa_data, "\0\0\0\0\0\0", ETHER_ADDR_LEN) == 0)) {
					strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
					if (ioctl(sfd, SIOCGIFHWADDR, &ifr) == 0) {
						strlcpy(ifr.ifr_name, lan_ifname, IFNAMSIZ);
						ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;

#ifdef RTCONFIG_QCA
#ifdef RTCONFIG_WIRELESSREPEATER
						if(nvram_get_int("sw_mode") == SW_MODE_REPEATER) {
							char *stamac;
							if((stamac=getStaMAC())!=NULL)
								ether_atoe(stamac,ifr.ifr_hwaddr.sa_data);
						}
#endif
#endif

#ifdef RTCONFIG_DPSTA
						memcpy(hwaddr, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
#endif
						_dprintf("%s: setting MAC of %s bridge to %s\n", __FUNCTION__,
							ifr.ifr_name, ether_etoa((const unsigned char *) ifr.ifr_hwaddr.sa_data, eabuf));
						ioctl(sfd, SIOCSIFHWADDR, &ifr);
						hwaddrset = 1;
					}
				}
#ifdef CONFIG_BCMWL5
				if (wlconf(ifname, unit, subunit) == 0) {
					const char *mode = nvram_safe_get(wl_nvname("mode", unit, subunit));

					if (strcmp(mode, "wet") == 0) {
						// Enable host DHCP relay
						if (nvram_match("lan_proto", "static")) {
#ifdef __CONFIG_DHDAP__
							is_dhd = !dhd_probe(ifname);
							if(is_dhd) {
								char macbuf[sizeof("wet_host_mac") + 1 + ETHER_ADDR_LEN];
								dhd_iovar_setbuf(ifname, "wet_host_mac", ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN , macbuf, sizeof(macbuf));
							}
							else
#endif /* __CONFIG_DHDAP__ */
							wl_iovar_set(ifname, "wet_host_mac", ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
						}
					}

					sta |= (strcmp(mode, "sta") == 0);
					if ((strcmp(mode, "ap") != 0) && (strcmp(mode, "wet") != 0)
						&& (strcmp(mode, "psta") != 0) && (strcmp(mode, "psr") != 0))
						continue;
				}
#endif
#ifdef RTCONFIG_QTN
				if (!strcmp(ifname, "wifi0"))
					continue;
#endif
				/* Don't attach the main wl i/f in wds mode */
				match = 0, i = 0;
				foreach (word, nvram_safe_get("wl_ifnames"), next) {
					if (!strcmp(ifname, word))
					{
						snprintf(prefix, sizeof(prefix), "wl%d_", i);
						if (nvram_match(strcat_r(prefix, "mode_x", tmp), "1"))
#ifdef RTCONFIG_QCA
							match = 0;
#else
							match = 1;
#endif

#ifdef RTCONFIG_PROXYSTA
#ifdef RTCONFIG_RALINK
						if (mediabridge_mode()) {
							ifconfig(ifname, 0, NULL, NULL);
							match = 1;
						}
#endif
#endif

						break;
					}

					i++;
				}
#if defined(RTCONFIG_PROXYSTA) && defined(RTCONFIG_DPSTA)
				/* Dont add main wl i/f when proxy sta is
			 	 * enabled in both bands. Instead add the
			 	 * dpsta interface.
			 	 */
				if (strstr(nvram_safe_get("dpsta_ifnames"), ifname)) {
					ifname = !dpsta ? "dpsta" : "";
					dpsta++;

					/* Assign hw address */
					if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) >= 0) {
						strncpy(ifr.ifr_name, "dpsta", IFNAMSIZ);
					if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0 &&
						memcmp(ifr.ifr_hwaddr.sa_data, "\0\0\0\0\0\0",
						ETHER_ADDR_LEN) == 0) {
							ether_etoa((const unsigned char *) hwaddr, macaddr);
							ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
							memcpy(ifr.ifr_hwaddr.sa_data, hwaddr,
								ETHER_ADDR_LEN);
							if (ioctl(s, SIOCSIFHWADDR, &ifr)) {
								close(s);
								return;
							}
						}
					}
					close(s);
				}
#endif
#ifdef RTCONFIG_GMAC3
				/* In 3GMAC mode, skip wl interfaces that avail of hw switching.
				 *
				 * Do not add these wl interfaces to the LAN bridge as they avail of
				 * HW switching. Misses in the HW switch's ARL will be forwarded via vlan1
				 * to br0 (i.e. via the network GMAC#2).
				 */
				if (nvram_match("gmac3_enable", "1") &&
					find_in_list(nvram_get("fwd_wlandevs"), ifname))
						goto gmac3_no_swbr;
#endif
#ifdef RTCONFIG_PORT_BASED_VLAN
				if (vlan_enable() && check_if_exist_vlan_ifnames(ifname))
					match = 1;
#endif
				if (!match)
				{
					eval("brctl", "addif", lan_ifname, ifname);
#ifdef RTCONFIG_GMAC3
gmac3_no_swbr:
#endif
#ifdef RTCONFIG_EMF
					if (nvram_match("emf_enable", "1"))
						eval("emf", "add", "iface", lan_ifname, ifname);
#endif
				}
				enable_wifi_bled(ifname);
			}

			free(lan_ifnames);
		}
	}
	// --- this shouldn't happen ---
	else if (*lan_ifname) {
		ifconfig(lan_ifname, IFUP | IFF_ALLMULTI, NULL, NULL);
#ifdef CONFIG_BCMWL5
		wlconf(lan_ifname, -1, -1);
#endif
	}
	else {
		close(sfd);
		free(lan_ifname);
		return;
	}

	// Get current LAN hardware address
	strlcpy(ifr.ifr_name, lan_ifname, IFNAMSIZ);
	if (ioctl(sfd, SIOCGIFHWADDR, &ifr) == 0) nvram_set("lan_hwaddr", ether_etoa((const unsigned char *) ifr.ifr_hwaddr.sa_data, eabuf));

	close(sfd);
	/* Set initial QoS mode for LAN ports. */
#ifdef CONFIG_BCMWL5
	set_et_qos_mode();
#endif

#ifdef RTCONFIG_DPSTA
	/* Configure dpsta module */
	if (dpsta) {
		int di = 0;

		/* Enable and set the policy to in-band and cross-band
		 * forwarding policy.
		 */
		info.enable = 1;
		info.policy = atoi(nvram_safe_get("dpsta_policy"));
		info.lan_uif = atoi(nvram_safe_get("dpsta_lan_uif"));
		foreach(name, nvram_safe_get("dpsta_ifnames"), next) {
			strcpy((char *)info.upstream_if[di], name);
			di++;
		}
		dpsta_ioctl("dpsta", &info, sizeof(dpsta_enable_info_t));

		/* Bring up dpsta interface */
		ifconfig("dpsta", IFUP, NULL, NULL);
	}
#endif

	// bring up and configure LAN interface
#ifdef RTCONFIG_DHCP_OVERRIDE
	if(nvram_match("lan_proto", "static") || nvram_get_int("sw_mode") == SW_MODE_AP)
#else
	if(nvram_match("lan_proto", "static"))
#endif
		ifconfig(lan_ifname, IFUP | IFF_ALLMULTI, nvram_safe_get("lan_ipaddr"), nvram_safe_get("lan_netmask"));
	else
		ifconfig(lan_ifname, IFUP | IFF_ALLMULTI, nvram_default_get("lan_ipaddr"), nvram_default_get("lan_netmask"));

#ifdef RTCONFIG_QCA
	gen_qca_wifi_cfgs();
#endif


#if defined(RTCONFIG_CFEZ) && defined(RTCONFIG_BCMARM)
	//move to ate-broadcom.c
#else
	config_loopback();
#endif

#ifdef RTCONFIG_PORT_BASED_VLAN
	start_vlan_ifnames();
#endif

#ifdef RTCONFIG_IPV6
	set_intf_ipv6_dad(lan_ifname, 0, 1);
	config_ipv6(ipv6_enabled() && is_routing_enabled(), 0);
	start_ipv6();
#endif

#ifdef RTCONFIG_EMF
	start_emf(lan_ifname);
#endif
	start_snooper(lan_ifname);

	if (nvram_match("lan_proto", "dhcp")
#ifdef RTCONFIG_DEFAULT_AP_MODE
			&& !nvram_match("ate_flag", "1")
#endif
	) {
		// only none routing mode need lan_proto=dhcp
		if (pids("udhcpc"))
		{
			killall("udhcpc", SIGUSR2);
			killall("udhcpc", SIGTERM);
			unlink("/tmp/udhcpc_lan");
		}

		hostname = nvram_safe_get("computer_name");

		char *dhcp_argv[] = { "udhcpc",
					"-i", "br0",
					"-p", "/var/run/udhcpc_lan.pid",
					"-s", "/tmp/udhcpc_lan",
					(*hostname != 0 ? "-H" : NULL), (*hostname != 0 ? hostname : NULL),
					NULL };
		pid_t pid;

		symlink("/sbin/rc", "/tmp/udhcpc_lan");
		_eval(dhcp_argv, NULL, 0, &pid);

		update_lan_state(LAN_STATE_CONNECTING, 0);
	}
	else {
		if(is_routing_enabled())
		{
			update_lan_state(LAN_STATE_CONNECTED, 0);
			add_lan_routes(lan_ifname);
			start_default_filter(0);
		}
		else lan_up(lan_ifname);
	}

	free(lan_ifname);

#ifdef RTCONFIG_SHP
	if(nvram_get_int("lfp_disable")==0) {
		restart_lfp();
	}
#endif

#ifdef WEB_REDIRECT
#ifdef RTCONFIG_WIRELESSREPEATER
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER) {
		// Drop the DHCP server from PAP.
		repeater_pap_disable();

		// When CONNECTED, need to redirect 10.0.0.1(from the browser's cache) to DUT's home page.
		repeater_nat_setting();
	}
#endif

	if(nvram_get_int("sw_mode") != SW_MODE_AP) {
		redirect_setting();
_dprintf("nat_rule: stop_nat_rules 1.\n");
		stop_nat_rules();
	}

	start_wanduck();
#endif

#ifdef RTCONFIG_BCMWL6
	set_acs_ifnames();
#endif

#ifdef RTCONFIG_WIRELESSREPEATER
	if(nvram_get_int("sw_mode")==SW_MODE_REPEATER) {
		start_wlcconnect();
	}

	if(get_model() == MODEL_APN12HP &&
		nvram_get_int("sw_mode") == SW_MODE_AP) {
		// When CONNECTED, need to redirect 10.0.0.1
		// (from the browser's cache) to DUT's home page.
		repeater_nat_setting();
		eval("ebtables", "-t", "broute", "-F");
		eval("ebtables", "-t", "filter", "-F");
		eval("ebtables", "-t", "broute", "-I", "BROUTING", "-d", "00:E0:11:22:33:44", "-j", "redirect", "--redirect-target", "DROP");
		sprintf(domain_mapping, "%x %s", inet_addr(nvram_safe_get("lan_ipaddr")), DUT_DOMAIN_NAME);
		f_write_string("/proc/net/dnsmqctrl", domain_mapping, 0, 0);
		start_nat_rules();
	}
#endif

#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
	if ((is_psta(nvram_get_int("wlc_band")) || is_psr(nvram_get_int("wlc_band")))
#if defined(RTCONFIG_BCM7) || defined(RTCONFIG_BCM_7114)
#ifdef RTCONFIG_GMAC3
		&& !nvram_match("gmac3_enable", "1")
#endif
		&& nvram_match("ctf_disable", "1")
#endif
	) setup_dnsmq(1);
#endif

#ifdef RTCONFIG_QTN
	if(*nvram_safe_get("QTN_RPC_CLIENT"))
		eval("ifconfig", "br0:0", nvram_safe_get("QTN_RPC_CLIENT"), "netmask", "255.255.255.0");
	else
		eval("ifconfig", "br0:0", "169.254.39.1", "netmask", "255.255.255.0");
#endif
	nvram_set("reload_svc_radio", "1");

	_dprintf("%s %d\n", __FUNCTION__, __LINE__);
}

#ifdef RTCONFIG_RALINK
static void
stop_wds_ra(const char* lan_ifname, const char* wif)
{
	char prefix[32];
	char wdsif[32];
	int i;

	if (strcmp(wif, WIF_2G) && strcmp(wif, WIF_5G))
		return;

	if (!strncmp(wif, "rai", 3))
		snprintf(prefix, sizeof(prefix), "wdsi");
	else
		snprintf(prefix, sizeof(prefix), "wds");

	for (i = 0; i < 4; i++)
	{
		sprintf(wdsif, "%s%d", prefix, i);
		doSystem("brctl delif %s %s 1>/dev/null 2>&1", lan_ifname, wdsif);
		ifconfig(wdsif, 0, NULL, NULL);
	}
}
#endif

void stop_lan(void)
{
	_dprintf("%s %d\n", __FUNCTION__, __LINE__);

	char *lan_ifname;
	char *lan_ifnames, *p, *ifname;

	lan_ifname = nvram_safe_get("lan_ifname");

	if(is_routing_enabled())
	{
		stop_wanduck();
		del_lan_routes(lan_ifname);
	}
#ifdef RTCONFIG_WIRELESSREPEATER
	else if(nvram_get_int("sw_mode") == SW_MODE_REPEATER) {
		stop_wlcconnect();

		stop_wanduck();
	}
#endif
#ifdef WEB_REDIRECT
	else if (is_apmode_enabled())
		stop_wanduck();
#endif

#ifdef RTCONFIG_IPV6
	stop_ipv6();
	set_intf_ipv6_dad(lan_ifname, 0, 0);
	config_ipv6(ipv6_enabled() && is_routing_enabled(), 1);
#endif

	ifconfig("lo", 0, NULL, NULL);

	ifconfig(lan_ifname, 0, NULL, NULL);

#ifdef RTCONFIG_GMAC3
	char name[20], *next;
	if (nvram_match("gmac3_enable", "1")) { /* __CONFIG_GMAC3__ */

		/* Bring down GMAC#0 and GMAC#1 forwarder(s) */
		foreach(name, nvram_safe_get("fwddevs"), next) {
			ifconfig(name, 0, NULL, NULL);
		}
	}
#endif

	if (module_loaded("ebtables")) {
		eval("ebtables", "-F");
		eval("ebtables", "-t", "broute", "-F");
	}

	if (strncmp(lan_ifname, "br", 2) == 0) {
		if ((lan_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
			p = lan_ifnames;
			while ((ifname = strsep(&p, " ")) != NULL) {
				while (*ifname == ' ') ++ifname;
				if (*ifname == 0) break;
				disable_wifi_bled(ifname);
#ifdef RTCONFIG_DPSTA
				if (!strcmp(ifname, "dpsta")) {
					char dp_uif[80], *dpnext;
					foreach(dp_uif, nvram_safe_get("dpsta_ifnames"),
						dpnext) {
						eval("wlconf", dp_uif, "down");
						ifconfig(dp_uif, 0, NULL, NULL);
					}
				}
#endif
#ifdef CONFIG_BCMWL5
#ifdef RTCONFIG_QTN
				if (strcmp(ifname, "wifi0"))
#endif
				{
					eval("wlconf", ifname, "down");
					eval("wl", "-i", ifname, "radio", "off");
					ifconfig(ifname, 0, NULL, NULL);
				}
#elif defined RTCONFIG_RALINK
				if (!strncmp(ifname, "ra", 2))
					stop_wds_ra(lan_ifname, ifname);
#elif defined(RTCONFIG_QCA)
				/* do nothing */
#endif	// BCMWL5

#ifdef RTCONFIG_GMAC3
				/* List of primary WLAN interfaces that avail of HW switching. */
				/* In 3GMAC mode, each wl interfaces in "fwd_wlandevs" don't
				 * attach to the bridge.
				 */
				if (nvram_match("gmac3_enable", "1") &&
					find_in_list(nvram_get("fwd_wlandevs"), ifname))
					goto gmac3_no_swbr;
#endif
				eval("brctl", "delif", lan_ifname, ifname);
#ifdef RTCONFIG_GMAC3
gmac3_no_swbr:
#endif
#ifdef RTCONFIG_EMF
				if (nvram_match("emf_enable", "1"))
					eval("emf", "del", "iface", lan_ifname, ifname);
#endif
#if defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)
				{ // remove interface for iNIC packets
					char *nic_if, *nic_ifs, *nic_lan_ifnames;
					if((nic_lan_ifnames = strdup(nvram_safe_get("nic_lan_ifnames"))))
					{
						nic_ifs = nic_lan_ifnames;
						while ((nic_if = strsep(&nic_ifs, " ")) != NULL) {
							while (*nic_if == ' ')
								nic_if++;
							if (*nic_if == 0)
								break;
							if(strcmp(ifname, nic_if) == 0)
							{
								eval("vconfig", "rem", ifname);
								break;
							}
						}
						free(nic_lan_ifnames);
					}
				}
#endif
			}
			free(lan_ifnames);
		}
#ifdef RTCONFIG_EMF
		stop_emf(lan_ifname);
#endif
		stop_snooper();
		eval("brctl", "delbr", lan_ifname);
	}
	else if (*lan_ifname) {
#ifdef CONFIG_BCMWL5
		eval("wlconf", lan_ifname, "down");
		eval("wl", "-i", lan_ifname, "radio", "off");
#endif
	}

#ifdef RTCONFIG_PORT_BASED_VLAN
	stop_vlan_ifnames();
#endif

#ifdef RTCONFIG_BCMWL6
#if defined(RTAC66U) || defined(BCM4352)
	nvram_set("led_5g", "0");
	led_control(LED_5G, LED_OFF);
#ifdef RTCONFIG_TURBO
	led_control(LED_TURBO, LED_OFF);
#endif
#endif
#endif

	// inform watchdog to stop WPS LED
	kill_pidfile_s("/var/run/watchdog.pid", SIGUSR2);

	fini_wl();

	init_nvram();	// init nvram lan_ifnames
	wl_defaults();	// init nvram wlx_ifnames & lan_ifnames

	update_lan_state(LAN_STATE_STOPPED, 0);

	killall("udhcpc", SIGUSR2);
	killall("udhcpc", SIGTERM);
	unlink("/tmp/udhcpc_lan");

#ifdef RTCONFIG_QTN
	nvram_unset("qtn_ready");
#endif

	_dprintf("%s %d\n", __FUNCTION__, __LINE__);
}

void do_static_routes(int add)
{
	char *buf;
	char *p, *q;
	char *dest, *mask, *gateway, *metric, *ifname;
	int r;

	if ((buf = strdup(nvram_safe_get(add ? "routes_static" : "routes_static_saved"))) == NULL) return;
	if (add) nvram_set("routes_static_saved", buf);
		else nvram_unset("routes_static_saved");
	p = buf;
	while ((q = strsep(&p, ">")) != NULL) {
		if (vstrsep(q, "<", &dest, &gateway, &mask, &metric, &ifname) != 5) continue;
		ifname = nvram_safe_get((*ifname == 'L') ? "lan_ifname" :
					((*ifname == 'W') ? "wan_iface" : "wan_ifname"));
		if (add) {
			for (r = 3; r >= 0; --r) {
				if (route_add(ifname, atoi(metric) + 1, dest, gateway, mask) == 0) break;
				sleep(1);
			}
		}
		else {
			route_del(ifname, atoi(metric) + 1, dest, gateway, mask);
		}
	}
	free(buf);
}

#ifdef CONFIG_BCMWL5

/*
 * EAP module
 */

static int
wl_send_dif_event(const char *ifname, uint32 event)
{
	static int s = -1;
	int len, n;
	struct sockaddr_in to;
	char data[IFNAMSIZ + sizeof(uint32)];

	if (ifname == NULL) return -1;

	/* create a socket to receive dynamic i/f events */
	if (s < 0) {
		s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s < 0) {
			perror("socket");
			return -1;
		}
	}

	/* Init the message contents to send to eapd. Specify the interface
	 * and the event that occured on the interface.
	 */
	strncpy(data, ifname, IFNAMSIZ);
	*(uint32 *)(data + IFNAMSIZ) = event;
	len = IFNAMSIZ + sizeof(uint32);

	/* send to eapd */
	to.sin_addr.s_addr = inet_addr(EAPD_WKSP_UDP_ADDR);
	to.sin_family = AF_INET;
	to.sin_port = htons(EAPD_WKSP_DIF_UDP_PORT);

	n = sendto(s, data, len, 0, (struct sockaddr *)&to,
		sizeof(struct sockaddr_in));

	if (n != len) {
		perror("udp send failed\n");
		return -1;
	}

	_dprintf("hotplug_net(): sent event %d\n", event);

	return n;
}
#endif

void hotplug_net(void)
{
	char *lan_ifname = nvram_safe_get("lan_ifname");
	char *interface, *action;
	bool psta_if, dyn_if, add_event, remove_event;
	int unit;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";
#ifdef RTCONFIG_USB_MODEM
	char device_path[128], usb_path[PATH_MAX], usb_node[32], port_path[8];
	char nvram_name[32];
	char word[PATH_MAX], *next;
	char modem_type[8];
#endif

	if (!(interface = getenv("INTERFACE")) ||
	    !(action = getenv("ACTION")))
		return;

	_dprintf("hotplug net INTERFACE=%s ACTION=%s\n", interface, action);

#ifdef LINUX26
	add_event = !strcmp(action, "add");
#else
	add_event = !strcmp(action, "register");
#endif

#ifdef LINUX26
	remove_event = !strcmp(action, "remove");
#else
	remove_event = !strcmp(action, "unregister");
#endif

#ifdef RTCONFIG_BCMWL6
	psta_if = wl_wlif_is_psta(interface);
#else
	psta_if = 0;
#endif

	dyn_if = !strncmp(interface, "wds", 3) || psta_if;

	if (!dyn_if && !remove_event)
		goto NEITHER_WDS_OR_PSTA;

	if (dyn_if && add_event) {
#ifdef RTCONFIG_RALINK
		if (nvram_match("sw_mode", "2"))
			return;

		if (strncmp(interface, WDSIF_5G, strlen(WDSIF_5G)) == 0 && isdigit(interface[strlen(WDSIF_5G)]))
		{
			if (nvram_match("wl1_mode_x", "0")) return;
		}
		else
		{
			if (nvram_match("wl0_mode_x", "0")) return;
		}
#endif

		/* Bring up the interface and add to the bridge */
		ifconfig(interface, IFUP, NULL, NULL);

#ifdef RTCONFIG_EMF
		if (nvram_match("emf_enable", "1")) {
			eval("emf", "add", "iface", lan_ifname, interface);
			emf_mfdb_update(lan_ifname, interface, TRUE);
			emf_uffp_update(lan_ifname, interface, TRUE);
			emf_rtport_update(lan_ifname, interface, TRUE);
		}
#endif
#ifdef CONFIG_BCMWL5
		/* Indicate interface create event to eapd */
		if (psta_if) {
			_dprintf("hotplug_net(): send dif event to %s\n", interface);
			wl_send_dif_event(interface, 0);
			return;
		}
#endif
		if (!strncmp(lan_ifname, "br", 2)) {
			eval("brctl", "addif", lan_ifname, interface);
#ifdef CONFIG_BCMWL5
			/* Inform driver to send up new WDS link event */
			if (wl_iovar_setint(interface, "wds_enable", 1)) {
				_dprintf("%s set wds_enable failed\n", interface);
				return;
			}
#endif
		}

		return;
	}

#ifdef CONFIG_BCMWL5
	if (remove_event) {
		/* Indicate interface delete event to eapd */
		wl_send_dif_event(interface, 1);

#ifdef RTCONFIG_EMF
		if (nvram_match("emf_enable", "1"))
			eval("emf", "del", "iface", lan_ifname, interface);
#endif /* RTCONFIG_EMF */
	}
#endif

#if defined(RTCONFIG_QCA)
	if (remove_event) { // TBD
		f_write_string("/dev/sfe", "clear", 0, 0);
	}
#endif

NEITHER_WDS_OR_PSTA:
	/* PPP interface removed */
	if (strncmp(interface, "ppp", 3) == 0 && remove_event) {
		while ((unit = ppp_ifunit(interface)) >= 0) {
			snprintf(prefix, sizeof(prefix), "wan%d_", unit);
			nvram_set(strcat_r(prefix, "pppoe_ifname", tmp), "");
		}
	}
#ifdef RTCONFIG_USB_MODEM
	// Android phone, RNDIS interface, NCM, qmi_wwan.
	else if(!strncmp(interface, "usb", 3)) {
		if(nvram_get_int("sw_mode") != SW_MODE_ROUTER)
			return;

		if ((unit = get_usbif_dualwan_unit()) < 0) {
			usb_dbg("(%s): in the current dual wan mode, didn't support the USB modem.\n", interface);
			return;
		}

		snprintf(prefix, sizeof(prefix), "wan%d_", unit);

		if(!strcmp(action, "add")) {
			unsigned int vid, pid;
			char buf[32];
			int i = 0;

			logmessage("hotplug", "add net %s.", interface);
			_dprintf("hotplug net: add net %s.\n", interface);

			snprintf(device_path, 128, "%s/%s/device", SYS_NET, interface);

			memset(usb_path, 0, PATH_MAX);
			if(realpath(device_path, usb_path) == NULL){
				_dprintf("hotplug net(%s): skip 1. device_path %s.\n", interface, device_path);
				return;
			}

			if(get_usb_node_by_string(usb_path, usb_node, 32) == NULL){
				_dprintf("hotplug net(%s): skip 2. usb_path %s.\n", interface, usb_path);
				return;
			}

			if(get_path_by_node(usb_node, port_path, 8) == NULL){
				_dprintf("hotplug net(%s): skip 3. usb_node %s.\n", interface, usb_node);
				return;
			}

#ifdef RTCONFIG_INTERNAL_GOBI
			if((nvram_get_int("usb_gobi") == 1 && strcmp(port_path, "2"))
					|| (nvram_get_int("usb_gobi") != 1 && !strcmp(port_path, "2"))
					)
				return;
#endif

			snprintf(buf, 32, "%s", nvram_safe_get("usb_modem_act_path"));
			if(strcmp(buf, "") && strcmp(buf, usb_node)){
				_dprintf("hotplug net(%s): skip 4. port_path %s.\n", interface, port_path);
				return;
			}

			if(!strcmp(buf, ""))
				nvram_set("usb_modem_act_path", usb_node); // needed by find_modem_type.sh.

			while(!strcmp(nvram_safe_get("usb_modem_act_type"), "") && i++ < 3){
				_dprintf("hotplug net(%s): wait for the modem driver at %d second...\n", interface, i);
				eval("find_modem_type.sh");
				sleep(1);
			}

			snprintf(modem_type, 8, "%s", nvram_safe_get("usb_modem_act_type"));
			_dprintf("hotplug net: usb_modem_act_type=%s.\n", modem_type);
			if(!strcmp(modem_type, "mbim")){
				_dprintf("hotplug net(%s): skip the MBIM interface.\n", interface);
				return;
			}

			vid = get_usb_vid(usb_node);
			pid = get_usb_pid(usb_node);
			logmessage("hotplug", "Got net %s, vid 0x%x, pid 0x%x.", interface, vid, pid);
			_dprintf("hotplug net: Got net %s, vid 0x%x, pid 0x%x.\n", interface, vid, pid);

			if(!strcmp(interface, "usb0")){
				_dprintf("hotplug net(%s): let usb0 wait for usb1.\n", interface);
				sleep(1);
			}

			snprintf(nvram_name, 32, "usb_path%s_act", port_path);
			snprintf(word, PATH_MAX, "%s", nvram_safe_get(nvram_name));
			_dprintf("hotplug net(%s): %s %s.\n", interface, nvram_name, word);

			//if(!strcmp(word, "usb1") && strcmp(interface, "usb1")){
			if(!strcmp(word, "usb1")){
				// If there are 2 usbX, use QMI:usb1 to connect.
				logmessage("hotplug", "skip to set net %s.", interface);
				_dprintf("hotplug net: skip to set net %s.\n", interface);
				return;
			}
			else{
				logmessage("hotplug", "set net %s.", interface);
				_dprintf("hotplug net: set net %s.\n", interface);
				nvram_set(nvram_name, interface);
				snprintf(buf, 32, "%u", vid);
				nvram_set("usb_modem_act_vid", buf);
				snprintf(buf, 32, "%u", pid);
				nvram_set("usb_modem_act_pid", buf);
				nvram_set("usb_modem_act_dev", interface);
				nvram_set(strcat_r(prefix, "ifname", tmp), interface);
			}

			// won't wait at the busy time of every start_wan when booting.
			if(!strcmp(nvram_safe_get("success_start_service"), "1")){
				// wait for Andorid phones.
				_dprintf("hotplug net INTERFACE=%s ACTION=%s: wait 2 seconds...\n", interface, action);
				sleep(2);
			}
			else{
				// wait that the modem nvrams are ready during booting.
				// e.q. Huawei E398.
				int i = 0;
				snprintf(nvram_name, 32, "usb_path%s", port_path);
				while(strcmp(nvram_safe_get(nvram_name), "modem") && i++ < 3){
					_dprintf("%s: waiting %d second for the modem nvrams...", __FUNCTION__, i);
					sleep(1);
				}
			}

			if(nvram_get_int("usb_modem_act_reset") != 0){
				logmessage("hotplug", "Modem had been reset and woken up net %s.", interface);
				_dprintf("hotplug net(%s) had been reset and woken up.\n", interface);
				nvram_set("usb_modem_act_reset", "2");
				return;
			}

#ifdef RTCONFIG_DUALWAN
			// avoid the busy time of every start_wan when booting.
			if(!strcmp(nvram_safe_get("success_start_service"), "0")
					&& strcmp(modem_type, "rndis") // rndis modem can't get IP when booting.
					&& strcmp(modem_type, "qmi") // qmi modem often be blocked when booting.
					&& (unit == WAN_UNIT_FIRST || nvram_match("wans_mode", "lb"))
					){
				_dprintf("%s: start_wan_if(%d)!\n", __FUNCTION__, unit);
				start_wan_if(unit);
			}
#endif
		}
		else{
			logmessage("hotplug", "remove net %s.", interface);
			_dprintf("hotplug net: remove net %s.\n", interface);

			snprintf(usb_node, 32, "%s", nvram_safe_get("usb_modem_act_path"));
			if(strlen(usb_node) <= 0)
				return;

			if(get_path_by_node(usb_node, port_path, 8) == NULL)
				return;

			snprintf(nvram_name, 32, "usb_path%s_act", port_path);
			if(strcmp(nvram_safe_get(nvram_name), interface))
				return;

			nvram_unset(nvram_name);

			clean_modem_state(1);

			nvram_set(strcat_r(prefix, "ifname", tmp), "");

			/* Stop dhcp client */
			stop_udhcpc(unit);
		}
	}
	// Beceem dongle, ASIX USB to RJ45 converter, ECM, rndis(LU-150: ethX with RNDIS).
	else if(!strncmp(interface, "eth", 3)) {
		if(nvram_get_int("sw_mode") != SW_MODE_ROUTER)
			return;

#ifdef RTCONFIG_RALINK
		// In the Ralink platform eth2 isn't defined at wan_ifnames or other nvrams,
		// so it need to deny additionally.
		if(!strncmp(interface, "eth2", 4))
			return;

#if defined(RTN67U) || defined(RTN36U3) || defined(RTN56U) || defined(RTN56UB1) || defined(RTN56UB2)
		/* eth3 may be used as WAN on some Ralink/MTK platform. */
		if(!strncmp(interface, "eth3", 4))
			return;
#endif

#elif defined(RTCONFIG_QCA)
		/* All models use eth0/eth1 as LAN or WAN. */
		if (!strncmp(interface, "eth0", 4) || !strncmp(interface, "eth1", 4))
			return;
#if defined(RTCONFIG_SWITCH_RTL8370M_PHY_QCA8033_X2) || defined(RTCONFIG_SWITCH_RTL8370MB_PHY_QCA8033_X2)
		/* BRT-AC828M2 SR1~SR3: eth2/eth3 are WAN1/WAN2.
		 * BRT-AC828M2 SR4+   : eth2/eth3 are LAN2/WAN2.
		 */
		if (!strncmp(interface, "eth2", 4) || !strncmp(interface, "eth3", 4))
			return;
#endif

#else
		// for all models, ethernet's physical interface.
		if(!strcmp(interface, "eth0"))
			return;
#endif

		// Not wired ethernet.
		foreach(word, nvram_safe_get("wan_ifnames"), next)
			if(!strcmp(interface, word))
				return;

		// Not wired ethernet.
		foreach(word, nvram_safe_get("lan_ifnames"), next)
			if(!strcmp(interface, word))
				return;

		// Not wireless ethernet.
		foreach(word, nvram_safe_get("wl_ifnames"), next)
			if(!strcmp(interface, word))
				return;

		if ((unit = get_usbif_dualwan_unit()) < 0) {
			usb_dbg("(%s): in the current dual wan mode, didn't support the USB modem.\n", interface);
			return;
		}

		snprintf(prefix, sizeof(prefix), "wan%d_", unit);

		if(!strcmp(action, "add")) {
			nvram_set(strcat_r(prefix, "ifname", tmp), interface);

			_dprintf("hotplug net INTERFACE=%s ACTION=%s: wait 2 seconds...\n", interface, action);
			sleep(2);

			snprintf(device_path, 128, "%s/%s/device", SYS_NET, interface);

			// Huawei E353.
			if(check_if_dir_exist(device_path)) {
				memset(usb_path, 0, PATH_MAX);
				if(realpath(device_path, usb_path) == NULL)
					return;

				if(get_usb_node_by_string(usb_path, usb_node, 32) == NULL)
					return;

				if(get_path_by_node(usb_node, port_path, 8) == NULL)
					return;

				snprintf(nvram_name, 32, "usb_path%s_act", port_path);

				nvram_set(nvram_name, interface);
				nvram_set("usb_modem_act_path", usb_node);
				nvram_set("usb_modem_act_dev", interface);
			}
			// Beceem dongle.
			else{
				// do nothing.
			}

#ifdef RTCONFIG_DUALWAN
			// avoid the busy time of every start_wan when booting.
			if(!strcmp(nvram_safe_get("success_start_service"), "0")
					&& (unit == WAN_UNIT_FIRST || nvram_match("wans_mode", "lb"))
					){
				_dprintf("%s: start_wan_if(%d)!\n", __FUNCTION__, unit);
				start_wan_if(unit);
			}
#endif
		}
		else{
			nvram_set(strcat_r(prefix, "ifname", tmp), "");

			stop_wan_if(unit);

			snprintf(usb_node, 32, "%s", nvram_safe_get("usb_modem_act_path"));
			if(strlen(usb_node) <= 0)
				return;

			if(get_path_by_node(usb_node, port_path, 8) == NULL)
				return;

			snprintf(nvram_name, 32, "usb_path%s_act", port_path);

			if(!strcmp(nvram_safe_get(nvram_name), interface)) {
				nvram_unset(nvram_name);

				clean_modem_state(1);
			}

			/* Stop dhcp client */
			stop_udhcpc(unit);

#ifdef RTCONFIG_USB_BECEEM
			if(strlen(port_path) <= 0)
				system("asus_usbbcm usbbcm remove");
#endif
		}
	}
#ifdef RTCONFIG_USB_BECEEM
	// WiMAX: madwimax, gctwimax.
	else if(!strncmp(interface, "wimax", 5)){
		if(nvram_get_int("sw_mode") != SW_MODE_ROUTER)
			return;

		if((unit = get_usbif_dualwan_unit()) < 0){
			usb_dbg("(%s): in the current dual wan mode, didn't support the USB modem.\n", interface);
			return;
		}

		snprintf(prefix, sizeof(prefix), "wan%d_", unit);

		if(!strcmp(action, "add")){
			snprintf(usb_node, 32, "%s", nvram_safe_get("usb_modem_act_path"));
			if(strlen(usb_node) <= 0)
				return;
			if(get_path_by_node(usb_node, port_path, 8) == NULL)
				return;

			nvram_set(strcat_r(prefix, "ifname", tmp), interface);

			snprintf(nvram_name, 32, "usb_path%s_act", port_path);
			nvram_set(nvram_name, interface);

			_dprintf("hotplug net INTERFACE=%s ACTION=%s: wait 2 seconds...\n", interface, action);
			sleep(2);

			// This is the second step of start_wan_if().
			// First start_wan_if(): call the WiMAX process up.
			// Second start_wan_if(): call the udhcpc up.
			_dprintf("%s: start_wan_if(%d)!\n", __FUNCTION__, unit);
			start_wan_if(unit);
		}
		else // remove: do nothing.
			;
	}
#endif
#endif
}

#ifdef CONFIG_BCMWL5
static int is_same_addr(struct ether_addr *addr1, struct ether_addr *addr2)
{
	int i;
	for (i = 0; i < 6; i++) {
		if (addr1->octet[i] != addr2->octet[i])
			return 0;
	}
	return 1;
}

#define WL_MAX_ASSOC	128
static int check_wl_client(char *ifname, int unit, int subunit)
{
	struct ether_addr bssid;
	wl_bss_info_t *bi;
	char buf[WLC_IOCTL_MAXLEN];
	struct maclist *mlist;
	int mlsize, i;
	int associated, authorized;

	*(uint32 *)buf = WLC_IOCTL_MAXLEN;
	if (wl_ioctl(ifname, WLC_GET_BSSID, &bssid, ETHER_ADDR_LEN) < 0 ||
	    wl_ioctl(ifname, WLC_GET_BSS_INFO, buf, WLC_IOCTL_MAXLEN) < 0)
		return 0;

	bi = (wl_bss_info_t *)(buf + 4);
	if ((bi->SSID_len == 0) ||
	    (bi->BSSID.octet[0] + bi->BSSID.octet[1] + bi->BSSID.octet[2] +
	     bi->BSSID.octet[3] + bi->BSSID.octet[4] + bi->BSSID.octet[5] == 0))
		return 0;

	associated = 0;
	authorized = strstr(nvram_safe_get(wl_nvname("akm", unit, subunit)), "psk") == 0;

	mlsize = sizeof(struct maclist) + (WL_MAX_ASSOC * sizeof(struct ether_addr));
	if ((mlist = malloc(mlsize)) != NULL) {
		mlist->count = WL_MAX_ASSOC;
		if (wl_ioctl(ifname, WLC_GET_ASSOCLIST, mlist, mlsize) == 0) {
			for (i = 0; i < mlist->count; ++i) {
				if (is_same_addr(&mlist->ea[i], &bi->BSSID)) {
					associated = 1;
					break;
				}
			}
		}

		if (associated && !authorized) {
			memset(mlist, 0, mlsize);
			mlist->count = WL_MAX_ASSOC;
			strcpy((char*)mlist, "autho_sta_list");
			if (wl_ioctl(ifname, WLC_GET_VAR, mlist, mlsize) == 0) {
				for (i = 0; i < mlist->count; ++i) {
					if (is_same_addr(&mlist->ea[i], &bi->BSSID)) {
						authorized = 1;
						break;
					}
				}
			}
		}
		free(mlist);
	}
	return (associated && authorized);
}
#else /* ! CONFIG_BCMWL5 */
static int check_wl_client(char *ifname, int unit, int subunit)
{
// add ralink client check code here
	return 0;
}
#endif /* CONFIG_BCMWL5 */

#define STACHECK_CONNECT	30
#define STACHECK_DISCONNECT	5

static int radio_join(int idx, int unit, int subunit, void *param)
{
	int i;
	char s[32], f[64];
	char *ifname;

	int *unit_filter = param;
	if (*unit_filter >= 0 && *unit_filter != unit) return 0;

	if (!nvram_get_int(wl_nvname("radio", unit, 0)) || !wl_client(unit, subunit)) return 0;

	ifname = nvram_safe_get(wl_nvname("ifname", unit, subunit));

	// skip disabled wl vifs
	if (strncmp(ifname, "wl", 2) == 0 && strchr(ifname, '.') &&
		!nvram_get_int(wl_nvname("bss_enabled", unit, subunit)))
		return 0;

	sprintf(f, "/var/run/radio.%d.%d.pid", unit, subunit < 0 ? 0 : subunit);
	if (f_read_string(f, s, sizeof(s)) > 0) {
		if ((i = atoi(s)) > 1) {
			kill(i, SIGTERM);
			sleep(1);
		}
	}

	if (fork() == 0) {
		sprintf(s, "%d", getpid());
		f_write(f, s, sizeof(s), 0, 0644);

		int stacheck_connect = nvram_get_int("sta_chkint");
		if (stacheck_connect <= 0)
			stacheck_connect = STACHECK_CONNECT;
		int stacheck;

		while (get_radio(unit, 0) && wl_client(unit, subunit)) {

			if (check_wl_client(ifname, unit, subunit)) {
				stacheck = stacheck_connect;
			}
			else {
#ifdef RTCONFIG_RALINK
// add ralink client mode code here.
#elif defined(RTCONFIG_QCA)
// add QCA client mode code here.
#else
				eval("wl", "-i", ifname, "disassoc");
#ifdef CONFIG_BCMWL5
				char *amode, *sec = nvram_safe_get(wl_nvname("akm", unit, subunit));

				if (strstr(sec, "psk2")) amode = "wpa2psk";
				else if (strstr(sec, "psk")) amode = "wpapsk";
				else if (strstr(sec, "wpa2")) amode = "wpa2";
				else if (strstr(sec, "wpa")) amode = "wpa";
				else if (nvram_get_int(wl_nvname("auth", unit, subunit))) amode = "shared";
				else amode = "open";

				eval("wl", "-i", ifname, "join", nvram_safe_get(wl_nvname("ssid", unit, subunit)),
					"imode", "bss", "amode", amode);
#else
				eval("wl", "-i", ifname, "join", nvram_safe_get(wl_nvname("ssid", unit, subunit)));
#endif
#endif
				stacheck = STACHECK_DISCONNECT;
			}
			sleep(stacheck);
		}
		unlink(f);
	}

	return 1;
}

enum {
	RADIO_OFF = 0,
	RADIO_ON = 1,
	RADIO_TOGGLE = 2,
	RADIO_SWITCH = 3
};

static int radio_toggle(int idx, int unit, int subunit, void *param)
{
	if (!nvram_get_int(wl_nvname("radio", unit, 0))) return 0;

	int *op = param;

	if (*op == RADIO_TOGGLE) {
		*op = get_radio(unit, subunit) ? RADIO_OFF : RADIO_ON;
	}

	set_radio(*op, unit, subunit);
	return *op;
}

#ifdef RTCONFIG_BCMWL6
static void led_bh_prep(int post)
{
	int wlon_unit = -1, i = 0, maxi = 1;
	char ifbuf[5];

	switch (get_model()) {
		case MODEL_RTAC56S:
		case MODEL_RTAC56U:
			if(post)
			{
				eval("wl", "ledbh", "3", "7");
				eval("wl", "-i", "eth2", "ledbh", "10", "7");
			}
			else
			{
				eval("wl", "ledbh", "3", "1");
				eval("wl", "-i", "eth2", "ledbh", "10", "1");
				led_control(LED_5G, LED_ON);
				eval("wlconf", "eth1", "up");
				eval("wl", "maxassoc", "0");
				eval("wlconf", "eth2", "up");
				eval("wl", "-i", "eth2", "maxassoc", "0");
			}
			break;
		case MODEL_RTAC5300:
		case MODEL_RTAC5300R:
		case MODEL_RTAC88U:
		case MODEL_RTAC3100:

			if(post)
			{
				wlon_unit = nvram_get_int("wlc_band");

				if(!mediabridge_mode()) {
					eval("wl", "ledbh", "9", "7");
					eval("wl", "-i", "eth2", "ledbh", "9", "7");
#if defined(RTAC5300) || defined(RTAC5300R)
					eval("wl", "-i", "eth3", "ledbh", "9", "7");
#endif
				} else { /* turn off other bands led */
#if defined(RTAC5300) || defined(RTAC5300R)
					maxi = 2;
#endif
					for(i=0; i<=maxi; ++i) {
						if( i == wlon_unit )
							continue;
						memset(ifbuf, 0, sizeof(ifbuf));
						sprintf(ifbuf, "eth%d", i+1);
						eval("wl", "-i", ifbuf, "ledbh", "9", "0");
					}
				}
			}
			else
			{

				eval("wl", "ledbh", "9", "1");
				eval("wl", "-i", "eth2", "ledbh", "9", "1");
#if defined(RTAC5300) || defined(RTAC5300R)
				eval("wl", "-i", "eth3", "ledbh", "9", "1");
#endif
				eval("wlconf", "eth1", "up");
				eval("wl", "maxassoc", "0");
				eval("wlconf", "eth2", "up");
				eval("wl", "-i", "eth2", "maxassoc", "0");
#if defined(RTAC5300) || defined(RTAC5300R)
				eval("wlconf", "eth3", "up");
				eval("wl", "-i", "eth3", "maxassoc", "0");
#endif
			}
			break;
		case MODEL_RTAC3200:
		case MODEL_DSLAC68U:
		case MODEL_RPAC68U:
		case MODEL_RTAC68U:
		case MODEL_RTAC87U:
			if(post)
			{
				eval("wl", "ledbh", "10", "7");
				eval("wl", "-i", "eth2", "ledbh", "10", "7");

#ifdef RTCONFIG_LEDARRAY
				eval("wl", "ledbh", "9", "7");
				eval("wl", "ledbh", "0", "7");
				eval("wl", "-i", "eth2", "ledbh", "9", "7");
				eval("wl", "-i", "eth2", "ledbh", "0", "7");
#endif
#if defined(RTAC3200)
				eval("wl", "-i", "eth3", "ledbh", "10", "7");
#endif
			}
			else
			{
				eval("wl", "ledbh", "10", "1");
				eval("wl", "-i", "eth2", "ledbh", "10", "1");

#ifdef RTCONFIG_LEDARRAY
				eval("wl", "ledbh", "9", "1");
				eval("wl", "ledbh", "0", "1");
				eval("wl", "-i", "eth2", "ledbh", "9", "1");
				eval("wl", "-i", "eth2", "ledbh", "0", "1");
#endif
#if defined(RTAC3200)
				eval("wl", "-i", "eth3", "ledbh", "10", "1");
#endif
#ifdef BCM4352
				led_control(LED_5G, LED_ON);
#endif
#ifdef RTCONFIG_TURBO
				led_control(LED_TURBO, LED_ON);
#endif
				eval("wlconf", "eth1", "up");
				eval("wl", "maxassoc", "0");
				eval("wlconf", "eth2", "up");
				eval("wl", "-i", "eth2", "maxassoc", "0");
#if defined(RTAC3200)
				eval("wlconf", "eth3", "up");
				eval("wl", "-i", "eth3", "maxassoc", "0");
#endif
			}
			break;
		case MODEL_RTAC53U:
			if(post)
			{
				eval("wl", "-i", "eth1", "ledbh", "3", "7");
				eval("wl", "-i", "eth2", "ledbh", "9", "7");
			}
			else
			{
				eval("wl", "-i", "eth1", "ledbh", "3", "1");
				eval("wl", "-i", "eth2", "ledbh", "9", "1");
				eval("wlconf", "eth1", "up");
				eval("wl", "maxassoc", "0");
				eval("wlconf", "eth2", "up");
				eval("wl", "-i", "eth2", "maxassoc", "0");
			}
			break;
		default:
			break;
	}
}
#endif

int radio_switch(int subunit)
{
	char tmp[100], tmp2[100], prefix[] = "wlXXXXXXXXXXXXXX";
	char *p;
	int i;		// unit
	int sw = 1;	// record get_radio status
	int MAX = 0;	// if MAX = 1: single band, 2: dual band

#ifdef RTCONFIG_WIRELESSREPEATER
	// repeater mode not support HW radio
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER) {
		dbG("[radio switch] repeater mode not support HW radio\n");
		return -1;
	}
#endif

	foreach(tmp, nvram_safe_get("wl_ifnames"), p)
		MAX++;

	for(i = 0; i < MAX; i++) {
		sw &= get_radio(i, subunit);
	}

	sw = !sw;

	for(i = 0; i < MAX; i++) {
		// set wlx_radio
		snprintf(prefix, sizeof(prefix), "wl%d_", i);
		nvram_set_int(strcat_r(prefix, "radio", tmp), sw);
		//dbG("[radio switch] %s=%d, MAX=%d\n", tmp, sw, MAX); // radio switch
		set_radio(sw, i, subunit); 	// set wifi radio

		// trigger wifi via WPS/HW toggle, no matter disable or enable wifi, must disable wifi time-scheduler
		// prio : HW switch > WPS/HW toggle > time-scheduler
		nvram_set_int(strcat_r(prefix, "timesched", tmp2), 0);	// disable wifi time-scheduler
		//dbG("[radio switch] %s=%s\n", tmp2, nvram_safe_get(tmp2));
	}

	// commit to flash once */
	nvram_commit();
#ifdef RTCONFIG_BCMWL6
	if (sw) led_bh_prep(0);	// early turn wl led on
#endif
	// make sure all interfaces work well
	restart_wireless();
#ifdef RTCONFIG_BCMWL6
	if (sw) led_bh_prep(1); // restore ledbh if needed
#endif
	return 0;
}

int delay_main(int argc, char *argv[])
{
	char buff[100];
	int i;
	if(argc>1)
	{
		sleep(atoi(argv[1]));
		memset(buff,0,sizeof(buff));
		i=2;
		while(i<argc)
		{
			strcat(buff,argv[i]);
			strcat(buff," ");
			i++;
		}
		doSystem(buff);
	}
	else
		dbG("delay_exec: args error!\n");

	return 0;
}

int radio_main(int argc, char *argv[])
{
	int op = RADIO_OFF;
	int unit;
	int subunit;

	if (argc < 2) {
HELP:
		usage_exit(argv[0], "on|off|toggle|switch|join [N]\n");
	}
	unit = (argc >= 3) ? atoi(argv[2]) : -1;
	subunit = (argc >= 4) ? atoi(argv[3]) : 0;

	if (strcmp(argv[1], "toggle") == 0)
		op = RADIO_TOGGLE;
	else if (strcmp(argv[1], "off") == 0)
		op = RADIO_OFF;
	else if (strcmp(argv[1], "on") == 0)
		op = RADIO_ON;
	else if (strcmp(argv[1], "switch") == 0) {
		op = RADIO_SWITCH;
		goto SWITCH;
	}
	else if (strcmp(argv[1], "join") == 0)
		goto JOIN;
	else
		goto HELP;

	if (unit >= 0)
		op = radio_toggle(0, unit, subunit, &op);
	else
		op = foreach_wif(0, &op, radio_toggle);

	if (!op) {
		//led(LED_DIAG, 0);
		return 0;
	}
JOIN:
	foreach_wif(1, &unit, radio_join);
SWITCH:
	if(op == RADIO_SWITCH) {
		radio_switch(subunit);
	}

	return 0;
}

#if 0
/*
int wdist_main(int argc, char *argv[])
{
	int n;
	rw_reg_t r;
	int v;

	if (argc != 2) {
		r.byteoff = 0x684;
		r.size = 2;
		if (wl_ioctl(nvram_safe_get("wl_ifname"), 101, &r, sizeof(r)) == 0) {
			v = r.val - 510;
			if (v <= 9) v = 0;
				else v = (v - (9 + 1)) * 150;
			printf("Current: %d-%dm (0x%02x)\n\n", v + (v ? 1 : 0), v + 150, r.val);
		}
		usage_exit(argv[0], "<meters>");
	}
	if ((n = atoi(argv[1])) <= 0) setup_wldistance();
		else set_wldistance(n);
	return 0;
}
*/
static int get_wldist(int idx, int unit, int subunit, void *param)
{
	int n;

	char *p = nvram_safe_get(wl_nvname("distance", unit, 0));
	if ((*p == 0) || ((n = atoi(p)) < 0)) return 0;

	return (9 + (n / 150) + ((n % 150) ? 1 : 0));
}

static int wldist(int idx, int unit, int subunit, void *param)
{
	rw_reg_t r;
	uint32 s;
	char *p;
	int n;

	n = get_wldist(idx, unit, subunit, param);
	if (n > 0) {
		s = 0x10 | (n << 16);
		p = nvram_safe_get(wl_nvname("ifname", unit, 0));
		wl_ioctl(p, 197, &s, sizeof(s));

		r.byteoff = 0x684;
		r.val = n + 510;
		r.size = 2;
		wl_ioctl(p, 102, &r, sizeof(r));
	}
	return 0;
}

// ref: wificonf.c
int wldist_main(int argc, char *argv[])
{
	if (fork() == 0) {
		if (foreach_wif(0, NULL, get_wldist) == 0) return 0;

		while (1) {
			foreach_wif(0, NULL, wldist);
			sleep(2);
		}
	}

	return 0;
}
#endif
int
update_lan_resolvconf(void)
{
	FILE *fp;
	char word[256], *next;
	int lock, dup_dns = 0;
	char *lan_dns, *lan_gateway;

	lock = file_lock("resolv");

	if (!(fp = fopen("/tmp/resolv.conf", "w+"))) {
		perror("/tmp/resolv.conf");
		file_unlock(lock);
		return errno;
	}

	lan_dns = nvram_safe_get("lan_dns");
	lan_gateway = nvram_safe_get("lan_gateway");
#ifdef RTCONFIG_WIRELESSREPEATER
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") != WLC_STATE_CONNECTED)
		fprintf(fp, "nameserver %s\n", nvram_default_get("lan_ipaddr"));
	else
#endif
	{
		foreach(word, lan_dns, next) {
			if (!strcmp(word, lan_gateway))
				dup_dns = 1;
			fprintf(fp, "nameserver %s\n", word);
		}
	}

	if (*lan_gateway != '\0' && !dup_dns)
		fprintf(fp, "nameserver %s\n", lan_gateway);

	fclose(fp);

	file_unlock(lock);
	return 0;
}


void
lan_up(char *lan_ifname)
{
#ifdef CONFIG_BCMWL5
	int32 ip;
	char tmp[100], prefix[]="wlXXXXXXX_";
#ifdef __CONFIG_DHDAP__
	int is_dhd;
#endif
#endif
#ifdef RTCONFIG_WIRELESSREPEATER
	char domain_mapping[64];
#endif

	_dprintf("%s(%s)\n", __FUNCTION__, lan_ifname);

#ifdef CONFIG_BCMWL5
#if defined(RTCONFIG_WIRELESSREPEATER) || defined(RTCONFIG_PROXYSTA)
	if (nvram_match("lan_proto", "dhcp")) {
		if (nvram_get_int("sw_mode") == SW_MODE_REPEATER
		) {
			snprintf(prefix, sizeof(prefix), "wl%d_", nvram_get_int("wlc_band"));
			inet_aton(nvram_safe_get("lan_ipaddr"), (struct in_addr *)&ip);
#ifdef __CONFIG_DHDAP__
			is_dhd = !dhd_probe(nvram_safe_get(strcat_r(prefix, "ifname", tmp)));
			if (is_dhd) {
				dhd_iovar_setint(nvram_safe_get(strcat_r(prefix, "ifname", tmp)), "wet_host_ipv4", ip);
			} else
#endif /* __CONFIG_DHDAP__ */
			wl_iovar_setint(nvram_safe_get(strcat_r(prefix, "ifname", tmp)), "wet_host_ipv4", ip);
		}
	}
#endif
#endif /* CONFIG_BCMWL5 */

	start_dnsmasq();

	update_lan_resolvconf();

	/* Set default route to gateway if specified */
	if(nvram_get_int("sw_mode") != SW_MODE_REPEATER
#ifdef RTCONFIG_WIRELESSREPEATER
			|| (nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") == WLC_STATE_CONNECTED)
#endif
			) {
		route_add(lan_ifname, 0, "0.0.0.0", nvram_safe_get("lan_gateway"), "0.0.0.0");

		/* Sync time */
		if (!pids("ntp"))
		{
			stop_ntpc();
			start_ntpc();
		}
		else
			kill_pidfile_s("/var/run/ntp.pid", SIGALRM);
	}

	/* Scan new subnetwork */
	stop_networkmap();
	start_networkmap(0);
	update_lan_state(LAN_STATE_CONNECTED, 0);

#ifdef RTCONFIG_WIRELESSREPEATER
	// when wlc_mode = 0 & wlc_state = WLC_STATE_CONNECTED, don't notify wanduck yet.
	// when wlc_mode = 1 & wlc_state = WLC_STATE_CONNECTED, need to notify wanduck.
	// When wlc_mode = 1 & lan_up, need to set wlc_state be WLC_STATE_CONNECTED always.
	// wlcconnect often set the wlc_state too late.
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_mode") == 1) {
		repeater_nat_setting();
		nvram_set_int("wlc_state", WLC_STATE_CONNECTED);

		logmessage("notify wanduck", "wlc_state change!");
		_dprintf("%s: notify wanduck: wlc_state=%d.\n", __FUNCTION__, nvram_get_int("wlc_state"));
		// notify the change to wanduck.
		kill_pidfile_s("/var/run/wanduck.pid", SIGUSR1);
	}

	if(get_model() == MODEL_APN12HP &&
		nvram_get_int("sw_mode") == SW_MODE_AP) {
		repeater_nat_setting();
		eval("ebtables", "-t", "broute", "-F");
		eval("ebtables", "-t", "filter", "-F");
		eval("ebtables", "-t", "broute", "-I", "BROUTING", "-d", "00:E0:11:22:33:44", "-j", "redirect", "--redirect-target", "DROP");
		sprintf(domain_mapping, "%x %s", inet_addr(nvram_safe_get("lan_ipaddr")), DUT_DOMAIN_NAME);
		f_write_string("/proc/net/dnsmqctrl", domain_mapping, 0, 0);
		start_nat_rules();
	}
#endif

#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
	if ((is_psta(nvram_get_int("wlc_band")) || is_psr(nvram_get_int("wlc_band")))
#if defined(RTCONFIG_BCM7) || defined(RTCONFIG_BCM_7114)
#ifdef RTCONFIG_GMAC3
		&& !nvram_match("gmac3_enable", "1")
#endif
		&& nvram_match("ctf_disable", "1")
#endif
	) setup_dnsmq(1);
#endif

#ifdef RTCONFIG_USB
#ifdef RTCONFIG_MEDIA_SERVER
	if(get_invoke_later()&INVOKELATER_DMS)
		notify_rc("restart_dms");
#endif
#endif
}

void
lan_down(char *lan_ifname)
{
	_dprintf("%s(%s)\n", __FUNCTION__, lan_ifname);

	/* Remove default route to gateway if specified */
	route_del(lan_ifname, 0, "0.0.0.0",
			nvram_safe_get("lan_gateway"),
			"0.0.0.0");

	/* Clear resolv.conf */
	f_write("/tmp/resolv.conf", NULL, 0, 0, 0);

	update_lan_state(LAN_STATE_STOPPED, 0);
}

void stop_lan_wl(void)
{
	char *p, *ifname;
	char *wl_ifnames;
	char *lan_ifname;
#ifdef CONFIG_BCMWL5
	int unit, subunit;
#endif

	if (module_loaded("ebtables")) {
		eval("ebtables", "-F");
		eval("ebtables", "-t", "broute", "-F");
	}

	lan_ifname = nvram_safe_get("lan_ifname");
	if ((wl_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
		p = wl_ifnames;
		while ((ifname = strsep(&p, " ")) != NULL) {
			while (*ifname == ' ') ++ifname;
			if (*ifname == 0) break;
			disable_wifi_bled(ifname);
#ifdef CONFIG_BCMWL5
#ifdef RTCONFIG_QTN
			if (!strcmp(ifname, "wifi0")) continue;
#endif
			if (strncmp(ifname, "wl", 2) == 0 && strchr(ifname, '.')) {
				if (get_ifname_unit(ifname, &unit, &subunit) < 0)
					continue;
			}
			else if (wl_ioctl(ifname, WLC_GET_INSTANCE, &unit, sizeof(unit)))
			{
#ifdef RTCONFIG_EMF
				if (nvram_match("emf_enable", "1"))
					eval("emf", "del", "iface", lan_ifname, ifname);
#endif
				continue;
			}

			eval("wlconf", ifname, "down");
			eval("wl", "-i", ifname, "radio", "off");
#elif defined RTCONFIG_RALINK
			if (!strncmp(ifname, "ra", 2))
				stop_wds_ra(lan_ifname, ifname);
#elif defined(RTCONFIG_QCA)
				/* do nothing */
#endif
			ifconfig(ifname, 0, NULL, NULL);
#ifdef RTCONFIG_GMAC3
			if (nvram_match("gmac3_enable", "1") && find_in_list(nvram_get("fwd_wlandevs"), ifname))
				goto gmac3_no_swbr;
#endif
			eval("brctl", "delif", lan_ifname, ifname);
#ifdef RTCONFIG_GMAC3
gmac3_no_swbr:
#endif
#ifdef RTCONFIG_EMF
			if (nvram_match("emf_enable", "1"))
				eval("emf", "del", "iface", lan_ifname, ifname);
#endif

#if defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)
			{ // remove interface for iNIC packets
				char *nic_if, *nic_ifs, *nic_lan_ifnames;
				if((nic_lan_ifnames = strdup(nvram_safe_get("nic_lan_ifnames"))))
				{
					nic_ifs = nic_lan_ifnames;
					while ((nic_if = strsep(&nic_ifs, " ")) != NULL) {
						while (*nic_if == ' ')
							nic_if++;
						if (*nic_if == 0)
							break;
						if(strcmp(ifname, nic_if) == 0)
						{
							eval("vconfig", "rem", ifname);
							break;
						}
					}
					free(nic_lan_ifnames);
				}
			}
#endif
		}

		free(wl_ifnames);
	}
#ifdef RTCONFIG_EMF
	stop_emf(lan_ifname);
#endif
	stop_snooper();

#ifdef RTCONFIG_PORT_BASED_VLAN
	stop_vlan_wl_ifnames();
#endif

#ifdef RTCONFIG_BCMWL6
#if defined(RTAC66U) || defined(BCM4352)
	nvram_set("led_5g", "0");
	led_control(LED_5G, LED_OFF);
#ifdef RTCONFIG_TURBO
	led_control(LED_TURBO, LED_OFF);
#endif
#endif
#endif

	// inform watchdog to stop WPS LED
	kill_pidfile_s("/var/run/watchdog.pid", SIGUSR2);

	fini_wl();
}

#if defined(RTCONFIG_RALINK) || defined(RTCONFIG_QCA)
pid_t pid_from_file(char *pidfile)
{
	FILE *fp;
	char buf[256];
	pid_t pid = 0;

	if ((fp = fopen(pidfile, "r")) != NULL) {
		if (fgets(buf, sizeof(buf), fp))
			pid = strtoul(buf, NULL, 0);

		fclose(fp);
	}

	return pid;
}
#endif

void start_lan_wl(void)
{
	char *lan_ifname;
	char *wl_ifnames, *ifname, *p;
	uint32 ip;
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXX";
	char word[256], *next;
	int match;
	int i;
#ifdef CONFIG_BCMWL5
	struct ifreq ifr;
	int unit, subunit, sta = 0;
#endif
#ifdef __CONFIG_DHDAP__
	int is_dhd;
#endif /* __CONFIG_DHDAP__ */

#if defined(RTCONFIG_RALINK) || defined(RTCONFIG_QCA)
	init_wl();
#endif

#ifdef CONFIG_BCMWL5
	init_wl_compact();
	wlconf_pre();

	if (0)
	{
		foreach_wif(1, NULL, set_wlmac);
		check_afterburner();
	}
#endif

	if (no_need_to_start_wps() ||
	    wps_band_radio_off(get_radio_band(nvram_get_int("wps_band"))) ||
	    wps_band_ssid_broadcast_off(get_radio_band(nvram_get_int("wps_band"))))
		nvram_set("wps_enable", "0");
	/* recover wps enable option back to original state */
	else if(!nvram_match("wps_enable", nvram_safe_get("wps_enable_old"))){
		nvram_set("wps_enable", nvram_safe_get("wps_enable_old"));
	}

	lan_ifname = strdup(nvram_safe_get("lan_ifname"));
#ifdef RTCONFIG_EMF
	if (nvram_match("emf_enable", "1")) {
		eval("emf", "add", "bridge", lan_ifname);
		eval("igs", "add", "bridge", lan_ifname);
	}
#endif
	if (strncmp(lan_ifname, "br", 2) == 0) {
		inet_aton(nvram_safe_get("lan_ipaddr"), (struct in_addr *)&ip);

		if ((wl_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
			p = wl_ifnames;
			while ((ifname = strsep(&p, " ")) != NULL) {
				while (*ifname == ' ') ++ifname;
				if (*ifname == 0) break;
#ifdef CONFIG_BCMWL5
				if (strncmp(ifname, "wl", 2) == 0) {
					if (!wl_vif_enabled(ifname, tmp)) {
						continue; /* Ignore disabled WL VIF */
					}
					wl_vif_hwaddr_set(ifname);
				}
				unit = -1; subunit = -1;
#endif

				// ignore disabled wl vifs

				if (guest_wlif(ifname)) {
					char nv[40];
					char nv2[40];
					char nv3[40];
					snprintf(nv, sizeof(nv) - 1, "%s_bss_enabled", wif_to_vif(ifname));
					if (!nvram_get_int(nv))
					{
						continue;
					}
					else
					{
						if (!nvram_get(strcat_r(prefix, "lanaccess", tmp)))
							nvram_set(strcat_r(prefix, "lanaccess", tmp), "off");

						if ((nvram_get_int("wl_unit") >= 0) && (nvram_get_int("wl_subunit") > 0))
						{
							snprintf(nv, sizeof(nv) - 1, "%s_expire", wif_to_vif(ifname));
							snprintf(nv2, sizeof(nv2) - 1, "%s_expire_tmp", wif_to_vif(ifname));
							snprintf(nv3, sizeof(nv3) - 1, "wl%d.%d", nvram_get_int("wl_unit"), nvram_get_int("wl_subunit"));
							if (!strcmp(nv3, wif_to_vif(ifname)))
							{
								nvram_set(nv2, nvram_safe_get(nv));
								nvram_set("wl_unit", "-1");
								nvram_set("wl_subunit", "-1");
							}
						}
					}
#ifdef CONFIG_BCMWL5
					if (get_ifname_unit(ifname, &unit, &subunit) < 0)
						continue;
#endif
				}
#ifdef CONFIG_BCMWL5
				else
					wl_ioctl(ifname, WLC_GET_INSTANCE, &unit, sizeof(unit));
#endif

#ifdef RTCONFIG_QCA
				qca_wif_up(ifname);
#endif
#ifdef RTCONFIG_RALINK
				gen_ra_config(ifname);
#endif
#if defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)
				{ // add interface for iNIC packets
					char *nic_if, *nic_ifs, *nic_lan_ifnames;
					if((nic_lan_ifnames = strdup(nvram_safe_get("nic_lan_ifnames"))))
					{
						nic_ifs = nic_lan_ifnames;
						while ((nic_if = strsep(&nic_ifs, " ")) != NULL) {
							while (*nic_if == ' ')
								nic_if++;
							if (*nic_if == 0)
								break;
							if(strcmp(ifname, nic_if) == 0)
							{
								int vlan_id;
								if((vlan_id = atoi(ifname + 4)) > 0)
								{
									char id[8];
									sprintf(id, "%d", vlan_id);
									eval("vconfig", "add", "eth2", id);
								}
								break;
							}
						}
						free(nic_lan_ifnames);
					}
				}
#endif
#if defined(RTCONFIG_QCA)
				if (!ifup_vap &&
				    (!strncmp(ifname, WIF_2G, strlen(WIF_2G)) ||
				     !strncmp(ifname, WIF_5G, strlen(WIF_5G))))
				{
					/* Don't up VAP interface here. */
				} else {
#endif
					// bring up interface
					if (ifconfig(ifname, IFUP | IFF_ALLMULTI, NULL, NULL) != 0) {
#ifdef RTCONFIG_QTN
						if (strcmp(ifname, "wifi0"))
#endif
							continue;
					}
#if defined(RTCONFIG_QCA)
				}
#endif

#ifdef RTCONFIG_RALINK
				wlconf_ra(ifname);
#elif defined(RTCONFIG_QCA)
				/* do nothing */
#elif defined CONFIG_BCMWL5
				if (wlconf(ifname, unit, subunit) == 0) {
					const char *mode = nvram_safe_get(wl_nvname("mode", unit, subunit));

					if (strcmp(mode, "wet") == 0) {
						// Enable host DHCP relay
						if (nvram_match("lan_proto", "static")) {
#ifdef __CONFIG_DHDAP__
							is_dhd = !dhd_probe(ifname);
							if(is_dhd) {
								char macbuf[sizeof("wet_host_mac") + 1 + ETHER_ADDR_LEN];
								dhd_iovar_setbuf(ifname, "wet_host_mac", ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN , macbuf, sizeof(macbuf));
							}
							else
#endif /* __CONFIG_DHDAP__ */
							wl_iovar_set(ifname, "wet_host_mac", ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
						}
					}

					sta |= (strcmp(mode, "sta") == 0);
					if ((strcmp(mode, "ap") != 0) && (strcmp(mode, "wet") != 0)
						&& (strcmp(mode, "psta") != 0) && (strcmp(mode, "psr") != 0))
						continue;
				} else {
#ifdef RTCONFIG_EMF
					if (nvram_match("emf_enable", "1"))
						eval("emf", "add", "iface", lan_ifname, ifname);
#endif
					continue;
				}
#endif
				/* Don't attach the main wl i/f in wds mode */
				match = 0, i = 0;
				foreach (word, nvram_safe_get("wl_ifnames"), next) {
					if (!strcmp(ifname, word))
					{
						snprintf(prefix, sizeof(prefix), "wl%d_", i);
						if (nvram_match(strcat_r(prefix, "mode_x", tmp), "1"))
#ifdef RTCONFIG_QCA
							match = 0;
#else
							match = 1;
#endif

#ifdef RTCONFIG_PROXYSTA
#ifdef RTCONFIG_RALINK
						if (mediabridge_mode()) {
							ifconfig(ifname, 0, NULL, NULL);
							match = 1;
						}
#endif
#endif

						break;
					}

					i++;
				}
#ifdef RTCONFIG_GMAC3
				/* In 3GMAC mode, skip wl interfaces that avail of hw switching.
				 *
				 * Do not add these wl interfaces to the LAN bridge as they avail of
				 * HW switching. Misses in the HW switch's ARL will be forwarded via vlan1
				 * to br0 (i.e. via the network GMAC#2).
				 */
				if (nvram_match("gmac3_enable", "1") &&
					find_in_list(nvram_get("fwd_wlandevs"), ifname))
						goto gmac3_no_swbr;
#endif
#if defined(RTCONFIG_PORT_BASED_VLAN)
				if (vlan_enable() && check_if_exist_vlan_ifnames(ifname))
					match = 1;
#endif
				if (!match)
				{
					eval("brctl", "addif", lan_ifname, ifname);
#ifdef RTCONFIG_GMAC3
gmac3_no_swbr:
#endif
#ifdef RTCONFIG_EMF
					if (nvram_match("emf_enable", "1"))
						eval("emf", "add", "iface", lan_ifname, ifname);
#endif
				}
				enable_wifi_bled(ifname);
			}
			free(wl_ifnames);
		}
	}

#ifdef RTCONFIG_EMF
	start_emf(lan_ifname);
#endif
	start_snooper(lan_ifname);

#ifdef RTCONFIG_PORT_BASED_VLAN
	start_vlan_wl_ifnames();
#endif

#ifdef RTCONFIG_BCMWL6
	set_acs_ifnames();
#endif

	free(lan_ifname);

	nvram_set("reload_svc_radio", "1");
}

void restart_wl(void)
{
#ifdef CONFIG_BCMWL5
	char *wl_ifnames, *ifname, *p;
	int unit, subunit;
	int is_client = 0;
	char tmp[100], tmp2[100], prefix[] = "wlXXXXXXXXXXXXXX";

	if ((wl_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
		p = wl_ifnames;
		while ((ifname = strsep(&p, " ")) != NULL) {
			while (*ifname == ' ') ++ifname;
			if (*ifname == 0) break;

			unit = -1; subunit = -1;

			// ignore disabled wl vifs
			if (strncmp(ifname, "wl", 2) == 0 && strchr(ifname, '.')) {
				char nv[40];
				snprintf(nv, sizeof(nv) - 1, "%s_bss_enabled", wif_to_vif(ifname));
				if (!nvram_get_int(nv))
					continue;
				if (get_ifname_unit(ifname, &unit, &subunit) < 0)
					continue;
			}
			// get the instance number of the wl i/f
			else if (wl_ioctl(ifname, WLC_GET_INSTANCE, &unit, sizeof(unit)))
				continue;

			is_client |= wl_client(unit, subunit) && nvram_get_int(wl_nvname("radio", unit, 0));

#ifdef CONFIG_BCMWL5
			snprintf(prefix, sizeof(prefix), "wl%d_", unit);
			if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
			{
				nvram_set_int(strcat_r(prefix, "timesched", tmp2), 0);	// disable wifi time-scheduler
#ifdef RTCONFIG_QTN
				if (strcmp(ifname, "wifi0"))
#endif
				{
					eval("wlconf", ifname, "down");
					eval("wl", "-i", ifname, "radio", "off");
				}
			}
			else
#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
			if (!psta_exist_except(unit)/* || !psr_exist_except(unit)*/)
#endif
			{
#ifdef RTCONFIG_QTN
				if (strcmp(ifname, "wifi0"))
#endif
				eval("wlconf", ifname, "start"); /* start wl iface */
			}
			wlconf_post(ifname);
#endif	// CONFIG_BCMWL5
		}
		free(wl_ifnames);
	}

#if 0
	killall("wldist", SIGTERM);
	eval("wldist");
#endif

	if (is_client)
		xstart("radio", "join");

#ifdef RTCONFIG_PORT_BASED_VLAN
	restart_vlan_wl();
#endif

#ifdef RTCONFIG_BCMWL6
#if defined(RTAC66U) || defined(BCM4352)
	if (nvram_match("wl1_radio", "1"))
	{
#ifndef RTCONFIG_LED_BTN
		if (!(nvram_get_int("sw_mode")==SW_MODE_AP && nvram_get_int("wlc_psta") && nvram_get_int("wlc_band")==0)) {
			nvram_set("led_5g", "1");
			led_control(LED_5G, LED_ON);
		}
#else
		nvram_set("led_5g", "1");
		if (nvram_get_int("AllLED"))
			led_control(LED_5G, LED_ON);
#endif
	}
	else
	{
		nvram_set("led_5g", "0");
		led_control(LED_5G, LED_OFF);
	}
#ifdef RTCONFIG_TURBO
	if ((nvram_match("wl0_radio", "1") || nvram_match("wl1_radio", "1")
#if defined(RTAC3200) || defined(RTAC5300) || defined(RTAC5300R)
		|| nvram_match("wl2_radio", "1")
#endif
	)
#ifdef RTCONFIG_LED_BTN
		&& nvram_get_int("AllLED")
#endif
	)
		led_control(LED_TURBO, LED_ON);
	else
		led_control(LED_TURBO, LED_OFF);
#endif
#endif
#endif
#endif /* CONFIG_BCMWL5 */


#if defined(RTCONFIG_USER_LOW_RSSI) && !defined(RTCONFIG_BCMARM)
	init_wllc();
#endif


}

void lanaccess_mssid_ban(const char *limited_ifname)
{
	char lan_subnet[32];
#ifdef RTAC87U
	char limited_ifname_real[32] = {0};
#endif

	if (limited_ifname == NULL) return;

	if (nvram_get_int("sw_mode") != SW_MODE_ROUTER) return;

#ifdef RTAC87U
	/* #565: Access Intranet off */
	/* workaround: use vlan4000, 4001, 4002 as QTN guest network VID */

	if(strcmp(limited_ifname, "wl1.1") == 0)
		snprintf(limited_ifname_real, sizeof(limited_ifname_real), "vlan4000");
	else if(strcmp(limited_ifname, "wl1.2") == 0)
		snprintf(limited_ifname_real, sizeof(limited_ifname_real), "vlan4001");
	else if(strcmp(limited_ifname, "wl1.3") == 0)
		snprintf(limited_ifname_real, sizeof(limited_ifname_real), "vlan4002");
	else
		snprintf(limited_ifname_real, "%s", limited_ifname);

	eval("ebtables", "-A", "FORWARD", "-i", (char*)limited_ifname_real, "-j", "DROP"); //ebtables FORWARD: "for frames being forwarded by the bridge"
	eval("ebtables", "-A", "FORWARD", "-o", (char*)limited_ifname_real, "-j", "DROP"); // so that traffic via host and nat is passed

	snprintf(lan_subnet, sizeof(lan_subnet), "%s/%s", nvram_safe_get("lan_ipaddr"), nvram_safe_get("lan_netmask"));
	eval("ebtables", "-t", "broute", "-A", "BROUTING", "-i", (char*)limited_ifname_real, "-p", "ipv4", "--ip-dst", lan_subnet, "--ip-proto", "tcp", "-j", "DROP");
#else
	eval("ebtables", "-A", "FORWARD", "-i", (char*)limited_ifname, "-j", "DROP"); //ebtables FORWARD: "for frames being forwarded by the bridge"
	eval("ebtables", "-A", "FORWARD", "-o", (char*)limited_ifname, "-j", "DROP"); // so that traffic via host and nat is passed

	snprintf(lan_subnet, sizeof(lan_subnet), "%s/%s", nvram_safe_get("lan_ipaddr"), nvram_safe_get("lan_netmask"));
	eval("ebtables", "-t", "broute", "-A", "BROUTING", "-i", (char*)limited_ifname, "-p", "ipv4", "--ip-dst", lan_subnet, "--ip-proto", "tcp", "-j", "DROP");
#endif
}

void lanaccess_wl(void)
{
	char *p, *ifname;
	char *wl_ifnames;
#ifdef CONFIG_BCMWL5
	int unit, subunit;
#endif

	if ((wl_ifnames = strdup(nvram_safe_get("lan_ifnames"))) != NULL) {
		p = wl_ifnames;
		while ((ifname = strsep(&p, " ")) != NULL) {
			while (*ifname == ' ') ++ifname;
			if (*ifname == 0) break;
#ifdef CONFIG_BCMWL5
			if (guest_wlif(ifname)) {
				if (get_ifname_unit(ifname, &unit, &subunit) < 0)
					continue;
			}
			else continue;

#ifdef RTCONFIG_WIRELESSREPEATER
			if(nvram_get_int("sw_mode")==SW_MODE_REPEATER && unit==nvram_get_int("wlc_band") && subunit==1) continue;
#endif
#elif defined RTCONFIG_RALINK
			if (guest_wlif(ifname))
				;
			else
				continue;
#elif defined(RTCONFIG_QCA)
			if (guest_wlif(ifname))
				;
			else
				continue;
#endif
#if defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)
			if (strncmp(ifname, "rai", 3) == 0)
				continue;
#endif

#if defined(CONFIG_BCMWL5) && defined(RTCONFIG_PORT_BASED_VLAN)
			if (vlan_enable() && check_if_exist_vlan_ifnames(ifname))
				continue;
#endif

			char nv[40];
			snprintf(nv, sizeof(nv) - 1, "%s_lanaccess", wif_to_vif(ifname));
			if (!strcmp(nvram_safe_get(nv), "off"))
				lanaccess_mssid_ban(ifname);
		}
		free(wl_ifnames);
	}
#if defined (RTCONFIG_WLMODULE_RT3352_INIC_MII)
	if ((wl_ifnames = strdup(nvram_safe_get("nic_lan_ifnames"))) != NULL) {
		p = wl_ifnames;
		while ((ifname = strsep(&p, " ")) != NULL) {
			while (*ifname == ' ') ++ifname;
			if (*ifname == 0) break;

			lanaccess_mssid_ban(ifname);
		}
		free(wl_ifnames);
	}
#endif
	setup_leds();   // Refresh LED state if in Stealth Mode
}

void restart_wireless(void)
{
#ifdef RTCONFIG_WIRELESSREPEATER
	char domain_mapping[64];
#endif
	int lock = file_lock("wireless");

	nvram_set_int("wlready", 0);

#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_PROXYSTA
	stop_psta_monitor();
#endif
	stop_acsd();
#ifdef BCM_BSD
	stop_bsd();
#endif
#ifdef BCM_SSD
	stop_ssd();
#endif
	stop_igmp_proxy();
#ifdef RTCONFIG_HSPOT
	stop_hspotap();
#endif
#endif
	stop_wps();
#ifdef CONFIG_BCMWL5
	stop_nas();
	stop_eapd();
#elif defined RTCONFIG_RALINK
	stop_8021x();
#endif

#if ((defined(RTCONFIG_USER_LOW_RSSI) && defined(RTCONFIG_BCMARM)) || defined(RTCONFIG_NEW_USER_LOW_RSSI))
	stop_roamast();
#endif
	stop_lan_wl();

	init_nvram();	// init nvram lan_ifnames
	wl_defaults();	// init nvram wlx_ifnames & lan_ifnames
#ifndef CONFIG_BCMWL5
	sleep(2);	// delay to avoid start interface on stoping.
#endif

#ifdef RTCONFIG_PORT_BASED_VLAN
	set_port_based_vlan_config(NULL);
#endif
	start_lan_wl();

	reinit_hwnat(-1);

#ifdef CONFIG_BCMWL5
	start_eapd();
	start_nas();
#elif defined RTCONFIG_RALINK
	start_8021x();
#endif
	start_wps();
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_HSPOT
	start_hspotap();
#endif
	start_igmp_proxy();
#ifdef BCM_BSD
	start_bsd();
#endif
#ifdef BCM_SSD
	start_ssd();
#endif
	start_acsd();
#ifdef RTCONFIG_PROXYSTA
	start_psta_monitor();
#endif
#endif
#ifndef RTCONFIG_BCMWL6
#ifdef RTCONFIG_WL_AUTO_CHANNEL
	if(nvram_match("AUTO_CHANNEL", "1")) {
		nvram_set("wl_channel", "6");
		nvram_set("wl0_channel", "6");
	}
#endif
#endif
	restart_wl();
	lanaccess_wl();

#ifdef CONFIG_BCMWL5
	/* for MultiSSID */
	if(nvram_get_int("qos_enable") == 1) {
		del_EbtablesRules(); // flush ebtables nat table
		add_EbtablesRules();
	}
#endif

#ifdef RTCONFIG_WIRELESSREPEATER
	// when wlc_mode = 0 & wlc_state = WLC_STATE_CONNECTED, don't notify wanduck yet.
	// when wlc_mode = 1 & wlc_state = WLC_STATE_CONNECTED, need to notify wanduck.
	// When wlc_mode = 1 & lan_up, need to set wlc_state be WLC_STATE_CONNECTED always.
	// wlcconnect often set the wlc_state too late.
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_mode") == 1) {
		repeater_nat_setting();
		nvram_set_int("wlc_state", WLC_STATE_CONNECTED);

		logmessage("notify wanduck", "wlc_state change!");
		_dprintf("%s: notify wanduck: wlc_state=%d.\n", __FUNCTION__, nvram_get_int("wlc_state"));
		// notify the change to wanduck.
		kill_pidfile_s("/var/run/wanduck.pid", SIGUSR1);
	}

	if(get_model() == MODEL_APN12HP &&
		nvram_get_int("sw_mode") == SW_MODE_AP) {
		repeater_nat_setting();
		eval("ebtables", "-t", "broute", "-F");
		eval("ebtables", "-t", "filter", "-F");
		eval("ebtables", "-t", "broute", "-I", "BROUTING", "-d", "00:E0:11:22:33:44", "-j", "redirect", "--redirect-target", "DROP");
		sprintf(domain_mapping, "%x %s", inet_addr(nvram_safe_get("lan_ipaddr")), DUT_DOMAIN_NAME);
		f_write_string("/proc/net/dnsmqctrl", domain_mapping, 0, 0);
		start_nat_rules();
	}
#endif

#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
	if ((is_psta(nvram_get_int("wlc_band")) || is_psr(nvram_get_int("wlc_band")))
#if defined(RTCONFIG_BCM7) || defined(RTCONFIG_BCM_7114)
#ifdef RTCONFIG_GMAC3
		&& !nvram_match("gmac3_enable", "1")
#endif
		&& nvram_match("ctf_disable", "1")
#endif
	) setup_dnsmq(1);
#endif

#ifdef RTCONFIG_QCA
	gen_qca_wifi_cfgs();
#endif
	nvram_set_int("wlready", 1);

#ifdef RTAC87U
	if(nvram_get_int("AllLED") == 0) setAllLedOff();
#endif

#if ((defined(RTCONFIG_USER_LOW_RSSI) && defined(RTCONFIG_BCMARM)) || defined(RTCONFIG_NEW_USER_LOW_RSSI))
	start_roamast();
#endif

	file_unlock(lock);
}

/* for WPS Reset */
void restart_wireless_wps(void)
{
	int lock = file_lock("wireless");
#ifdef RTCONFIG_WIRELESSREPEATER
	char domain_mapping[64];
#endif

	nvram_set_int("wlready", 0);

#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_PROXYSTA
	stop_psta_monitor();
#endif
	stop_acsd();
#ifdef BCM_BSD
	stop_bsd();
#endif
#ifdef BCM_SSD
	stop_ssd();
#endif
	stop_igmp_proxy();
#ifdef RTCONFIG_HSPOT
	stop_hspotap();
#endif
#endif
	stop_wps();
#ifdef CONFIG_BCMWL5
	stop_nas();
	stop_eapd();
#elif defined RTCONFIG_RALINK
	stop_8021x();
#endif
	stop_lan_wl();
	wl_defaults_wps();
#ifndef CONFIG_BCMWL5
	sleep(2);	// delay to avoid start interface on stoping.
#endif

	start_lan_wl();

	reinit_hwnat(-1);

#ifdef CONFIG_BCMWL5
	start_eapd();
	start_nas();
#elif defined RTCONFIG_RALINK
	start_8021x();
#endif
	start_wps();
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_HSPOT
	start_hspotap();
#endif
	start_igmp_proxy();
#ifdef BCM_BSD
	start_bsd();
#endif
#ifdef BCM_SSD
	start_ssd();
#endif
	start_acsd();
#ifdef RTCONFIG_PROXYSTA
	start_psta_monitor();
#endif
#endif
#ifndef RTCONFIG_BCMWL6
#ifdef RTCONFIG_WL_AUTO_CHANNEL
	if(nvram_match("AUTO_CHANNEL", "1")) {
		nvram_set("wl_channel", "6");
		nvram_set("wl0_channel", "6");
	}
#endif
#endif
	restart_wl();
	lanaccess_wl();

#ifdef CONFIG_BCMWL5
	/* for MultiSSID */
	if(nvram_get_int("qos_enable") == 1) {
		del_EbtablesRules(); // flush ebtables nat table
		add_EbtablesRules();
	}
#endif

#ifdef RTCONFIG_WIRELESSREPEATER
	// when wlc_mode = 0 & wlc_state = WLC_STATE_CONNECTED, don't notify wanduck yet.
	// when wlc_mode = 1 & wlc_state = WLC_STATE_CONNECTED, need to notify wanduck.
	// When wlc_mode = 1 & lan_up, need to set wlc_state be WLC_STATE_CONNECTED always.
	// wlcconnect often set the wlc_state too late.
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_mode") == 1) {
		repeater_nat_setting();
		nvram_set_int("wlc_state", WLC_STATE_CONNECTED);

		logmessage("notify wanduck", "wlc_state change!");
		_dprintf("%s: notify wanduck: wlc_state=%d.\n", __FUNCTION__, nvram_get_int("wlc_state"));
		// notify the change to wanduck.
		kill_pidfile_s("/var/run/wanduck.pid", SIGUSR1);
	}

	if(get_model() == MODEL_APN12HP &&
		nvram_get_int("sw_mode") == SW_MODE_AP) {
		repeater_nat_setting();
		eval("ebtables", "-t", "broute", "-F");
		eval("ebtables", "-t", "filter", "-F");
		eval("ebtables", "-t", "broute", "-I", "BROUTING", "-d", "00:E0:11:22:33:44", "-j", "redirect", "--redirect-target", "DROP");
		sprintf(domain_mapping, "%x %s", inet_addr(nvram_safe_get("lan_ipaddr")), DUT_DOMAIN_NAME);
		f_write_string("/proc/net/dnsmqctrl", domain_mapping, 0, 0);
		start_nat_rules();
	}
#endif

#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
	if ((is_psta(nvram_get_int("wlc_band")) || is_psr(nvram_get_int("wlc_band")))
#if defined(RTCONFIG_BCM7) || defined(RTCONFIG_BCM_7114)
#ifdef RTCONFIG_GMAC3
		&& !nvram_match("gmac3_enable", "1")
#endif
		&& nvram_match("ctf_disable", "1")
#endif
	) setup_dnsmq(1);
#endif

	nvram_set_int("wlready", 1);

	file_unlock(lock);
}

#ifdef RTCONFIG_BCM_7114
void stop_wl_bcm(void)
{
#ifdef RTCONFIG_WIRELESSREPEATER
	if (nvram_get_int("sw_mode") == SW_MODE_REPEATER)
		stop_wlcconnect();
#endif

#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_PROXYSTA
	stop_psta_monitor();
#endif
	stop_acsd();
#ifdef BCM_BSD
	stop_bsd();
#endif
#ifdef BCM_SSD
	stop_ssd();
#endif
	stop_igmp_proxy();
#ifdef RTCONFIG_HSPOT
	stop_hspotap();
#endif
#endif
	stop_wps();
#ifdef CONFIG_BCMWL5
	stop_nas();
	stop_eapd();
#elif defined RTCONFIG_RALINK
	stop_8021x();
#endif

#if ((defined(RTCONFIG_USER_LOW_RSSI) && defined(RTCONFIG_BCMARM)) || defined(RTCONFIG_NEW_USER_LOW_RSSI))
	stop_roamast();
#endif
	stop_lan_wl();
}
#endif

//FIXME: add sysdep wrapper

void start_wan_port(void)
{
	wanport_ctrl(1);
}

void stop_wan_port(void)
{
	wanport_ctrl(0);
}

void restart_lan_port(int dt)
{
	stop_lan_port();
	start_lan_port(dt);
}

void start_lan_port(int dt)
{
	int now = 0;

	if (dt <= 0)
		now = 1;

	_dprintf("%s(%d) %d\n", __func__, dt, now);
	if (!s_last_lan_port_stopped_ts) {
		s_last_lan_port_stopped_ts = uptime();
		now = 1;
	}

	while (!now && (uptime() - s_last_lan_port_stopped_ts < dt)) {
		_dprintf("sleep 1\n");
		sleep(1);
	}

#ifdef RTCONFIG_QTN
	if (nvram_get_int("qtn_ready") == 1)
		dbG("do not start lan port due to QTN not ready\n");
	else
		lanport_ctrl(1);
#else
	lanport_ctrl(1);
#endif

#ifdef RTCONFIG_PORT_BASED_VLAN
	vlan_lanaccess_wl();
#endif
}

void stop_lan_port(void)
{
	lanport_ctrl(0);
	s_last_lan_port_stopped_ts = uptime();
	_dprintf("%s() stop lan port. ts %ld\n", __func__, s_last_lan_port_stopped_ts);
}

void start_lan_wlport(void)
{
	char word[256], *next;
	int unit, subunit;

#ifdef RTCONFIG_PROXYSTA
#if defined(RTCONFIG_RALINK) || defined(RTCONFIG_QCA)
	if (mediabridge_mode())
		return;
#endif
#endif

	unit=0;
	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		if(unit!=nvram_get_int("wlc_band")) set_radio(1, unit, 0);
		for (subunit = 1; subunit < 4; subunit++)
		{
			set_radio(1, unit, subunit);
		}
		unit++;
	}
}

void stop_lan_wlport(void)
{
	char word[256], *next;
	int unit, subunit;

#ifdef RTCONFIG_PROXYSTA
#if defined(RTCONFIG_RALINK) || defined(RTCONFIG_QCA)
	if (mediabridge_mode())
		return;
#endif
#endif

	unit=0;
	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		if(unit!=nvram_get_int("wlc_band")) set_radio(0, unit, 0);
		for (subunit = 1; subunit < 4; subunit++)
		{
			set_radio(0, unit, subunit);
		}
		unit++;
	}
}
#if 0
static int
net_dev_exist(const char *ifname)
{
	DIR *dir_to_open = NULL;
	char tmpstr[128];

	if (ifname == NULL) return 0;

	sprintf(tmpstr, "/sys/class/net/%s", ifname);
	dir_to_open = opendir(tmpstr);
	if (dir_to_open)
	{
		closedir(dir_to_open);
		return 1;
	}
		return 0;
}

int
wl_dev_exist(void)
{
	char word[256], *next;
	int val = 1;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		if (!net_dev_exist(word))
		{
			val = 0;
			break;
		}
	}

	return val;
}
#endif
#ifdef RTCONFIG_WIRELESSREPEATER
void start_lan_wlc(void)
{
	_dprintf("%s %d\n", __FUNCTION__, __LINE__);

	char *lan_ifname;

	lan_ifname = nvram_safe_get("lan_ifname");
	update_lan_state(LAN_STATE_INITIALIZING, 0);

	// bring up and configure LAN interface
	if(nvram_match("lan_proto", "static"))
		ifconfig(lan_ifname, IFUP | IFF_ALLMULTI, nvram_safe_get("lan_ipaddr"), nvram_safe_get("lan_netmask"));
	else
		ifconfig(lan_ifname, IFUP | IFF_ALLMULTI, nvram_default_get("lan_ipaddr"), nvram_default_get("lan_netmask"));

	if(nvram_match("lan_proto", "dhcp"))
	{
		// only none routing mode need lan_proto=dhcp
		if (pids("udhcpc"))
		{
			killall("udhcpc", SIGUSR2);
			killall("udhcpc", SIGTERM);
			unlink("/tmp/udhcpc_lan");
		}
		char *dhcp_argv[] = { "udhcpc",
					"-i", "br0",
					"-p", "/var/run/udhcpc_lan.pid",
					"-s", "/tmp/udhcpc_lan",
					NULL };
		pid_t pid;

		symlink("/sbin/rc", "/tmp/udhcpc_lan");
		_eval(dhcp_argv, NULL, 0, &pid);

		update_lan_state(LAN_STATE_CONNECTING, 0);
	}
	else {
		lan_up(lan_ifname);
	}

	_dprintf("%s %d\n", __FUNCTION__, __LINE__);
}

void stop_lan_wlc(void)
{
	_dprintf("%s %d\n", __FUNCTION__, __LINE__);

	char *lan_ifname;

	lan_ifname = nvram_safe_get("lan_ifname");

	ifconfig(lan_ifname, 0, NULL, NULL);

	update_lan_state(LAN_STATE_STOPPED, 0);

	killall("udhcpc", SIGUSR2);
	killall("udhcpc", SIGTERM);
	unlink("/tmp/udhcpc_lan");

	_dprintf("%s %d\n", __FUNCTION__, __LINE__);
}
#endif //RTCONFIG_WIRELESSREPEATER


#ifdef RTCONFIG_QTN
int reset_qtn(int restart)
{
	int wait_loop;
	gen_stateless_conf();
	system("cp -p /rom/qtn/* /tmp/");
	if( restart == 0){
		lanport_ctrl(0);
#if 1	/* replaced by raw Ethernet frame */
		if(*nvram_safe_get("QTN_RPC_CLIENT"))
			eval("ifconfig", "br0:0", nvram_safe_get("QTN_RPC_CLIENT"), "netmask", "255.255.255.0");
		else
			eval("ifconfig", "br0:0", "169.254.39.1", "netmask", "255.255.255.0");
#endif
		eval("ifconfig", "br0:1", "1.1.1.1", "netmask", "255.255.255.0");
		eval("tftpd");
	}
	led_control(BTN_QTN_RESET, LED_ON);
	led_control(BTN_QTN_RESET, LED_OFF);
	if(nvram_match("lan_proto", "dhcp")){
		if(nvram_get_int("sw_mode") == SW_MODE_REPEATER ||
			nvram_get_int("sw_mode") == SW_MODE_AP)
		{
			if (pids("udhcpc"))
			{
				killall("udhcpc", SIGUSR2);
				killall("udhcpc", SIGTERM);
				unlink("/tmp/udhcpc_lan");
			}

			wait_loop = 3;
			while(wait_loop > 0) {
				dbG("[reset_qtn] *** kill udhcpc to waiting tftp loading ***\n");
				sleep(15);	/* waiting tftp loading */
				wait_loop--;
			}
			dbG("[reset_qtn] *** finish tftp loading, restart udhcpc ***\n");

			char *dhcp_argv[] = { "udhcpc",
						"-i", "br0",
						"-p", "/var/run/udhcpc_lan.pid",
						"-s", "/tmp/udhcpc_lan",
						NULL };
			pid_t pid;

			symlink("/sbin/rc", "/tmp/udhcpc_lan");
			_eval(dhcp_argv, NULL, 0, &pid);
		}
	}

	return 0;
}

int start_qtn(void)
{
	gen_rpc_qcsapi_ip();
	reset_qtn(0);

	return 0;
}

#endif

