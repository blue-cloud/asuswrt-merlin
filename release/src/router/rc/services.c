/*
 * Miscellaneous services
 *
 * Copyright (C) 2009, Broadcom Corporation. All Rights Reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: services.c,v 1.100 2010/03/04 09:39:18 Exp $
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <bcmnvram.h>
#include <shutils.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <rc.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/param.h>
#include <net/ethernet.h>

#ifdef RTCONFIG_RALINK
#include <ralink.h>
#endif
#ifdef RTCONFIG_QCA
#include <qca.h>
#endif
#include <shared.h>
#ifdef RTCONFIG_DSL
#include <dsl-upg.h>
#endif
#ifdef RTCONFIG_USB
#include <disk_io_tools.h>	//mkdir_if_none()
#else
#ifdef RTCONFIG_MDNS
extern int mkdir_if_none(const char *path)
{
	DIR *dp;
	char cmd[PATH_MAX];

	if (!(dp = opendir(path))) {
		memset(cmd, 0, PATH_MAX);
		sprintf(cmd, "mkdir -m 0777 -p '%s'", (char *)path);
		system(cmd);
		return 1;
	}
	closedir(dp);

	return 0;
}
#endif
#endif	/* RTCONFIG_USB */

#ifdef RTCONFIG_QTN
#include "web-qtn.h"
#endif

#ifdef RTCONFIG_HTTPS
#include <errno.h>
#include <getopt.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#endif

#ifdef RTCONFIG_QCA_PLC_UTILS
#include <plc_utils.h>
#endif
extern char *crypt __P((const char *, const char *)); //should be defined in unistd.h with _XOPEN_SOURCE defined
#define sin_addr(s) (((struct sockaddr_in *)(s))->sin_addr)

/* The g_reboot global variable is used to skip several unnecessary delay
 * and redundant steps during reboot / restore to default procedure.
 */
int g_reboot = 0;

// Pop an alarm to recheck pids in 500 msec.
static const struct itimerval pop_tv = { {0,0}, {0, 500 * 1000} };

// Pop an alarm to reap zombies.
static const struct itimerval zombie_tv = { {0,0}, {307, 0} };

// -----------------------------------------------------------------------------

static const char dmhosts[] = "/etc/hosts.dnsmasq";
static const char dmresolv[] = "/tmp/resolv.conf";
static const char dmservers[] = "/tmp/resolv.dnsmasq";

#ifdef RTCONFIG_TOAD
static void start_toads(void);
static void stop_toads(void);
#endif

#ifdef RTCONFIG_CROND
void stop_cron(void);
void start_cron(void);
#endif
void start_wlcscan(void);
void stop_wlcscan(void);


#ifndef MS_MOVE
#define MS_MOVE		8192
#endif
#ifndef MNT_DETACH
#define MNT_DETACH	0x00000002
#endif

#ifdef BCMDBG
#include <assert.h>
#else
#define assert(a)
#endif

#define logs(s) syslog(LOG_NOTICE, s)

#if 0
static char
*make_var(char *prefix, int index, char *name)
{
	static char buf[100];

	assert(prefix);
	assert(name);

	if (index)
		snprintf(buf, sizeof(buf), "%s%d%s", prefix, index, name);
	else
		snprintf(buf, sizeof(buf), "%s%s", prefix, name);
	return buf;
}

static int is_wet(int idx, int unit, int subunit, void *param)
{
	return nvram_match(wl_nvname("mode", unit, subunit), "wet");
}
#endif

#define TMP_ROOTFS_MNT_POINT	"/sysroot"
/* Build temporarily rootfilesystem
 * @newroot:	Mount point of new root filesystem.
 * @return:
 * 	0:	success
 *     -1:	mount tmp filesystem fail
 *     -2:	copy files fail
 *     -3:	make directory fail
 *
 * WARNING
 * ==========================================================================
 *  YOU HAVE TO HANDLE THIS FUNCTION VERY CAREFUL.  IF YOU MISS FILE(S) THAT
 *  ARE NEED BY init PROCESS, e.g. /usr/lib/libnvram.so,
 *  KERNEL REBOOTS SYSTEM IN 3 SECONDS.  ERROR MESSAGE IS SHOWN BELOW:
 * ==========================================================================
 * Image successfully flashed
 * Kernel panic - not syncing: Attempted to kill init!
 * Rebooting in 3 seconds..
 * /sbin/init: can't load library 'libnvram.so'
 */
#if defined(RTCONFIG_TEMPROOTFS)
static int remove_tail_char(char *str, char c)
{
	char *p;

	if (!str || *str == '\0' || c == '\0')
		return -1;

	for (p = str + strlen(str); p >= str && *p == c; p--)
		*p = '\0';
	if (p == str)
		return -2;

	return 0;
}

/*
 * @param:	extra parameter of cp command
 * @dir:	base directory
 * @files:	files, e.g., "fileA fileB fileC"
 * @newroot:	new root directory
 * @return:
 * 	0:	success
 *
 * 1. cp("", "/bin", "fileA fileB fileC", "/sysroot") equals to below commands:
 *    a. mkdir -p /sysroot/bin
 *    b. cp -a /bin/fileA /bin/fileB /bin/fileC /sysroot/bin
 *
 * 2. cp("L", "/usr/bin", "" or NULL, "/sysroot") equals to below commands:
 *    a. if [ -e "/sysroot/usr/bin" ] ; then rmdir "/sysroot/usr/bin" ; fi
 *    b. mkdir -p `dirname /sysroot/usr/bin`
 *    c. cp -aL /usr/bin /sysroot
 */
static int __cp(const char *param, const char *dir, const char *files, const char *newroot)
{
	struct stat st;
	const char *sep = "/";
	int i, l, len = 0, len2 = 0, mode = 0;	/* copy files and sub-directory */
	char cmd[2048], *f, *p, *token, *ptr1;
	char d[PATH_MAX], dest_dir[PATH_MAX];
	char str1[] = "cp -afXXXXXXXXXXXXXYYY";
	const char delim[] = " ";

	if (!dir || !newroot || *newroot == '\0')
		return -1;

	if (!files || *files == '\0')
		mode = 1;	/* copy a directory recursive */

	if (!param)
		param = "";

	sprintf(str1, "cp -af%s", param);
	if (dir && *dir == '/')
		sep = "";
	sprintf(dest_dir, "%s", newroot);
	if (stat(dest_dir, &st) || !S_ISDIR(st.st_mode))
		return -2;

	switch (mode) {
	case 0:	/* copy files and sub-directory */
		if (!(f = strdup(files)))
			return -3;
		if (*dir != '\0') {
			sprintf(dest_dir, "%s%s%s", newroot, sep, dir);
			if (!d_exists(dest_dir))
				eval("mkdir", "-p", dest_dir);
			else if (!S_ISDIR(st.st_mode)) {
				_dprintf("%s exist and is not a directory!\n", dest_dir);
				return -4;
			}
		}

		strcpy(cmd, str1);
		len = strlen(cmd);
		p = cmd + len;
		len2 = strlen(dest_dir) + 2;	/* leading space + tail NULL */
		len += len2;
		for (i = l = 0, token = strtok_r(f, delim, &ptr1);
			token != NULL;
			token = strtok_r(NULL, delim, &ptr1), p += l, len += l)
		{
			sprintf(d, "%s/%s", dir, token);
			/* don't check existence if '?' or '*' exist */
			if (!strchr(d, '?') && !strchr(d, '*') && stat(d, &st) < 0) {
				_dprintf("%s: %s not exist, skip!\n", __func__, d);
				l = 0;
				continue;
			}

			l = strlen(d) + 1;
			if ((len + l) < sizeof(cmd)) {
				strcat(p, " ");
				strcat(p, d);
				++i;
				continue;
			}

			/* cmd buffer is not enough, flush */
			strcat(p, " ");
			strcat(p, dest_dir);
			system(cmd);

			strcpy(cmd, str1);
			len = strlen(cmd);
			p = cmd + len;
			len += len2;

			strcat(p, " ");
			strcat(p, d);
			i = 1;
		}

		if (i > 0) {
			strcat(p, " ");
			strcat(p, dest_dir);
			system(cmd);
		}
		free(f);
		break;
	case 1:	/* copy a directory recursive */
		/* If /newroot/bin exist and is directory, rmdir /newroot/bin.
		 * If not, /bin would be copied to newroot/bin/bin.
		 */
		if (*dir == '\0')
			return -10;

		sprintf(dest_dir, "%s%s%s", newroot, sep, dir);
		if (d_exists(dest_dir)) {
			_dprintf("%s exist, remove it\n", dest_dir);
			if (rmdir(dest_dir)) {
				_dprintf("rmdir %s (%s)\n", dest_dir, strerror(errno));
				return -11;
			}
		}

		/* remove tail '/' */
		strcpy(d, dest_dir);
		remove_tail_char(d, '/');

		/* make sure parent directory of destination directory exist */
		p = strrchr(d, '/');
		if (p && p != d) {
			*p = '\0';
			remove_tail_char(d, '/');
			if (!d_exists(d))
				eval("mkdir", "-p", d);
		}
		sprintf(cmd, "%s %s %s", str1, dir, dest_dir);
		system(cmd);

		break;
	default:
		_dprintf("%s: mode %d is not defined!\n", __func__, mode);
		return -4;
	}

	return 0;
}

/* Build a temporary rootfilesystem.
 *
 * If you add new binary to temp. rootfilesystem, check whether it needs another library!
 * For example, iwpriv needs libiw.so.29, libgcc_s.so.1, and libc.so.0:
 * $ mipsel-linux-objdump -x bin/iwpriv | grep NEED
 *   NEEDED               libiw.so.29
 *   NEEDED               libgcc_s.so.1
 *   NEEDED               libc.so.0
 *   VERNEED              0x004008d4
 *   VERNEEDNUM           0x00000001
 */
static int build_temp_rootfs(const char *newroot)
{
	int i, r;
	struct stat st;
	char d1[PATH_MAX], d2[1024];
	const char *mdir[] = { "/proc", "/tmp", "/sys", "/usr", "/var", "/var/lock" };
	const char *bin = "ash busybox cat cp dd df echo grep iwpriv kill ls ps mkdir mount nvram ping sh tar umount uname";
	const char *sbin = "init rc hotplug2 insmod lsmod modprobe reboot rmmod rtkswitch";
	const char *lib = "librt*.so* libnsl* libdl* libm* ld-* libiw* libgcc* libpthread* libdisk* libc*";
	const char *usrbin = "killall";
#ifdef RTCONFIG_BCMARM
	const char *usrsbin = "nvram";
#endif
	const char *usrlib = "libnvram.so libshared.so libcrypto.so* libbcm*"
#if defined(RTCONFIG_HTTPS) || defined(RTCONFIG_PUSH_EMAIL)
			     " libssl*"
#endif
		;
	const char *kmod = "find /lib/modules -name '*.ko'|"
		"grep '\\("
#if defined(RTCONFIG_BLINK_LED)
		"bled\\|"			/* bled.ko */
		"usbcore\\|"			/* usbcore.ko */
#if LINUX_KERNEL_VERSION >= KERNEL_VERSION(3,2,0)
		"usb-common\\|"			/* usb-common.ko, kernel 3.2 or above */
#endif
#endif
		"nvram_linux\\)";		/* nvram_linux.ko */

	if (!newroot || *newroot == '\0')
		newroot = TMP_ROOTFS_MNT_POINT;

	if ((r = mount("tmpfs", newroot, "tmpfs", MS_NOATIME, "")) != 0)
		return -1;

	_dprintf("Build temp rootfs\n");
	__cp("", "/dev", "", newroot);
	__cp("", "/lib/modules", "modules.dep", newroot);
	__cp("", "/bin", bin, newroot);
	__cp("", "/sbin", sbin, newroot);
	__cp("", "/lib", lib, newroot);
	__cp("", "/lib", "libcrypt*", newroot);
	__cp("", "/usr/bin", usrbin, newroot);
#ifdef RTCONFIG_BCMARM
	__cp("", "/usr/sbin", usrsbin, newroot);
#endif
	__cp("", "/usr/lib", usrlib, newroot);
	__cp("L", "/etc", "", newroot);		/* don't creat symbolic link (/tmp/etc/foo) that will be broken soon */

#if defined(RTCONFIG_QCA)
	__cp("", "/sbin", "wlanconfig", newroot);
	__cp("", "/usr/lib", "libnl-tiny.so", newroot);
	__cp("", "/usr/sbin", "swconfig", newroot);
#endif

	/* copy mandatory kernel modules */
	sprintf(d2, "tar cvf - `%s` | tar xf - -C %s", kmod, newroot);
	system(d2);

	/* make directory, if not exist */
	for (i = 0; i < ARRAY_SIZE(mdir); ++i) {
		sprintf(d1, "%s/%s", newroot, mdir[i]);
		if (stat(d1, &st) && (r = mkdir(d1, 0755) == -1))
			return -3;
	}

	return 0;
}

/* Switch rootfilesystem to newroot.
 * @newroot:	Mount point of new root filesystem.
 * @return:
 * 	-1:	Not init process.
 * 	-2:	chdir to newroot fail
 * 	-3:	Newroot is not a mount point
 * 	-4:	Move mount point to / fail
 * 	-5:	exec new init process fail
 */
static int switch_root(const char *newroot)
{
	int r;
	dev_t rdev;
	struct stat st;
	char *const argv[] = { "/sbin/init", "reboot", NULL };

	if (!newroot || *newroot == '\0')
		newroot = TMP_ROOTFS_MNT_POINT;

	if (getpid() != 1) {
		_dprintf("%s: PID != 1\n", __func__);
		return -1;
	}

	if (chdir(newroot))
		return -2;
	stat("/", &st);
	rdev = st.st_dev;
	stat(".", &st);
	if (rdev == st.st_dev)
		return -3;

	/* emulate switch_root command */
	if ((r = mount(".", "/", NULL, MS_MOVE, NULL)) != 0)
		return -4;

	chroot(".");
	chdir("/");

	/* WARNING:
	 * If new rootfilesystem lacks libraries that are need by init process,
	 * kernel reboots system in 3 seconds.
	 */
	if ((r = execv(argv[0], argv)))
		return -5;

	/* NEVER REACH HERE */
	return 0;
}
#else	/* !RTCONFIG_TEMPROOTFS */
static inline int build_temp_rootfs(const char *newroot) { return -999; }
static inline int switch_root(const char *newroot) { return -999; }
#endif	/* RTCONFIG_TEMPROOTFS */

void setup_passwd(void)
{
	create_passwd();
}

void create_passwd(void)
{
	char s[512];
	char *p;
	char salt[32];
	FILE *f;
	mode_t m;
	char *http_user;

#ifdef RTCONFIG_SAMBASRV	//!!TB
	char *smbd_user;

	create_custom_passwd();
#endif
#ifdef RTCONFIG_OPENVPN
	mkdir_if_none("/etc/pam.d");
	f_write_string("/etc/pam.d/openvpn",
		"auth required pam_unix.so\n",
		0, 0644);
	create_openvpn_passwd();
#endif

	strcpy(salt, "$1$");
	f_read("/dev/urandom", s, 6);
	base64_encode((unsigned char *) s, salt + 3, 6);
	salt[3 + 8] = 0;
	p = salt;
	while (*p) {
		if (*p == '+') *p = '.';
		++p;
	}
	if (((p = nvram_get("http_passwd")) == NULL) || (*p == 0)) p = "admin";

	if (((http_user = nvram_get("http_username")) == NULL) || (*http_user == 0)) http_user = "admin";

#ifdef RTCONFIG_SAMBASRV	//!!TB
	if (((smbd_user = nvram_get("smbd_user")) == NULL) || (*smbd_user == 0) || !strcmp(smbd_user, "root"))
		smbd_user = "nas";
#endif

	m = umask(0777);
	if ((f = fopen("/etc/shadow", "w")) != NULL) {
		p = crypt(p, salt);
		fprintf(f, "%s:%s:0:0:99999:7:0:0:\n"
			   "nobody:*:0:0:99999:7:0:0:\n", http_user, p);
#ifdef RTCONFIG_SAMBASRV	//!!TB
		fprintf(f, "%s:*:0:0:99999:7:0:0:\n", smbd_user);
#endif
#ifdef RTCONFIG_OPENVPN
		fappend(f, "/etc/shadow.openvpn");
#endif

		fappend(f, "/etc/shadow.custom");
		append_custom_config("shadow", f);
#ifdef RTCONFIG_OPENVPN
		fappend(f, "/etc/shadow.openvpn");
#endif
		fclose(f);
		run_postconf("shadow", "/etc/shadow");
	}
	umask(m);
	chmod("/etc/shadow", 0600);

#ifdef RTCONFIG_SAMBASRV	//!!TB
	sprintf(s,
		"%s:x:0:0:%s:/root:/bin/sh\n"
		"%s:x:100:100:nas:/dev/null:/dev/null\n"
		"nobody:x:65534:65534:nobody:/dev/null:/dev/null\n",
		http_user,
		http_user,
		smbd_user);
#else	//!!TB
	sprintf(s,
		"%s:x:0:0:%s:/root:/bin/sh\n"
		"nobody:x:65534:65534:nobody:/dev/null:/dev/null\n",
		http_user,
		http_user);
#endif	//!!TB
	f_write_string("/etc/passwd", s, 0, 0644);
	fappend_file("/etc/passwd", "/etc/passwd.custom");
#ifdef RTCONFIG_OPENVPN
	fappend_file("/etc/passwd", "/etc/passwd.openvpn");
#endif
	fappend_file("/etc/passwd", "/jffs/configs/passwd.add");
	run_postconf("passwd","/etc/passwd");

	sprintf(s,
		"%s:*:0:\n"
#ifdef RTCONFIG_SAMBASRV	//!!TB
		"nas:*:100:\n"
#endif
		"nobody:*:65534:\n",
		http_user);
	f_write_string("/etc/gshadow", s, 0, 0644);
	fappend_file("/etc/gshadow", "/etc/gshadow.custom");
        fappend_file("/etc/gshadow", "/jffs/configs/gshadow.add");
	run_postconf("gshadow","/etc/gshadow");

	f_write_string("/etc/group",
		"root:x:0:\n"
#ifdef RTCONFIG_SAMBASRV	//!!TB
		"nas:x:100:\n"
#endif
		"nobody:x:65534:\n",
		0, 0644);
	fappend_file("/etc/group", "/etc/group.custom");
#ifdef RTCONFIG_OPENVPN
	fappend_file("/etc/group", "/etc/group.openvpn");
#endif
	fappend_file("/etc/group", "/jffs/configs/group.add");
	run_postconf("group","/etc/group");
}

#ifdef CONFIG_BCMWL5
int ate_cond()
{
	switch(get_model()){
		case MODEL_RPAC68U:
			if (factory_debug())
				return 0;
			break;
		default:
			break;
	}

	return 1;
}
#endif

void get_dhcp_pool(char **dhcp_start, char **dhcp_end, char *buffer)
{
	if (dhcp_start == NULL || dhcp_end == NULL || buffer == NULL)
		return;

#ifdef RTCONFIG_WIRELESSREPEATER
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") != WLC_STATE_CONNECTED){
		if(nvram_match("lan_proto", "static")) {
			in_addr_t lan_ipaddr, lan_netmask;
			char *p = buffer;
			unsigned char lan1[4], lan2[4];
			unsigned offset;

			inet_aton(nvram_safe_get("lan_ipaddr") , (struct in_addr*) &lan_ipaddr);
			inet_aton(nvram_safe_get("lan_netmask"), (struct in_addr*) &lan_netmask);
//			cprintf("#### lan_ipaddr(%08x) lan_netmask(%08x)\n", lan_ipaddr, lan_netmask);

			//start
			if ((ntohl(lan_ipaddr & lan_netmask) | 1 ) == ntohl(lan_ipaddr))
				offset = 2;
			else
				offset = 1;
			*(in_addr_t *) &lan1 = (lan_ipaddr & lan_netmask) | htonl(offset);
			*dhcp_start = p;
			p += sprintf(p, "%u.%u.%u.%u", lan1[0], lan1[1], lan1[2], lan1[3]);
			p += 1;

			//end
			if ((ntohl(lan_ipaddr & lan_netmask) | 254) == ntohl(lan_ipaddr))
				offset = 253;
			else
				offset = 254;
			*((in_addr_t *) &lan2) = (lan_ipaddr & lan_netmask) | htonl(offset);
			*dhcp_end = p;
			p += sprintf(p, "%u.%u.%u.%u", lan2[0], lan2[1], lan2[2], lan2[3]);
			p += 1;

//			cprintf("#### dhcp_start(%s) dhcp_end(%s)\n", *dhcp_start, *dhcp_end);
		} else {
			*dhcp_start = nvram_default_get("dhcp_start");
			*dhcp_end = nvram_default_get("dhcp_end");
		}
	}
	else
#endif
	{
		*dhcp_start = nvram_safe_get("dhcp_start");
		*dhcp_end = nvram_safe_get("dhcp_end");
	}
}

#if 0
int get_dhcpd_lmax()
{
	unsigned int lstart, lend, lip;
	int dhlease_size, invalid_ipnum, except_lanip;
	char *dhcp_start, *dhcp_end, *lan_netmask, *lan_ipaddr;
	char buffer[64];

	get_dhcp_pool(&dhcp_start, &dhcp_end, buffer);
#ifdef RTCONFIG_WIRELESSREPEATER
	if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") != WLC_STATE_CONNECTED){
		lan_netmask = nvram_default_get("lan_netmask");
		lan_ipaddr = nvram_default_get("lan_ipaddr");
	}
	else
#endif
	{
		lan_netmask = nvram_safe_get("lan_netmask");
		lan_ipaddr = nvram_safe_get("lan_ipaddr");
	}

	lstart = htonl(inet_addr(dhcp_start)) & ~htonl(inet_addr(lan_netmask));
	lend = htonl(inet_addr(dhcp_end)) & ~htonl(inet_addr(lan_netmask));
	lip = htonl(inet_addr(lan_ipaddr)) & ~htonl(inet_addr(lan_netmask));

	dhlease_size = lend - lstart + 1;
	invalid_ipnum = dhlease_size / 256 * 2;
	except_lanip = (lip >= lstart && lip <= lend)? 1:0;
	dhlease_size -= invalid_ipnum + except_lanip;

	return dhlease_size;
}
#endif

void stop_hour_monitor_service()
{
	//logmessage("hour monitor", "stop_hour_monitor_service");
	killall("hour_monitor", SIGTERM);
}

void start_hour_monitor_service()
{
	char *cmd[] = {"hour_monitor", NULL};
	int pid;

	if(nvram_get_int("sw_mode") != SW_MODE_ROUTER)
		return;

	if (!nvram_get_int("ntp_ready"))
		return;

	if(!pids("hour_monitor")){
	//logmessage("hour monitor", "start again due to not pid");
		_eval(cmd, NULL, 0, &pid);
	}
}

void check_hour_monitor_service()
{
	if(hour_monitor_function_check()) start_hour_monitor_service();
}

#ifdef RTCONFIG_DHCP_OVERRIDE
static int chk_same_subnet(char *ip1, char *ip2, char *sub)
{
	unsigned int addr1, addr2, submask;

	if (!*ip1 || !*ip2 || !*sub)
		return 0;

	addr1 = ntohl(inet_addr(ip1));
	addr2 = ntohl(inet_addr(ip2));
	submask = ntohl(inet_addr(sub));

	return (addr1 & submask) == (addr2 & submask);
}

static void simple_dhcp_range(char *ip, char *dip1, char *dip2, char *mask)
{
	struct in_addr ina;
	unsigned int new_start, new_end, lmask;

	new_start = (ntohl(inet_addr(ip)) & (lmask = ntohl(inet_addr(mask)))) + 1;
	new_end = ntohl(inet_addr(ip)) & (lmask = ntohl(inet_addr(mask))) | ~(lmask) - 1;

	ina.s_addr = htonl(new_start);
	strcpy(dip1, inet_ntoa(ina));
	ina.s_addr = htonl(new_end);
	strcpy(dip2, inet_ntoa(ina));
}

static int chk_valid_startend(char *ip, char *ip1, char *ip2, char *sub)
{
	int result1, result2;

	result1 = chk_same_subnet(ip, ip1, sub);
	result2 = chk_same_subnet(ip, ip2, sub);

	if (!result1 || !result2) {
		simple_dhcp_range(ip, ip1, ip2, sub);
		return 0;
	}
	return 1;
}

static void link_down(void)
{
	char word[256], *next, ifnames[128];

	/* link down LAN ports */
	eval("rtkswitch", "15");

	/* ifconfig down wireless */
	strcpy(ifnames, nvram_safe_get("wl_ifnames"));
	foreach (word, ifnames, next)
		ifconfig(word, 0, NULL, NULL);
}

static void link_up(void)
{
	char word[256], *next, ifnames[128];

	/* link up LAN ports */
	eval("rtkswitch", "14");

	/* ifconfig up wireless */
	strcpy(ifnames, nvram_safe_get("wl_ifnames"));
	foreach (word, ifnames, next)
		ifconfig(word, IFUP, NULL, NULL);
}

int restart_dnsmasq(int need_link_DownUp)
{
	if (need_link_DownUp) {
#if (defined(PLN12) || defined(PLAC56) || defined(PLAC66))
		nvram_set("plc_ready", "0");
#endif
		link_down();
		sleep(9);
	}

	stop_dnsmasq();
	sleep(1);
	start_dnsmasq();

	if (need_link_DownUp) {
		link_up();
#if (defined(PLN12) || defined(PLAC56) || defined(PLAC66))
		nvram_set("plc_ready", "1");
#endif
	}

	return 0;
}
#endif

void start_dnsmasq()
{
	FILE *fp;
	char *lan_ifname, *lan_ipaddr;
	char *value;
	int i, have_dhcp = 0;

	TRACE_PT("begin\n");

	if (getpid() != 1) {
		notify_rc("start_dnsmasq");
		return;
	}

	stop_dnsmasq();

	lan_ifname = nvram_safe_get("lan_ifname");
#ifdef RTCONFIG_WIRELESSREPEATER
	if (nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") != WLC_STATE_CONNECTED && !nvram_match("lan_proto", "static")) {
		lan_ipaddr = nvram_default_get("lan_ipaddr");
	} else
#endif
	{
		lan_ipaddr = nvram_safe_get("lan_ipaddr");
	}

	/* write /etc/hosts */
	if ((fp = fopen("/etc/hosts", "w")) != NULL) {
		/* loclhost ipv4 */
		fprintf(fp, "127.0.0.1 localhost.localdomain localhost\n");
		fprintf(fp, "%s %s\n", lan_ipaddr, DUT_DOMAIN_NAME);
		fprintf(fp, "%s %s\n", lan_ipaddr, OLD_DUT_DOMAIN_NAME1);
		fprintf(fp, "%s %s\n", lan_ipaddr, OLD_DUT_DOMAIN_NAME2);
		/* productid/samba name */
		if (is_valid_hostname(value = nvram_safe_get("computer_name")) ||
		    is_valid_hostname(value = get_productid()))
			fprintf(fp, "%s %s.%s %s\n", lan_ipaddr,
				    value, nvram_safe_get("lan_domain"), value);
		/* lan hostname.domain hostname */
		if (nvram_invmatch("lan_hostname", "")) {
			fprintf(fp, "%s %s.%s %s\n", lan_ipaddr,
				    nvram_safe_get("lan_hostname"),
				    nvram_safe_get("lan_domain"),
				    nvram_safe_get("lan_hostname"));
		}
#ifdef RTCONFIG_IPV6
		if (ipv6_enabled()) {
			/* localhost ipv6 */
			fprintf(fp, "::1 localhost6.localdomain6 localhost6\n");
			/* lan6 hostname.domain hostname */
			value = (char*) ipv6_router_address(NULL);
			if (*value && nvram_invmatch("lan_hostname", "")) {
				fprintf(fp, "%s %s.%s %s\n", value,
					    nvram_safe_get("lan_hostname"),
					    nvram_safe_get("lan_domain"),
					    nvram_safe_get("lan_hostname"));
			}
		}
#endif
		append_custom_config("hosts", fp);
		fclose(fp);
		use_custom_config("hosts", "/etc/hosts");
		run_postconf("hosts","/etc/hosts");
	} else
		perror("/etc/hosts");


#ifdef RTCONFIG_REDIRECT_DNAME
	if (nvram_get_int("sw_mode") == SW_MODE_AP) {
#ifdef RTCONFIG_DHCP_OVERRIDE
		if (nvram_match("dhcp_enable_x", "1") && nvram_match("dnsqmode", "2")
#ifdef RTCONFIG_DEFAULT_AP_MODE
				&& !nvram_match("ate_flag", "1")
#endif
		) {
			if ((fp = fopen("/etc/dnsmasq.conf", "w+")) != NULL) {
				/* DHCP range */
				char dhcp_start[16], dhcp_end[16], lan_netmask[16];

				strcpy(dhcp_start, nvram_safe_get("dhcp_start"));
				strcpy(dhcp_end, nvram_safe_get("dhcp_end"));
				strcpy(lan_netmask, nvram_safe_get("lan_netmask"));

				if (!chk_valid_startend(lan_ipaddr, dhcp_start, dhcp_end, lan_netmask)) {
					dbg("reset DHCP range: %s ~ %s\n", dhcp_start, dhcp_end);
					nvram_set("dhcp_start", dhcp_start);
					nvram_set("dhcp_end", dhcp_end);
				}

				fprintf(fp, "interface=%s\n", nvram_safe_get("lan_ifname"));
				fprintf(fp, "dhcp-range=lan,%s,%s,%s,%ss\n",
								dhcp_start,
								dhcp_end,
								lan_netmask,
								nvram_safe_get("dhcp_lease"));
				fprintf(fp, "dhcp-leasefile=/tmp/dnsmasq.leases\n");
				/* Gateway */
				fprintf(fp, "dhcp-option=lan,3,%s\n", lan_ipaddr);
				/* Faster for moving clients, if authoritative */
				fprintf(fp, "dhcp-authoritative\n");
				/* caching */
				fprintf(fp, "cache-size=1500\n"
					    "no-negcache\n");
				fclose(fp);
			}
			else
				perror("/etc/dnsmasq.conf");
		}
		else
			unlink("/etc/dnsmasq.conf");
#endif

		eval("dnsmasq", "--log-async");
	}
#endif
	if (!is_routing_enabled()
#ifdef RTCONFIG_WIRELESSREPEATER
	 && nvram_get_int("sw_mode") != SW_MODE_REPEATER
#endif
	) return;

	// we still need dnsmasq in wet
	//if (foreach_wif(1, NULL, is_wet)) return;

	if ((fp = fopen("/etc/dnsmasq.conf", "w")) == NULL)
		return;

	fprintf(fp, "pid-file=/var/run/dnsmasq.pid\n"
		    "user=nobody\n"
		    "bind-dynamic\n"		// listen only on interface & lo
		);

#if defined(RTCONFIG_REDIRECT_DNAME)
	if(nvram_get_int("sw_mode") != SW_MODE_REPEATER)
#endif
	{
		fprintf(fp,"interface=%s\n",		// dns & dhcp on LAN interface
			lan_ifname);
#if defined(RTCONFIG_PPTPD) || defined(RTCONFIG_ACCEL_PPTPD)
		if (is_routing_enabled())
		fprintf(fp, "interface=%s\n"		// dns on VPN clients interfaces
		    	"no-dhcp-interface=%s\n",	// no dhcp for VPN clients
			"ppp1*", "ppp1*");
#endif
	}

#ifdef  __CONFIG_NORTON__
	/* TODO: dnsmasq doesn't support a single hostname across multiple interfaces */
	if (nvram_get_int("nga_enable"))
		fprintf(fp, "interface-name=norton.local,%s\n", lan_ifname);
#endif /* __CONFIG_NORTON__ */

#ifdef RTCONFIG_YANDEXDNS
	if (nvram_get_int("yadns_enable_x") && nvram_get_int("yadns_mode") != YADNS_DISABLED) {
		fprintf(fp, "no-resolv\n");	// no resolv
	} else
#endif
	fprintf(fp, "resolv-file=%s\n",		// the real stuff is here
		dmresolv);

	fprintf(fp, "servers-file=%s\n"		// additional servers list
		    "no-poll\n"			// don't poll resolv file
		    "no-negcache\n"		// don't cace nxdomain
		    "cache-size=%u\n"		// dns cache size
		    "min-port=%u\n",		// min port used for random src port
		dmservers, 1500, nvram_get_int("dns_minport") ? : 4096);

	/* lan domain */
	value = nvram_safe_get("lan_domain");
	if (*value) {
	fprintf(fp, "domain=%s\n"
			    "expand-hosts\n", value);	// expand hostnames in hosts file
		if (nvram_get_int("lan_dns_fwd_local") != 1)
			fprintf(fp, "bogus-priv\n"
			            "local=/%s/\n", value);	// Don't forward local queries upstream
	}

	if (
		(is_routing_enabled() && nvram_get_int("dhcp_enable_x"))
#ifdef RTCONFIG_WIRELESSREPEATER
	 || (nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") != WLC_STATE_CONNECTED)
#endif
	) {
		char *dhcp_start, *dhcp_end;
		int dhcp_lease;
		char buffer[64];
#if defined(RTCONFIG_TR069) && !defined(RTCONFIG_TR181)
		unsigned char hwaddr[6];
#endif

		have_dhcp |= 1; /* DHCPv4 */

		get_dhcp_pool(&dhcp_start, &dhcp_end, buffer);
#ifdef RTCONFIG_WIRELESSREPEATER
		if(nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_state") != WLC_STATE_CONNECTED){
			dhcp_lease = atoi(nvram_default_get("dhcp_lease"));
		}
		else
#endif
		{
			dhcp_lease = nvram_get_int("dhcp_lease");
		}

		if (dhcp_lease <= 0)
			dhcp_lease = 86400;

		/* LAN range */
		if (*dhcp_start && *dhcp_end) {
			fprintf(fp, "dhcp-range=lan,%s,%s,%s,%ds\n",
				dhcp_start, dhcp_end, nvram_safe_get("lan_netmask"), dhcp_lease);
		} else {
			/* compatibility */
			char lan[24];
			int start = nvram_get_int("dhcp_start");
			int count = nvram_get_int("dhcp_num");

			strlcpy(lan, lan_ipaddr, sizeof(lan));
			if ((value = strrchr(lan, '.')) != NULL) *(value + 1) = 0;

			fprintf(fp, "dhcp-range=lan,%s%d,%s%d,%s,%ds\n",
				lan, start, lan, start + count - 1, nvram_safe_get("lan_netmask"), dhcp_lease);
		}

		/* Gateway, if not set, force use lan ipaddr to avoid repeater issue */
		value = nvram_safe_get("dhcp_gateway_x");
		value = (*value && inet_addr(value)) ? value : lan_ipaddr;
		fprintf(fp, "dhcp-option=lan,3,%s\n", value);

		/* DNS server and additional router address */
		value = nvram_safe_get("dhcp_dns1_x");
		if (*value && inet_addr(value)) {
			char *value2;

			value2 = nvram_safe_get("dhcp_dns2_x");
			fprintf(fp, "dhcp-option=lan,6,%s%s%s%s\n",
				     value,
				     (*value2 && inet_addr(value2) ? "," : ""),
				     (*value2 && inet_addr(value2) ? value2 : ""),
				     (nvram_match("dhcpd_dns_router","1") ? ",0.0.0.0" : ""));
		}

		/* LAN Domain */
		value = nvram_safe_get("lan_domain");
		if (*value)
			fprintf(fp, "dhcp-option=lan,15,%s\n", value);

		/* WINS server */
		value = nvram_safe_get("dhcp_wins_x");
		if (*value && inet_addr(value)) {
			fprintf(fp, "dhcp-option=lan,44,%s\n"
			/*	    "dhcp-option=lan,46,8\n"*/, value);
		}
#ifdef RTCONFIG_SAMBASRV
		/* Samba will serve as a WINS server */
		else if (nvram_invmatch("lan_domain", "") && nvram_get_int("smbd_wins")) {
			fprintf(fp, "dhcp-option=lan,44,%s\n"
			/*	    "dhcp-option=lan,46,8\n"*/, lan_ipaddr);
		}
#endif
		/* Shut up WPAD info requests */
		fprintf(fp, "dhcp-option=lan,252,\"\\n\"\n");

#if defined(RTCONFIG_TR069) && !defined(RTCONFIG_TR181)
		if (ether_atoe(get_lan_hwaddr(), hwaddr)) {
			snprintf(buffer, sizeof(buffer), "%02X%02X%02X", hwaddr[0], hwaddr[1], hwaddr[2]);
			fprintf(fp, "dhcp-option-force=cpewan-id,vi-encap:%d,%d,\"%s\"\n",
				3561, 4, buffer);
			snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X",
				 hwaddr[0], hwaddr[1], hwaddr[2], hwaddr[3], hwaddr[4], hwaddr[5]);
			fprintf(fp, "dhcp-option-force=cpewan-id,vi-encap:%d,%d,\"%s\"\n",
				3561, 5, buffer);
			fprintf(fp, "dhcp-option-force=cpewan-id,vi-encap:%d,%d,\"%s\"\n",
				3561, 6, nvram_safe_get("productid"));
		}
#endif
	}

#ifdef RTCONFIG_IPV6
	if (ipv6_enabled() && is_routing_enabled()) {
		struct in6_addr addr;
		int ra_lifetime, dhcp_lifetime;
		int service, stateful, dhcp_start, dhcp_end;

		service = get_ipv6_service();
		stateful = (service == IPV6_NATIVE_DHCP || service == IPV6_MANUAL) ?
			nvram_get_int(ipv6_nvname("ipv6_autoconf_type")) : 0;
		ra_lifetime = 600; /* 10 minutes for now */
		dhcp_lifetime = nvram_get_int(ipv6_nvname("ipv6_dhcp_lifetime"));
		if (dhcp_lifetime <= 0)
			dhcp_lifetime = 86400;

		if (nvram_get_int(ipv6_nvname("ipv6_radvd"))) {
			fprintf(fp, "ra-param=%s,%d,%d\n"
				    "enable-ra\n"
				    "quiet-ra\n",
				lan_ifname, 10, ra_lifetime);
		}

		/* LAN prefix or range */
		if (stateful) {
			/* TODO: rework WEB UI to specify ranges without prefix
			 * TODO: add size checking, now range takes all of 16 bit */
			dhcp_start = (inet_pton(AF_INET6, nvram_safe_get(ipv6_nvname("ipv6_dhcp_start")), &addr) > 0) ?
			    ntohs(addr.s6_addr16[7]) : 0x1000;
			dhcp_end = (inet_pton(AF_INET6, nvram_safe_get(ipv6_nvname("ipv6_dhcp_end")), &addr) > 0) ?
			    ntohs(addr.s6_addr16[7]) : 0x2000;
			fprintf(fp, "dhcp-range=lan,::%04x,::%04x,constructor:%s,%d\n",
				(dhcp_start < dhcp_end) ? dhcp_start : dhcp_end,
				(dhcp_start < dhcp_end) ? dhcp_end : dhcp_start,
				lan_ifname, dhcp_lifetime);
			have_dhcp |= 2; /* DHCPv6 */
		} else if (nvram_get_int(ipv6_nvname("ipv6_radvd"))) {
			if (nvram_get_int("ipv6_dhcp6s_enable")) {
				fprintf(fp, "dhcp-range=lan,::,constructor:%s,ra-stateless,%d,%d\n",
					lan_ifname, 64, ra_lifetime);
				have_dhcp |= 2; /* DHCPv6 */
			} else {
				fprintf(fp, "dhcp-range=lan,::,constructor:%s,ra-only,%d,%d\n",
					lan_ifname, 64, ra_lifetime);
			}
		}

#ifdef RTCONFIG_YANDEXDNS
		if (nvram_get_int("yadns_enable_x")) {
			unsigned char ea[ETHER_ADDR_LEN];
			char *name, *mac, *mode, *enable, *server[2];
			char *nv, *nvp, *b;
			int i, count, dnsmode, defmode = nvram_get_int("yadns_mode");

			for (dnsmode = YADNS_FIRST; dnsmode < YADNS_COUNT; dnsmode++) {
				if (dnsmode == defmode)
					continue;
				count = get_yandex_dns(AF_INET6, dnsmode, server, sizeof(server)/sizeof(server[0]));
				if (count <= 0)
					continue;
				fprintf(fp, "dhcp-option=yadns%u,option6:23", dnsmode);
				for (i = 0; i < count; i++)
					fprintf(fp, ",[%s]", server[i]);
				fprintf(fp, "\n");
			}

			/* DNS server per client */
			nv = nvp = strdup(nvram_safe_get("yadns_rulelist"));
			while (nv && (b = strsep(&nvp, "<")) != NULL) {
				if (vstrsep(b, ">", &name, &mac, &mode, &enable) < 3)
					continue;
				if (enable && atoi(enable) == 0)
					continue;
				if (!*mac || !*mode || !ether_atoe(mac, ea))
					continue;
				dnsmode = atoi(mode);
				/* Skip incorrect and default levels */
				if (dnsmode < YADNS_FIRST || dnsmode >= YADNS_COUNT || dnsmode == defmode)
					continue;
				fprintf(fp, "dhcp-host=%s,set:yadns%u\n", mac, dnsmode);
			}
			free(nv);
		}
#endif /* RTCONFIG_YANDEXDNS */

#ifdef RTCONFIG_DNSFILTER
		if (nvram_get_int("dnsfilter_enable_x")) {
			unsigned char ea[ETHER_ADDR_LEN];
			char *name, *mac, *mode, *enable, *server[2];
			char *nv, *nvp, *b;
			int count, dnsmode, defmode = nvram_get_int("dnsfilter_mode");

			for (dnsmode = 1; dnsmode < 13; dnsmode++) {
				if (dnsmode == defmode)
					continue;
				count = get_dns_filter(AF_INET6, dnsmode, server);
				if (count == 0)
					continue;
				fprintf(fp, "dhcp-option=dnsf%u,option6:23,[%s]", dnsmode, server[0]);
				if (count == 2)
					fprintf(fp, ",[%s]", server[1]);
				fprintf(fp, "\n");
			}
			/* DNS server per client */
			nv = nvp = strdup(nvram_safe_get("dnsfilter_rulelist"));
			while (nv && (b = strsep(&nvp, "<")) != NULL) {
				if (vstrsep(b, ">", &name, &mac, &mode, &enable) < 3)
					continue;
				if (enable && atoi(enable) == 0)
					continue;
				if (!*mac || !*mode || !ether_atoe(mac, ea))
					continue;
				dnsmode = atoi(mode);
				/* Skip unfiltered, default, or non-IPv6 capable levels */
				if ((dnsmode == 0) || (dnsmode == defmode) || (get_dns_filter(AF_INET6, dnsmode, server) == 0))
					continue;
				fprintf(fp, "dhcp-host=%s,set:dnsf%u\n", mac, dnsmode);
			}
			free(nv);
		}
#endif /* DNSFilter */

		/* DNS server */
		fprintf(fp, "dhcp-option=lan,option6:23,[::]\n");

		/* LAN Domain */
		value = nvram_safe_get("lan_domain");
		if (*value)
			fprintf(fp, "dhcp-option=lan,option6:24,%s\n", value);
	}
#endif

	if (have_dhcp) {
#if 0	//this would limit the total count of dhcp client (including dhcp pool and manually assigned static IP).
		/* Maximum leases */
		if ((i = get_dhcpd_lmax()) > 0)
			fprintf(fp, "dhcp-lease-max=%d\n", i);
#endif

		/* Faster for moving clients, if authoritative */
		if (nvram_get_int("dhcpd_auth") >= 0)
			fprintf(fp, "dhcp-authoritative\n");
#ifdef RTCONFIG_MULTICAST_IPTV
		/* Rawny: Add vendor class ID and DNS info for Movistar IPTV */
		if (nvram_get_int("switch_stb_x") > 6 &&
		    nvram_match("switch_wantag", "movistar")) {
			fprintf(fp, "dhcp-vendorclass=ial,IAL\n");
			fprintf(fp, "dhcp-option=ial,6,172.26.23.3\n");
			fprintf(fp, "dhcp-option=ial,240,:::::239.0.2.10:22222:v6.0:239.0.2.30:22222\n");
		}
#endif
	} else
		fprintf(fp, "no-dhcp-interface=%s\n", lan_ifname);

	/* Static IP MAC binding */
	if (nvram_match("dhcp_static_x","1")) {
		fprintf(fp, "read-ethers\n"
			    "addn-hosts=%s\n", dmhosts);

		write_static_leases("/etc/ethers");
	}

	/* Don't log DHCP queries */
	if (nvram_match("dhcpd_querylog","0")) {
		fprintf(fp,"quiet-dhcp\n");
#ifdef RTCONFIG_IPV6
		fprintf(fp,"quiet-dhcp6\n");
		fprintf(fp,"quiet-ra\n");
#endif
	}
#ifdef RTCONFIG_FINDASUS
	fprintf(fp, "address=/findasus.local/%s\n", lan_ipaddr);
#endif
#ifdef RTCONFIG_OPENVPN
	write_vpn_dnsmasq_config(fp);
#endif
#ifdef RTCONFIG_DNSSEC
	if (nvram_match("dnssec_enable", "1")) {
		fprintf(fp, "conf-file=/etc/dnssec-trust-anchors\n"
		            "dnssec\n");

		/* If NTP isn't set yet, wait until rc's ntp signals us to start validating time */
		if (!nvram_match("ntp_ready","1"))
			fprintf(fp, "dnssec-no-timecheck\n");
	}
#endif

	append_custom_config("dnsmasq.conf",fp);

#ifdef RTCONFIG_PORT_BASED_VLAN
	/* Write dhcpd config for vlan's subnet */
	vlan_subnet_dnsmasq_conf(fp);
#endif

	fclose(fp);

	use_custom_config("dnsmasq.conf","/etc/dnsmasq.conf");
	run_postconf("dnsmasq","/etc/dnsmasq.conf");

	/* Create resolv.conf with empty nameserver list */
	f_write(dmresolv, NULL, 0, FW_APPEND, 0666);

	/* Create resolv.dnsmasq with empty server list */
	f_write(dmservers, NULL, 0, FW_APPEND, 0666);

#if (defined(RTCONFIG_TR069) && !defined(RTCONFIG_TR181))
	eval("dnsmasq", "--log-async", "-6", "/sbin/dhcpc_lease");
#else
	eval("dnsmasq", "--log-async");
#endif

	for ( i = 1; i < 4; i++ ) {
                if (!pids("dnsmasq")) {
			sleep(i);
		} else {
			// Make the router use dnsmasq for its own local resolution if it did start
			unlink("/etc/resolv.conf");
			symlink("/rom/etc/resolv.conf", "/etc/resolv.conf");	// nameserver 127.0.0.1
			i = 4;
		}
	}

	TRACE_PT("end\n");
}

void stop_dnsmasq(void)
{
	TRACE_PT("begin\n");

	if (getpid() != 1) {
		notify_rc("stop_dnsmasq");
		return;
	}

	// Revert back to ISP-filled resolv.conf
        unlink("/etc/resolv.conf");
        symlink(dmresolv, "/etc/resolv.conf");

	killall_tk("dnsmasq");

	TRACE_PT("end\n");
}

void reload_dnsmasq(void)
{

#ifdef RTCONFIG_DNSSEC
	if (nvram_match("dnssec_enable", "1") && (!nvram_match("ntp_ready","1"))) {
		/* Don't reload, as it would prematurely enable timestamp validation */
		stop_dnsmasq();
		sleep(1);
		start_dnsmasq();
	}
	else
#endif

	/* notify dnsmasq */
	kill_pidfile_s("/var/run/dnsmasq.pid", SIGHUP);
}

#ifdef RTCONFIG_IPV6
void add_ip6_lanaddr(void)
{
	char ip[INET6_ADDRSTRLEN + 4];
	const char *p;

	p = ipv6_router_address(NULL);
	if (*p) {
		snprintf(ip, sizeof(ip), "%s/%d", p, nvram_get_int(ipv6_nvname("ipv6_prefix_length")) ? : 64);
		eval("ip", "-6", "addr", "add", ip, "dev", nvram_safe_get("lan_ifname"));
		if (!nvram_match(ipv6_nvname("ipv6_rtr_addr"), (char*)p))
			nvram_set(ipv6_nvname("ipv6_rtr_addr"), (char*)p);
	}

	switch (get_ipv6_service()) {
	case IPV6_NATIVE_DHCP:
		if (nvram_get_int(ipv6_nvname("ipv6_dhcp_pd")))
			break;
		/* fall through */
	case IPV6_MANUAL:
		p = ipv6_prefix(NULL);
		if (*p && !nvram_match(ipv6_nvname("ipv6_prefix"), (char*)p))
			nvram_set(ipv6_nvname("ipv6_prefix"), (char*)p);
		break;
	}
}

void start_ipv6_tunnel(void)
{
	char ip[INET6_ADDRSTRLEN + 4];
	char router[INET6_ADDRSTRLEN + 1];
	struct in_addr addr4;
	struct in6_addr addr;
	char *wanip, *mtu, *tun_dev, *gateway;
	int service;

	// for one wan only now
	service = get_ipv6_service();
	tun_dev = (char*)get_wan6face();
	wanip = (char*)get_wanip();

	mtu = (nvram_get_int(ipv6_nvname("ipv6_tun_mtu")) > 0) ? nvram_safe_get(ipv6_nvname("ipv6_tun_mtu")) : "1480";
	modprobe("sit");

	eval("ip", "tunnel", "add", tun_dev, "mode", "sit",
		"remote", (service == IPV6_6IN4) ? nvram_safe_get(ipv6_nvname("ipv6_tun_v4end")) : "any",
		"local", wanip,
		"ttl", nvram_safe_get(ipv6_nvname("ipv6_tun_ttl")));
	eval("ip", "link", "set", tun_dev, "mtu", mtu, "up");

	switch (service) {
	case IPV6_6TO4: {
		int prefixlen = 16;
		int mask4size = 0;

		/* address */
		addr4.s_addr = 0;
		memset(&addr, 0, sizeof(addr));
		inet_aton(wanip, &addr4);
		addr.s6_addr16[0] = htons(0x2002);
		ipv6_mapaddr4(&addr, prefixlen, &addr4, mask4size);
		//addr.s6_addr16[7] |= htons(0x0001);
		inet_ntop(AF_INET6, &addr, ip, sizeof(ip));
		snprintf(ip, sizeof(ip), "%s/%d", ip, prefixlen);

		/* gateway */
		snprintf(router, sizeof(router), "::%s", nvram_safe_get(ipv6_nvname("ipv6_relay")));
		gateway = router;

		add_ip6_lanaddr();
		break;
	}
	case IPV6_6RD: {
		int prefixlen = nvram_get_int(ipv6_nvname("ipv6_6rd_prefixlen"));
		int mask4size = nvram_get_int(ipv6_nvname("ipv6_6rd_ip4size"));
		char brprefix[sizeof("255.255.255.255/32")];

		/* 6rd domain */
		addr4.s_addr = 0;
		if (mask4size) {
			inet_aton(wanip, &addr4);
			addr4.s_addr &= htonl(0xffffffffUL << (32 - mask4size));
		} else	addr4.s_addr = 0;
		snprintf(ip, sizeof(ip), "%s/%d", nvram_safe_get(ipv6_nvname("ipv6_6rd_prefix")), prefixlen);
		snprintf(brprefix, sizeof(brprefix), "%s/%d", inet_ntoa(addr4), mask4size);
		eval("ip", "tunnel", "6rd", "dev", tun_dev,
		     "6rd-prefix", ip, "6rd-relay_prefix", brprefix);

		/* address */
		addr4.s_addr = 0;
		memset(&addr, 0, sizeof(addr));
		inet_aton(wanip, &addr4);
		inet_pton(AF_INET6, nvram_safe_get(ipv6_nvname("ipv6_6rd_prefix")), &addr);
		ipv6_mapaddr4(&addr, prefixlen, &addr4, mask4size);
		//addr.s6_addr16[7] |= htons(0x0001);
		inet_ntop(AF_INET6, &addr, ip, sizeof(ip));
		snprintf(ip, sizeof(ip), "%s/%d", ip, prefixlen);

		/* gateway */
		snprintf(router, sizeof(router), "::%s", nvram_safe_get(ipv6_nvname("ipv6_6rd_router")));
		gateway = router;

		add_ip6_lanaddr();
		break;
	}
	default:
		/* address */
		snprintf(ip, sizeof(ip), "%s/%d",
			nvram_safe_get(ipv6_nvname("ipv6_tun_addr")),
			nvram_get_int(ipv6_nvname("ipv6_tun_addrlen")) ? : 64);

		/* gateway */
		gateway = nvram_safe_get(ipv6_nvname("ipv6_tun_peer"));
		if (gateway && *gateway)
			eval("ip", "-6", "route", "add", gateway, "dev", tun_dev, "metric", "1");
	}

	eval("ip", "-6", "addr", "add", ip, "dev", tun_dev);
	eval("ip", "-6", "route", "del", "::/0");

	if (gateway && *gateway)
		eval("ip", "-6", "route", "add", "::/0", "via", gateway, "dev", tun_dev, "metric", "1");
	else	eval("ip", "-6", "route", "add", "::/0", "dev", tun_dev, "metric", "1");
}

void stop_ipv6_tunnel(void)
{
	int service = get_ipv6_service();

	if (service == IPV6_6TO4 || service == IPV6_6RD || service == IPV6_6IN4) {
		eval("ip", "tunnel", "del", (char *)get_wan6face());
	}
	if (service == IPV6_6TO4 || service == IPV6_6RD) {
		// get rid of old IPv6 address from lan iface
		eval("ip", "-6", "addr", "flush", "dev", nvram_safe_get("lan_ifname"), "scope", "global");
		nvram_set(ipv6_nvname("ipv6_rtr_addr"), "");
		nvram_set(ipv6_nvname("ipv6_prefix"), "");
	}
	modprobe_r("sit");
}

void start_ipv6(void)
{
	int service = get_ipv6_service();

	// Check if turned on
	switch (service) {
	case IPV6_NATIVE_DHCP:
		nvram_set(ipv6_nvname("ipv6_prefix"), "");
		if (nvram_get_int(ipv6_nvname("ipv6_dhcp_pd"))) {
			nvram_set(ipv6_nvname("ipv6_prefix_length"), "");
			nvram_set(ipv6_nvname("ipv6_rtr_addr"), "");
		} else {
			nvram_set_int(ipv6_nvname("ipv6_prefix_length"), 64);
			add_ip6_lanaddr();
		}
		nvram_set(ipv6_nvname("ipv6_get_dns"), "");
		nvram_set(ipv6_nvname("ipv6_get_domain"), "");
		break;
	case IPV6_6IN4:
		nvram_set(ipv6_nvname("ipv6_rtr_addr"), "");
		nvram_set(ipv6_nvname("ipv6_prefix_length"), nvram_safe_get(ipv6_nvname("ipv6_prefix_length_s")));
		nvram_set(ipv6_nvname("ipv6_prefix"), nvram_safe_get(ipv6_nvname("ipv6_prefix_s")));
		/* fall through */
	case IPV6_MANUAL:
		nvram_set(ipv6_nvname("ipv6_rtr_addr"), nvram_safe_get(ipv6_nvname("ipv6_rtr_addr_s")));
		nvram_set(ipv6_nvname("ipv6_prefix_length"), nvram_safe_get(ipv6_nvname("ipv6_prefix_length_s")));
		add_ip6_lanaddr();
		break;
	case IPV6_6TO4:
	case IPV6_6RD:
		nvram_set(ipv6_nvname("ipv6_prefix"), "");
		nvram_set(ipv6_nvname("ipv6_prefix_length"), "");
		nvram_set(ipv6_nvname("ipv6_rtr_addr"), "");
		break;
	}
}

void stop_ipv6(void)
{
	char *lan_ifname = nvram_safe_get("lan_ifname");
	char *wan_ifname = (char *) get_wan6face();

	stop_dhcp6c();
	stop_ipv6_tunnel();

	eval("ip", "-6", "addr", "flush", "scope", "global", "dev", lan_ifname);
	eval("ip", "-6", "addr", "flush", "scope", "global", "dev", wan_ifname);
	eval("ip", "-6", "route", "flush", "scope", "global");
	eval("ip", "-6", "neigh", "flush", "dev", lan_ifname);
}
#endif

// -----------------------------------------------------------------------------

int no_need_to_start_wps(void)
{
	int i, j, wps_band, multiband = get_wps_multiband();
	char tmp[100], tmp2[100], prefix[] = "wlXXXXXXXXXXXXXX", prefix_mssid[] = "wlXXXXXXXXXX_mssid_";
	char word[256], *next, ifnames[128];
	int c = 0, ret = 0;

#ifdef RTCONFIG_DSL
	if (nvram_match("asus_mfg", "1")) /* Paul add 2012/12/13 */
		return 0;
#endif
	if ((nvram_get_int("sw_mode") != SW_MODE_ROUTER) &&
		(nvram_get_int("sw_mode") != SW_MODE_AP))
		return 1;

	i = 0;
	wps_band = nvram_get_int("wps_band");
	strcpy(ifnames, nvram_safe_get("wl_ifnames"));
	foreach (word, ifnames, next) {
		if (i >= MAX_NR_WL_IF)
			break;
		if (!multiband && wps_band != i) {
			++i;
			continue;
		}
		++c;
		snprintf(prefix, sizeof(prefix), "wl%d_", i);
		if ((nvram_match(strcat_r(prefix, "auth_mode_x", tmp), "open") &&
		     nvram_get_int(strcat_r(prefix, "wep_x", tmp2))) ||
		     nvram_match(strcat_r(prefix, "auth_mode_x", tmp), "shared") ||
		     strstr(nvram_safe_get(strcat_r(prefix, "auth_mode_x", tmp)), "wpa") ||
		     nvram_match(strcat_r(prefix, "auth_mode_x", tmp), "radius"))
			ret++;

#ifdef RTCONFIG_RALINK
		if (nvram_match("wl_mssid", "1"))
#endif
		for (j = 1; j < MAX_NO_MSSID; j++) {
			sprintf(prefix_mssid, "wl%d.%d_", wps_band, j);
			if (!nvram_match(strcat_r(prefix_mssid, "bss_enabled", tmp), "1"))
				continue;
			++c;
			if ((nvram_match(strcat_r(prefix, "auth_mode_x", tmp), "open") &&
			     nvram_get_int(strcat_r(prefix, "wep_x", tmp2))) ||
			     nvram_match(strcat_r(prefix_mssid, "auth_mode_x", tmp), "shared") ||
			     strstr(nvram_safe_get(strcat_r(prefix_mssid, "auth_mode_x", tmp)), "wpa") ||
			     nvram_match(strcat_r(prefix_mssid, "auth_mode_x", tmp), "radius"))
				ret++;
		}
		i++;
	}

	if (multiband && ret < c)
		ret = 0;

	return ret;
}

/* @wps_band:	if wps_band < 0 and RTCONFIG_WPSMULTIBAND is defined, check radio of all band */
int wps_band_radio_off(int wps_band)
{
	int i, c = 0, ret = 0;
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXX";
	char word[256], *next, ifnames[128];

	i = 0;
	strcpy(ifnames, nvram_safe_get("wl_ifnames"));
	foreach (word, ifnames, next) {
		if (i >= MAX_NR_WL_IF)
			break;
		if (wps_band >= 0 && wps_band != i) {
			++i;
			continue;
		}
		++c;
		snprintf(prefix, sizeof(prefix), "wl%d_", i);
		if (nvram_match(strcat_r(prefix, "radio", tmp), "0"))
			ret++;

		i++;
	}

	if (wps_band < 0 && ret < c)
		ret = 0;

	return ret;
}

/* @wps_band:	if wps_band < 0 and RTCONFIG_WPSMULTIBAND is defined, check ssid broadcast status of all band */
int wps_band_ssid_broadcast_off(int wps_band)
{
	int i, c = 0, ret = 0;
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXX";
	char word[256], *next, ifnames[128];

	i = 0;
	strcpy(ifnames, nvram_safe_get("wl_ifnames"));
	foreach (word, ifnames, next) {
		if (i >= MAX_NR_WL_IF)
			break;
		if (wps_band >= 0 && wps_band != i) {
			++i;
			continue;
		}
		++c;
		snprintf(prefix, sizeof(prefix), "wl%d_", i);
		if (nvram_match(strcat_r(prefix, "closed", tmp), "1"))
			ret++;

		i++;
	}

	if (wps_band < 0 && ret < c)
		ret = 0;

	return ret;
}

int
wl_wpsPincheck(char *pin_string)
{
	unsigned long PIN = strtoul(pin_string, NULL, 10);
	unsigned long int accum = 0;
	unsigned int len = strlen(pin_string);

	if (len != 4 && len != 8)
		return 	-1;

	if (len == 8) {
		accum += 3 * ((PIN / 10000000) % 10);
		accum += 1 * ((PIN / 1000000) % 10);
		accum += 3 * ((PIN / 100000) % 10);
		accum += 1 * ((PIN / 10000) % 10);
		accum += 3 * ((PIN / 1000) % 10);
		accum += 1 * ((PIN / 100) % 10);
		accum += 3 * ((PIN / 10) % 10);
		accum += 1 * ((PIN / 1) % 10);

		if (0 == (accum % 10))
			return 0;
	}
	else if (len == 4)
		return 0;

	return -1;
}

int
start_wps_pbc(int unit)
{
	if (wps_band_radio_off(get_radio_band(unit))) return 1;

	if (wps_band_ssid_broadcast_off(get_radio_band(unit))) return 1;

	if (!no_need_to_start_wps() && nvram_match("wps_enable", "0"))
	{
		nvram_set("wps_enable", "1");
#ifdef CONFIG_BCMWL5
#ifdef RTCONFIG_BCMWL6
		restart_wireless();
		int delay_count = 10;
		while ((delay_count-- > 0) && !nvram_get_int("wlready"))
			sleep(1);
#else
		restart_wireless();
#endif
#else
		stop_wps();
#if !defined(RTCONFIG_WPSMULTIBAND)
		nvram_set_int("wps_band", unit);
#endif
		start_wps();
#endif
	}

#if !defined(RTCONFIG_WPSMULTIBAND)
	nvram_set_int("wps_band", unit);
#endif
	nvram_set("wps_sta_pin", "00000000");

	return start_wps_method();
}

int
start_wps_pin(int unit)
{
	if (!strlen(nvram_safe_get("wps_sta_pin"))) return 0;

	if (wl_wpsPincheck(nvram_safe_get("wps_sta_pin"))) return 0;

	nvram_set_int("wps_band", unit);

	return start_wps_method();
}

#ifdef RTCONFIG_WPS
int
stop_wpsaide()
{
	if (pids("wpsaide"))
		killall("wpsaide", SIGTERM);

	return 0;
}

int
start_wpsaide()
{
	char *wpsaide_argv[] = {"wpsaide", NULL};
	pid_t pid;
	int ret = 0;

	stop_wpsaide();

	ret = _eval(wpsaide_argv, NULL, 0, &pid);
	return ret;
}
#endif

extern int restore_defaults_g;

int
start_wps(void)
{
#ifdef RTCONFIG_WPS
#ifdef CONFIG_BCMWL5
	char *wps_argv[] = {"/bin/wps_monitor", NULL};
	pid_t pid;
#endif
	if (wps_band_radio_off(get_radio_band(nvram_get_int("wps_band"))))
		return 1;

	if (no_need_to_start_wps() ||
	    wps_band_ssid_broadcast_off(get_radio_band(nvram_get_int("wps_band"))))
		nvram_set("wps_enable", "0");

	if (nvram_match("wps_restart", "1")) {
		nvram_set("wps_restart", "0");
	}
	else {
		nvram_set("wps_restart", "0");
		nvram_set("wps_proc_status", "0");
	}

	nvram_set("wps_sta_pin", "00000000");
	if (nvram_match("w_Setting", "1"))
		nvram_set("lan_wps_oob", "disabled");
	else
		nvram_set("lan_wps_oob", "enabled");
#ifdef CONFIG_BCMWL5
	killall_tk("wps_monitor");
	killall_tk("wps_ap");
	killall_tk("wps_enr");
	unlink("/tmp/wps_monitor.pid");
#endif
	if (nvram_match("wps_enable", "1"))
	{
#ifdef CONFIG_BCMWL5
		nvram_set("wl_wps_mode", "enabled");
#ifdef CONFIG_BCMWL5
		if (!restore_defaults_g)
#endif
		_eval(wps_argv, NULL, 0, &pid);
#elif defined RTCONFIG_RALINK
		start_wsc_pin_enrollee();
		if (f_exists("/var/run/watchdog.pid"))
		{
			doSystem("iwpriv %s set WatchdogPid=`cat %s`", WIF_2G, "/var/run/watchdog.pid");
#if defined(RTCONFIG_HAS_5G)
			doSystem("iwpriv %s set WatchdogPid=`cat %s`", WIF_5G, "/var/run/watchdog.pid");
#endif	/* RTCONFIG_HAS_5G */
		}
#endif
	}
#ifdef CONFIG_BCMWL5
	else
		nvram_set("wl_wps_mode", "disabled");
#endif
#else
	/* if we don't support WPS, make sure we unset any remaining wl_wps_mode */
	nvram_unset("wl_wps_mode");
#endif /* RTCONFIG_WPS */
	return 0;
}

int
stop_wps(void)
{
	int ret = 0;
#ifdef RTCONFIG_WPS
#ifdef CONFIG_BCMWL5
	killall_tk("wps_monitor");
	killall_tk("wps_ap");
#elif defined RTCONFIG_RALINK
	stop_wsc_both();
#endif
#endif /* RTCONFIG_WPS */
	return ret;
}

/* check for dual band case */
void
reset_wps(void)
{
#ifdef CONFIG_BCMWL5
//	int unit;
//	char prefix[]="wlXXXXXX_", tmp[100];

	stop_wps_method();

	stop_wps();

//	snprintf(prefix, sizeof(prefix), "wl%s_", nvram_safe_get("wps_band"));
//	nvram_set(strcat_r(prefix, "wps_config_state", tmp), "0");

	nvram_set("w_Setting", "0");

//	start_wps();
	restart_wireless_wps();
#elif defined (RTCONFIG_RALINK) || defined (RTCONFIG_QCA)
	wps_oob_both();
#endif
}

#ifdef RTCONFIG_HSPOT
void
stop_hspotap(void)
{
	killall_tk("hspotap");
}

int
start_hspotap(void)
{
	char *hs_argv[] = {"/bin/hspotap", NULL};
	pid_t pid;

	if (!check_if_file_exist("/bin/hspotap")) return 0;

	stop_hspotap();

	_eval(hs_argv, NULL, 0, &pid);

	return 0;
}
#endif

#ifdef CONFIG_BCMWL5
#ifdef BCM_ASPMD
int
start_aspmd(void)
{
        int ret = eval("/usr/sbin/aspmd");

        return ret;
}

int
stop_aspmd(void)
{
        int ret = eval("killall", "aspmd");

        return ret;
}
#endif /* BCM_ASPMD */

#if defined(BCM_EVENTD)
int start_eventd(void)
{
        int ret = 0;
        char *ssd_argv[] = {"/usr/sbin/eventd", NULL};
        pid_t pid;

        if (nvram_match("eventd_enable", "1"))
                ret = _eval(ssd_argv, NULL, 0, &pid);

        return ret;
}

int stop_eventd(void)
{
        int ret = eval("killall", "eventd");

        return ret;
}
#endif /* BCM_EVENTD */

int
start_eapd(void)
{
	int ret = 0;

	stop_eapd();

	if (!restore_defaults_g) {
#ifdef RTCONFIG_PORT_BASED_VLAN
		change_lan_ifnames();
#endif	
		ret = eval("/bin/eapd");
	}

	return ret;
}

void
stop_eapd(void)
{
	killall_tk("eapd");
}
#endif

#if defined(BCM_DCS)
int
start_dcsd(void)
{
	int ret = eval("/usr/sbin/dcsd");

	return ret;
}

void
stop_dcsd(void)
{
	killall_tk("dcsd");
}
#endif /* BCM_DCS */

#ifdef RTCONFIG_BCM_7114
void
start_dfs()
{
	int unit = 0;
	char word[256], *next;
	char tmp[128], prefix[] = "wlXXXXXXXXXX_";

	if (!nvram_match("wl1_country_code", "E0") && !nvram_match("wl1_country_code", "JP") && !nvram_match("wl1_country_code", "AU"))
		return ;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		snprintf(prefix, sizeof(prefix), "wl%d_", unit);
		if (nvram_match(strcat_r(prefix, "nband", tmp), "1"))
		{
			eval("wl", "-i", word, "down");
			eval("wl", "-i", word, "spect", "1");
			eval("wl", "-i", word, "up");
		}
		unit++;
	}
}
#endif
//2008.10 magic{
int start_networkmap(int bootwait)
{
	char *networkmap_argv[] = {"networkmap", NULL, NULL};
	pid_t pid;

	//if (!is_routing_enabled())
	//	return 0;

#ifdef CONFIG_BCMWL5
	if(ate_cond() == 0)
		return 0;
#endif

	stop_networkmap();

	if (bootwait)
		networkmap_argv[1] = "--bootwait";

#ifdef RTCONFIG_UPNPC
	start_miniupnpc();
#endif
#ifdef RTCONFIG_BONJOUR
	start_netmonitor();
#endif
	_eval(networkmap_argv, NULL, 0, &pid);

	return 0;
}

//2008.10 magic}

void stop_networkmap(void)
{
	killall_tk("networkmap");
#ifdef RTCONFIG_BONJOUR
	stop_netmonitor();
#endif
#ifdef RTCONFIG_UPNPC
	stop_miniupnpc();
#endif
}

#ifdef RTCONFIG_JFFS2USERICON
void stop_lltdc(void)
{
	if (pids("lld2c"))
		killall_tk("lld2c");
}

int start_lltdc(void)
{
	char *lld2c_argv[] = {"lld2c", "br0", NULL};
	pid_t pid;

	if (pids("lld2c"))
		return 0;

	_eval(lld2c_argv, NULL, 0, &pid);

	return 0;
}
#endif

#ifdef RTCONFIG_UPNPC
void stop_miniupnpc(void)
{
	if (pids("miniupnpc"))
		killall_tk("miniupnpc");
}

int start_miniupnpc(void)
{
	if (pids("miniupnpc"))
		return 0;

	return xstart("miniupnpc", "-m", "br0", "-t");
}
#endif

#ifdef RTCONFIG_BONJOUR
void stop_netmonitor(void)
{
	if (pids("mDNSNetMonitor"))
		killall_tk("mDNSNetMonitor");
}

int start_netmonitor(void)
{
	char *netmonitor_argv[] = {"mDNSNetMonitor", NULL};
	pid_t pid;

	if (pids("mDNSNetMonitor"))
		return 0;

	_eval(netmonitor_argv, NULL, 0, &pid);

	return 0;
}
#endif

#ifdef RTCONFIG_LLDP
int start_lldpd(void)
{
	char *lldpd_argv[] = {"lldpd", nvram_safe_get("lan_ifname"), NULL};
	pid_t pid;

	_eval(lldpd_argv, NULL, 0, &pid);

	return 0;
}
#endif
// -----------------------------------------------------------------------------
#ifdef LINUX26

static pid_t pid_hotplug2 = -1;

void start_hotplug2(void)
{
	stop_hotplug2();

	f_write_string("/proc/sys/kernel/hotplug", "", FW_NEWLINE, 0);
	xstart("hotplug2", "--persistent", "--no-coldplug");
	// FIXME: Don't remember exactly why I put "sleep" here -
	// but it was not for a race with check_services()... - TB
	sleep(1);

	if (!nvram_contains_word("debug_norestart", "hotplug2")) {
		pid_hotplug2 = -2;
	}
}

void stop_hotplug2(void)
{
	pid_hotplug2 = -1;
	killall_tk("hotplug2");
}

#endif	/* LINUX26 */


void
stop_infosvr()
{
	killall_tk("infosvr");
}

int
start_infosvr()
{
	char *infosvr_argv[] = {"/usr/sbin/infosvr", "br0", NULL};
	pid_t pid;

	return _eval(infosvr_argv, NULL, 0, &pid);
}

#ifdef RTCONFIG_RALINK
int
exec_8021x_start(int band, int is_iNIC)
{
	char tmp[100], prefix[] = "wlXXXXXXX_";
	char *str;
	int flag_8021x = 0;
	int i;

	if (nvram_get_int("sw_mode") == SW_MODE_REPEATER && nvram_get_int("wlc_band") == band)
		return 0;

	for (i = 0; i < MAX_NO_MSSID; i++)
	{
		if (i)
		{
			sprintf(prefix, "wl%d.%d_", band, i);

			if (!nvram_match(strcat_r(prefix, "bss_enabled", tmp), "1"))
				continue;
		}
		else
			sprintf(prefix, "wl%d_", band);

		str = nvram_safe_get(strcat_r(prefix, "auth_mode_x", tmp));

		if(str && strlen(str) > 0)
		{
			if (    !strcmp(str, "radius") ||
				!strcmp(str, "wpa") ||
				!strcmp(str, "wpa2") ||
				!strcmp(str, "wpawpa2") )
			{ //need daemon
				flag_8021x = 1;
				break;
			}
		}
	}

	if(flag_8021x)
	{
		if (is_iNIC)
			return xstart("rtinicapd");
		else
			return xstart("rt2860apd");
	}
	return 0;
}

int
start_8021x(void)
{
	char word[256], *next;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		if (!strcmp(word, WIF_2G))
		{
			if (!strncmp(word, "rai", 3))	// iNIC
				exec_8021x_start(0, 1);
			else
				exec_8021x_start(0, 0);
		}
#if defined(RTCONFIG_HAS_5G)
		else if (!strcmp(word, WIF_5G))
		{
			if (!strncmp(word, "rai", 3))	// iNIC
				exec_8021x_start(1, 1);
			else
				exec_8021x_start(1, 0);
		}
#endif	/* RTCONFIG_HAS_5G */
	}

	return 0;
}

int
exec_8021x_stop(int band, int is_iNIC)
{
		if (is_iNIC)
			return killall("rtinicapd", SIGTERM);
		else
			return killall("rt2860apd", SIGTERM);
}

int
stop_8021x(void)
{
	char word[256], *next;

	foreach (word, nvram_safe_get("wl_ifnames"), next) {
		if (!strcmp(word, WIF_2G))
		{
			if (!strncmp(word, "rai", 3))	// iNIC
				exec_8021x_stop(0, 1);
			else
				exec_8021x_stop(0, 0);
		}
#if defined(RTCONFIG_HAS_5G)
		else if (!strcmp(word, WIF_5G))
		{
			if (!strncmp(word, "rai", 3))	// iNIC
				exec_8021x_stop(1, 1);
			else
				exec_8021x_stop(1, 0);
		}
#endif	/* RTCONFIG_HAS_5G */
	}

	return 0;
}
#endif

void write_static_leases(char *file)
{
	FILE *fp, *fp2;
	char *nv, *nvp, *b;
	char *mac, *ip, *name;
	int vars;

	fp=fopen(file, "w");
	if (fp==NULL) return;

	fp2=fopen(dmhosts, "w");
	if (fp2==NULL) {
		fclose(fp);
		return;
	}

	nv = nvp = strdup(nvram_safe_get("dhcp_staticlist"));

	if(nv) {
	while ((b = strsep(&nvp, "<")) != NULL) {
		vars = vstrsep(b, ">", &mac, &ip, &name);
		if ((vars == 2) || (vars == 3)) {
			if(strlen(mac)==0||strlen(ip)==0) continue;
			fprintf(fp, "%s %s\n", mac, ip);
			if ((vars == 3) && (strlen(name) > 0) && (is_valid_hostname(name))) {
				fprintf(fp2, "%s %s\n", ip, name);
			}
		} else {
			continue;
		}
	}
	free(nv);
	}
	fclose(fp);
	fclose(fp2);
}

int
ddns_updated_main(int argc, char *argv[])
{
	FILE *fp;
	char buf[64], *ip;

	if (!(fp=fopen("/tmp/ddns.cache", "r"))) return 0;

	fgets(buf, sizeof(buf), fp);
	fclose(fp);

	if (!(ip=strchr(buf, ','))) return 0;

	nvram_set("ddns_cache", buf);
	nvram_set("ddns_ipaddr", ip+1);
	nvram_set("ddns_status", "1");
	nvram_set("ddns_server_x_old", nvram_safe_get("ddns_server_x"));
	nvram_set("ddns_hostname_old", nvram_safe_get("ddns_hostname_x"));
	nvram_set("ddns_updated", "1");

	logmessage("ddns", "ddns update ok");

	_dprintf("done\n");

	return 0;
}

// TODO: handle wan0 only now
int
start_ddns(void)
{
	FILE *fp;
	char *wan_ip, *wan_ifname;
//	char *ddns_cache;
	char *server;
	char *user;
	char *passwd;
	char *host;
	char *service;
	char usrstr[64];
	int wild = nvram_get_int("ddns_wildcard_x");
	int unit, asus_ddns = 0;
	char tmp[32], prefix[] = "wanXXXXXXXXXX_";

	if (!is_routing_enabled()) {
		_dprintf("return -1\n");
		return -1;
	}
	if (nvram_invmatch("ddns_enable_x", "1"))
		return -1;

	unit = wan_primary_ifunit();
	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	wan_ip = nvram_safe_get(strcat_r(prefix, "ipaddr", tmp));
	wan_ifname = get_wan_ifname(unit);

	if (!wan_ip || strcmp(wan_ip, "") == 0 || !inet_addr(wan_ip)) {
		logmessage("ddns", "WAN IP is empty.");
		return -1;
	}

#if 0 //Move the ddns check mechanism to UI
	if(!nvram_match("ddns_update_by_wdog", "1")) {
		if ((inet_addr(wan_ip) == inet_addr(nvram_safe_get("ddns_ipaddr"))) &&
		    (strcmp(nvram_safe_get("ddns_server_x"), nvram_safe_get("ddns_server_x_old")) == 0) &&
		    (strcmp(nvram_safe_get("ddns_hostname_x"), nvram_safe_get("ddns_hostname_old")) == 0) &&
		    (nvram_match("ddns_updated", "1"))) {
			nvram_set("ddns_return_code", "no_change");
			logmessage("ddns", "IP address, server and hostname have not changed since the last update.");
			_dprintf("IP address, server and hostname have not changed since the last update.");
			return -1;
		}

		// TODO : Check /tmp/ddns.cache to see current IP in DDNS
		// update when,
		// 	1. if ipaddr!= ipaddr in cache
		//
		// update
		// * nvram ddns_cache, the same with /tmp/ddns.cache

		if (	(!nvram_match("ddns_server_x_old", "") &&
			strcmp(nvram_safe_get("ddns_server_x"), nvram_safe_get("ddns_server_x_old"))) ||
			(!nvram_match("ddns_hostname_x_old", "") &&
			strcmp(nvram_safe_get("ddns_hostname_x"), nvram_safe_get("ddns_hostname_x_old")))
		) {
			logmessage("ddns", "clear ddns cache file for server/hostname change");
			unlink("/tmp/ddns.cache");
		}
		else if (!(fp = fopen("/tmp/ddns.cache", "r")) && (ddns_cache = nvram_get("ddns_cache"))) {
			if ((fp = fopen("/tmp/ddns.cache", "w+"))) {
				fprintf(fp, "%s", ddns_cache);
				fclose(fp);
			}
		}
	}
	else
		nvram_unset("ddns_update_by_wdog");
#endif

	server = nvram_safe_get("ddns_server_x");
	user = nvram_safe_get("ddns_username_x");
	passwd = nvram_safe_get("ddns_passwd_x");
	host = nvram_safe_get("ddns_hostname_x");
	unlink("/tmp/ddns.cache");

	if (strcmp(server, "WWW.DYNDNS.ORG")==0)
		service = "dyndns";
	else if (strcmp(server, "WWW.DYNDNS.ORG(CUSTOM)")==0)
		service = "dyndns-custom";
	else if (strcmp(server, "WWW.DYNDNS.ORG(STATIC)")==0)
		service = "dyndns-static";
	else if (strcmp(server, "WWW.TZO.COM")==0)
		service = "tzo";
	else if (strcmp(server, "WWW.ZONEEDIT.COM")==0)
		service = "zoneedit";
	else if (strcmp(server, "WWW.JUSTLINUX.COM")==0)
		service = "justlinux";
	else if (strcmp(server, "WWW.EASYDNS.COM")==0)
		service = "easydns";
	else if (strcmp(server, "WWW.DNSOMATIC.COM")==0)
		service = "dnsomatic";
	else if (strcmp(server, "WWW.TUNNELBROKER.NET")==0)
		service = "heipv6tb";
	else if (strcmp(server, "WWW.NO-IP.COM")==0)
		service = "noip";
	else if (strcmp(server, "WWW.NAMECHEAP.COM")==0)
		service = "namecheap";
        else if (strcmp(server, "CUSTOM")==0)
                service = "";
	else if (strcmp(server, "WWW.SELFHOST.DE") == 0)
		service = "selfhost";
	else if (strcmp(server, "WWW.ASUS.COM")==0) {
		service = "dyndns", asus_ddns = 1;
	}
	else if (strcmp(server, "WWW.ORAY.COM")==0) {
		service = "peanuthull", asus_ddns = 2;
	} else {
		logmessage("start_ddns", "Error ddns server name: %s\n", server);
		return 0;
	}

	snprintf(usrstr, sizeof(usrstr), "%s:%s", user, passwd);

	_dprintf("start_ddns update %s %s\n", server, service);

	nvram_set("ddns_return_code", "ddns_query");

	if (pids("ez-ipupdate")) {
		killall("ez-ipupdate", SIGINT);
		sleep(1);
	}
	if (pids("phddns")) {
		killall("phddns", SIGINT);
		sleep(1);
	}

	nvram_unset("ddns_cache");
	nvram_unset("ddns_ipaddr");
	nvram_unset("ddns_status");
	nvram_unset("ddns_updated");

	if (asus_ddns == 2) { //Peanuthull DDNS
		if( (fp = fopen("/etc/phddns.conf", "w")) != NULL ) {
			fprintf(fp, "[settings]\n");
			fprintf(fp, "szHost = phddns60.oray.net\n");
			fprintf(fp, "szUserID = %s\n", user);
			fprintf(fp, "szUserPWD = %s\n", passwd);
			fprintf(fp, "nicName = %s\n", wan_ifname);
			fprintf(fp, "szLog = /var/log/phddns.log\n");
			fclose(fp);

			eval("phddns", "-c", "/etc/phddns.conf", "-d");
		}
	}
	else if (asus_ddns == 1) {
		char *nserver = nvram_invmatch("ddns_serverhost_x", "") ?
			nvram_safe_get("ddns_serverhost_x") :
			"nwsrv-ns1.asus.com";
		eval("ez-ipupdate",
		     "-S", service, "-i", wan_ifname, "-h", host,
		     "-A", "2", "-s", nserver,
		     "-e", "/sbin/ddns_updated", "-b", "/tmp/ddns.cache");
	} else if (*service) {
		eval("ez-ipupdate",
		     "-S", service, "-i", wan_ifname, "-h", host,
		     "-u", usrstr, wild ? "-w" : "",
		     "-e", "/sbin/ddns_updated", "-b", "/tmp/ddns.cache");
	} else {	// Custom DDNS
		// Block until it completes and updates the DDNS update results in nvram
		run_custom_script_blocking("ddns-start", wan_ip);
		return 0;
	}

	run_custom_script("ddns-start", wan_ip);
	return 0;
}

void
stop_ddns(void)
{
	if (pids("ez-ipupdate"))
		killall("ez-ipupdate", SIGINT);
	if (pids("phddns"))
		killall("phddns", SIGINT);
}

int
asusddns_reg_domain(int reg)
{
	FILE *fp;
	char *wan_ip, *wan_ifname;
	char *ddns_cache;
	char *nserver;
	int unit;
	char tmp[32], prefix[] = "wanXXXXXXXXXX_";

	if (!is_routing_enabled()) {
		_dprintf("return -1\n");
		return -1;
	}

	if (reg) { //0:Aidisk, 1:Advanced Setting
		if (nvram_invmatch("ddns_enable_x", "1"))
			return -1;
	}

	unit = wan_primary_ifunit();
	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	wan_ip = nvram_safe_get(strcat_r(prefix, "ipaddr", tmp));
	wan_ifname = get_wan_ifname(unit);

	if (!wan_ip || strcmp(wan_ip, "") == 0 || !inet_addr(wan_ip)) {
		logmessage("asusddns", "WAN IP is empty.");
		return -1;
	}

	// TODO : Check /tmp/ddns.cache to see current IP in DDNS,
	// update when ipaddr!= ipaddr in cache.
	// nvram ddns_cache, the same with /tmp/ddns.cache

	if ((inet_addr(wan_ip) == inet_addr(nvram_safe_get("ddns_ipaddr"))) &&
		(strcmp(nvram_safe_get("ddns_server_x"), nvram_safe_get("ddns_server_x_old")) == 0) &&
		(strcmp(nvram_safe_get("ddns_hostname_x"), nvram_safe_get("ddns_hostname_old")) == 0)) {
		nvram_set("ddns_return_code", "no_change");
		logmessage("asusddns", "IP address, server and hostname have not changed since the last update.");
		return -1;
	}

	if (	(!nvram_match("ddns_server_x_old", "") &&
		strcmp(nvram_safe_get("ddns_server_x"), nvram_safe_get("ddns_server_x_old"))) ||
		(!nvram_match("ddns_hostname_x_old", "") &&
		strcmp(nvram_safe_get("ddns_hostname_x"), nvram_safe_get("ddns_hostname_x_old")))
	) {
		logmessage("asusddns", "clear ddns cache file for server/hostname change");
		unlink("/tmp/ddns.cache");
	}
	else if (!(fp = fopen("/tmp/ddns.cache", "r")) && (ddns_cache = nvram_get("ddns_cache"))) {
		if ((fp = fopen("/tmp/ddns.cache", "w+"))) {
			fprintf(fp, "%s", ddns_cache);
			fclose(fp);
		}
	}

	nvram_set("ddns_return_code", "ddns_query");

	if (pids("ez-ipupdate"))
	{
		killall("ez-ipupdate", SIGINT);
		sleep(1);
	}

	nserver = nvram_invmatch("ddns_serverhost_x", "") ?
		    nvram_safe_get("ddns_serverhost_x") :
		    "nwsrv-ns1.asus.com";

	eval("ez-ipupdate",
	     "-S", "dyndns", "-i", wan_ifname, "-h", nvram_safe_get("ddns_hostname_x"),
	     "-A", "1", "-s", nserver,
	     "-e", "/sbin/ddns_updated", "-b", "/tmp/ddns.cache");

	return 0;
}

void
stop_syslogd(void)
{
#if defined(RTCONFIG_JFFS2LOG) && (defined(RTCONFIG_JFFS2)||defined(RTCONFIG_BRCM_NAND_JFFS2))
	int running = pids("syslogd");
#endif

	killall_tk("syslogd");

#if defined(RTCONFIG_JFFS2LOG) && (defined(RTCONFIG_JFFS2)||defined(RTCONFIG_BRCM_NAND_JFFS2))
	if (running)
		eval("cp", "/tmp/syslog.log", "/tmp/syslog.log-1", "/jffs");
#endif
}

void
stop_klogd(void)
{
	killall_tk("klogd");
}

int
start_syslogd(void)
{
	int argc;
	char syslog_path[PATH_MAX];
	char syslog_addr[sizeof("255.255.255.255:65535")];
	char *syslogd_argv[] = {"/sbin/syslogd",
		"-m", "0",				/* disable marks */
		"-S",					/* small log */
//		"-D",					/* suppress dups */
		"-O", syslog_path,			/* /tmp/syslog.log or /jffs/syslog.log */
		NULL, NULL,				/* -s log_size */
		NULL, NULL,				/* -l log_level */
		NULL, NULL,				/* -R log_ipaddr[:port] */
		NULL,					/* -L log locally too */
		NULL
	};

	snprintf(syslog_path, sizeof(syslog_path), "%s", get_syslog_fname(0));
	for (argc = 0; syslogd_argv[argc]; argc++);

	if (nvram_invmatch("log_size", "")) {
		syslogd_argv[argc++] = "-s";
		syslogd_argv[argc++] = nvram_safe_get("log_size");
	}
	if (nvram_invmatch("log_level", "")) {
		syslogd_argv[argc++] = "-l";
		syslogd_argv[argc++] = nvram_safe_get("log_level");
	}
	if (nvram_invmatch("log_ipaddr", "")) {
		char *addr = nvram_safe_get("log_ipaddr");
		int port = nvram_get_int("log_port");

		if (port) {
			snprintf(syslog_addr, sizeof(syslog_addr), "%s:%d", addr, port);
			addr = syslog_addr;
		}
		syslogd_argv[argc++] = "-R";
		syslogd_argv[argc++] = addr;
		syslogd_argv[argc++] = "-L";
	}

//#if defined(RTCONFIG_JFFS2LOG) && defined(RTCONFIG_JFFS2)
#if defined(RTCONFIG_JFFS2LOG) && (defined(RTCONFIG_JFFS2)||defined(RTCONFIG_BRCM_NAND_JFFS2))
	eval("touch", "-c", "/jffs/syslog.log", "/jffs/syslog.log-1");
	eval("cp", "/jffs/syslog.log", "/jffs/syslog.log-1", "/tmp");
#endif

	// TODO: make sure is it necessary?
	//time_zone_x_mapping();
	//setenv("TZ", nvram_safe_get("time_zone_x"), 1);

	return _eval(syslogd_argv, NULL, 0, NULL);
}

int
start_klogd(void)
{
	int argc;
	char *klogd_argv[] = {"/sbin/klogd",
		NULL, NULL,				/* -c console_loglevel */
		NULL
	};

	for (argc = 0; klogd_argv[argc]; argc++);

	if (nvram_invmatch("console_loglevel", "")) {
		klogd_argv[argc++] = "-c";
		klogd_argv[argc++] = nvram_safe_get("console_loglevel");
	}

	return _eval(klogd_argv, NULL, 0, NULL);
}

int
start_logger(void)
{
_dprintf("%s:\n", __FUNCTION__);
	start_syslogd();
	start_klogd();

#if defined(DUMP_PREV_OOPS_MSG) && defined(RTCONFIG_BCMARM)
#if defined(RTAC88U) || defined(RTAC3100) || defined(RTAC5300)|| defined(RTAC5300R)
	eval("et", "dump", "oops");
#else
	eval("et", "dump_oops");
#endif
#endif

	return 0;
}

#ifdef RTCONFIG_BCMWL6
int
wl_igs_enabled(void)
{
	int i;
	char tmp[100], prefix[] = "wlXXXXXXXXXXXXXX";
	char word[256], *next, ifnames[128];

	i = 0;
	strcpy(ifnames, nvram_safe_get("wl_ifnames"));
	foreach (word, ifnames, next) {
		if (i >= MAX_NR_WL_IF)
			break;

		snprintf(prefix, sizeof(prefix), "wl%d_", i);
		if (nvram_match(strcat_r(prefix, "radio", tmp), "1") &&
		    nvram_match(strcat_r(prefix, "igs", tmp), "1"))
			return 1;

		i++;
	}

	return 0;
}

void
start_igmp_proxy(void)
{
	/* Start IGMP Proxy in Router mode only */
#if 0
	if (nvram_get_int("sw_mode") == SW_MODE_ROUTER)
		eval("igmp", nvram_get("wan_ifname"));
	else
#endif
	if (nvram_get_int("sw_mode") == SW_MODE_AP)
	{
		if (nvram_get_int("emf_enable") || wl_igs_enabled()) {
			/* Start IGMP proxy in AP mode */
			eval("igmp", nvram_get("lan_ifname"));
		}
	}
}

void
stop_igmp_proxy(void)
{
	killall_tk("igmp");
}

void
stop_acsd(void)
{
	killall_tk("acsd");
}

int
start_acsd()
{
	int ret = 0;

	if(nvram_match("nowl", "1")) return 0;

#ifdef RTCONFIG_PROXYSTA
	if (psta_exist())
		return 0;
#endif

	stop_acsd();

	if (!restore_defaults_g && strlen(nvram_safe_get("acs_ifnames")))
		ret = eval("/usr/sbin/acsd");

	return ret;
}
#endif

void
stop_misc(void)
{
	fprintf(stderr, "stop_misc()\n");

	if (pids("infosvr"))
		killall_tk("infosvr");

#ifdef RTCONFIG_SMALL_FW_UPDATE
	if (pids("watchdog02"))
		killall_tk("watchdog02");
#endif
#ifdef SW_DEVLED
	if (pids("sw_devled"))
		killall_tk("sw_devled");
#endif
	if (pids("watchdog")
#if defined(RTAC68U) || defined(RTCONFIG_FORCE_AUTO_UPGRADE)
		&& !nvram_get_int("auto_upgrade")
#endif
	)
		killall_tk("watchdog");

#ifdef RTCONFIG_FANCTRL
	if (pids("phy_tempsense"))
		killall_tk("phy_tempsense");
#endif
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_PROXYSTA
	if (pids("psta_monitor"))
		killall_tk("psta_monitor");
#endif
#ifdef RTCONFIG_IPERF
	if (pids("monitor"))
		killall_tk("monitor");
#endif
#endif
#ifdef RTCONFIG_QTN
	if (pids("qtn_monitor"))
		killall_tk("qtn_monitor");
#endif
	if (pids("ntp"))
		killall_tk("ntp");
	if (pids("ntpclient"))
		killall_tk("ntpclient");

#ifdef RTCONFIG_BCMWL6
#ifdef BCM_ASPMD
	stop_aspmd();
#endif
	stop_acsd();
#ifdef BCM_BSD
	stop_bsd();
#endif
#ifdef BCM_SSD
	stop_ssd();
#endif
#if defined(RTCONFIG_DHDAP)
	stop_dhd_monitor();
#endif
	stop_igmp_proxy();
#ifdef RTCONFIG_HSPOT
	stop_hspotap();
#endif
#endif
	stop_wps();
#ifdef RTCONFIG_BCMWL6
#endif
	stop_upnp();
	stop_lltd();
	stop_rstats();
	stop_cstats();
#ifdef RTCONFIG_DSL
	stop_spectrum(); //Ren
#endif //For DSL-N55U
	stop_networkmap();
}

void
stop_misc_no_watchdog(void)
{
	_dprintf("done\n");
}

int
chpass(char *user, char *pass)
{
//	FILE *fp;

	if (!user)
		user = "admin";

	if (!pass)
		pass = "admin";
/*
	if ((fp = fopen("/etc/passwd", "a")))
	{
		fprintf(fp, "%s::0:0::/:\n", user);
		fclose(fp);
	}

	if ((fp = fopen("/etc/group", "a")))
	{
		fprintf(fp, "%s:x:0:%s\n", user, user);
		fclose(fp);
	}
*/
	eval("chpasswd.sh", user, pass);
	return 0;
}

void
set_hostname(void)
{
	FILE *fp;
	char hostname[32];

	strncpy(hostname, nvram_safe_get("computer_name"), 31);

	if (*hostname == 0) {
		if (get_productid()) {
			strncpy(hostname, get_productid(), 31);
		}
	}

	if ((fp=fopen("/proc/sys/kernel/hostname", "w+"))) {
		fputs(hostname, fp);
		fclose(fp);
	}
}

int
start_telnetd(void)
{
	if (getpid() != 1) {
		notify_rc("start_telnetd");
		return 0;
	}

	if (!nvram_match("telnetd_enable", "1"))
		return 0;

	if (pids("telnetd"))
		killall_tk("telnetd");

	set_hostname();
	setup_passwd();
	//chpass(nvram_safe_get("http_username"), nvram_safe_get("http_passwd"));	// vsftpd also needs

	return xstart("telnetd");
}

void
stop_telnetd(void)
{
	if (getpid() != 1) {
		notify_rc("stop_telnetd");
		return;
	}

	if (pids("telnetd"))
		killall_tk("telnetd");
}

int
run_telnetd(void)
{
	if (pids("telnetd"))
		killall_tk("telnetd");

	set_hostname();
	setup_passwd();
	//chpass(nvram_safe_get("http_username"), nvram_safe_get("http_passwd"));

	return xstart("telnetd");
}

void
start_httpd(void)
{
	char *httpd_argv[] = {"httpd", NULL};
#ifdef RTCONFIG_HTTPS
	char *https_argv[] = {"httpds", "-s", "-p", nvram_safe_get("https_lanport"), NULL};
#endif
	char *cur_dir;
	pid_t pid;
	int enable;
#ifdef DEBUG_RCTEST
	char *httpd_dir;
#endif

	if (getpid() != 1) {
		notify_rc("start_httpd");
		return;
	}

	cur_dir = getcwd(NULL, 0);
#ifdef DEBUG_RCTEST // Left for UI debug
	httpd_dir = nvram_safe_get("httpd_dir");
	if(strlen(httpd_dir)) chdir(httpd_dir);
	else
#endif
	chdir("/www");

	enable = nvram_get_int("http_enable");
	if (enable != 1) {
		logmessage(LOGNAME, "start httpd");
		_eval(httpd_argv, NULL, 0, &pid);
 	}
#ifdef RTCONFIG_HTTPS
	if (enable != 0) {
		logmessage(LOGNAME, "start httpd - SSL");
		_eval(https_argv, NULL, 0, &pid);
	}
#endif

	chdir(cur_dir ? : "/");
	free(cur_dir);
}

void
stop_httpd(void)
{
	if (getpid() != 1) {
		notify_rc("stop_httpd");
		return;
	}

	if (pids("httpd"))
		killall_tk("httpd");
#ifdef RTCONFIG_HTTPS
	if (pids("httpds"))
		killall_tk("httpds");
#endif
}

//////////vvvvvvvvvvvvvvvvvvvvvjerry5 2009.07
void
stop_rstats(void)
{
	if (pids("rstats"))
		killall_tk("rstats");
}

void
start_rstats(int new)
{
	if (!is_routing_enabled()) return;

	stop_rstats();
	if (new) xstart("rstats", "--new");
	else xstart("rstats");
}

void
restart_rstats()
{
	if (nvram_match("rstats_new", "1"))
	{
		start_rstats(1);
		nvram_set("rstats_new", "0");
		nvram_commit();		// Otherwise it doesn't get written back to mtd
	}
	else
	{
		start_rstats(0);
	}
}
////////^^^^^^^^^^^^^^^^^^^jerry5 2009.07

//Ren.B
#ifdef RTCONFIG_DSL
void stop_spectrum(void)
{
	if (getpid() != 1) {
		notify_rc("stop_spectrum");
		return;
	}

	if (pids("spectrum"))
		killall_tk("spectrum");
}

void start_spectrum(void)
{
	if (getpid() != 1) {
		notify_rc("start_spectrum");
		return;
	}

	stop_spectrum();
	xstart("spectrum");
}
#endif
//Ren.E

// TODO: so far, support wan0 only

void start_upnp(void)
{
	FILE *f;
	char tmp[32], prefix[sizeof("wanXXXXXXXXXX_")];
	char et0macaddr[18];
	char *proto, *port, *lport, *dstip, *desc;
	char *nv, *nvp, *b;
	int upnp_enable, upnp_mnp_enable, upnp_port;
	int unit, i, httpx_port;
#if defined(RTCONFIG_APP_PREINSTALLED) || defined(RTCONFIG_APP_NETINSTALLED) || defined(RTCONFIG_APP_NOLOCALDM)
	FILE *ifp = NULL;
	char tmpstr[80];
	int statDownloadMaster = 0;
#endif
	int min_lifetime, max_lifetime;

	if (getpid() != 1) {
		notify_rc("start_upnp");
		return;
	}

	if (!is_routing_enabled())
		return;

	unit = wan_primary_ifunit();
	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	upnp_enable = nvram_get_int("upnp_enable");
	upnp_mnp_enable = nvram_get_int("upnp_mnp");

	if (nvram_get_int(strcat_r(prefix, "upnp_enable", tmp))) {
		mkdir("/etc/upnp", 0777);
		if (f_exists("/etc/upnp/config.alt")) {
			xstart("miniupnpd", "-f", "/etc/upnp/config.alt");
		} else {
			if ((f = fopen("/etc/upnp/config", "w")) != NULL) {
				char *lanip = nvram_safe_get("lan_ipaddr");
				char *lanmask = nvram_safe_get("lan_netmask");

				upnp_port = nvram_get_int("upnp_port");
				if (upnp_port < 0 || upnp_port > 65535)
					upnp_port = 0;

#if defined(RTCONFIG_RGMII_BRCM5301X) || defined(RTCONFIG_QCA)
				strcpy(et0macaddr, nvram_safe_get("lan_hwaddr"));
#elif defined(RTCONFIG_GMAC3)
				if (nvram_match("gmac3_enable", "1"))
					strcpy(et0macaddr, nvram_safe_get("et2macaddr"));
				else
					strcpy(et0macaddr, nvram_safe_get("et0macaddr"));
#else
				strcpy(et0macaddr, nvram_safe_get("et0macaddr"));
#endif
				if (strlen(et0macaddr))
					for (i = 0; i < strlen(et0macaddr); i++)
						et0macaddr[i] = tolower(et0macaddr[i]);;

				fprintf(f,
					"ext_ifname=%s\n"
					"listening_ip=%s\n"
					"port=%d\n"
					"enable_upnp=%s\n"
					"enable_natpmp=%s\n"
					"secure_mode=%s\n"
					"upnp_nat_postrouting_chain=PUPNP\n"
					"upnp_forward_chain=FUPNP\n"
					"upnp_nat_chain=VUPNP\n"
					"notify_interval=%d\n"
					"system_uptime=yes\n"
					"friendly_name=%s\n"
					"model_number=%s\n"
					"serial=%s\n",
					get_wan_ifname(wan_primary_ifunit()),
					nvram_safe_get("lan_ifname"),
					upnp_port,
					upnp_enable ? "yes" : "no",	// upnp enable
					upnp_mnp_enable ? "yes" : "no",	// natpmp enable
					nvram_get_int("upnp_secure") ? "yes" : "no",	// secure_mode (only forward to self)
					nvram_get_int("upnp_ssdp_interval"),
					get_productid(),
					rt_serialno,
					nvram_get("serial_no") ? : et0macaddr);

				if (nvram_get_int("upnp_clean")) {
					int interval = nvram_get_int("upnp_clean_interval");
					if (interval < 60)
						interval = 60;
					fprintf(f,
						"clean_ruleset_interval=%d\n"
						"clean_ruleset_threshold=%d\n",
						interval,
						nvram_get_int("upnp_clean_threshold"));
				} else
					fprintf(f,"clean_ruleset_interval=%d\n", 0);


				if (nvram_match("upnp_mnp", "1")) {
					int https = nvram_get_int("http_enable");
					int https_port = nvram_get_int("https_lanport");
					sprintf(tmpstr, ":%d", https_port);

					fprintf(f, "presentation_url=http%s://%s%s\n",
						(https > 0) ? "s" : "", lanip,
						(https > 0) ? tmpstr : "");
				} else {
					// Empty parameters are not included into XML service description
					fprintf(f, "presentation_url=\n");
				}


				char uuid[45];
				f_read_string("/proc/sys/kernel/random/uuid", uuid, sizeof(uuid));
				fprintf(f, "uuid=%s\n", uuid);

				if (is_nat_enabled() && nvram_match("vts_enable_x", "1")) {
					nvp = nv = strdup(nvram_safe_get("vts_rulelist"));
					while (nv && (b = strsep(&nvp, "<")) != NULL) {
						char *portv, *portp, *c;

						if ((vstrsep(b, ">", &desc, &port, &dstip, &lport, &proto) != 5))
							continue;

						// Handle port1,port2,port3 format
						portp = portv = strdup(port);
						while (portv && (c = strsep(&portp, ",")) != NULL) {
							if (strcmp(proto, "TCP") == 0 || strcmp(proto, "BOTH") == 0) {
								fprintf(f, "deny %s 0.0.0.0/0 0-65535\n", c);

								int local_ftpport = nvram_get_int("vts_ftpport");
								if (!strcmp(c, "21") && local_ftpport != 0 && local_ftpport != 21)
									fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", local_ftpport);
							}
							if (strcmp(proto, "UDP") == 0 || strcmp(proto, "BOTH") == 0)
								fprintf(f, "deny %s 0.0.0.0/0 0-65535\n", c);
						}
						free(portv);
					}
					free(nv);
				}

				if (nvram_match("misc_http_x", "1"))
				{
					httpx_port = nvram_get_int("misc_httpport_x") ? : 8080;
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", httpx_port);
#ifdef RTCONFIG_HTTPS
					if (nvram_get_int("http_enable")) {
						httpx_port = nvram_get_int("misc_httpsport_x") ? : 8443;
						fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", httpx_port);
					}
#endif
				}

#ifdef RTCONFIG_WEBDAV
				if (nvram_match("enable_webdav", "1") && nvram_match("webdav_aidisk", "1")) {
					httpx_port = nvram_get_int("webdav_https_port");
					if (!httpx_port || httpx_port > 65535)
						httpx_port = 443;
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", httpx_port);

					httpx_port = nvram_get_int("webdav_http_port");
					if (!httpx_port || httpx_port > 65535)
						httpx_port = 8082;
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", httpx_port);
				}
#endif

#if defined(RTCONFIG_APP_PREINSTALLED) || defined(RTCONFIG_APP_NETINSTALLED) || defined(RTCONFIG_APP_NOLOCALDM)
				ifp = fopen("/opt/lib/ipkg/status", "r");
				if (ifp) {
					while (fgets(tmpstr, 80, ifp)) {
						if (strstr(tmpstr, "downloadmaster")) {
							statDownloadMaster = 1; //installed
							break;
						}
					}
					fclose(ifp);
				}

				if (statDownloadMaster == 1) {
					ifp = fopen("/opt/lib/ipkg/info/downloadmaster.control", "r");
					if (ifp) {
						while (fgets(tmpstr, 80, ifp)) {
							if (strstr(tmpstr, "Enabled") && strstr(tmpstr, "yes")) {
								statDownloadMaster = 2; //installed and enabled
								break;
							}
						}
						fclose(ifp);
					}
				}

				if (statDownloadMaster == 2) {
					// Transmisson
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", nvram_get_int("trs_peer_port"));
					// amuled
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", 4662);
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", 4665);
					fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", 4672);
					// lighttpd
					if (nvram_match("gen_http_x", "1"))
						fprintf(f, "deny %d 0.0.0.0/0 0-65535\n", nvram_get_int("dm_http_port"));
				}
#endif

				int ports[4];
				if ((ports[0] = nvram_get_int("upnp_min_port_int")) > 0 &&
				    (ports[1] = nvram_get_int("upnp_max_port_int")) > 0 &&
				    (ports[2] = nvram_get_int("upnp_min_port_ext")) > 0 &&
				    (ports[3] = nvram_get_int("upnp_max_port_ext")) > 0) {
					fprintf(f,
						"allow %d-%d %s/%s %d-%d\n",
						ports[0], ports[1],
						lanip, lanmask,
						ports[2], ports[3]
					);
				}
				else {
					// by default allow only redirection of ports above 1024
					fprintf(f, "allow 1024-65535 %s/%s 1024-65535\n", lanip, lanmask);
				}

				/* For PCP */
				min_lifetime = nvram_get_int("upnp_min_lifetime");
				max_lifetime = nvram_get_int("upnp_max_lifetime");

				fprintf(f, "min_lifetime=%d\n"
					   "max_lifetime=%d\n",
					   (min_lifetime > 0 ? min_lifetime : 120),
					   (max_lifetime > 0 ? max_lifetime : 86400));

				fprintf(f, "\ndeny 0-65535 0.0.0.0/0 0-65535\n"
				           "lease_file=/var/lib/misc/upnp.leases\n");

				fappend(f, "/etc/upnp/config.custom");
				append_custom_config("upnp", f);

				fclose(f);
				use_custom_config("upnp", "/etc/upnp/config");
				run_postconf("upnp", "/etc/upnp/config");
				xstart("miniupnpd", "-f", "/etc/upnp/config");
			}
		}
	}
}

void stop_upnp(void)
{
	if (getpid() != 1) {
		notify_rc("stop_upnp");
		return;
	}

	killall_tk("miniupnpd");
}

int
start_ntpc(void)
{
	char *ntp_argv[] = {"ntp", NULL};
	int pid;

	if (pids("ntpclient"))
		killall_tk("ntpclient");

	if (!pids("ntp"))
		_eval(ntp_argv, NULL, 0, &pid);

	return 0;
}

void
stop_ntpc(void)
{
	if (pids("ntpclient"))
		killall_tk("ntpclient");
}


void refresh_ntpc(void)
{
	setup_timezone();

	if (pids("ntpclient"))
		killall_tk("ntpclient");

	if (!pids("ntp"))
	{
		stop_ntpc();
		start_ntpc();
	}
	else
		kill_pidfile_s("/var/run/ntp.pid", SIGALRM);
}

int start_lltd(void)
{
	chdir("/usr/sbin");

#ifdef CONFIG_BCMWL5
	char *odmpid = nvram_safe_get("odmpid");
	int model = get_model();

	if (strlen(odmpid) && is_valid_hostname(odmpid))
	{
		switch (model) {
		case MODEL_RTN66U:
			eval("lld2d.rtn66r", "br0");
			break;
		case MODEL_RTAC66U:
			if (!strcmp(odmpid, "RT-AC66R"))
				eval("lld2d.rtac66r", "br0");
			else if (!strcmp(odmpid, "RT-AC66W"))
				eval("lld2d.rtac66w", "br0");
			else if (!strcmp(odmpid, "RT-AC1750"))
				eval("lld2d.rtac1750", "br0");
			break;
		case MODEL_RTAC68U:
			if (!strcmp(odmpid, "RT-AC68P"))
				eval("lld2d.rtac68p", "br0");
			else if (!strcmp(odmpid, "RT-AC68R"))
				eval("lld2d.rtac68r", "br0");
			else if (!strcmp(odmpid, "RT-AC68W"))
				eval("lld2d.rtac68w", "br0");
			else if (!strcmp(odmpid, "RT-AC68RW"))
				eval("lld2d.rtac68rw", "br0");
			else if (!strcmp(odmpid, "RT-AC1900"))
				eval("lld2d.rtac1900", "br0");
			else if (!strcmp(odmpid, "RT-AC1900P"))
				eval("lld2d.rtac1900p", "br0");
			else if (!strcmp(odmpid, "RT-AC66U V2"))
				eval("lld2d.rtac66u_v2", "br0");
			break;
		case MODEL_DSLAC68U:
			if (!strcmp(odmpid, "DSL-AC68R"))
				eval("lld2d.dslac68r", "br0");
			else
				eval("lld2d", "br0");
			break;
		default:
			eval("lld2d", "br0");
			break;
		}
	}
	else
#endif
		eval("lld2d", "br0");

	chdir("/");

	return 0;
}

void stop_lltd(void)
{
#ifdef CONFIG_BCMWL5
	char *odmpid = nvram_safe_get("odmpid");
	int model = get_model();
	if (strlen(odmpid) && is_valid_hostname(odmpid))
	{
		switch (model) {
		case MODEL_RTN66U:
			killall_tk("lld2d.rtn66r");
			break;
		case MODEL_RTAC66U:
			killall_tk("lld2d.rtac66r");
			break;
		case MODEL_RTAC68U:
			if (!strcmp(odmpid, "RT-AC68P"))
				killall_tk("lld2d.rtac68p");
			else if (!strcmp(odmpid, "RT-AC68R"))
				killall_tk("lld2d.rtac68r");
			else if (!strcmp(odmpid, "RT-AC68W"))
				killall_tk("lld2d.rtac68w");
			else if (!strcmp(odmpid, "RT-AC68RW"))
				killall_tk("lld2d.rtac68rw");
			else if (!strcmp(odmpid, "RT-AC1900"))
				killall_tk("lld2d.rtac1900");
			else if (!strcmp(odmpid, "RT-AC1900P"))
				killall_tk("lld2d.rtac1900p");
			else if (!strcmp(odmpid, "RT-AC66U V2"))
                                killall_tk("lld2d.rtac66u_v2");
			break;
		case MODEL_DSLAC68U:
			if (!strcmp(odmpid, "DSL-AC68R"))
				killall_tk("lld2d.dslac68r");
			else
				killall_tk("lld2d");
			break;
		default:
			killall_tk("lld2d");
			break;
		}
	}
	else
#endif
	killall_tk("lld2d");
}

#if defined(RTCONFIG_MDNS)

#define AVAHI_CONFIG_PATH	"/tmp/avahi"
#define AVAHI_SERVICES_PATH	"/tmp/avahi/services"
#define AVAHI_CONFIG_FN		"avahi-daemon.conf"
#define AVAHI_AFPD_SERVICE_FN	"afpd.service"
#define AVAHI_ADISK_SERVICE_FN	"adisk.service"
#define AVAHI_ITUNE_SERVICE_FN  "mt-daap.service"
#define TIMEMACHINE_BACKUP_NAME	"Backups.backupdb"

int generate_mdns_config(void)
{
	FILE *fp;
	char avahi_config[80];
	char et0macaddr[18];
	int ret = 0;

	sprintf(avahi_config, "%s/%s", AVAHI_CONFIG_PATH, AVAHI_CONFIG_FN);

#if defined(RTCONFIG_RGMII_BRCM5301X) || defined(RTCONFIG_QCA)
	strcpy(et0macaddr, nvram_safe_get("lan_hwaddr"));
#elif defined(RTCONFIG_GMAC3)
	if (nvram_match("gmac3_enable", "1"))
		strcpy(et0macaddr, nvram_safe_get("et2macaddr"));
	else
		strcpy(et0macaddr, nvram_safe_get("et0macaddr"));
#else
	strcpy(et0macaddr, nvram_safe_get("et0macaddr"));
#endif

	/* Generate avahi configuration file */
	if (!(fp = fopen(avahi_config, "w"))) {
		perror(avahi_config);
		return -1;
	}

	/* Set [server] configuration */
	fprintf(fp, "[Server]\n");
	fprintf(fp, "host-name=%s-%c%c%c%c\n", get_productid(),et0macaddr[12],et0macaddr[13],et0macaddr[15],et0macaddr[16]);
#ifdef RTCONFIG_FINDASUS
	fprintf(fp, "aliases=findasus,%s\n",get_productid());
	fprintf(fp, "aliases_llmnr=findasus,%s\n",get_productid());
#else
	fprintf(fp, "aliases=%s\n",get_productid());
	fprintf(fp, "aliases_llmnr=%s\n",get_productid());
#endif
	fprintf(fp, "use-ipv4=yes\n");
	fprintf(fp, "use-ipv6=no\n");
	fprintf(fp, "deny-interfaces=%s\n", nvram_safe_get("wan0_ifname"));
	fprintf(fp, "ratelimit-interval-usec=1000000\n");
	fprintf(fp, "ratelimit-burst=1000\n");

	/* Set [publish] configuration */
	fprintf(fp, "\n[publish]\n");
	fprintf(fp, "publish-a-on-ipv6=no\n");
	fprintf(fp, "publish-aaaa-on-ipv4=no\n");

	/* Set [wide-area] configuration */
	fprintf(fp, "\n[wide-area]\n");
	fprintf(fp, "enable-wide-area=yes\n");

	/* Set [rlimits] configuration */
	fprintf(fp, "\n[rlimits]\n");
	fprintf(fp, "rlimit-core=0\n");
	fprintf(fp, "rlimit-data=4194304\n");
	fprintf(fp, "rlimit-fsize=0\n");
	fprintf(fp, "rlimit-nofile=768\n");
	fprintf(fp, "rlimit-stack=4194304\n");
	fprintf(fp, "rlimit-nproc=3\n");

	append_custom_config(AVAHI_CONFIG_FN, fp);
	fclose(fp);
	use_custom_config(AVAHI_CONFIG_FN, avahi_config);
	run_postconf("avahi-daemon", avahi_config);

	return ret;
}

int generate_afpd_service_config(void)
{
	FILE *fp;
	char afpd_service_config[80];
	int ret = 0;

	sprintf(afpd_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_AFPD_SERVICE_FN);

	/* Generate afpd service configuration file */
	if (!(fp = fopen(afpd_service_config, "w"))) {
		perror(afpd_service_config);
		return -1;
	}

	fprintf(fp, "<service-group>\n");
	fprintf(fp, "<name replace-wildcards=\"yes\">%%h</name>\n");
	fprintf(fp, "<service>\n");
	fprintf(fp, "<type>_afpovertcp._tcp</type>\n");
	fprintf(fp, "<port>548</port>\n");
	fprintf(fp, "</service>\n");
	fprintf(fp, "<service>\n");
	fprintf(fp, "<type>_device-info._tcp</type>\n");
	fprintf(fp, "<port>0</port>\n");
	fprintf(fp, "<txt-record>model=Xserve</txt-record>\n");
	fprintf(fp, "</service>\n");
	fprintf(fp, "</service-group>\n");

	append_custom_config(AVAHI_AFPD_SERVICE_FN, fp);
	fclose(fp);
	use_custom_config(AVAHI_AFPD_SERVICE_FN, afpd_service_config);
	run_postconf("afpd", afpd_service_config);

	return ret;
}

int generate_adisk_service_config(void)
{
	FILE *fp;
	char adisk_service_config[80];
	int ret = 0;

	sprintf(adisk_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_ADISK_SERVICE_FN);

	/* Generate adisk service configuration file */
	if (!(fp = fopen(adisk_service_config, "w"))) {
		perror(adisk_service_config);
		return -1;
	}

	fprintf(fp, "<service-group>\n");
	fprintf(fp, "<name replace-wildcards=\"yes\">%%h</name>\n");
	fprintf(fp, "<service>\n");
	fprintf(fp, "<type>_adisk._tcp</type>\n");
	fprintf(fp, "<port>9</port>\n");
	fprintf(fp, "<txt-record>dk0=adVN=%s,adVF=0x81</txt-record>\n", TIMEMACHINE_BACKUP_NAME);
	fprintf(fp, "</service>\n");
	fprintf(fp, "</service-group>\n");

	append_custom_config(AVAHI_ADISK_SERVICE_FN, fp);
	fclose(fp);
	use_custom_config(AVAHI_ADISK_SERVICE_FN, adisk_service_config);
	run_postconf("adisk", adisk_service_config);

	return ret;
}

int generate_itune_service_config(void)
{
	FILE *fp;
	char itune_service_config[80];
	int ret = 0;
	char servername[32];

	sprintf(itune_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_ITUNE_SERVICE_FN);

	/* Generate afpd service configuration file */
	if (!(fp = fopen(itune_service_config, "w"))) {
		perror(itune_service_config);
		return -1;
	}

	if (is_valid_hostname(nvram_safe_get("daapd_friendly_name")))
		strncpy(servername, nvram_safe_get("daapd_friendly_name"), sizeof(servername));
	else
		servername[0] = '\0';
	if(strlen(servername)==0) strncpy(servername, get_productid(), sizeof(servername));


	fprintf(fp, "<service-group>\n");
	fprintf(fp, "<name replace-wildcards=\"yes\">%s</name>\n",servername);
	fprintf(fp, "<service>\n");
	fprintf(fp, "<type>_daap._tcp</type>\n");
	fprintf(fp, "<port>3689</port>\n");
	fprintf(fp, "<txt-record>txtvers=1 iTShVersion=131073 Version=196610</txt-record>\n");
	fprintf(fp, "</service>\n");
	fprintf(fp, "</service-group>\n");

	append_custom_config(AVAHI_ITUNE_SERVICE_FN, fp);
	fclose(fp);
	use_custom_config(AVAHI_ITUNE_SERVICE_FN, itune_service_config);
	run_postconf("mt-daap", itune_service_config);

	return ret;
}

int start_mdns(void)
{
	char afpd_service_config[80];
	char adisk_service_config[80];
	char itune_service_config[80];

	sprintf(afpd_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_AFPD_SERVICE_FN);
	sprintf(adisk_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_ADISK_SERVICE_FN);
	sprintf(itune_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_ITUNE_SERVICE_FN);

	mkdir_if_none(AVAHI_CONFIG_PATH);
	mkdir_if_none(AVAHI_SERVICES_PATH);

	generate_mdns_config();

	if (pids("afpd") && nvram_match("timemachine_enable", "1"))
	{
		if (!f_exists(afpd_service_config))
			generate_afpd_service_config();
		if (!f_exists(adisk_service_config))
			generate_adisk_service_config();
	}else{
		if (f_exists(afpd_service_config)){
			unlink(afpd_service_config);
		}
		if (f_exists(adisk_service_config)){
			unlink(adisk_service_config);
		}
	}

	if(nvram_match("daapd_enable", "1") && pids("mt-daapd")){
		if (!f_exists(itune_service_config)){
			generate_itune_service_config();
		}
	}else{
		if (f_exists(itune_service_config)){
			unlink(itune_service_config);
		}
	}

	// Execute avahi_daemon daemon
	//xstart("avahi-daemon");
	char *avahi_daemon_argv[] = {"avahi-daemon", NULL};
	pid_t pid;

	return _eval(avahi_daemon_argv, NULL, 0, &pid);
}

void stop_mdns(void)
{
	if (pids("avahi-daemon"))
		killall("avahi-daemon", SIGTERM);
}

void restart_mdns(void)
{
	char afpd_service_config[80];
	char itune_service_config[80];
	sprintf(afpd_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_AFPD_SERVICE_FN);
	sprintf(itune_service_config, "%s/%s", AVAHI_SERVICES_PATH, AVAHI_ITUNE_SERVICE_FN);

	if (nvram_match("timemachine_enable", "1") == f_exists(afpd_service_config)){
		if(nvram_match("daapd_enable", "1") == f_exists(itune_service_config)){
			unlink(itune_service_config);
			generate_itune_service_config();
			return;
		}
	}

	stop_mdns();
	sleep(2);
	start_mdns();
}

#endif

#ifdef  __CONFIG_NORTON__

int start_norton(void)
{
	eval("/opt/nga/init/bootstrap.sh", "start", "rc");

	return 0;
}

int stop_norton(void)
{
	int ret;

	ret = eval("/opt/nga/init/bootstrap.sh", "stop", "rc");

	return ret;
}

#endif /* __CONFIG_NORTON__ */


#ifdef RTCONFIG_IXIAEP
int
stop_ixia_endpoint(void)
{
	if (pids("endpoint"))
		killall_tk("endpoint");
	return 0;
}

int
start_ixia_endpoint(void)
{
	eval("start_endpoint");
}
#endif

#ifdef RTCONFIG_IPERF
int
stop_iperf(void)
{
	if (pids("iperf"))
		killall_tk("iperf");
	return 0;
}

int
start_iperf(void)
{
	char *iperf_argv[] = {"iperf", "-s", "-w", "1024k", NULL};
	pid_t pid;

	return _eval(iperf_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_QCA_PLC_UTILS
int
stop_plchost(void)
{
	if (pids("plchost"))
		killall_tk("plchost");
	return 0;
}

int
start_plchost(void)
{
	char *plchost_argv[] = {"/usr/local/bin/plchost", "-i", "br0", "-N", BOOT_NVM_PATH, "-P", BOOT_PIB_PATH, NULL};
	pid_t pid;

	return _eval(plchost_argv, NULL, 0, &pid);
}

void
reset_plc(void)
{
	FILE *fp;
	int rlen;
	char buf[1024], plc_mac[18];

#if defined(PLN12)
	if (!get_qca8337_PHY_power(1))
		doSystem("swconfig dev %s port 1 set power 1", MII_IFNAME);
#elif defined(PLAC56)
	int wake_gpio = nvram_get_int("plc_wake_gpio") & 0xff;

	if (get_gpio(wake_gpio))
		set_gpio(wake_gpio, 0);
#endif

	memset(plc_mac, 0, sizeof(plc_mac));
	strcpy(plc_mac, nvram_safe_get("plc_macaddr"));

	while (1) {
		fp = popen("/usr/local/bin/plctool -i br0 -I -e", "r");
		rlen = fread(buf, 1, sizeof(buf), fp);
		pclose(fp);
		if (rlen > 1) {
			buf[rlen-1] = '\0';
			if (strstr(buf, plc_mac))
				break;
		}
		dbg("%s: wait Powerline wake up...\n", __func__);
		sleep(1);
	}

	stop_plchost();
	eval("/usr/local/bin/plctool", "-i", "br0", "-R");
}
#endif

#ifdef RTCONFIG_DHCP_OVERRIDE
int
stop_detectWAN_arp(void)
{
	if (pids("detectWAN_arp"))
		killall_tk("detectWAN_arp");
	return 0;
}

int
start_detectWAN_arp(void)
{
	char *detectWAN_arp_argv[] = {"detectWAN_arp", NULL};
	pid_t pid;

	return _eval(detectWAN_arp_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_NOTIFICATION_CENTER
int
start_notification_center(void)
{
	char *nt_monitor_argv[] = {"nt_monitor", NULL};
	pid_t pid;

	return _eval(nt_monitor_argv, NULL, 0, &pid);
}

int
stop_notification_center(void)
{
	eval("kill", "-SIGUSR2", "`cat /tmp/Notification_Center_Monitor.pid`");
	return 0;
}
#endif

int
start_services(void)
{
#ifdef __CONFIG_NORTON__
	start_norton();
#endif /* __CONFIG_NORTON__ */

#ifdef RTCONFIG_NOTIFICATION_CENTER
	start_notification_center();
#endif
	start_telnetd();
#ifdef RTCONFIG_SSH
	start_sshd();
#endif
#ifdef CONFIG_BCMWL5
	start_eapd();
	start_nas();
#elif defined RTCONFIG_RALINK
	start_8021x();
#endif
	start_wps();
#ifdef RTCONFIG_WPS
	start_wpsaide();
#endif
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
#if defined(BCM_EVENTD)
	start_eventd();
#endif

#if defined(RTCONFIG_DHDAP)
	start_dhd_monitor();
#endif
	start_acsd();
#ifdef BCM_ASPMD
	start_aspmd();
#endif /* BCM_ASPMD */
#endif	// RTCONFIG_BCMWL6
	start_dnsmasq();
#ifdef RTCONFIG_DHCP_OVERRIDE
	start_detectWAN_arp();
#endif
#if defined(RTCONFIG_MDNS)
	start_mdns();
#endif
	/* Link-up LAN ports after DHCP server ready. */
	start_lan_port(0);

	start_cifs();
	start_httpd();
#ifdef RTCONFIG_CROND
	start_cron();
#endif
	start_infosvr();

	restart_rstats();
	restart_cstats();
#ifdef RTCONFIG_DSL
	start_spectrum(); //Ren
#endif
#ifdef RTCONFIG_TRAFFIC_LIMITER
	init_traffic_limiter();
#endif
	start_watchdog();
#ifdef RTAC87U
	start_watchdog02();
#endif
#ifdef SW_DEVLED
	start_sw_devled();
#endif
#ifdef RTCONFIG_DUALWAN
	restart_dualwan();
#endif
#ifdef RTCONFIG_FANCTRL
	start_phy_tempsense();
#endif
#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
	start_psta_monitor();
#endif
#if 0
	start_lldpd();
#else
	start_lltd();
#endif
#ifdef RTCONFIG_TOAD
	start_toads();
#endif

#ifdef RTCONFIG_JFFS2USERICON
	start_lltdc();
#endif
	start_networkmap(1);

#if defined(RTCONFIG_PPTPD) || defined(RTCONFIG_ACCEL_PPTPD)
	start_pptpd();
#endif

#ifdef RTCONFIG_USB
//	_dprintf("restart_nas_services(%d): test 8.\n", getpid());
	//restart_nas_services(0, 1);
#ifdef RTCONFIG_DISK_MONITOR
	start_diskmon();
#endif
#endif

#ifdef RTCONFIG_WEBDAV
	start_webdav();
#else
	if(f_exists("/opt/etc/init.d/S50aicloud"))
		system("sh /opt/etc/init.d/S50aicloud scan");
#endif

#ifdef RTCONFIG_SNMPD
	start_snmpd();
#endif

#if defined(RTCONFIG_RALINK) && defined(RTCONFIG_WIRELESSREPEATER)
	apcli_start();
#endif

#ifdef RTCONFIG_BWDPI
	start_bwdpi_check();
#endif
	start_hour_monitor_service();

#ifdef RTCONFIG_IPERF
	start_iperf();
	start_monitor();
#endif

#ifdef RTCONFIG_IXIAEP
	start_ixia_endpoint();
#endif

#ifdef RTCONFIG_SAMBASRV
	start_samba();	// We might need it for wins/browsing services
#endif

#ifdef RTCONFIG_INTERNAL_GOBI
	start_lteled();
#endif

#ifdef RTCONFIG_PARENTALCTRL
	start_pc_block();
#endif

#ifdef RTCONFIG_TOR
	start_Tor_proxy();
#endif

#ifdef RTCONFIG_CLOUDCHECK
	start_cloudcheck();
#endif

#ifdef RTCONFIG_QCA_PLC_UTILS
	start_plchost();
#endif
#if ((defined(RTCONFIG_USER_LOW_RSSI) && defined(RTCONFIG_BCMARM)) || defined(RTCONFIG_NEW_USER_LOW_RSSI))
	start_roamast();
#endif

#if defined(RTCONFIG_KEY_GUARD)
	start_keyguard();
#endif

#if 0//defined(RTCONFIG_WTFAST)
	start_wtfast();
#endif

	start_ecoguard();

	run_custom_script("services-start", NULL);

	start_upnp();

	return 0;
}

void
stop_logger(void)
{
	if (pids("klogd"))
		killall("klogd", SIGTERM);
	if (pids("syslogd"))
		killall("syslogd", SIGTERM);
}

void
stop_services(void)
{
#ifdef RTCONFIG_INTERNAL_GOBI
	stop_lteled();
#endif

	stop_hour_monitor_service();
#ifdef RTCONFIG_BWDPI
	stop_bwdpi_wred_alive();
	stop_bwdpi_check();
	stop_dpi_engine_service(1);
#endif

#ifdef RTCONFIG_IXIAEP
	stop_monitor();
	stop_ixia_endpoint();
#endif

#ifdef RTCONFIG_IPERF
	stop_iperf();
#endif

	run_custom_script("services-stop", NULL);

#ifdef RTCONFIG_WEBDAV
	stop_webdav();
#else
	if(f_exists("/opt/etc/init.d/S50aicloud"))
		system("sh /opt/etc/init.d/S50aicloud scan");
#endif

#ifdef RTCONFIG_USB
//_dprintf("restart_nas_services(%d): test 9.\n", getpid());
	restart_nas_services(1, 0);
#ifdef RTCONFIG_DISK_MONITOR
	stop_diskmon();
#endif
#endif
	stop_upnp();
	stop_lltd();
	stop_watchdog();
#ifdef RTCONFIG_FANCTRL
	stop_phy_tempsense();
#endif
#ifdef RTCONFIG_BCMWL6
	stop_igmp_proxy();
#ifdef RTCONFIG_PROXYSTA
	stop_psta_monitor();
#endif
#endif
	stop_cstats();
	stop_rstats();
#ifdef RTCONFIG_DSL
	stop_spectrum(); //Ren
#endif
	stop_networkmap();
	stop_infosvr();
#ifdef RTCONFIG_CROND
	stop_cron();
#endif
	stop_httpd();
	stop_cifs();
#ifdef RTCONFIG_DHCP_OVERRIDE
	stop_detectWAN_arp();
#endif
	stop_dnsmasq();
#if defined(RTCONFIG_MDNS)
	stop_mdns();
#endif
#ifdef RTCONFIG_IPV6
	/* what? */
#endif
#ifdef RTCONFIG_BCMWL6
#ifdef BCM_ASPMD
	stop_aspmd();
#endif
	stop_acsd();
#ifdef BCM_BSD
	stop_bsd();
#endif
#ifdef BCM_SSD
	stop_ssd();
#endif
#if defined(RTCONFIG_DHDAP)
	stop_dhd_monitor();
#endif
	stop_igmp_proxy();
#ifdef RTCONFIG_HSPOT
	stop_hspotap();
#endif
#endif
#ifdef RTCONFIG_WPS
	stop_wpsaide();
#endif
	stop_wps();
#ifdef CONFIG_BCMWL5
	stop_nas();
	stop_eapd();
#elif defined RTCONFIG_RALINK
	stop_8021x();
#endif
#ifdef RTCONFIG_TOAD
	stop_toads();
#endif
	stop_telnetd();
#ifdef RTCONFIG_SSH
	stop_sshd();
#endif

#ifdef RTCONFIG_SNMPD
	stop_snmpd();
#endif

#ifdef  __CONFIG_NORTON__
	stop_norton();
#endif /* __CONFIG_NORTON__ */
#ifdef RTCONFIG_QTN
	stop_qtn_monitor();
#endif

#ifdef RTCONFIG_PARENTALCTRL
	stop_pc_block();
#endif

#ifdef RTCONFIG_TOR
	stop_Tor_proxy();
#endif

#ifdef RTCONFIG_CLOUDCHECK
	stop_cloudcheck();
#endif

#ifdef RTCONFIG_TUNNEL
	stop_mastiff();
#endif

#ifdef RTCONFIG_KEY_GUARD
	stop_keyguard();
#endif
}

// 2008.10 magic

int start_wanduck(void)
{
	char *argv[] = {"/sbin/wanduck", NULL};
	pid_t pid;

#if 0
	int sw_mode = nvram_get_int("sw_mode");
	if(sw_mode != SW_MODE_ROUTER && sw_mode != SW_MODE_REPEATER)
		return -1;
#endif
#ifdef CONFIG_BCMWL5
	if(ate_cond() == 0)
		return 0;
#endif

	if(!strcmp(nvram_safe_get("wanduck_down"), "1"))
		return 0;

	return _eval(argv, NULL, 0, &pid);
}

void stop_wanduck(void)
{
	killall("wanduck", SIGTERM);
}

void
stop_watchdog(void)
{
	killall_tk("watchdog");
}

#if ! (defined(RTCONFIG_QCA) || defined(RTCONFIG_RALINK))
void
stop_watchdog02(void)
{
	/* do nothing */
	return;
}
#endif  /* ! (RTCONFIG_QCA || RTCONFIG_RALINK) */

void
stop_sw_devled(void)
{
	killall_tk("sw_devled");
	return;
}

#ifdef RTCONFIG_DUALWAN
int restart_dualwan(void)
{
	char *dualwan_argv[] = {"dualwan", NULL};
	pid_t dualpid;

	killall("dualwan", SIGTERM);

	return _eval(dualwan_argv, NULL, 0, &dualpid);
}
#endif

int
start_watchdog(void)
{
	char *watchdog_argv[] = {"watchdog", NULL};
	pid_t whpid;

	return _eval(watchdog_argv, NULL, 0, &whpid);
}

#if ! (defined(RTCONFIG_QCA) || defined(RTCONFIG_RALINK))
int
start_watchdog02(void)
{
	char *watchdog_argv[] = {"watchdog02", NULL};
	pid_t whpid;

	if (pidof("watchdog02") > 0) return -1;

	return _eval(watchdog_argv, NULL, 0, &whpid);
}
#endif  /* ! (RTCONFIG_QCA || RTCONFIG_RALINK) */

int
start_sw_devled(void)
{
	char *sw_devled_argv[] = {"sw_devled", NULL};
	pid_t whpid;

	if (pidof("sw_devled") > 0) return -1;

	return _eval(sw_devled_argv, NULL, 0, &whpid);
}

#ifdef RTCONFIG_DSL
#ifdef RTCONFIG_RALINK
//Ren.B
int check_tc_upgrade(void)
{
	int ret_val_sep = 0;

	TRACE_PT("check_tc_upgrade\n");

	ret_val_sep = separate_tc_fw_from_trx();
	TRACE_PT("check_tc_upgrade, ret_val_sep=%d\n", ret_val_sep);
	if(ret_val_sep)
	{
		if(check_tc_firmware_crc() == 0)
		{
			TRACE_PT("check_tc_upgrade ret=1\n");
			return 1; //success
		}
	}
	TRACE_PT("check_tc_upgrade ret=0\n");
	return 0; //fail
}

//New version will rename downloaded firmware to /tmp/linux.trx as default.
int start_tc_upgrade(void)
{
	int ret_val_trunc = 0;
	int ret_val_comp = 0;

	TRACE_PT("start_tc_upgrade\n");

	if(check_tc_upgrade())
	{
		ret_val_trunc = truncate_trx();
		TRACE_PT("start_tc_upgrade ret_val_trunc=%d\n", ret_val_trunc);
		if(ret_val_trunc)
		{
			do_upgrade_adsldrv();
			ret_val_comp = compare_linux_image();
			TRACE_PT("start_tc_upgrade ret_val_comp=%d\n", ret_val_comp);
			if (ret_val_comp == 0)
			{
				// same trx
				TRACE_PT("same firmware\n");
				unlink("/tmp/linux.trx");
			}
			else
			{
				// different trx
				TRACE_PT("different firmware\n");
			}
		}
	}
	return 0;
}
//Ren.E
#endif
#endif

#ifdef RTCONFIG_FANCTRL
int
stop_phy_tempsense()
{
	if (pids("phy_tempsense")) {
		killall_tk("phy_tempsense");
	}
	return 0;
}

int
start_phy_tempsense()
{
	char *phy_tempsense_argv[] = {"phy_tempsense", NULL};
	pid_t pid;

	return _eval(phy_tempsense_argv, NULL, 0, &pid);
}
#endif

#if defined(RTCONFIG_BCMWL6) && defined(RTCONFIG_PROXYSTA)
int
stop_psta_monitor()
{
	if (pids("psta_monitor")) {
		killall_tk("psta_monitor");
	}
	return 0;
}

int
start_psta_monitor()
{
	char *psta_monitor_argv[] = {"psta_monitor", NULL};
	pid_t pid;

	return _eval(psta_monitor_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_IPERF
int
stop_monitor()
{
	if (pids("monitor")) {
		killall_tk("monitor");
	}
	return 0;
}

int
start_monitor()
{
	char *monitor_argv[] = {"monitor", NULL};
	pid_t pid;

	return _eval(monitor_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_QTN
int
stop_qtn_monitor()
{
	if (pids("qtn_monitor")) {
		killall_tk("qtn_monitor");
	}
	return 0;
}

int
start_qtn_monitor()
{
	char *qtn_monitor_argv[] = {"qtn_monitor", NULL};
	pid_t pid;

	return _eval(qtn_monitor_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_USB
int
start_usbled(void)
{
	char *usbled_argv[] = {"usbled", NULL};
	pid_t whpid;

	stop_usbled();
	return _eval(usbled_argv, NULL, 0, &whpid);
}

int
stop_usbled(void)
{
	if (pids("usbled"))
		killall("usbled", SIGTERM);

	return 0;
}
#endif

#ifdef RTCONFIG_CROND
void start_cron(void)
{
	stop_cron();
	eval("crond");
}


void stop_cron(void)
{
	killall_tk("crond");
}
#endif

#ifdef RTCONFIG_QUAGGA
void stop_quagga(void)
{
	if (pids("zebra")){
		killall("zebra", SIGINT);
	}
	if (pids("ripd")){
		killall("ripd", SIGINT);
	}
}

int start_quagga(void)
{
	FILE *fp, *fp2;
	char *zebra_hostname;
	char *zebra_passwd;
	char *zebra_enpasswd;
	char *rip_hostname;
	char *rip_passwd;
/*
	char *wan_ip, *wan_ifname;
	int   unit;
	char tmp[32], prefix[] = "wanXXXXXXXXXX_";
*/

	if (!is_routing_enabled()) {
		_dprintf("return -1\n");
		return -1;
	}
	if (nvram_invmatch("quagga_enable", "1"))
		return -1;

/*
	unit = wan_primary_ifunit();
	snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	wan_ip = nvram_safe_get(strcat_r(prefix, "ipaddr", tmp));
	wan_ifname = get_wan_ifname(unit);

	if (!wan_ip || strcmp(wan_ip, "") == 0 || !inet_addr(wan_ip)) {
		logmessage("quagga", "WAN IP is empty.");
		return -1;
	}
*/
	zebra_passwd = nvram_safe_get("zebra_passwd");
	zebra_enpasswd = nvram_safe_get("zebra_enpasswd");
	rip_passwd = nvram_safe_get("rip_passwd");

	zebra_hostname = nvram_safe_get("productid");
	rip_hostname = nvram_safe_get("productid");

	if (pids("zebra")){
		killall("zebra", SIGINT);
		sleep(1);
	}
	if (pids("ripd")){
		killall("ripd", SIGINT);
		sleep(1);
	}
	if ((fp = fopen("/etc/zebra.conf", "w"))){
		fprintf(fp, "hostname %s\n", zebra_hostname);
		fprintf(fp, "password %s\n", zebra_passwd);
		fprintf(fp, "enable password %s\n", zebra_enpasswd);
		fprintf(fp, "log file /etc/zebra.log\n");
		fclose(fp);
		eval("zebra", "-d", "-f", "/etc/zebra.conf");
	}
	if ((fp2 = fopen("/etc/ripd.conf", "w"))){
		fprintf(fp2, "hostname %s\n", rip_hostname);
		fprintf(fp2, "password %s\n", rip_passwd);
		fprintf(fp2, "debug rip events\n");
		fprintf(fp2, "debug rip packet\n");
		fprintf(fp2, "router rip\n");
		fprintf(fp2, " version 2\n");
		fprintf(fp2, " network vlan2\n");
		fprintf(fp2, " network vlan3\n");
		fprintf(fp2, " passive-interface vlan2\n");
		fprintf(fp2, " passive-interface vlan3\n");
		fprintf(fp2, "log file /etc/ripd.log\n");
		fprintf(fp2, "log stdout\n");
		fclose(fp2);
		eval("ripd", "-d", "-f", "/etc/ripd.conf");
	}
	return 0;
}
#endif

void start_script(int argc, char *argv[])
{
	int pid;

	argv[argc] = NULL;
	_eval(argv, NULL, 0, &pid);

}

// -----------------------------------------------------------------------------

/* -1 = Don't check for this program, it is not expected to be running.
 * Other = This program has been started and should be kept running.  If no
 * process with the name is running, call func to restart it.
 * Note: At startup, dnsmasq forks a short-lived child which forks a
 * long-lived (grand)child.  The parents terminate.
 * Many daemons use this technique.
 */
static void _check(pid_t pid, const char *name, void (*func)(void))
{
	if (pid == -1) return;

	if (pidof(name) > 0) return;

	syslog(LOG_DEBUG, "%s terminated unexpectedly, restarting.\n", name);
	func();

	// Force recheck in 500 msec
	setitimer(ITIMER_REAL, &pop_tv, NULL);
}

void check_services(void)
{
//	TRACE_PT("keep alive\n");

	// Periodically reap any zombies
	setitimer(ITIMER_REAL, &zombie_tv, NULL);

#ifdef LINUX26
	_check(pids("hotplug2"), "hotplug2", start_hotplug2);
#endif
}

#define RC_SERVICE_STOP 0x01
#define RC_SERVICE_START 0x02

void handle_notifications(void)
{
	char nv[256], nvtmp[32], *cmd[8], *script;
	char *nvp, *b, *nvptr;
	int action = 0;
	int count;
	int i;
	int unit;

	// handle command one by one only
	// handle at most 7 parameters only
	// maximum rc_service strlen is 256
	strcpy(nv, nvram_safe_get("rc_service"));
	nvptr = nv;
again:
	nvp = strsep(&nvptr, ";");

	count = 0;
	while ((b = strsep(&nvp, " ")) != NULL)
	{
		_dprintf("cmd[%d]=%s\n", count, b);
		cmd[count] = b;
		count ++;
		if(count == 7) break;
	}
	cmd[count] = 0;

	if(cmd[0]==0 || strlen(cmd[0])==0) {
		nvram_set("rc_service", "");
		return;
	}

	if(strncmp(cmd[0], "start_", 6)==0) {
		action |= RC_SERVICE_START;
		script = &cmd[0][6];
	}
	else if(strncmp(cmd[0], "stop_", 5)==0) {
		action |= RC_SERVICE_STOP;
		script = &cmd[0][5];
	}
	else if(strncmp(cmd[0], "restart_", 8)==0) {
		action |= (RC_SERVICE_START | RC_SERVICE_STOP);
		script = &cmd[0][8];
	}
	else {
		action = 0;
		script = cmd[0];
	}

	TRACE_PT("running: %d %s\n", action, script);

	if (strcmp(script, "reboot") == 0 || strcmp(script,"rebootandrestore")==0) {
		g_reboot = 1;

#ifdef RTCONFIG_QCA_PLC_UTILS
		reset_plc();
		sleep(1);
#endif

		stop_wan();
#ifdef RTCONFIG_USB
#if defined(RTCONFIG_USB_MODEM) && (defined(RTCONFIG_JFFS2) || defined(RTCONFIG_BRCM_NAND_JFFS2) || defined(RTCONFIG_UBIFS))
		_dprintf("modem data: save the data during the reboot service\n");
		eval("modem_status.sh", "bytes+");
#endif

#ifdef RTCONFIG_USB_MODEM
#if defined(RTCONFIG_JFFS2) || defined(RTCONFIG_BRCM_NAND_JFFS2) || defined(RTCONFIG_UBIFS)
		eval("rm", "-rf", "/jffs/sim");
#endif
#endif

		if (get_model() == MODEL_RTN53){
			eval("wlconf", "eth2", "down");
			modprobe_r("wl_high");
		}

#if !(defined(RTN56UB1) || defined(RTN56UB2))
		stop_usb(0);
		stop_usbled();
#endif
#endif
//#if defined(RTCONFIG_JFFS2LOG) && defined(RTCONFIG_JFFS2)
#if defined(RTCONFIG_JFFS2LOG) && (defined(RTCONFIG_JFFS2)||defined(RTCONFIG_BRCM_NAND_JFFS2))
		eval("cp", "/tmp/syslog.log", "/tmp/syslog.log-1", "/jffs");
#endif
		if(strcmp(script,"rebootandrestore")==0) {
			for(i=1;i<count;i++) {
				if(cmd[i]) restore_defaults_module(cmd[i]);
			}
		}

		/* Fall through to signal handler of init process. */
	}
	else if (strcmp(script, "resetdefault") == 0) {
		g_reboot = 1;
#ifdef RTCONFIG_DSL
		eval("adslate", "sysdefault");
#endif
		stop_wan();
#ifdef RTCONFIG_USB
		if (get_model() == MODEL_RTN53){
			eval("wlconf", "eth2", "down");
			modprobe_r("wl_high");
		}

#if !defined(RTN56UB1) && !defined(RTN56UB2)
		stop_usb(0);
		stop_usbled();
#endif
#endif
		sleep(3);
		nvram_set(ASUS_STOP_COMMIT, "1");
		if (nvram_contains_word("rc_support", "nandflash"))	/* RT-AC56S,U/RT-AC68U/RT-N18U */
			eval("mtd-erase2", "nvram");
		else
#if defined(RTAC1200G) || defined(RTAC1200GP)
			eval("mtd-erase2", "nvram");
#else
			eval("mtd-erase", "-d", "nvram");
#endif

#ifdef RTCONFIG_QCA_PLC_UTILS
		reset_plc();
		eval("mtd-erase", "-d", "plc");
#endif

		kill(1, SIGTERM);
	}
	else if (strcmp(script, "all") == 0) {
#ifdef RTCONFIG_QCA_PLC_UTILS
		reset_plc();
#endif
		sleep(2); // wait for all httpd event done
		stop_lan_port();
		start_lan_port(6);
		kill(1, SIGTERM);
	}
	else if(strcmp(script, "upgrade") == 0) {
		if(action&RC_SERVICE_STOP) {
#ifdef RTCONFIG_WIRELESSREPEATER
		if(nvram_get_int("sw_mode") == SW_MODE_REPEATER)
			stop_wlcconnect();
#endif

			stop_hour_monitor_service();
#if defined(RTCONFIG_USB_MODEM) && (defined(RTCONFIG_JFFS2) || defined(RTCONFIG_BRCM_NAND_JFFS2) || defined(RTCONFIG_UBIFS))
			_dprintf("modem data: save the data during upgrading\n");
			eval("modem_status.sh", "bytes+");
#endif

			eval("/sbin/ejusb", "-1", "0");
			logmessage("usb", "USB is ejected");
		   if(!(nvram_match("webs_state_flag", "1") && nvram_match("webs_state_upgrade", "0")))
			stop_wanduck();
			stop_wan();

			// what process need to stop to free memory or
			// to avoid affecting upgrade
			stop_misc();
			stop_logger();
			stop_upnp();
#if defined(RTCONFIG_MDNS)
			stop_mdns();
#endif
			stop_all_webdav();
#if defined(RTN56U)
			stop_if_misc();
#endif
#ifdef RTCONFIG_USB
			/* fix upgrade fail issue : remove wl_high before rmmod ehci_hcd */
			if (get_model() == MODEL_RTAC53U){
				eval("wlconf", "eth1", "down");
				eval("wlconf", "eth2", "down");
				modprobe_r("wl_high");
				modprobe_r("wl");
			}

#if !defined(RTN53) && !defined(RTN56UB1) && !defined(RTN56UB2)
			stop_usb(0);
			stop_usbled();
			remove_storage_main(1);
			remove_usb_module();
#endif

#endif
			remove_conntrack();
			stop_udhcpc(-1);
#ifdef RTCONFIG_IPV6
			stop_dhcp6c();
#endif

#ifdef RTCONFIG_TR069
			stop_tr();
#endif
			stop_jffs2(1);
			stop_networkmap();

#ifdef RTCONFIG_QCA_PLC_UTILS
			reset_plc();
#endif
			// TODO free necessary memory here
		}
		if(action & RC_SERVICE_START) {
			int sw = 0, r;
			char upgrade_file[64] = "/tmp/linux.trx";
			char *webs_state_info = nvram_safe_get("webs_state_info");

#ifdef RTCONFIG_SMALL_FW_UPDATE
			snprintf(upgrade_file,sizeof(upgrade_file),"/tmp/mytmpfs/linux.trx");
#endif

		#ifdef RTCONFIG_DSL
		#ifdef RTCONFIG_RALINK
			_dprintf("to do start_tc_upgrade\n");
			start_tc_upgrade();
		#else
			do_upgrade_adsldrv();
		#endif
		#endif

			limit_page_cache_ratio(90);

			/* /tmp/linux.trx has the priority */
			if (!f_exists(upgrade_file) && strlen(webs_state_info) > 5) {
				snprintf(upgrade_file, sizeof(upgrade_file),
					"/tmp/%s_%c.%c.%c.%c_%s.trx",
					nvram_safe_get("productid"),
					webs_state_info[0],
					webs_state_info[1],
					webs_state_info[2],
					webs_state_info[3],
					webs_state_info+5);
				_dprintf("upgrade file : %s \n", upgrade_file);
			}

			/* flash it if exists */
			if (f_exists(upgrade_file)) {
				/* stop wireless here */
#ifdef RTCONFIG_SMALL_FW_UPDATE
/* TODO should not depend on platform, move to stop_lan_wl()?
 * cope with stop_usb() above for BRCM AP dependencies */
#ifdef CONFIG_BCMWL5
/* TODO should not depend on exact interfaces */
				eval("wlconf", "eth1", "down");
				eval("wlconf", "eth2", "down");
/* TODO fix fini_wl() for BCM USBAP */
				modprobe_r("wl_high");
				modprobe_r("wl");
#ifdef RTCONFIG_USB
#if defined(RTN53)
				stop_usb();
				stop_usbled();
				remove_storage_main(1);
				remove_usb_module();
#endif
#endif
#endif
#elif defined(RTCONFIG_TEMPROOTFS)
				stop_lan_wl();
				stop_dnsmasq();
				stop_networkmap();
				stop_wpsaide();
#endif
				if (!(r = build_temp_rootfs(TMP_ROOTFS_MNT_POINT)))
					sw = 1;
#ifdef RTCONFIG_DUAL_TRX
				if (!nvram_match("nflash_swecc", "1"))
				{
					_dprintf(" Write FW to the 2nd partition.\n");
					if (nvram_contains_word("rc_support", "nandflash"))     /* RT-AC56S,U/RT-AC68U/RT-N16UHP */
						eval("mtd-write2", upgrade_file, "linux2");
					else
						eval("mtd-write", "-i", upgrade_file, "-d", "linux2");
				}
#endif
				if (nvram_contains_word("rc_support", "nandflash"))	/* RT-AC56S,U/RT-AC68U/RT-N16UHP */
					eval("mtd-write2", upgrade_file, "linux");
				else
#if defined(RTAC1200G) || defined(RTAC1200GP)
					eval("mtd-write2", upgrade_file, "linux");
#else
					eval("mtd-write", "-i", upgrade_file, "-d", "linux");
#endif

				/* erase trx and free memory on purpose */
				unlink(upgrade_file);
				if (sw) {
					_dprintf("switch to temp rootfilesystem\n");
					if (!(r = switch_root(TMP_ROOTFS_MNT_POINT))) {
						/* Do nothing. If switch_root() success, never reach here. */
					} else {
						kill(1, SIGTERM);
					}
				} else {
					kill(1, SIGTERM);
				}
			}
			else {
				// recover? or reboot directly
				kill(1, SIGTERM);
			}
		}
	}
	else if(strcmp(script, "mfgmode") == 0) {
		nvram_set("asus_mfg", "2");
		//stop_infosvr(); //ATE need ifosvr
		stop_ntpc();
#ifdef RTCONFIG_BCMWL6
#ifdef BCM_ASPMD
		stop_aspmd();
#endif
		stop_acsd();
#ifdef BCM_BSD
		stop_bsd();
#endif
#ifdef BCM_SSD
		stop_ssd();
#endif
#if defined(RTCONFIG_DHDAP)
		stop_dhd_monitor();
#endif
		stop_igmp_proxy();
#ifdef RTCONFIG_HSPOT
		stop_hspotap();
#endif
#endif
		stop_wpsaide();
		stop_wps();
		stop_upnp();
		stop_lltd();
		stop_wanduck();
		stop_logger();
		stop_dnsmasq();
#if defined(RTCONFIG_MDNS)
		stop_mdns();
#endif
		stop_ots();
		stop_networkmap();
#ifdef RTCONFIG_DISK_MONITOR
		stop_diskmon();
#endif
#ifdef RTCONFIG_BWDPI
		stop_bwdpi_check();
#endif
		stop_ntpc();
		stop_udhcpc(-1);
#ifdef RTCONFIG_USB
		stop_usbled();
#endif
#ifdef RTCONFIG_USB_PRINTER
		stop_lpd();
		stop_u2ec();
#endif
		platform_start_ate_mode();
#ifdef RTCONFIG_QCA_PLC_UTILS
		ate_ctl_plc_led();
#endif
#ifdef SW_DEVLED
		stop_sw_devled();
#endif
#if defined(RTCONFIG_CFEZ) && defined(RTCONFIG_BCMARM)
		start_envrams();
#endif
	}
	else if (strcmp(script, "allnet") == 0) {
		if(action&RC_SERVICE_STOP) {
			// including switch setting
			// used for system mode change and vlan setting change
			sleep(2); // wait for all httpd event done
			stop_networkmap();
			stop_httpd();
			stop_dnsmasq();
#if defined(RTCONFIG_MDNS)
			stop_mdns();
#endif
			stop_upnp();
			stop_lltd();
#ifdef RTCONFIG_BCMWL6
#ifdef BCM_ASPMD
			stop_aspmd();
#endif
			stop_acsd();
#ifdef BCM_BSD
			stop_bsd();
#endif
#ifdef BCM_SSD
			stop_ssd();
#endif
#if defined(RTCONFIG_DHDAP)
			stop_dhd_monitor();
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
			stop_wan();
			stop_lan();
#ifdef RTCONFIG_DSL_TCLINUX
			stop_dsl();
#endif
			stop_vlan();


			// TODO free memory here
		}
		if(action & RC_SERVICE_START) {
			config_switch();

			start_vlan();
#ifdef RTCONFIG_DSL_TCLINUX
			start_dsl();
#endif
			start_lan();
			start_dnsmasq();
#if defined(RTCONFIG_MDNS)
			start_mdns();
#endif
			start_wan();
#ifdef RTCONFIG_USB_MODEM
			if((unit = get_usbif_dualwan_unit()) >= 0)
				start_wan_if(unit);
#endif
#ifdef CONFIG_BCMWL5
			start_eapd();
			start_nas();
#elif defined RTCONFIG_RALINK
			start_8021x();
#endif
			start_wps();
#ifdef RTCONFIG_BCMWL6
			start_igmp_proxy();
#ifdef BCM_BSD
			start_bsd();
#endif
#ifdef BCM_SSD
			start_ssd();
#endif
#if defined(BCM_EVENTD)
			start_eventd();
#endif
#if defined(RTCONFIG_DHDAP)
			start_dhd_monitor();
#endif
			start_acsd();
#ifdef BCM_ASPMD
			start_aspmd();
#endif
#endif
			start_lltd();
			/* Link-up LAN ports after DHCP server ready. */
			start_lan_port(0);

			start_upnp();

			start_httpd();
			start_networkmap(0);
			start_wl();
			lanaccess_wl();
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_HSPOT
			start_hspotap();
#endif
#endif
		}
	}
	else if (strcmp(script, "net") == 0) {
		if(action & RC_SERVICE_STOP) {
			sleep(2); // wait for all httpd event done
#ifdef RTCONFIG_USB_PRINTER
			stop_u2ec();
#endif
			stop_networkmap();
			stop_httpd();
			stop_dnsmasq();
#if defined(RTCONFIG_MDNS)
			stop_mdns();
#endif
			stop_upnp();
			stop_lltd();
#ifdef RTCONFIG_BCMWL6
#ifdef BCM_ASPMD
			stop_aspmd();
#endif
			stop_acsd();
#ifdef BCM_BSD
			stop_bsd();
#endif
#ifdef BCM_SSD
			stop_ssd();
#endif
#if defined(RTCONFIG_DHDAP)
			stop_dhd_monitor();
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
			stop_wan();
			stop_lan();
			//stop_vlan();

			// free memory here
		}
		if(action & RC_SERVICE_START) {
			//start_vlan();
			start_lan();
			start_dnsmasq();
#if defined(RTCONFIG_MDNS)
			start_mdns();
#endif
			start_wan();
#ifndef RTCONFIG_INTERNAL_GOBI
#ifdef RTCONFIG_USB_MODEM
			if((unit = get_usbif_dualwan_unit()) >= 0)
				start_wan_if(unit);
#endif
#endif
#ifdef CONFIG_BCMWL5
			start_eapd();
			start_nas();
#elif defined RTCONFIG_RALINK
			start_8021x();
#endif
			start_wps();
#ifdef RTCONFIG_BCMWL6
			start_igmp_proxy();
#ifdef BCM_BSD
			start_bsd();
#endif
#ifdef BCM_SSD
			start_ssd();
#endif
#if defined(BCM_EVENTD)
			start_eventd();
#endif
#if defined(RTCONFIG_DHDAP)
			start_dhd_monitor();
#endif
			start_acsd();
#ifdef BCM_ASPMD
			start_aspmd();
#endif
#endif
			start_lltd();
			/* Link-up LAN ports after DHCP server ready. */
			start_lan_port(0);

			start_upnp();

			start_httpd();
			start_networkmap(0);
#ifdef RTCONFIG_USB_PRINTER
			start_u2ec();
#endif
			start_wl();
			lanaccess_wl();
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_HSPOT
			start_hspotap();
#endif
#endif
		}
	}
	else if (strcmp(script, "net_and_phy") == 0) {
		if(action & RC_SERVICE_STOP) {
			sleep(2); // wait for all httpd event done

#ifdef RTCONFIG_MEDIA_SERVER
			force_stop_dms();
			stop_mt_daapd();
#endif

#if defined(RTCONFIG_SAMBASRV) && defined(RTCONFIG_FTP)
			stop_ftpd();
			stop_samba();
#endif

#ifdef RTCONFIG_USB_PRINTER
			stop_u2ec();
#endif
			stop_networkmap();
			stop_httpd();
#ifdef RTCONFIG_DHCP_OVERRIDE
			stop_detectWAN_arp();
#endif
			stop_dnsmasq();
#if defined(RTCONFIG_MDNS)
			stop_mdns();
#endif
			//Andy Chiu, 2015/09/16.
			stop_upnp();
			stop_lltd();
#ifdef RTCONFIG_BCMWL6
#ifdef BCM_ASPMD
			stop_aspmd();
#endif
			stop_acsd();
#ifdef BCM_BSD
			stop_bsd();
#endif
#ifdef BCM_SSD
			stop_ssd();
#endif
#if defined(RTCONFIG_DHDAP)
			stop_dhd_monitor();
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
#if defined(RTCONFIG_PPTPD) || defined(RTCONFIG_ACCEL_PPTPD)
			//stop_pptpd();
#endif
			stop_wan();
			stop_lan();
			//stop_vlan();
			stop_lan_port();

			// free memory here
		}
		if(action & RC_SERVICE_START) {
			//start_vlan();
			start_lan();
			start_dnsmasq();
#ifdef RTCONFIG_DHCP_OVERRIDE
			start_detectWAN_arp();
#endif
#if defined(RTCONFIG_MDNS)
			start_mdns();
#endif
			start_wan();
#ifdef RTCONFIG_USB_MODEM
			if((unit = get_usbif_dualwan_unit()) >= 0)
				start_wan_if(unit);
#endif
#ifdef CONFIG_BCMWL5
			start_eapd();
			start_nas();
#elif defined RTCONFIG_RALINK
			start_8021x();
#endif
#if defined(RTCONFIG_PPTPD) || defined(RTCONFIG_ACCEL_PPTPD)
			//start_pptpd();
#endif
			start_wps();
#ifdef RTCONFIG_BCMWL6
			start_igmp_proxy();
#ifdef BCM_BSD
			start_bsd();
#endif
#ifdef BCM_SSD
			start_ssd();
#endif
#if defined(BCM_EVENTD)
			start_eventd();
#endif
#if defined(RTCONFIG_DHDAP)
			start_dhd_monitor();
#endif
			start_acsd();
#ifdef BCM_ASPMD
			start_aspmd();
#endif
#endif
			start_lltd();
			/* Link-up LAN ports after DHCP server ready. */
			start_lan_port(6);

			//Andy Chiu, 2015/09/16
			start_upnp();

			start_httpd();
			start_networkmap(0);
#ifdef RTCONFIG_USB_PRINTER
			start_u2ec();
#endif

#if defined(RTCONFIG_SAMBASRV) && defined(RTCONFIG_FTP)
			setup_passwd();
			start_samba();
			start_ftpd();
#endif
			start_wl();
			lanaccess_wl();
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_HSPOT
			start_hspotap();
#endif
#endif
#ifdef RTCONFIG_MEDIA_SERVER
			start_dms();
			start_mt_daapd();
#endif
		}
	}
#ifdef RTCONFIG_DUALWAN
	else if(!strcmp(script, "multipath")){
		char mode[4], if_now[16], if_next[16];
		int unit;
		int unit_now = wan_primary_ifunit();
		int unit_next = (unit_now+1)%WAN_UNIT_MAX;
		int state_now = is_wan_connect(unit_now);
		int state_next = is_wan_connect(unit_next);

		snprintf(mode, 4, "%s", nvram_safe_get("wans_mode"));
		snprintf(if_now, 16, "%s", get_wan_ifname(unit_now));
		snprintf(if_next, 16, "%s", get_wan_ifname(unit_next));
_dprintf("multipath(%s): unit_now: (%d, %d, %s), unit_next: (%d, %d, %s).\n", mode, unit_now, state_now, if_now, unit_next, state_next, if_next);

		if(!strcmp(mode, "lb")){
			if(state_now == 1 && state_next == 1){
				wan_up(if_now);
			}
			else if(state_now == 1 && state_next == 0){
				stop_wan_if(unit_next);
				start_wan_if(unit_next);
			}
			else if(state_now == 0 && state_next == 1){
				wan_up(if_next);
				stop_wan_if(unit_now);
				start_wan_if(unit_now);
			}
			else{ // state_now == 0 && state_next == 0
				for(unit = WAN_UNIT_FIRST; unit < WAN_UNIT_MAX; ++unit){
					stop_wan_if(unit);
					start_wan_if(unit);
				}
			}
		}
		else if(!strcmp(mode, "fb")){
			if(state_now == 1 && state_next == 1){
				if(unit_next == WAN_UNIT_FIRST){
					set_wan_primary_ifunit(unit_next);
					wan_up(if_next);
					stop_wan_if(unit_now);
				}
				else{ // unit_now == WAN_UNIT_FIRST
					wan_up(if_now);
					stop_wan_if(unit_next);
				}
			}
			else if(state_now == 1 && state_next == 0){
				wan_up(if_now);
				if(unit_next == WAN_UNIT_FIRST){
					stop_wan_if(unit_next);
					start_wan_if(unit_next);
				}
			}
			else if(state_now == 0 && state_next == 1){
				set_wan_primary_ifunit(unit_next);
				wan_up(if_next);
				if(unit_now == WAN_UNIT_FIRST){
					stop_wan_if(unit_now);
					start_wan_if(unit_now);
				}
			}
			else{ // state_now == 0 && state_next == 0
				if(unit_next == WAN_UNIT_FIRST){
					set_wan_primary_ifunit(unit_next);
					stop_wan_if(unit_next);
					start_wan_if(unit_next);
				}
				else{
					stop_wan_if(unit_now);
					start_wan_if(unit_now);
				}
			}
		}
		else if(!strcmp(mode, "fo")){
			if(state_now == 1 && state_next == 1){
				wan_up(if_now);
				stop_wan_if(unit_next);
			}
			else if(state_now == 1 && state_next == 0){
				wan_up(if_now);
			}
			else if(state_now == 0 && state_next == 1){
				set_wan_primary_ifunit(unit_next);
				wan_up(if_next);
			}
			else{ // state_now == 0 && state_next == 0
				stop_wan_if(unit_now);
				start_wan_if(unit_now);
			}
		}
	}
#endif
	else if (strcmp(script, "wireless") == 0) {
		if(action&RC_SERVICE_STOP) {
#ifdef RTCONFIG_WIRELESSREPEATER
			stop_wlcconnect();

			kill_pidfile_s("/var/run/wanduck.pid", SIGUSR1);
#endif

#ifdef RTCONFIG_USB_PRINTER
			stop_u2ec();
#endif
			stop_networkmap();
		}
		if((action & RC_SERVICE_STOP) && (action & RC_SERVICE_START)) {
			// TODO: free memory here
			reinit_hwnat(-1);
			restart_wireless();
		}
		if(action & RC_SERVICE_START) {
#ifdef RTCONFIG_WIRELESSREPEATER
			start_wlcconnect();
#endif

			start_networkmap(0);
#ifdef RTCONFIG_USB_PRINTER
			start_u2ec();
#endif
			start_ecoguard(); //for app eco mode
		}
		setup_leds();
	}
#ifdef CONFIG_BCMWL5
#ifdef RTCONFIG_BCMWL6A
	else if (strcmp(script, "clkfreq") == 0) {
		dbG("clkfreq: %s\n", nvram_safe_get("clkfreq"));

		char *string = nvram_safe_get("clkfreq");
		char *cpu, *ddr, buf[100];
		unsigned int cpu_clock = 0, ddr_clock = 0;
		static unsigned int cpu_clock_table[] = {600, 800, 1000, 1200, 1400, 1600};
		static unsigned int ddr_clock_table[] = {333, 389, 400, 533, 666, 775, 800};

		if (strchr(string, ','))
		{
			strncpy(ddr = buf, string, sizeof(buf));
			cpu = strsep(&ddr, ",");
			cpu_clock=atoi(cpu);
			ddr_clock=atoi(ddr);
		}
		else
			cpu_clock=atoi(string);


		for (i = 0; i < (sizeof(cpu_clock_table)/sizeof(cpu_clock_table[0])); i++)
		{
			if (cpu_clock == cpu_clock_table[i])
				goto check_ddr_clock;
		}
		cpu_clock = 800;
check_ddr_clock:
		for (i = 0; i < (sizeof(ddr_clock_table)/sizeof(ddr_clock_table[0])); i++)
		{
			if (ddr_clock == ddr_clock_table[i])
				goto check_ddr_done;
		}
		ddr_clock = 533;
check_ddr_done:
		if (cpu_clock) dbG("target CPU clock: %d\n", cpu_clock);
		if (ddr_clock) dbG("target DDR clock: %d\n", ddr_clock);

		nvram_unset("sdram_ncdl");
		nvram_commit();
	}
#endif
	else if (strcmp(script, "set_wltxpower") == 0) {
		switch (get_model()) {
		case MODEL_RTAC66U:
		case MODEL_RTAC56S:
		case MODEL_RTAC56U:
		case MODEL_RTAC3200:
		case MODEL_RPAC68U:
		case MODEL_RTAC68U:
		case MODEL_DSLAC68U:
		case MODEL_RTAC87U:
		case MODEL_RTN12HP:
		case MODEL_RTN12HP_B1:
		case MODEL_APN12HP:
		case MODEL_RTN66U:
		case MODEL_RTN18U:
		case MODEL_RTAC5300:
		case MODEL_RTAC5300R:
		case MODEL_RTAC3100:
		case MODEL_RTAC88U:
		//case MODEL_RTAC1200G:
			set_wltxpower();
			break;
		default:
			dbG("\n\tDon't do this!\n\n");
			break;
		}
	}
#endif
#ifdef RTCONFIG_FANCTRL
	else if (strcmp(script, "fanctrl") == 0) {
		if((action & RC_SERVICE_STOP)&&(action & RC_SERVICE_START)) restart_fanctrl();
	}
#endif
	else if (strcmp(script, "wan") == 0) {
		if(action & RC_SERVICE_STOP)
		{
			stop_upnp();
			stop_wan();
		}
		if(action & RC_SERVICE_START)
		{
			start_wan();
			start_upnp();
		}
	}
	else if (strcmp(script, "wan_if") == 0) {
		_dprintf("%s: wan_if: %s.\n", __FUNCTION__, cmd[1]);
		if(cmd[1]) {
			if(action & RC_SERVICE_STOP)
			{
				stop_wan_if(atoi(cmd[1]));
#if defined(RTCONFIG_IPV6) && defined(RTCONFIG_DUALWAN)
				stop_lan_ipv6();
#endif
			}
			if(action & RC_SERVICE_START)
			{
#if defined(RTCONFIG_IPV6) && defined(RTCONFIG_DUALWAN)
				start_lan_ipv6();
#endif
#ifdef DSL_AC68U	//Andy Chiu, 2015/09/15.
				//Check the vlan config of ethernet wan, reset the config by new vlan id.
				check_wan_if(atoi(cmd[1]));

#endif
				start_wan_if(atoi(cmd[1]));
#if defined(RTCONFIG_IPV6) && defined(RTCONFIG_DUALWAN)
				restart_dnsmasq_ipv6();
#endif
			}
		}
	}
#ifdef RTCONFIG_DSL
	else if (strcmp(script, "dslwan_if") == 0) {
		_dprintf("%s: restart_dslwan_if: %s.\n", __FUNCTION__, cmd[1]);
		if(cmd[1]) {
			if(action & RC_SERVICE_STOP)
			{
				stop_wan_if(atoi(cmd[1]));
			}
			if(action & RC_SERVICE_START)
			{
				remove_dsl_autodet();
				dsl_configure(2);
				start_wan_if(atoi(cmd[1]));
			}
		}
	}
	else if (strcmp(script, "dsl_wireless") == 0) {
		if(action&RC_SERVICE_STOP) {
#ifdef RTCONFIG_USB_PRINTER
			stop_u2ec();
#endif
			stop_networkmap();
		}
		if((action&RC_SERVICE_STOP) && (action & RC_SERVICE_START)) {
// qis
			remove_dsl_autodet();
			stop_wan_if(atoi(cmd[1]));
			dsl_configure(2);
			start_wan_if(atoi(cmd[1]));
			restart_wireless();
		}
		if(action & RC_SERVICE_START) {
			start_networkmap(0);
#ifdef RTCONFIG_USB_PRINTER
			start_u2ec();
#endif
		}
	}
	else if (strcmp(script, "dsl_setting") == 0) {
		if((action & RC_SERVICE_STOP) && (action & RC_SERVICE_START)) {
			nvram_set("dsltmp_syncloss_apply", "1");
			eval("req_dsl_drv", "dslsetting");
		}
	}
#ifdef RTCONFIG_DSL_TCLINUX
	else if (strcmp(script, "dsl_autodet") == 0) {
		if(action & RC_SERVICE_STOP) stop_dsl_autodet();
		if(action & RC_SERVICE_START) start_dsl_autodet();
	}
	else if (strcmp(script, "dsl_diag") == 0) {
		if(action & RC_SERVICE_STOP) stop_dsl_diag();
		if(action & RC_SERVICE_START) start_dsl_diag();
	}
#endif
#endif
	else if (strcmp(script, "wan_line") == 0) {
	_dprintf("%s: restart_wan_line: %s.\n", __FUNCTION__, cmd[1]);
		if(cmd[1]) {
			int wan_unit = atoi(cmd[1]);
			char *current_ifname = get_wan_ifname(wan_unit);

			wan_up(current_ifname);
		}
	}
#ifdef CONFIG_BCMWL5
	else if (strcmp(script, "nas") == 0) {
		if(action & RC_SERVICE_STOP) stop_nas();
		if(action & RC_SERVICE_START) {
			start_eapd();
			start_nas();
			start_wps();
#ifdef RTCONFIG_BCMWL6
			start_igmp_proxy();
#ifdef BCM_BSD
			start_bsd();
#endif
#ifdef BCM_SSD
			start_ssd();
#endif
#if defined(BCM_EVENTD)
			start_eventd();
#endif
#if defined(RTCONFIG_DHDAP)
			start_dhd_monitor();
#endif
			start_acsd();
#ifdef BCM_ASPMD
			start_aspmd();
#endif
#endif
			start_wl();
			lanaccess_wl();
#ifdef RTCONFIG_BCMWL6
#ifdef RTCONFIG_HSPOT
			start_hspotap();
#endif
#endif
		}
	}
#endif
#ifdef RTCONFIG_USB
	else if (strcmp(script, "nasapps") == 0)
	{
		if(action&RC_SERVICE_STOP){
//_dprintf("restart_nas_services(%d): test 10.\n", getpid());
			restart_nas_services(1, 0);
		}
		if(action&RC_SERVICE_START){
			stop_upnp();
//_dprintf("restart_nas_services(%d): test 11.\n", getpid());
			restart_nas_services(0, 1);
			start_upnp();
		}
	}
#if defined(RTCONFIG_SAMBASRV) && defined(RTCONFIG_FTP)
	else if (strcmp(script, "ftpsamba") == 0)
	{
		if(action & RC_SERVICE_STOP) {
			stop_ftpd();
			stop_samba();
		}
		if(action & RC_SERVICE_START) {
			start_dnsmasq();	// this includes stop_dnsmasq
			setup_passwd();
			set_hostname();
			start_samba();
			start_ftpd();
		}
	}
#endif
#ifdef RTCONFIG_FTP
	else if (strcmp(script, "ftpd") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_ftpd();
		if(action & RC_SERVICE_START) start_ftpd();

		/* for security concern, even if you stop ftp daemon, it is better to restart firewall to clean FTP port: 21. */
		start_firewall(wan_primary_ifunit(), 0);
	}
#endif
	else if (strcmp(script, "ftpd_force") == 0)
	{
		nvram_set("st_ftp_force_mode", nvram_safe_get("st_ftp_mode"));
		nvram_commit();

		if(action & RC_SERVICE_STOP) stop_ftpd();
		if(action & RC_SERVICE_START) start_ftpd();

		/* for security concern, even if you stop ftp daemon, it is better to restart firewall to clean FTP port: 21. */
		start_firewall(wan_primary_ifunit(), 0);
	}
#ifdef RTCONFIG_SAMBASRV
	else if (strcmp(script, "samba") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_samba();
		if(action & RC_SERVICE_START) {
			start_dnsmasq();	// this includes stop_dnsmasq
			start_samba();
		}
	}
	else if (strcmp(script, "samba_force") == 0)
	{
		nvram_set("st_samba_force_mode", nvram_safe_get("st_samba_mode"));
		nvram_commit();

		if(action & RC_SERVICE_STOP) stop_samba();
		if(action & RC_SERVICE_START) {
			start_dnsmasq();	// this includes stop_dnsmasq
			start_samba();
		}
	}
#endif
#ifdef RTCONFIG_WEBDAV
	else if (strcmp(script, "webdav") == 0)
	{
		if(action & RC_SERVICE_STOP){
			stop_webdav();
		}
		if(action & RC_SERVICE_START) {
			stop_upnp();
			start_firewall(wan_primary_ifunit(), 0);
			start_webdav();
			start_upnp();
		}
	}
#else
	else if (strcmp(script, "webdav") == 0){
		if(f_exists("/opt/etc/init.d/S50aicloud"))
			system("sh /opt/etc/init.d/S50aicloud scan");
	}
	else if (strcmp(script, "setting_webdav") == 0){
		if(f_exists("/opt/etc/init.d/S50aicloud"))
			system("sh /opt/etc/init.d/S50aicloud restart");
	}
#endif
	else if (strcmp(script, "enable_webdav") == 0)
	{
		stop_upnp();
		stop_ddns();
#ifdef RTCONFIG_WEBDAV
		stop_webdav();
#endif
		start_firewall(wan_primary_ifunit(), 0);
		start_webdav();
		start_ddns();
		start_upnp();

	}
#ifdef RTCONFIG_TUNNEL
	else if (strcmp(script, "aae") == 0)
	{
		if(action&RC_SERVICE_STOP){
			stop_mastiff();
		}
		if(action&RC_SERVICE_START) {
			start_mastiff();
		}
	}
#endif

//#endif
//#ifdef RTCONFIG_CLOUDSYNC
	else if (strcmp(script, "cloudsync") == 0)
	{
#ifdef RTCONFIG_CLOUDSYNC
		int fromUI = 0;

		if(action & RC_SERVICE_STOP && action & RC_SERVICE_START)
			fromUI = 1;

		if(action&RC_SERVICE_STOP){
			if(cmd[1])
				stop_cloudsync(atoi(cmd[1]));
			else
				stop_cloudsync(-1);
		}
		if(action & RC_SERVICE_START) start_cloudsync(fromUI);
#else
		system("sh /opt/etc/init.d/S50smartsync restart");
#endif
	}
#ifdef RTCONFIG_WTFAST
	else if (strcmp(script, "wtfast") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_wtfast();
		if(action & RC_SERVICE_START) start_wtfast();
	}
	else if(strcmp(script, "wtfast_rule") == 0){
		//_dprintf("send SIGHUP to wtfast_rule SIGHUP = %d\n", SIGHUP);
		killall("wtfslhd", SIGHUP);
	}
#endif
//#endif
#ifdef RTCONFIG_USB_PRINTER
	else if (strcmp(script, "lpd") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_lpd();
		if(action & RC_SERVICE_START) start_lpd();
	}
	else if (strcmp(script, "u2ec") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_u2ec();
		if(action & RC_SERVICE_START) start_u2ec();
	}
#endif
#ifdef RTCONFIG_MEDIA_SERVER
	else if (strcmp(script, "media") == 0)
	{
		if(action & RC_SERVICE_STOP) {
			force_stop_dms();
			stop_mt_daapd();
		}
		if(action & RC_SERVICE_START) {
			start_dms();
			start_mt_daapd();
		}
	}
	else if (strcmp(script, "dms") == 0)
	{
		if(action & RC_SERVICE_STOP) force_stop_dms();
		if(action & RC_SERVICE_START) start_dms();
	}
	else if (strcmp(script, "mt_daapd") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_mt_daapd();
		if(action & RC_SERVICE_START) start_mt_daapd();
	}
#endif
#ifdef RTCONFIG_DISK_MONITOR
	else if (strcmp(script, "diskmon")==0)
	{
		if(action & RC_SERVICE_STOP) stop_diskmon();
		if(action & RC_SERVICE_START) start_diskmon();
	}
	else if (strcmp(script, "diskscan")==0)
	{
		if(action & RC_SERVICE_START)
			kill_pidfile_s("/var/run/disk_monitor.pid", SIGUSR2);
	}
#endif
	else if(!strncmp(script, "apps_", 5))
	{
		if(action & RC_SERVICE_START) {
			if(strcmp(script, "apps_update")==0)
				strcpy(nvtmp, "app_update.sh");
			else if(strcmp(script, "apps_stop")==0)
				strcpy(nvtmp, "app_stop.sh");
			else if(strcmp(script, "apps_upgrade")==0)
				strcpy(nvtmp, "app_upgrade.sh");
			else if(strcmp(script, "apps_install")==0)
				strcpy(nvtmp, "app_install.sh");
			else if(strcmp(script, "apps_remove")==0)
				strcpy(nvtmp, "app_remove.sh");
			else if(strcmp(script, "apps_enable")==0)
				strcpy(nvtmp, "app_set_enabled.sh");
			else if(strcmp(script, "apps_switch")==0)
				strcpy(nvtmp, "app_switch.sh");
			else if(strcmp(script, "apps_cancel")==0)
				strcpy(nvtmp, "app_cancel.sh");
			else strcpy(nvtmp, "");

			if(strlen(nvtmp) > 0) {
				nvram_set("apps_state_autorun", "");
				nvram_set("apps_state_install", "");
				nvram_set("apps_state_remove", "");
				nvram_set("apps_state_switch", "");
				nvram_set("apps_state_stop", "");
				nvram_set("apps_state_enable", "");
				nvram_set("apps_state_update", "");
				nvram_set("apps_state_upgrade", "");
				nvram_set("apps_state_cancel", "");
				nvram_set("apps_state_error", "");

				free_caches(FREE_MEM_PAGE, 1, 0);

				cmd[0] = nvtmp;
				start_script(count, cmd);
			}
		}
	}
#ifdef RTCONFIG_USB_MODEM
	else if(!strncmp(script, "simauth", 7)){
		char *at_cmd[] = {"modem_status.sh", "simauth", NULL};

		_eval(at_cmd, NULL, 0, NULL);
	}
	else if(!strncmp(script, "simpin", 6)){
		char pincode[8];
		char *at_cmd[] = {"modem_status.sh", "simpin", pincode, NULL};
		char *at_cmd2[] = {"modem_status.sh", "simauth", NULL};

		if(nvram_get_int("usb_modem_act_sim") == 2){
			snprintf(pincode, 8, "%s", cmd[1]);

			_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
			_eval(at_cmd2, NULL, 0, NULL);
		}
	}
	else if(!strncmp(script, "simpuk", 6)){
		char pukcode[10], pincode[8];
		char *at_cmd[] = {"modem_status.sh", "simpuk", pukcode, pincode, NULL};
		char *at_cmd2[] = {"modem_status.sh", "simauth", NULL};

		if(nvram_get_int("usb_modem_act_sim") == 3){
			snprintf(pukcode, 10, "%s", cmd[1]);
			snprintf(pincode, 8, "%s", cmd[2]);

			_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
			_eval(at_cmd2, NULL, 0, NULL);
		}
	}
	else if(!strncmp(script, "lockpin", 7)){
		char lock[4], pincode[8];
		char *at_cmd[] = {"modem_status.sh", "lockpin", lock, pincode, NULL};
		char *at_cmd2[] = {"modem_status.sh", "simauth", NULL};

		if(nvram_get_int("usb_modem_act_sim") == 1){
			snprintf(pincode, 8, "%s", cmd[1]);

			if(action & RC_SERVICE_STOP){ // unlock
				snprintf(lock, 4, "%s", "0");
				_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
				_eval(at_cmd2, NULL, 0, NULL);
			}
			else if(action & RC_SERVICE_START){ // lock
				snprintf(lock, 4, "%s", "1");
				_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
				_eval(at_cmd2, NULL, 0, NULL);
			}
		}
	}
	else if(!strncmp(script, "pwdpin", 6)){
		char pincode[8], pincode_new[8];
		char *at_cmd[] = {"modem_status.sh", "pwdpin", pincode, pincode_new, NULL};
		char *at_cmd2[] = {"modem_status.sh", "simauth", NULL};

		if(nvram_get_int("usb_modem_act_sim") == 1){
			snprintf(pincode, 8, "%s", cmd[1]);
			snprintf(pincode_new, 8, "%s", cmd[2]);

			_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
			_eval(at_cmd2, NULL, 0, NULL);
		}
	}
	else if(!strncmp(script, "modemscan", 9)){
		char *at_cmd[] = {"modem_status.sh", "scan", NULL};
		int usb_unit;
#ifdef RTCONFIG_DUALWAN
		char word[256], *next;

		usb_unit = 0;
		foreach(word, nvram_safe_get("wans_dualwan"), next){
			if(!strcmp(word, "usb")){
				break;
			}

			++usb_unit;
		}
#else
		usb_unit = 1;
#endif

		if(usb_unit != WAN_UNIT_MAX){
			nvram_set("usb_modem_act_scanning", "3");

			stop_wan_if(usb_unit);
			start_wan_if(usb_unit);

			_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
		}
	}
	else if(!strncmp(script, "modemsta", 8)){
		char isp[32];
		char *at_cmd[] = {"modem_status.sh", "station", isp, NULL};

		snprintf(isp, 32, "%s", nvram_safe_get("modem_roaming_isp"));

		if(strlen(isp) > 0)
			_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
	else if(!strncmp(script, "sendSMS", 7)){
		char phone[32], message[PATH_MAX];
		char *at_cmd[] = {"modem_status.sh", "send_sms", phone, message, NULL};

		snprintf(phone, 32, "%s", nvram_safe_get("modem_sms_phone"));
		if(!strcmp(cmd[1], "alert"))
			snprintf(message, PATH_MAX, "%s %s bytes.", nvram_safe_get("modem_sms_message1"), nvram_safe_get("modem_bytes_data_warning"));
		else
			snprintf(message, PATH_MAX, "%s %s bytes.", nvram_safe_get("modem_sms_message2"), nvram_safe_get("modem_bytes_data_limit"));

#ifdef RTCONFIG_INTERNAL_GOBI
		stop_lteled();
#endif
		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
#ifdef RTCONFIG_INTERNAL_GOBI
		start_lteled();
#endif
	}
#if defined(RTCONFIG_JFFS2) || defined(RTCONFIG_BRCM_NAND_JFFS2) || defined(RTCONFIG_UBIFS)
	else if(!strncmp(script, "datacount", 9)){
		char *at_cmd[] = {"modem_status.sh", "bytes", NULL};

		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
	else if(!strncmp(script, "resetcount", 10)){
		time_t now;
		char timebuf[32];
		char *at_cmd[] = {"modem_status.sh", "bytes-", NULL};

		time(&now);
		snprintf(timebuf, 32, "%d", (int)now);
		nvram_set("modem_bytes_data_start", timebuf);

		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
	else if(!strncmp(script, "sim_del", 7)){
		char sim_order[32];
		char *at_cmd[] = {"modem_status.sh", "imsi_del", sim_order, NULL};

		snprintf(sim_order, 32, "%s", cmd[1]);

		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
	else if(!strncmp(script, "set_dataset", 11)){
		char *at_cmd[] = {"modem_status.sh", "set_dataset", NULL};

		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
#endif
#ifdef RTCONFIG_INTERNAL_GOBI
	else if(!strncmp(script, "simdetect", 9)){
		// Need to reboot after this.
		char buf[4];
		char *at_cmd1[] = {"modem_status.sh", "simdetect", NULL};
		char *at_cmd2[] = {"modem_status.sh", "simdetect", buf, NULL};

		if(cmd[1]){
			snprintf(buf, 4, "%s", cmd[1]);
			_eval(at_cmd2, ">/tmp/modem_action.ret", 0, NULL);
		}
		else
			_eval(at_cmd1, ">/tmp/modem_action.ret", 0, NULL);
	}
	else if(!strncmp(script, "getband", 7)){
		char *at_cmd[] = {"modem_status.sh", "band", NULL};

		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
	else if(!strncmp(script, "setband", 7)){
		char buf[8];
		char *at_cmd[] = {"modem_status.sh", "setband", buf, NULL};

		snprintf(buf, 8, "%s", nvram_safe_get("modem_lte_band"));
		if(strlen(buf) <= 0)
			snprintf(buf, 8, "%s", "auto");

		_eval(at_cmd, ">/tmp/modem_action.ret", 0, NULL);
	}
#endif
#endif // RTCONFIG_USB_MODEM
#endif // RTCONFIG_USB
	else if(!strncmp(script, "webs_", 5) || !strncmp(script, "gobi_", 5))
	{
		if(action & RC_SERVICE_START) {
#ifdef DEBUG_RCTEST // Left for UI debug
			char *webscript_dir;
			webscript_dir = nvram_safe_get("webscript_dir");
			if(strlen(webscript_dir))
				sprintf(nvtmp, "%s/%s.sh", webscript_dir, script);
			else
#endif
			sprintf(nvtmp, "%s.sh", script);
			cmd[0] = nvtmp;
			start_script(count, cmd);
		}
	}
	else if (strcmp(script, "ddns") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_ddns();
		if(action & RC_SERVICE_START) start_ddns();
	}
	else if (strcmp(script, "aidisk_asusddns_register") == 0)
	{
		asusddns_reg_domain(0);
	}
	else if (strcmp(script, "adm_asusddns_register") == 0)
	{
		asusddns_reg_domain(1);
	}
	else if (strcmp(script, "httpd") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_httpd();
		if(action & RC_SERVICE_START){
			start_httpd();
#ifdef RTCONFIG_BWDPI
			setup_wrs_conf();
#endif
		}
	}
#ifdef RTCONFIG_IPV6
	else if (strcmp(script, "ipv6") == 0) {
		if (action & RC_SERVICE_STOP)
			stop_ipv6();
		if (action & RC_SERVICE_START)
			start_ipv6();
	}
	else if (strcmp(script, "dhcp6c") == 0) {
		if (action & RC_SERVICE_STOP)
			stop_dhcp6c();
		if (action & RC_SERVICE_START)
			start_dhcp6c();
	}
	else if (strcmp(script, "wan6") == 0) {
		if (action & RC_SERVICE_STOP) {
			stop_wan6();
			stop_ipv6();
		}
		if (action & RC_SERVICE_START) {
			start_ipv6();
			// when no option from ipv4, restart wan entirely
			if(update_6rd_info()==0)
			{
				stop_wan_if(wan_primary_ifunit());
				start_wan_if(wan_primary_ifunit());
			}
			else
			{
				start_wan6();
			}
		}
	}
#endif
	else if (strcmp(script, "dns") == 0)
	{
		if(action & RC_SERVICE_START) reload_dnsmasq();
	}
	else if (strcmp(script, "dnsmasq") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_dnsmasq();
		if(action & RC_SERVICE_START) start_dnsmasq();
	}
#ifdef RTCONFIG_DHCP_OVERRIDE
	else if (strcmp(script, "dhcpd") == 0)
	{
		if (nvram_match("dhcp_enable_x", "0")) {
			// disable dhcp server
			if (nvram_match("dnsqmode", "2")) {
				nvram_set("dnsqmode", "1");
				restart_dnsmasq(0);
			}
		}
		else {
			// enable dhcp server
			if (nvram_match("dnsqmode", "1")) {
				nvram_set("dnsqmode", "2");
				restart_dnsmasq(0);
			}
		}

	}
#endif
	else if (strcmp(script, "upnp") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_upnp();
		if(action & RC_SERVICE_START) start_upnp();
	}
	else if (strcmp(script, "qos") == 0)
	{
		if(action&RC_SERVICE_STOP) {
			stop_iQos();
#ifdef RTCONFIG_BWDPI
			stop_dpi_engine_service(0);
#endif
			del_iQosRules();
		}
		if(action & RC_SERVICE_START) {
			reinit_hwnat(-1);
			add_iQosRules(get_wan_ifname(wan_primary_ifunit()));
#ifdef RTCONFIG_BWDPI
			start_dpi_engine_service();
			// force to rebuild firewall to avoid some loopback issue
			if (nvram_match("fw_nat_loopback", "2"))
				start_firewall(wan_primary_ifunit(), 0);
#endif
			start_iQos();
		}
	}
#ifdef RTCONFIG_BWDPI
	else if (strcmp(script, "wrs") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_dpi_engine_service(0);
		if(action & RC_SERVICE_START) {
			start_dpi_engine_service();
			// force to rebuild firewall to avoid some loopback issue
			if (nvram_match("fw_nat_loopback", "2"))
				start_firewall(wan_primary_ifunit(), 0);
		}
	}
	else if (strcmp(script, "wrs_force") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_dpi_engine_service(1);
	}
	else if (strcmp(script, "sig_check") == 0)
	{
		if(action & RC_SERVICE_START){
			eval("sig_update.sh");
			if(nvram_get_int("sig_state_flag")) eval("sig_upgrade.sh", "1");
			stop_dpi_engine_service(0);
			start_dpi_engine_service();
		}
	}
#endif
	else if (strcmp(script, "traffic_analyzer") == 0)
	{
		// only stop service need to save database
		if(action & RC_SERVICE_STOP) hm_traffic_analyzer_save();
	}
#ifdef RTCONFIG_TRAFFIC_LIMITER
	else if (strcmp(script, "reset_traffic_limiter") == 0)
	{
		hm_traffic_limiter_save();
		reset_traffic_limiter_counter(0);
	}
	else if (strcmp(script, "reset_tl_count") == 0)
	{
		eval("echo", "-n", "0", ">", "/tmp/tl_count");
	}
#endif
#ifdef RTCONFIG_PUSH_EMAIL
	else if (strcmp(script, "send_confirm_mail") == 0)
	{
		am_send_mail(0x4, NULL);
	}
#endif
	else if (strcmp(script, "logger") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_logger();
		if(action & RC_SERVICE_START) start_logger();
	}
#ifdef RTCONFIG_CROND
	else if (strcmp(script, "crond") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_cron();
		if(action & RC_SERVICE_START) start_cron();
	}
#endif
	else if (strcmp(script, "firewall") == 0)
	{
		if(action & RC_SERVICE_START)
		{
//			char wan_ifname[16];

			reinit_hwnat(-1);
			// multiple instance is handled, but 0 is used
			start_default_filter(0);

#ifdef WEB_REDIRECT
			// handled in start_firewall already
			// redirect_setting();
#endif

#ifdef RTCONFIG_PARENTALCTRL
			start_pc_block();
#endif

			// TODO handle multiple wan
			//start_firewall(get_wan_ifname(0, wan_ifname), nvram_safe_get("wan0_ipaddr"), "br0", nvram_safe_get("lan_ipaddr"));
			start_firewall(wan_primary_ifunit(), 0);
		}
	}
	else if (strcmp(script, "iptrestore") == 0)
	{
		// center control for iptable restore, called by process out side of rc
		_dprintf("%s: restart_iptrestore: %s.\n", __FUNCTION__, cmd[1]);
		if(cmd[1]) {
			if(action&RC_SERVICE_START){
				for ( i = 1; i <= 5; i++ ) {
					if (eval("iptables-restore", cmd[1])) {
						_dprintf("iptables-restore failed - attempt: %d ...\n", i);
						sleep(1);
					} else {
						i = 6;
					}
				}
			}
		}
	}
	else if (strcmp(script, "pppoe_relay") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_pppoe_relay();
		if(action & RC_SERVICE_START) start_pppoe_relay(get_wanx_ifname(wan_primary_ifunit()));
	}
	else if (strcmp(script, "ntpc") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_ntpc();
		if(action & RC_SERVICE_START) start_ntpc();
	}
	else if (strcmp(script, "rebuild_cifs_config_and_password") ==0)
	{
		fprintf(stderr, "rc rebuilding CIFS config and password databases.\n");
//		regen_passwd_files(); /* Must be called before regen_cifs_config_file(). */
//		regen_cifs_config_file();
	}
	else if (strcmp(script, "time") == 0)
	{
		if(action & RC_SERVICE_STOP) {
			stop_hour_monitor_service();
			stop_telnetd();
#ifdef RTCONFIG_SSH
			stop_sshd();
#endif
			stop_logger();
			//stop_httpd();
		}
		if(action & RC_SERVICE_START) {
			refresh_ntpc();
			start_logger();
			start_telnetd();
#ifdef RTCONFIG_SSH
			start_sshd();
#endif
			//start_httpd();
			start_firewall(wan_primary_ifunit(), 0);
			start_hour_monitor_service();
		}
	}
	else if (strcmp(script, "wps_method")==0)
	{
		if(action & RC_SERVICE_STOP) {
			stop_wps_method();
			if(!nvram_match("wps_ign_btn", "1"))
				kill_pidfile_s("/var/run/watchdog.pid", SIGUSR2);
		}
		if(action & RC_SERVICE_START) {
			if (!wps_band_radio_off(get_radio_band(nvram_get_int("wps_band"))) &&
			    !wps_band_ssid_broadcast_off(get_radio_band(nvram_get_int("wps_band")))) {
				start_wps_method();
				if(!nvram_match("wps_ign_btn", "1"))
					kill_pidfile_s("/var/run/watchdog.pid", SIGUSR1);
				else
					kill_pidfile_s("/var/run/watchdog.pid", SIGTSTP);
			}
			nvram_unset("wps_ign_btn");
		}
	}
	else if (strcmp(script, "reset_wps")==0)
	{
		reset_wps();
		kill_pidfile_s("/var/run/watchdog.pid", SIGUSR2);
	}
	else if (strcmp(script, "wps")==0)
	{
		if(action & RC_SERVICE_STOP) stop_wps();
		if(action & RC_SERVICE_START) start_wps();
		kill_pidfile_s("/var/run/watchdog.pid", SIGUSR2);
	}
	else if (strcmp(script, "autodet")==0)
	{
		if(action & RC_SERVICE_STOP) stop_autodet();
		if(action & RC_SERVICE_START) start_autodet();
	}
#ifdef RTCONFIG_QCA_PLC_UTILS
	else if (strcmp(script, "plcdet")==0)
	{
		if(action & RC_SERVICE_STOP) stop_plcdet();
		if(action & RC_SERVICE_START) start_plcdet();
	}
#endif
#if defined(CONFIG_BCMWL5)|| (defined(RTCONFIG_RALINK) && defined(RTCONFIG_WIRELESSREPEATER)) || defined(RTCONFIG_QCA)
	else if (strcmp(script, "wlcscan")==0)
	{
		if(action & RC_SERVICE_STOP) stop_wlcscan();
		if(action & RC_SERVICE_START) start_wlcscan();
	}
#endif
#ifdef RTCONFIG_WIRELESSREPEATER
	else if (strcmp(script, "wlcconnect")==0)
	{
		if(action & RC_SERVICE_STOP) stop_wlcconnect();

#ifdef WEB_REDIRECT
		_dprintf("%s: notify wanduck: wlc_state=%d.\n", __FUNCTION__, nvram_get_int("wlc_state"));
		// notify the change to wanduck.
		kill_pidfile_s("/var/run/wanduck.pid", SIGUSR1);
#endif

		if(action & RC_SERVICE_START) {
			restart_wireless();
			sleep(1);
			start_wlcconnect();
		}
	}
	else if (strcmp(script, "wlcmode")==0)
	{
		if(cmd[1]&& (atoi(cmd[1]) != nvram_get_int("wlc_mode"))) {
			nvram_set_int("wlc_mode", atoi(cmd[1]));
			if(nvram_match("lan_proto", "dhcp") && atoi(cmd[1])==0) {
				nvram_set("lan_ipaddr", nvram_default_get("lan_ipaddr"));
			}

#if defined(RTCONFIG_SAMBASRV) && defined(RTCONFIG_FTP)
			stop_ftpd();
			stop_samba();
#endif

#ifdef RTCONFIG_USB_PRINTER
			stop_u2ec();
#endif
			stop_networkmap();
			stop_httpd();
			stop_dnsmasq();
			stop_lan_wlc();
			stop_lan_port();
			stop_lan_wlport();
			start_lan_wlport();
			start_lan_port(8);
			start_lan_wlc();
			start_dnsmasq();
			start_httpd();
			start_networkmap(0);
#ifdef RTCONFIG_USB_PRINTER
			start_u2ec();
#endif

#if defined(RTCONFIG_SAMBASRV) && defined(RTCONFIG_FTP)
			setup_passwd();
			start_samba();
			start_ftpd();
#endif
		}
	}
#endif
	else if (strcmp(script, "restore") == 0) {
		if(cmd[1]) restore_defaults_module(cmd[1]);
	}
	else if (strcmp(script, "chpass") == 0) {
			setup_passwd();
	}
#if RTCONFIG_SPEEDTEST
	else if (strcmp(script, "speedtest") == 0) {
		wan_bandwidth_detect();
	}
#endif
	// handle button action
	else if (strcmp(script, "wan_disconnect")==0) {
		logmessage("wan", "disconnected manually");
		stop_upnp();
		stop_wan();
	}
	else if (strcmp(script,"wan_connect")==0)
	{
		logmessage("wan", "connected manually");

		rename("/tmp/ppp/log", "/tmp/ppp/log.~");
		start_wan();
		sleep(2);
		// TODO: function to force ppp connection
		start_upnp();
	}
#if defined(RTCONFIG_PPTPD) || defined(RTCONFIG_ACCEL_PPTPD)
	else if (strcmp(script, "pptpd") == 0)
	{
		if (action & RC_SERVICE_STOP)
		{
			stop_pptpd();
		}
		if (action & RC_SERVICE_START)
		{
			start_pptpd();
			start_firewall(wan_primary_ifunit(), 0);
		}
	}
#endif

#ifdef RTCONFIG_SNMPD
	else if (strcmp(script, "snmpd") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_snmpd();
		if(action & RC_SERVICE_START) {
			start_snmpd();
			start_firewall(wan_primary_ifunit(), 0);
		}
	}
#endif

#ifdef RTCONFIG_OPENVPN
	else if (strncmp(script, "vpnclient", 9) == 0) {
		if (action & RC_SERVICE_STOP) stop_vpnclient(atoi(&script[9]));
		if (action & RC_SERVICE_START) start_vpnclient(atoi(&script[9]));
	}
	else if (strncmp(script, "vpnserver" ,9) == 0) {
		if (action & RC_SERVICE_STOP) stop_vpnserver(atoi(&script[9]));
		if (action & RC_SERVICE_START) start_vpnserver(atoi(&script[9]));
	}
	else if (strncmp(script, "vpnrouting" ,10) == 0) {
		if (action & RC_SERVICE_START) update_vpnrouting(atoi(&script[10]));
	}
#endif
#if defined(RTCONFIG_PPTPD) || defined(RTCONFIG_ACCEL_PPTPD)
	else if (strcmp(script, "vpnd") == 0)
	{
		if (action & RC_SERVICE_STOP){
			stop_pptpd();
		}
		if (action & RC_SERVICE_START){
			stop_upnp();
			start_pptpd();
			start_firewall(wan_primary_ifunit(), 0);
			start_upnp();
		}
	}
#endif
#if defined(RTCONFIG_OPENVPN)
	else if (strcmp(script, "openvpnd") == 0)
	{
		int openvpn_unit = nvram_get_int("vpn_server_unit");
		if (action & RC_SERVICE_STOP){
			stop_vpnserver(openvpn_unit);
		}
		if (action & RC_SERVICE_START){
 			start_vpnserver(openvpn_unit);
 		}
 	}
#endif
#ifdef RTCONFIG_YANDEXDNS
	else if (strcmp(script, "yadns") == 0)
	{
		if (action & RC_SERVICE_STOP)
			stop_dnsmasq();
		if (action & RC_SERVICE_START) {
			update_resolvconf();
 			start_dnsmasq();
 		}
		start_firewall(wan_primary_ifunit(), 0);
	}
#endif
#ifdef RTCONFIG_DNSFILTER
	else if (strcmp(script, "dnsfilter") == 0)
	{
		if(action & RC_SERVICE_START) {
			start_dnsmasq();
			start_firewall(wan_primary_ifunit(), 0);
		}
	}
#endif
#ifdef RTCONFIG_ISP_METER
	else if (strcmp(script, "isp_meter") == 0) {
		_dprintf("%s: isp_meter: %s\n", __FUNCTION__, cmd[1]);
		if(strcmp(cmd[1], "down")==0) {
			stop_wan_if(0);
			update_wan_state("wan0_", WAN_STATE_STOPPED, WAN_STOPPED_REASON_METER_LIMIT);
		}
		else if(strcmp(cmd[1], "up")==0) {
			_dprintf("notify wan up!\n");
			start_wan_if(0);
		}
	}
#endif
#ifdef RTCONFIG_TIMEMACHINE
	else if (strcmp(script, "timemachine") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_timemachine();
		if(action & RC_SERVICE_START) start_timemachine();
	}
	else if (strcmp(script, "afpd") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_afpd();
		if(action & RC_SERVICE_START) start_afpd();
	}
	else if (strcmp(script, "cnid_metad") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_cnid_metad();
		if(action & RC_SERVICE_START) start_cnid_metad();
	}
#endif
#if defined(RTCONFIG_MDNS)
	else if (strcmp(script, "mdns") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_mdns();
		if(action & RC_SERVICE_START) start_mdns();
	}
#endif

#ifdef RTCONFIG_PUSH_EMAIL
	else if (strcmp(script, "sendmail") == 0)
	{
		start_DSLsendmail();
	}
#ifdef RTCONFIG_DSL_TCLINUX
	else if (strcmp(script, "DSLsenddiagmail") == 0)
	{
		start_DSLsenddiagmail();
	}
#endif
#endif

#ifdef RTCONFIG_VPNC
	else if (strcmp(script, "vpncall") == 0)
	{
#if defined(RTCONFIG_OPENVPN)
		char buf[32] = {0};
		int i;
		int openvpnc_unit = nvram_get_int("vpn_client_unit");
#endif
		if (action & RC_SERVICE_STOP){
			stop_vpnc();
#if defined(RTCONFIG_OPENVPN)
			for( i = 1; i <= MAX_OVPN_CLIENT; i++ )
			{
				sprintf(buf, "vpnclient%d", i);
				if ( pidof(buf) >= 0 )
				{
					stop_vpnclient(i);
				}
			}
#endif
		}

		if (action & RC_SERVICE_START){
#if defined(RTCONFIG_OPENVPN)
			if(nvram_match("vpnc_proto", "openvpn")){
				if (check_ovpn_client_enabled(openvpnc_unit)) {
					start_vpnclient(openvpnc_unit);
				}
				stop_vpnc();
			}
			else{
				for( i = 1; i <= MAX_OVPN_CLIENT; i++ )
				{
					sprintf(buf, "vpnclient%d", i);
					if ( pidof(buf) >= 0 )
					{
						stop_vpnclient(i);
					}
				}
#endif
				start_vpnc();
#if defined(RTCONFIG_OPENVPN)
			}
#endif
		}
	}
#endif
#ifdef RTCONFIG_TR069
	else if (strncmp(script, "tr", 2) == 0) {
		if (action & RC_SERVICE_STOP) stop_tr();
		if (action & RC_SERVICE_START) start_tr();
	}
#endif
	else if (strcmp(script, "sh") == 0) {
		_dprintf("%s: shell: %s\n", __FUNCTION__, cmd[1]);
		if(cmd[1]) system(cmd[1]);
	}

	else if (strcmp(script, "rstats") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_rstats();
		if(action & RC_SERVICE_START) restart_rstats();
	}
        else if (strcmp(script, "cstats") == 0)
        {
                if(action & RC_SERVICE_STOP) stop_cstats();
                if(action & RC_SERVICE_START) restart_cstats();
        }
	else if (strcmp(script, "conntrack") == 0)
	{
		setup_conntrack();
		setup_udp_timeout(TRUE);
//            start_firewall(wan_primary_ifunit(), 0);
	}
#ifdef RTCONFIG_USB
#ifdef LINUX26
        else if (strcmp(script, "sdidle") == 0) {
                if(action & RC_SERVICE_STOP){
                        stop_sd_idle();
                }
                if(action & RC_SERVICE_START){
                        start_sd_idle();
                }
	}
#endif
#endif
	else if (strcmp(script, "leds") == 0) {
		setup_leds();
	}
	else if (strcmp(script, "updateresolv") == 0) {
		update_resolvconf();
	}
	else if (strcmp(script, "app") == 0) {
#if defined(RTCONFIG_APP_PREINSTALLED) || defined(RTCONFIG_APP_NETINSTALLED) || defined(RTCONFIG_APP_NOLOCALDM)
		if(action & RC_SERVICE_STOP)
			stop_app();
#endif
	}
#ifdef RTCONFIG_USBRESET
	else if (strcmp(script, "usbreset") == 0) {
#define MAX_USBRESET_NUM 5
		char reset_seconds[] = {2, 4, 6, 8, 10};
		char *usbreset_active = nvram_safe_get("usbreset_active");
		char *usbreset_num = nvram_safe_get("usbreset_num");
		char buf[4];
		int reset_num = 0;
_dprintf("test 1. usbreset_active=%s, usbreset_num=%s.\n", usbreset_active, usbreset_num);

		if(strlen(usbreset_num) > 0 && strlen(usbreset_active) > 0 && strcmp(usbreset_active, "0")){
			reset_num = atoi(usbreset_num);
			if(reset_num < MAX_USBRESET_NUM){
				stop_usb_program(1);

_dprintf("test 2. turn off the USB power during %d seconds.\n", reset_seconds[reset_num]);
				set_pwr_usb(0);
				sleep(reset_seconds[reset_num]);

				++reset_num;
				memset(buf, 0, 4);
				sprintf(buf, "%d", reset_num);
				nvram_set("usbreset_num", buf);
				nvram_set("usbreset_active", "0");

				set_pwr_usb(1);
			}
		}
	}
#endif
#if defined (RTCONFIG_USB_XHCI)
#ifdef RTCONFIG_XHCIMODE
	else if(!strcmp(script, "xhcimode")){
		char param[32];
		int usb2enable = nvram_get_int("usb_usb2");
		int uhcienable = nvram_get_int("usb_uhci");
		int ohcienable = nvram_get_int("usb_ohci");
		int i;

		_dprintf("xhcimode: stop_usb_program...\n");
		stop_usb_program(1);

		_dprintf("xhcimode: remove xhci...\n");
		modprobe_r(USB30_MOD);

		if(usb2enable){
			_dprintf("xhcimode: remove ehci...\n");
			modprobe_r(USB20_MOD);
		}

		if(ohcienable){
			_dprintf("xhcimode: remove ohci...\n");
			modprobe_r(USBOHCI_MOD);
		}

		if(uhcienable){
			_dprintf("xhcimode: remove uhci...\n");
			modprobe_r(USBUHCI_MOD);
		}

		// It's necessary to wait the device being ready.
		int sec = nvram_get_int("xhcimode_waitsec");
		_dprintf("xhcimode: sleep %d second...\n", sec);
		sleep(sec);

		memset(param, 0, 32);
		sprintf(param, "usb2mode=%s", cmd[1]);
		_dprintf("xhcimode: insert xhci %s...\n", param);
		modprobe(USB30_MOD, param);

		if(usb2enable){
			i = nvram_get_int("usb_irq_thresh");
			if(i < 0 || i > 6)
				i = 0;
			memset(param, 0, 32);
			sprintf(param, "log2_irq_thresh=%d", i);
			_dprintf("xhcimode: insert ehci %s...\n", param);
			modprobe(USB20_MOD, param);
		}

		if(ohcienable){
			_dprintf("xhcimode: insert ohci...\n");
			modprobe(USBOHCI_MOD);
		}

		if(uhcienable){
			_dprintf("xhcimode: insert uhci...\n");
			modprobe(USBUHCI_MOD);
		}
	}
#endif
#endif
#ifdef RTCONFIG_JFFS2USERICON
	else if (strcmp(script, "lltdc") == 0) {
		if(action&RC_SERVICE_START) start_lltdc();
	}
#endif
#ifdef RTCONFIG_UPNPC
	else if (strcmp(script, "miniupnpc") == 0) {
		if(action&RC_SERVICE_STOP) stop_miniupnpc();
		if(action&RC_SERVICE_START) start_miniupnpc();
	}
#endif
#ifdef RTCONFIG_TOR
	else if (strcmp(script, "tor") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_Tor_proxy();
		if(action & RC_SERVICE_START) start_Tor_proxy();
		start_firewall(wan_primary_ifunit(), 0);
	}
#endif
#ifdef RTCONFIG_CLOUDCHECK
        else if(!strcmp(script, "cloudcheck")){
                if(action & RC_SERVICE_STOP) stop_cloudcheck();
                if(action & RC_SERVICE_START) start_cloudcheck();
        }
#endif
#ifdef RTCONFIG_GETREALIP
	else if(!strcmp(script, "getrealip")){
		char tmp[128], prefix[] = "wlXXXXXXXXXX_";
		int wan_unit = atoi(cmd[1]);
		char *getip[] = {"getrealip.sh", NULL};
		pid_t pid;

		snprintf(prefix, sizeof(prefix), "wan%d_", wan_unit);

		if(action & RC_SERVICE_STOP){
			nvram_set(strcat_r(prefix, "realip_state", tmp), "0");
			nvram_set(strcat_r(prefix, "realip_ip", tmp), "");
		}
		if(action & RC_SERVICE_START){
			_eval(getip, ">>/dev/null", 0, &pid);
		}
	}
#endif
#ifdef RTCONFIG_QCA_PLC_UTILS
	else if (!strcmp(script, "plc_upgrade")) {
		if (nvram_match("asus_mfg", "0"))
			save_plc_setting();
	}
#endif
#ifdef RTCONFIG_KEY_GUARD
	else if (!strcmp(script, "key_guard")) {
		start_keyguard();
	}
#endif
	else if (!strcmp(script, "eco_guard")) {
		start_ecoguard();
	}
#ifdef RTCONFIG_QUAGGA
	else if (strcmp(script, "quagga") == 0)
	{
		if(action & RC_SERVICE_STOP) stop_quagga();
		if(action & RC_SERVICE_START) start_quagga();
	}
#endif
 	else
	{
		fprintf(stderr,
			"WARNING: rc notified of unrecognized event `%s'.\n",
					script);
	}

	if(nvptr){
_dprintf("goto again(%d)...\n", getpid());
		goto again;
	}

	nvram_set("rc_service", "");
	nvram_set("rc_service_pid", "");
_dprintf("handle_notifications() end\n");
}

#if defined(CONFIG_BCMWL5) || (defined(RTCONFIG_RALINK) && defined(RTCONFIG_WIRELESSREPEATER)) || defined(RTCONFIG_QCA)
void
start_wlcscan(void)
{
	if(getpid()!=1) {
		notify_rc("start_wlcscan");
		return;
	}

	killall("wlcscan", SIGTERM);

	system("wlcscan");
}

void
stop_wlcscan(void)
{
	if(getpid()!=1) {
		notify_rc("stop_wlcscan");
		return;
	}

	killall("wlcscan", SIGTERM);
}
#endif
#ifdef RTCONFIG_WIRELESSREPEATER
void
start_wlcconnect(void)
{
	char *wlcconnect_argv[] = {"wlcconnect", NULL};
	pid_t pid;

	if(nvram_get_int("sw_mode")!=SW_MODE_REPEATER) {
		_dprintf("Not repeater mode, do not start_wlcconnect\n");
		return;
	}

	if(getpid()!=1) {
		notify_rc("start_wlcconnect");
		return;
	}

	killall("wlcconnect", SIGTERM);

	_eval(wlcconnect_argv, NULL, 0, &pid);
}

void
stop_wlcconnect(void)
{
	if(getpid()!=1) {
		notify_rc("stop_wlcconnect");
		return;
	}

	killall("wlcconnect", SIGTERM);
}
#endif

#ifdef RTCONFIG_QCA_PLC_UTILS
void
start_plcdet(void)
{
	char *autodet_argv[] = {"autodet_plc", NULL};
	pid_t pid;

	if(getpid()!=1) {
		notify_rc("start_plcdet");
		return;
	}

	killall_tk("autodet_plc");

	_eval(autodet_argv, NULL, 0, &pid);

	_dprintf("rc: ==> start_plcdet!!!\n");
	return;
}

void
stop_plcdet(void)
{
	if(getpid()!=1) {
		notify_rc("stop_plcdet");
		return;
	}

	killall_tk("autodet_plc");
	_dprintf("rc: ==> stop_plcdet!!!\n");
}
#endif

void
start_autodet(void)
{
	char *autodet_argv[] = {"autodet", NULL};
	pid_t pid;

	if(getpid()!=1) {
		notify_rc("start_autodet");
		return;
	}

	killall_tk("autodet");

	_eval(autodet_argv, NULL, 0, &pid);
}

void
stop_autodet(void)
{
	if(getpid()!=1) {
		notify_rc("stop_autodet");
		return;
	}

	killall_tk("autodet");
}

// string = S20transmission -> return value = transmission.
int get_apps_name(const char *string)
{
	char *ptr;

	if(string == NULL)
		return 0;

	if((ptr = rindex(string, '/')) != NULL)
		++ptr;
	else
		ptr = (char*) string;
	if(ptr[0] != 'S')
		return 0;
	++ptr; // S.

	while(ptr != NULL){
		if(isdigit(ptr[0]))
			++ptr;
		else
			break;
	}

	printf("%s", ptr);

	return 1;
}

int run_app_script(const char *pkg_name, const char *pkg_action)
{
	char app_name[128];

	if(pkg_action == NULL || strlen(pkg_action) <= 0)
		return -1;

	stop_upnp();
	memset(app_name, 0, 128);
	if(pkg_name == NULL)
		strcpy(app_name, "allpkg");
	else
		strcpy(app_name, pkg_name);

	doSystem("app_init_run.sh %s %s", app_name, pkg_action);

	sleep(5);
	start_upnp();

	return 0;
}

void start_nat_rules(void)
{
	int len;
	char *fn = NAT_RULES, ln[PATH_MAX];
	struct stat s;
	int i;
	int retry;

	// all rules applied directly according to currently status, wanduck help to triger those not cover by normal flow
 	if(nvram_match("x_Setting", "0")){
_dprintf("nat_rule: stop_nat_rules 2.\n");
		stop_nat_rules();
		return;
	}

	if(nvram_get_int("nat_state") == NAT_STATE_NORMAL){
_dprintf("nat_rule: skip the 2nd start_nat_rules.\n");
		return;
	}

	retry = 1;
	while(lstat(NAT_RULES, &s) || !S_ISLNK(s.st_mode) || (len = readlink(NAT_RULES, ln, sizeof(ln))) <= 0){
		if(retry > 6){
_dprintf("nat_rule: the nat rule file was gone.\n");
			return;
		}

_dprintf("nat_rule: the nat rule file was not ready. wait %d seconds...\n", retry);
		sleep(retry);
		++retry;
	}

	ln[len] = '\0';
	fn = ln;

	nvram_set_int("nat_state", NAT_STATE_NORMAL);

	_dprintf("%s: apply the nat_rules(%s)!\n", __FUNCTION__, fn);
	logmessage("start_nat_rules", "apply the nat_rules(%s)!", fn);

	setup_ct_timeout(TRUE);
	setup_udp_timeout(TRUE);

	eval("iptables-restore", NAT_RULES);
	run_custom_script("nat-start", NULL);

	return;
}

void stop_nat_rules(void)
{
	int i;

	if(nvram_match("nat_redirect_enable", "0")) return;

	if (nvram_get_int("nat_state")==NAT_STATE_REDIRECT) return ;

	nvram_set_int("nat_state", NAT_STATE_REDIRECT);

	_dprintf("%s: apply the redirect_rules!\n", __FUNCTION__);
	logmessage("stop_nat_rules", "apply the redirect_rules!");

	setup_ct_timeout(FALSE);
	setup_udp_timeout(FALSE);

	for ( i = 1; i <= 5; i++ ) {
		if (eval("iptables-restore", "/tmp/redirect_rules")) {
			_dprintf("iptables-restore failed - attempt: %d ...\n", i);
			sleep(1);
		} else {
			i = 6;
		}
	}

	return;
}

#ifdef RTCONFIG_TOAD
static void
start_toads(void)
{
	char toad_ifname[16];
	char *next;

	stop_toads();

	foreach(toad_ifname, nvram_safe_get("toad_ifnames"), next) {
		eval("/usr/sbin/toad", "-i", toad_ifname);
	}
}

static void
stop_toads(void)
{
	killall_tk("toad");
}
#endif

#if defined(BCM_BSD)
int start_bsd(void)
{
	int ret = 0;

	stop_bsd();

	if (!nvram_get_int("smart_connect_x"))
		ret = -1;
	else {
#if 0
		nvram_unset("bsd_ifnames");
#endif
		ret = eval("/usr/sbin/bsd");
	}

	return ret;
}

void stop_bsd(void)
{
	killall_tk("bsd");
}
#endif /* BCM_BSD */

#if defined(BCM_SSD)
int start_ssd(void)
{
	int ret = 0;
	char *ssd_argv[] = {"/usr/sbin/ssd", NULL};
	pid_t pid;

	stop_ssd();

	if (nvram_match("ssd_enable", "1"))
		ret = _eval(ssd_argv, NULL, 0, &pid);

	return ret;
}

void stop_ssd(void)
{
	killall_tk("ssd");
}
#endif /* BCM_SSD */

#if defined(RTCONFIG_DHDAP)
int start_dhd_monitor(void)
{
	int ret = 0;

#if defined(RTCONFIG_BCM7)
	return ret;
#endif
	killall_tk("dhd_monitor");

	nvram_set("fast_restart", "1");
	ret = eval("/usr/sbin/dhd_monitor");

	return ret;
}

int stop_dhd_monitor(void)
{
	/* Don't kill dhd_monitor here */
	return 0;
}
#endif /* RTCONFIG_DHDAP */

#ifdef RTCONFIG_INTERNAL_GOBI
int start_lteled(void)
{
	char *lteled_argv[] = {"lteled", NULL};
	pid_t pid;

	stop_lteled();

	if(nvram_get_int("lteled_down"))
		return 0;

	return _eval(lteled_argv, NULL, 0, &pid);
}

void stop_lteled(void)
{
	killall_tk("lteled");
}
#endif	/* RTCONFIG_INTERNAL_GOBI */


int
firmware_check_main(int argc, char *argv[])
{
	if(argc!=2)
		return -1;

	_dprintf("FW: %s\n", argv[1]);

#ifdef RTCONFIG_DSL
#ifdef RTCONFIG_RALINK
#else
	int isTcFwExist = 0;
	isTcFwExist = separate_tc_fw_from_trx(argv[1]);
#endif
#endif

	if(check_imagefile(argv[1])) {
		_dprintf("FW OK\n");
		nvram_set("firmware_check", "1");
	}
	else {
		_dprintf("FW Fail\n");
		nvram_set("firmware_check", "0");
	}

#ifdef RTCONFIG_DSL
#ifdef RTCONFIG_RALINK
#else
	if(isTcFwExist) {
		if(check_tc_firmware_crc()) // return 0 when pass
		{
			_dprintf("FW Fail\n");
			nvram_set("firmware_check", "0");
		}
	}
#endif
#endif

	return 0;

}

#ifdef RTCONFIG_HTTPS
int
rsasign_check_main(int argc, char *argv[])
{
	if(argc!=2)
		return -1;

	_dprintf("rsa fw: %s\n", argv[1]);

	if(check_rsasign(argv[1])) {
		_dprintf("rsasign check FW OK\n");
		nvram_set("rsasign_check", "1");
	}
	else {
		_dprintf("rsasign check FW Fail\n");
		nvram_set("rsasign_check", "0");
	}
	return 0;
}
#endif

#ifdef RTCONFIG_BWDPI
int
rsasign_sig_check_main(int argc, char *argv[])
{
	if(argc!=2)
		return -1;

	_dprintf("rsa fw: %s\n", argv[1]);

#ifdef RTCONFIG_HTTPS
	if(check_rsasign(argv[1])) {
		_dprintf("rsasign check sig OK\n");
		nvram_set("bwdpi_rsa_check", "1");
	}
	else
#endif
	{
		_dprintf("rsasign check sig Fail\n");
		nvram_set("bwdpi_rsa_check", "0");
	}
	return 0;
}
#endif

#ifdef RTCONFIG_DSL_TCLINUX
void
start_dsl_autodet(void)
{
	char *autodet_argv[] = {"auto_det", NULL};
	pid_t pid;

	if(getpid()!=1) {
		notify_rc("start_dsl_autodet");
		return;
	}

	killall_tk("auto_det");
	nvram_set("dsltmp_adslatequit", "0");
	nvram_set("dsltmp_autodet_state", "Detecting");
	sleep(1);
	_eval(autodet_argv, NULL, 0, &pid);

	return;
}

void
stop_dsl_autodet(void)
{
	if(getpid()!=1) {
		notify_rc("stop_dsl_autodet");
		return;
	}

	killall_tk("auto_det");
}
#endif

#ifdef RTCONFIG_HTTPS
int check_rsasign(char *fname)
{

    RSA *rsa_pkey = NULL;
    EVP_PKEY *pkey;
    EVP_MD_CTX ctx;
    unsigned char buffer[16*1024];
    size_t len;
    unsigned char *sig;
    unsigned int siglen;
    struct stat stat_buf;

	FILE * publicKeyFP;
	FILE * dataFileFP;
	FILE * sigFileFP;

	publicKeyFP = fopen( "/usr/sbin/public.pem", "r" );
    	if (publicKeyFP == NULL){
    	    _dprintf( "Open publicKeyFP failure\n" );
    	    return 0;
    	}

    if (!PEM_read_RSA_PUBKEY(publicKeyFP, &rsa_pkey, NULL, NULL)) {
        _dprintf("Error loading RSA public Key File.\n");
        return 0;
    }

	fclose(publicKeyFP);

    pkey = EVP_PKEY_new();
    if (!EVP_PKEY_assign_RSA(pkey, rsa_pkey)) {
        _dprintf("EVP_PKEY_assign_RSA: failed.\n");
        return 0;
    }

	sigFileFP = fopen( "/tmp/rsasign.bin", "r" );
    	if (sigFileFP == NULL){
    	    _dprintf( "Open sigFileFP failure\n" );
    	    return 0;
    	}

    /* Read the signature */
    if (fstat(fileno(sigFileFP), &stat_buf) == -1) {
        _dprintf("Unable to read signature \n");
        return 0;
    }

    siglen = stat_buf.st_size;
    sig = (unsigned char *)malloc(siglen);
    if (sig == NULL) {
        _dprintf("Unable to allocated %d bytes for signature\n",
            siglen);
        return 0;
    }

    if ((fread(sig, 1, siglen, sigFileFP)) != siglen) {
        _dprintf("Unable to read %d bytes for signature\n",
            siglen);
        return 0;
    }
	fclose(sigFileFP);

    EVP_MD_CTX_init(&ctx);
    if (!EVP_VerifyInit(&ctx, EVP_sha1())) {
        _dprintf("EVP_SignInit: failed.\n");
        EVP_PKEY_free(pkey);
        return 0;
    }

	dataFileFP = fopen( fname, "r" );
    	if (dataFileFP == NULL){
    	    _dprintf( "Open dataFileFP failure\n" );
    	    return 0;
    	}

    while ((len = fread(buffer, 1, sizeof buffer, dataFileFP)) > 0) {
        if (!EVP_VerifyUpdate(&ctx, buffer, len)) {
            _dprintf("EVP_SignUpdate: failed.\n");
            EVP_PKEY_free(pkey);
            return 0;
        }
    }

    if (ferror(dataFileFP)) {
        _dprintf("input file");
        EVP_PKEY_free(pkey);
        return 0;
    }
	fclose(dataFileFP);

    if (!EVP_VerifyFinal(&ctx, sig, siglen, pkey)) {
        _dprintf("EVP_VerifyFinal: failed.\n");
        free(sig);
        EVP_PKEY_free(pkey);
        return 0;
    }else
	_dprintf("EVP_VerifyFinal: ok.\n");

    free(sig);
    EVP_PKEY_free(pkey);
    return 1;
}
#endif

#ifdef RTCONFIG_PARENTALCTRL
void stop_pc_block(void)
{
	if (pids("pc_block"))
		killall("pc_block", SIGTERM);
}

void start_pc_block(void)
{
	char *pc_block_argv[] = {"pc_block", NULL};
	pid_t pid;

	stop_pc_block();

	if(nvram_get_int("MULTIFILTER_ALL") !=0 && count_pc_rules() > 0)
		_eval(pc_block_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_TOR
void stop_Tor_proxy(void)
{
	if (pids("Tor"))
		killall("Tor", SIGTERM);
	sleep(1);
	remove("/tmp/torlog");
}

void start_Tor_proxy(void)
{
	FILE *fp;
	pid_t pid;
	char *Tor_argv[] = { "Tor",
		"-f", "/tmp/torrc", "--quiet", NULL};
	char *Socksport;
	char *Transport;
	char *Dnsport;
	struct stat mdstat_jffs, mdstat_tmp;
	int mdesc_stat_jffs, mdesc_stat_tmp;

	stop_Tor_proxy();

	if(!nvram_get_int("Tor_enable"))
		return;

	if ((fp = fopen("/tmp/torrc", "w")) == NULL)
		return;

#if (defined(RTCONFIG_JFFS2)||defined(RTCONFIG_BRCM_NAND_JFFS2))
	mdesc_stat_tmp = stat("/tmp/.tordb/cached-microdesc-consensus", &mdstat_tmp);
	if(mdesc_stat_tmp == -1){
		mdesc_stat_jffs = stat("/jffs/.tordb/cached-microdesc-consensus", &mdstat_jffs);
		if(mdesc_stat_jffs != -1){
			_dprintf("Tor: restore microdescriptor directory\n");
			eval("cp", "-rf", "/jffs/.tordb", "/tmp/.tordb");
			sleep(1);
		}
	}
#endif
	if ((Socksport = nvram_get("Tor_socksport")) == NULL)	Socksport = "9050";
	if ((Transport = nvram_get("Tor_transport")) == NULL)   Transport = "9040";
	if ((Dnsport = nvram_get("Tor_dnsport")) == NULL)   	Dnsport = "9053";

	fprintf(fp, "SocksPort %s\n", Socksport);
	fprintf(fp, "Log notice file /tmp/torlog\n");
	fprintf(fp, "VirtualAddrNetwork 10.192.0.0/10\n");
	fprintf(fp, "AutomapHostsOnResolve 1\n");
	fprintf(fp, "TransPort %s\n", Transport);
	fprintf(fp, "TransListenAddress %s\n", nvram_safe_get( "lan_ipaddr" ));
	fprintf(fp, "DNSPort %s\n", Dnsport);
	fprintf(fp, "DNSListenAddress %s\n", nvram_safe_get( "lan_ipaddr" ));
	fprintf(fp, "RunAsDaemon 1\n");
	fprintf(fp, "DataDirectory /tmp/.tordb\n");
	fprintf(fp, "AvoidDiskWrites 1\n");

	append_custom_config("torrc", fp);
	fclose(fp);
	use_custom_config("torrc", "/tmp/torrc");
	run_postconf("torrc", "/tmp/torrc");

	_eval(Tor_argv, NULL, 0, &pid);
}
#endif

#ifdef RTCONFIG_CLOUDCHECK
void stop_cloudcheck(void){
	if(getpid() != 1){
		notify_rc("stop_cloudcheck");
		return;
	}

	killall("booster_watchdo", SIGTERM); // only 15 characters can be identified.
}

void start_cloudcheck(void){
	char *cmd[] = {"/bin/booster_watchdog", NULL};
	pid_t pid;

	if(getpid() != 1){
		notify_rc("start_cloudcheck");
		return;
	}

	if(nvram_get_int("enable_cloudcheck") != 1)
		return;

	_eval(cmd, NULL, 0, &pid);
}
#endif

#if ((defined(RTCONFIG_USER_LOW_RSSI) && defined(RTCONFIG_BCMARM)) || defined(RTCONFIG_NEW_USER_LOW_RSSI))
void stop_roamast(void){
	if (pids("roamast"))
		killall("roamast", SIGTERM);
}

void start_roamast(void){
	char *cmd[] = {"roamast", NULL};
	char prefix[] = "wl_XXXXX";
	char tmp[32];
	pid_t pid;
	int i;

	stop_roamast();

	for (i = 0; i <= DEV_NUMIFS; i++) {
		sprintf(prefix, "wl%d_", i);
		if( nvram_get_int(strcat_r(prefix, "user_rssi", tmp)) != 0 ) {
			_eval(cmd, NULL, 0, &pid);
			break;
		}
	}
}
#endif

#if defined(RTCONFIG_KEY_GUARD)
void stop_keyguard(void){
	if(pids("keyguard"))
		killall("keyguard", SIGTERM);
}

void start_keyguard(void){
	char *cmd[] = {"keyguard", NULL};
	pid_t pid;

	if(!nvram_get_int("kg_enable")) {
		stop_keyguard();
		return;
	}
	else {
		if(pids("keyguard"))
			killall("keyguard", SIGUSR1);
		else
			_eval(cmd, NULL, 0, &pid);
	}
}
#endif

/* for APP ECO mode changing RF to 1x1 */
void start_ecoguard(void){

	char *next = NULL;
	char ifname[32];

	if ( !nvram_get_int("wlready") || nvram_get_int("guard_mode") != 1)	//guard_mode 0:default 1:eco mode
		return;
	else{
		foreach(ifname, nvram_safe_get("wl_ifnames"), next) {
#if defined(RTCONFIG_RALINK)
#elif defined(RTCONFIG_QCA)
#else /* BCM */

#if defined(RTCONFIG_QTN)
#else
		eval("wl", "-i", ifname, "txchain", "1");
		eval("wl", "-i", ifname, "rxchain", "1");
		eval("wl", "-i", ifname, "down");
		eval("wl", "-i", ifname, "up");
#endif
#endif
		}
	}
}

int service_main(int argc, char *argv[])
{
	if (argc != 2) usage_exit(argv[0], "<action_service>");
	notify_rc(argv[1]);
	printf("\nDone.\n");
	return 0;
}

void setup_leds()
{
	int model;

	model = get_model();

	if (nvram_get_int("led_disable") == 1) {
		if ((model == MODEL_RTAC56U) || (model == MODEL_RTAC56S) || (model == MODEL_RTAC68U) || (model == MODEL_RTAC87U) || (model == MODEL_RTAC3200) || (model == MODEL_RTAC88U) || (model == MODEL_RTAC3100) || (model == MODEL_RTAC5300)) {
			setAllLedOff();
			if (model == MODEL_RTAC87U)
				led_control_atomic(LED_5G, LED_OFF);
		} else {        // TODO: Can other routers also use the same code?
			led_control_atomic(LED_2G, LED_OFF);
			led_control_atomic(LED_5G, LED_OFF);
			led_control_atomic(LED_POWER, LED_OFF);
			led_control_atomic(LED_SWITCH, LED_OFF);
			led_control_atomic(LED_LAN, LED_OFF);
			led_control_atomic(LED_WAN, LED_OFF);
		}
#ifdef RTCONFIG_USB
		stop_usbled();
		led_control_atomic(LED_USB, LED_OFF);
#endif

	} else {
#ifdef RTCONFIG_USB
		start_usbled();
#endif
#ifdef RTCONFIG_LED_ALL
		led_control_atomic(LED_ALL, LED_ON);
#endif

		if (nvram_match("wl1_radio", "1")
#if defined(RTAC3200) || defined(RTAC5300)
		    || nvram_match("wl2_radio", "1")
#endif
		   ) {
			led_control_atomic(LED_5G_FORCED, LED_ON);
		}
		if (nvram_match("wl0_radio", "1")) {
			led_control_atomic(LED_2G, LED_ON);
		}
#ifdef RTCONFIG_QTN
		setAllLedOn_qtn();
#endif
		led_control_atomic(LED_SWITCH, LED_ON);
		led_control_atomic(LED_POWER, LED_ON);

#if defined(RTAC3200) || defined(RTAC88U) || defined(RTAC3100) || defined(RTAC5300)
		kill_pidfile_s("/var/run/wanduck.pid", SIGUSR2);
#endif
	}
}

void stop_cstats(void)
{
	int n, m;
	int pid;
	int pidz;
	int ppidz;
	int w = 0;

	n = 60;
	m = 15;
	while ((n-- > 0) && ((pid = pidof("cstats")) > 0)) {
		w = 1;
		pidz = pidof("gzip");
		if (pidz < 1) pidz = pidof("cp");
		ppidz = ppid(ppid(pidz));
		if ((m > 0) && (pidz > 0) && (pid == ppidz)) {
			syslog(LOG_DEBUG, "cstats(PID %d) shutting down, waiting for helper process to complete(PID %d, PPID %d).\n", pid, pidz, ppidz);
			--m;
		} else {
			kill(pid, SIGTERM);
		}
		sleep(1);
	}
	if ((w == 1) && (n > 0))
		syslog(LOG_DEBUG, "cstats stopped.\n");
}

void start_cstats(int new)
{
	if (nvram_match("cstats_enable", "1")) {
		stop_cstats();
		if (new) {
			syslog(LOG_DEBUG, "starting cstats (new datafile).\n");
			xstart("cstats", "--new");
		} else {
			syslog(LOG_DEBUG, "starting cstats.\n");
			xstart("cstats");
		}
	}
}

void restart_cstats(void)
{
        if (nvram_match("cstats_new", "1"))
        {
                start_cstats(1);
                nvram_set("cstats_new", "0");
		nvram_commit();		// Otherwise it doesn't get written back to mtd
        }
        else
        {
                start_cstats(0);
        }
}

#ifdef RTCONFIG_DNSFILTER
// ARG: server must be an array of two pointers, each pointing to an array of chars
int get_dns_filter(int proto, int mode, char **server)
{
	int count = 0;
	static char *server_table[13][2] = {
		{"", ""},				/* 0: Unfiltered (handled separately below) */
		{"208.67.222.222", ""},	/* 1: OpenDNS */
		{"199.85.126.10", ""},	/* 2: Norton Connect Safe A (Security) */
		{"199.85.126.20", ""},	/* 3: Norton Connect Safe B (Security + Adult) */
		{"199.85.126.30", ""},	/* 4: Norton Connect Safe C (Sec. + Adult + Violence */
		{"77.88.8.88", ""},		/* 5: Secure Mode safe.dns.yandex.ru */
		{"77.88.8.7", ""},		/* 6: Family Mode family.dns.yandex.ru */
		{"208.67.222.123", ""},	/* 7: OpenDNS Family Shield */
		{"", ""},				/* 8: Custom1 */
		{"", ""},				/* 9: Custom2 */
		{"", ""},				/* 10: Custom3 */
		{"", ""},				/* 11: Router */
		{"8.26.56.26", ""}		/* 12: Comodo Secure DNS */
        };
#ifdef RTCONFIG_IPV6
	static char *server6_table[][2] = {
		{"", ""},		/* 0: Unfiltered (handled separately below) */
		{"", ""},		/* 1: OpenDNS */
		{"", ""},		/* 2: Norton Connect Safe A (Security) */
		{"", ""},		/* 3: Norton Connect Safe B (Security + Adult) */
		{"", ""},		/* 4: Norton Connect Safe C (Sec. + Adult + Violence */
		{"2a02:6b8::feed:bad","2a02:6b8:0:1::feed:bad"},		/* 5: Secure Mode safe.dns.yandex.ru */
		{"2a02:6b8::feed:a11","2a02:6b8:0:1::feed:a11"},		/* 6: Family Mode family.dns.yandex.ru */
		{"", ""},			/* 7: OpenDNS Family Shield */
		{"", ""},			/* 8: Custom1 - not supported yet */
		{"", ""},			/* 9: Custom2 - not supported yet */
		{"", ""},			/* 10: Custom3 - not supported yet */
		{"", ""},			/* 11: Router */
		{"", ""}			/* 12: Comodo Secure DNS */
        };
#endif
	// Initialize
	server[0] = server_table[0][0];
	server[1] = server_table[0][1];

	if (mode >= (sizeof(server_table)/sizeof(server_table[0]))) mode = 0;

	// Custom IP, will fallback to router IP if it's not defined.  Only IPv4 supported.
	if ((mode == 8) && (proto == AF_INET)) {
		server[0] = nvram_safe_get("dnsfilter_custom1");
		server[1] = server_table[mode][1];
	} else if ((mode == 9) && (proto == AF_INET)) {
		server[0] = nvram_safe_get("dnsfilter_custom2");
		server[1] = server_table[mode][1];
	} else if ((mode == 10) && (proto == AF_INET)) {
		server[0] = nvram_safe_get("dnsfilter_custom3");
		server[1] = server_table[mode][1];
	// Force to use what's returned by the router's DHCP server to clients (which means either
	// the router's IP, or a user-defined nameserver from the DHCP webui page)
	} else if (mode == 11) {
		server[0] = nvram_safe_get("dhcp_dns1_x");
		server[1] = server_table[mode][1];
	} else {
#ifdef RTCONFIG_IPV6	// Also handle IPv6 custom servers, which are always empty for now
		if (proto == AF_INET6) {
			server[0] = server6_table[mode][0];
			server[1] = server6_table[mode][1];
		} else
#endif
		{
			server[0] = server_table[mode][0];
			server[1] = server_table[mode][1];
		}
	}

// Ensure that custom and DHCP-provided DNS do contain something
	if (((mode == 8) || (mode == 9) || (mode == 10) || (mode == 11)) && (!strlen(server[0])) && (proto == AF_INET)) {
		server[0] = nvram_safe_get("lan_ipaddr");
	}

// Report how many non-empty server we are returning
	if (strlen(server[0])) count++;
	if (strlen(server[1])) count++;
	return count;
}
#endif

// Takes one argument:  0 = update failure
//                      1 (or missing argument) = update success
int
ddns_custom_updated_main(int argc, char *argv[])
{
	if ((argc == 2 && !strcmp(argv[1], "1")) || (argc == 1)) {
		nvram_set("ddns_status", "1");
		nvram_set("ddns_updated", "1");
		nvram_set("ddns_return_code", "200");
		nvram_set("ddns_return_code_chk", "200");
		nvram_set("ddns_server_x_old", nvram_safe_get("ddns_server_x"));
		nvram_set("ddns_hostname_old", nvram_safe_get("ddns_hostname_x"));
		logmessage("ddns", "Completed custom ddns update");
	} else {
		nvram_set("ddns_return_code", "unknown_error");
		nvram_set("ddns_return_code_chk", "unknown_error");
		logmessage("ddns", "Custom ddns update failed");
	}

        return 0;
}
