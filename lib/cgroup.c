/*
 * Copyright (c) 2015-2017, Parallels International GmbH
 *
 * This file is part of OpenVZ libraries. OpenVZ is free software; you can
 * redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either version
 * 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/> or write to Free Software Foundation,
 * 51 Franklin Street, Fifth Floor Boston, MA 02110, USA.
 *
 * Our contact details: Parallels International GmbH, Vordergasse 59, 8200
 * Schaffhausen, Switzerland.
 */

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <math.h>
#include <dirent.h>

#include "list.h"
#include "cgroup.h"
#include "bitmap.h"
#include "vzerror.h"
#include "logger.h"
#include "util.h"
#include "net.h"

struct cg_ctl {
	char subsys[64];
	int is_prvt;
	char *mount_path;
};

static struct cg_ctl cg_ctl_map[] = {
	{CG_CPU},
	{CG_CPUSET},
	{CG_NET_CLS},
	{CG_MEMORY},
	{CG_DEVICES},
	{CG_BLKIO},
	{CG_FREEZER},
	{CG_UB, 1},
	{CG_VE, 1},
	{CG_PERF_EVENT},
	{CG_HUGETLB},
	{CG_PIDS},
	{"systemd"},
};

static pthread_mutex_t cg_ctl_map_mtx = PTHREAD_MUTEX_INITIALIZER;
typedef int (*cgroup_filter_f)(const char *subsys);

static int cg_is_systemd(const char *subsys)
{
	return strcmp(subsys, "systemd") == 0;
}

static int has_substr(char *buf, const char *str)
{
	char *token;
	char *str_ptr = buf;

	while ((token = strsep(&str_ptr, ",")) != NULL) {
		if (!strcmp(token, str))
			return 1;
	}
	return 0;
}

static int get_mount_path(const char *subsys, char *out, int size)
{
	FILE *fp;
	int n;
	char buf[PATH_MAX];
	char target[4096];
	char ops[4096];
	int ret = 1;

	fp = fopen("/proc/mounts", "r");
	if (fp == NULL)
		return vzctl_err(-1, errno, "Can't open /proc/mounts");

	while (fgets(buf, sizeof(buf), fp)) {
		/* cgroup /sys/fs/cgroup/devices cgroup rw,nosuid,nodev,noexec,relatime,devices */
		n = sscanf(buf, "%*s %4095s cgroup %4095s",
				target, ops);
		if (n != 2)
			continue;

		if (has_substr(ops, !cg_is_systemd(subsys) ?
					subsys : "name=systemd"))
		{
			strncpy(out, target, size -1);
			out[size-1] = '\0';
			ret = 0;
			break;
		}
	}
	fclose(fp);

	return ret;
}

static struct cg_ctl *find_cg_ctl(const char *subsys)
{
	int i;

	for (i = 0; i < sizeof(cg_ctl_map)/sizeof(cg_ctl_map[0]); i++)
		if (!strcmp(cg_ctl_map[i].subsys, subsys))
			return &cg_ctl_map[i];

	return NULL;
}

static int cg_get_ctl(const char *subsys, struct cg_ctl **ctl)
{
	int ret;
	char mount_path[PATH_MAX];

	*ctl = find_cg_ctl(subsys);
	if (*ctl == NULL)
		return vzctl_err(-1, 0, "Unknown cgroup subsystem %s", subsys);

	pthread_mutex_lock(&cg_ctl_map_mtx);
	if ((*ctl)->mount_path != NULL) {
		ret = 0;
		goto out;
	}

	ret = get_mount_path(subsys, mount_path, sizeof(mount_path));
	if (ret) {
		if (ret != -1)
			vzctl_err(-1, 0, "Unable to find mount point for %s cgroup",
					subsys);
		goto out;
	}

	if (xstrdup(&(*ctl)->mount_path, mount_path)) {
		ret = -1;
		goto out;
	}

	debug(DBG_CG, "cgroup %s mount point: %s ", subsys, mount_path);
out:
	pthread_mutex_unlock(&cg_ctl_map_mtx);

	return ret;
}

int do_write_data(const int fd, const char *fname, const char *data,
		const int len)
{
	int w;

	w = write(fd, data, len);
	if (w != len) {
		int eno = errno;
		if (w < 0)
			logger(-1, errno, "Error writing to %s data='%s'",
					fname ?: "", data);
		else
			logger(-1, 0, "Output truncated while writing to %s",
					fname ?: "");
		errno = eno;
		return -1;
	}

	return 0;
}

int write_data(const char *path, const char *data)
{
	int fd;
	int ret;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return vzctl_err(errno == ENOENT ? 1 : -1, errno,
					"Can't open %s for writing", path);

	logger(3, 0, "Write %s <%s>", path, data);
	ret = do_write_data(fd, path, data, strlen(data));
	if (ret == -1) {
		int eno = errno;
		close(fd);
		errno = eno;
		return -1;
	}

	if (close(fd))
		return vzctl_err(-1, errno, "Error on on close fd %s", path);

	return 0;
}

static int cg_read(const char *path, char *out, int size)
{
	int fd, r;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return vzctl_err(-errno, errno, "Can't open %s for reading", path);

	r = read(fd, out, size-1);
	close(fd);
	if (r < 0)
		return vzctl_err(-errno, errno, "Error reading from file %s", path);

	if (out[r-1] == '\n')
		out[r-1]= '\0';
	else
		out[r] = '\0';

	return 0;
}

const char *cg_get_slice_name(void)
{
	static int inited = 0;
	static char slice[64];

	if (!inited) {
		if (get_global_param("VE_CGROUP_SLICE", slice, sizeof(slice)))
			strcat(slice, "machine.slice");
		inited = 1;
	}

	return slice;
}

static void get_cgroup_name(const char *ctid, struct cg_ctl *ctl,
		char *out, int size)
{
	if (cg_is_systemd(ctl->subsys))
		snprintf(out, size, "%s/"SYSTEMD_CTID_SCOPE_FMT,
				ctl->mount_path, ctid);
	else if (ctl->is_prvt)
		snprintf(out, size, "%s/%s", ctl->mount_path, ctid);
	else
		snprintf(out, size, "%s/%s/%s",
				ctl->mount_path, cg_get_slice_name(), ctid);
}

int cg_get_path(const char *ctid, const char *subsys, const char *name,
		char *out, int size)
{
	int ret;
	struct cg_ctl *ctl;
	char path[PATH_MAX];

	ret = cg_get_ctl(subsys, &ctl);
	if (ret)
		return ret;

	if (ctid == NULL)
		snprintf(out, size, "%s/%s", ctl->mount_path, name);
	else {
		get_cgroup_name(ctid, ctl, path, sizeof(path));
		snprintf(out, size, "%s/%s", path, name);
	}

	return 0;
}

int cg_set_param(const char *ctid, const char *subsys, const char *name, const char *data)
{
	int ret;
	char path[PATH_MAX];

	ret = cg_get_path(ctid, subsys, name, path, sizeof(path));
	if (ret)
		return ret;

	return write_data(path, data);
}

int cg_set_ul(const char *ctid, const char *subsys, const char *name,
		unsigned long value)
{
        char data[32];

        snprintf(data, sizeof(data), "%lu", value);

        return cg_set_param(ctid, subsys, name, data);
}

int cg_set_ull(const char *ctid, const char *subsys, const char *name,
		unsigned long long value)
{
        char data[32];

        snprintf(data, sizeof(data), "%llu", value);

        return cg_set_param(ctid, subsys, name, data);
}


int cg_get_param(const char *ctid, const char *subsys, const char *name, char *out, int size)
{
	char path[PATH_MAX];
	int ret;

	ret = cg_get_path(ctid, subsys, name, path, sizeof(path));
	if (ret)
		return ret;

	return cg_read(path, out, size);
}

int cg_get_ul(const char *ctid, const char *subsys, const char *name,
		unsigned long *value)
{
	int ret;
	char data[32];

	ret = cg_get_param(ctid, subsys, name, data, sizeof(data));
	if (ret)
		return ret;
	return parse_ul(data, value);
}

int cg_get_ull(const char *ctid, const char *subsys, const char *name,
		unsigned long long *value)
{
	int ret;
	char data[32];
	char *tail;

	ret = cg_get_param(ctid, subsys, name, data, sizeof(data));
	if (ret)
		return ret;

	errno = 0;
	*value = strtoull(data, (char **)&tail, 10);
	if (*tail != '\0' || errno == ERANGE)
		return -1;

	return 0;
}

static int cg_create(const char *ctid, struct cg_ctl *ctl)
{
	char path[PATH_MAX];

	get_cgroup_name(ctid, ctl, path, sizeof(path));

	logger(3, 0, "Create cgroup %s", path);
	return make_dir(path, 1);
}

static int rmdir_retry(int fd, const char *name)
{
	useconds_t total = 0;
	useconds_t wait = 10000;
	const useconds_t maxwait = 500000;
	const useconds_t timeout = 30 * 1000000;

	do {
		if (unlinkat(fd, name, AT_REMOVEDIR) == 0)
			return 0;
		if (errno != EBUSY)
			break;
		usleep(wait);
		total += wait;
		wait *= 2;
		if (wait > maxwait)
			wait = maxwait;
	} while (total < timeout);

	return vzctl_err(-1, errno, "Cannot remove dir %s", name);
}

/* change fd to first found dir
 * retrun: 0 - fd sucesfully chnaged
 *	   1 - fd not changed (no dirs)
 *	  -1 - error
 */
static int goto_next_dir(int *parentfd, int *fd, char *out, int size)
{
	DIR *dir;
	struct stat st;
	struct dirent *ent;

	dir = fdopendir(*fd);
	if (dir == NULL)
		return vzctl_err(-1, errno, "Can't opendir");

	rewinddir(dir);

	while ((ent = readdir(dir)) != NULL) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;

		if (fstatat(*fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) &&
				errno != ENOENT)
		{
			vzctl_err(-1, errno, "goto_next_dir fstatat %s",
					ent->d_name);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			int next = openat(*fd, ent->d_name, O_DIRECTORY);
			if (next == -1) {
				if (errno == ENOENT)
					continue;

				return vzctl_err(-1, errno, "openat %s",
						ent->d_name);
			}

			if (*parentfd != -1)
				close(*parentfd);
			*parentfd = *fd;
			*fd = next;

			snprintf(out, size, "%s", ent->d_name);

			return 0;
		}
	}

	/* do not release fd */
	return 1;
}

static int rm_tree(const char *path)
{
	int ret = 0;
	int parentfd = -1, fd = -1;
	struct stat st, pst;
	char name[PATH_MAX] = "";
	int level = 0;

	fd = open(path, O_DIRECTORY);
	if (fd == -1) {
		if (errno == ENOENT)
			return 0;
		return vzctl_err(-1, errno, "Can't open %s", path);
	}

	if (fstat(fd, &st)) {
		vzctl_err(-1, errno, "fstat %s", path);
		close(fd);
		return -1;
	}

	do {
		int rc = goto_next_dir(&parentfd, &fd, name, sizeof(name));
		if (rc == 0) {
			level++;
			continue;
		} else if (rc == -1) {
			ret = -1;
			break;
		}

		if (fstat(fd, &pst)) {
			ret = vzctl_err(-1, errno, "rm_tree fstat()");
			break;
		}
		if (st.st_ino == pst.st_ino)
			break;

		int pfd = openat(parentfd, "..", O_DIRECTORY);
		if (pfd == -1) {
			ret = vzctl_err(-1, errno, "openad ..");
			break;
		}

		close(fd);
		fd = parentfd;
		parentfd = pfd;

		if (name[0] != '\0' && rmdir_retry(fd, name)) {
			ret = -1;
			break;
		}

		name[0] = '\0';
		level--;
	} while (level >= 0);

	if (fd != -1)
		close(fd);
	if (parentfd != -1)
		close(parentfd);

	rmdir(path);

	return ret;
}

static int cg_destroy(const char *ctid, struct cg_ctl *ctl)
{
	char path[PATH_MAX];

	if (ctl->mount_path == NULL)
		return 0;

	get_cgroup_name(ctid, ctl, path, sizeof(path));

	if (rm_tree(path))
		return VZCTL_E_SYSTEM;

	return 0;
}

int cg_get_cgroup_env_param(const char *ctid, char *out, int size)
{
	int i, ret;
	struct cg_ctl *ctl;
	char *p = out;
	char *ep = p + size;
	char path[PATH_MAX];

	p += snprintf(p, ep - p, "VE_CGROUP_MOUNT_MAP=");
	for (i = 0; i < sizeof(cg_ctl_map)/sizeof(cg_ctl_map[0]); i++) {
		ret = cg_get_ctl(cg_ctl_map[i].subsys, &ctl);
		if (ret == -1)
			return 1;
		if (ctl->is_prvt)
			continue;
		/* Skip non exists */
		if (ret)
			continue;
		if (ctid) {
			get_cgroup_name(ctid, ctl, path, sizeof(path));
			p += snprintf(p, ep - p, " %s:%s", ctl->subsys, path);
		} else
			p += snprintf(p, ep - p, " %s:%s",
					ctl->mount_path, ctl->subsys);
		if (p > ep)
			return vzctl_err(VZCTL_E_INVAL, 0, "cg_get_cgroup_env_param");
	}

	return 0;
}

int cg_new_cgroup(const char *ctid)
{
	int ret, i;
	struct cg_ctl *ctl;

	for (i = 0; i < sizeof(cg_ctl_map)/sizeof(cg_ctl_map[0]); i++) {
		ret = cg_get_ctl(cg_ctl_map[i].subsys, &ctl);
		if (ret == -1)
			goto err;
		/* Skip non exists */
		if (ret)
			continue;

		ret = cg_create(ctid, ctl);
		if (ret)
			goto err;
	}
	return 0;
err:
	for (i = i - 1; i >= 0; i--)
		cg_destroy(ctid, &cg_ctl_map[i]);

	return ret;
}

int cg_destroy_cgroup(const char *ctid)
{
	int rc, i, ret = 0;
	struct cg_ctl *ctl;

	for (i = 0; i < sizeof(cg_ctl_map)/sizeof(cg_ctl_map[0]); i++) {
		rc = cg_get_ctl(cg_ctl_map[i].subsys, &ctl);
		if (rc)
			continue;

		ret |= cg_destroy(ctid, ctl);
	}
	return ret;
}

int cg_enable_pseudosuper(const char *ctid)
{
	return cg_set_ul(ctid, CG_VE, "ve.pseudosuper", 1);
}

int cg_pseudosuper_open(const char *ctid, int *fd)
{
	int ret;
	char path[PATH_MAX];

	ret = cg_get_path(ctid, CG_VE, "ve.pseudosuper", path, sizeof(path));
	if (ret)
		return ret;

	*fd = open(path, O_WRONLY|O_CLOEXEC);
	if (*fd == -1)
		return vzctl_err(-1, errno, "Cannot open %s", path);

	return 0;
}

int cg_disable_pseudosuper(const int pseudosuper_fd)
{
	return do_write_data(pseudosuper_fd, NULL, "0", 1);
}

int cg_attach_task(const char *ctid, pid_t pid, char *cg_subsys_except)
{
	int ret, i;

	for (i = 0; i < sizeof(cg_ctl_map)/sizeof(cg_ctl_map[0]); i++) {
		if (cg_subsys_except &&
			 !strcmp(cg_ctl_map[i].subsys, cg_subsys_except))
			continue;
		ret = cg_set_ul(ctid, cg_ctl_map[i].subsys, "tasks", pid);
		if (ret == -1)
			return -1;
		/* Skip non exists */
		if (ret) {
			ret = 0;
			continue;
		}
	}

	return 0;
}

/**************************************************************************/
int cg_env_set_cpuunits(const char *ctid, unsigned int cpuunits)
{
	return cg_set_ul(ctid, CG_CPU, "cpu.shares", cpuunits * 1024 / 1000);
}

int cg_env_set_cpulimit(const char *ctid, float limit)
{
	unsigned long limit1024 = limit * 1024 / 100;

	return cg_set_ul(ctid, CG_CPU, "cpu.rate", limit1024);
}

int cg_env_get_cpulimit(const char *ctid, float *limit)
{
	int ret;
	unsigned long limit1024;

	ret = cg_get_ul(ctid, CG_CPU, "cpu.rate", &limit1024);
	if (ret)
		return ret;

	*limit = limit1024 * 100 / 1024;

	return 0;
}

int cg_env_set_vcpus(const char *ctid, unsigned int vcpus)
{
	return cg_set_ul(ctid, CG_CPU, "cpu.nr_cpus", vcpus);
}

static int cg_env_set_mask(const char *ctid, const char *name,  unsigned long *cpumask, int size)
{
	char cg_name[64];
	char buf[4096];
	unsigned long *mask;

	snprintf(cg_name, sizeof(cg_name), "cpuset.%s", name);
	if (cg_get_param("", CG_CPUSET, cg_name, buf, sizeof(buf)) < 0)
		return vzctl_err(VZCTL_E_CPUMASK, 0,
				"Unable to get active %s mask", cg_name);

	mask = malloc(size);
	if (mask == NULL)
		return vzctl_err(VZCTL_E_NOMEM, ENOMEM, "cg_env_set_mask");

	if (vzctl2_bitmap_parse(buf, mask, size)) {
		free(mask);
		return vzctl_err(VZCTL_E_CPUMASK, 0,
				"Can't parse active %s mask: %s", name, buf);
	}

	/* Autocorrect mask */
	if (!bitmap_and(mask, cpumask, mask, size))
	{
		free(mask);

		char val[4096];
		bitmap_snprintf(val, sizeof(val), cpumask, size);
		return vzctl_err(VZCTL_E_CPUMASK, 0,
				"Unable to set %s value %s, supported range: %s", name, val, buf);
	}

	bitmap_snprintf(buf, sizeof(buf), mask, size);
	free(mask);

	snprintf(cg_name, sizeof(cg_name), "cpuset.%s", name);
	if (cg_set_param(ctid, CG_CPUSET, cg_name, buf))
		return vzctl_err(VZCTL_E_CPUMASK, errno,
				"Unable to set %s", cg_name);

	return 0;
}

int cg_env_set_cpumask(const char *ctid, unsigned long *cpumask, int size)
{
	return cg_env_set_mask(ctid, "cpus", cpumask, size);
}

int cg_env_set_nodemask(const char *ctid, unsigned long *nodemask, int size)
{
	return cg_env_set_mask(ctid, "mems", nodemask, size);
}

int cg_env_set_devices(const char *ctid, const char *name, const char *data)
{
	return cg_set_param(ctid, CG_DEVICES, name, data);
}

int cg_env_set_memory(const char *ctid, const char *name, unsigned long value)
{
	return cg_set_ul(ctid, CG_MEMORY, name, value);
}

int cg_env_set_ub(const char *ctid, const char *name, unsigned long b, unsigned long l)
{
	int rc;
	char _name[STR_SIZE];

	snprintf(_name, sizeof(_name), "beancounter.%s.barrier", name);
	rc = cg_set_ul(ctid, CG_UB, _name, b);
	if (rc)
		return rc;

	snprintf(_name, sizeof(_name), "beancounter.%s.limit", name);
	return cg_set_ul(ctid, CG_UB, _name, l);
}

static int cg_env_set_io(const char *ctid, const char *name, unsigned int speed,
		unsigned int burst, unsigned int latency)
{
	int ret;
	char buf[STR_SIZE];

	snprintf(buf, sizeof(buf), CG_UB".%s.speed", name);
	ret = cg_set_ul(ctid, CG_UB, buf, speed);
	if (ret)
		return ret;

	snprintf(buf, sizeof(buf), CG_UB".%s.burst", name);
	ret = cg_set_ul(ctid, CG_UB, buf, burst);
	if (ret)
		return ret;

	snprintf(buf, sizeof(buf), CG_UB".%s.latency", name);
	ret = cg_set_ul(ctid, CG_UB, buf, latency);
	if (ret)
		return ret;

	return 0;
}

int cg_env_set_iolimit(const char *ctid, unsigned int speed,
		unsigned int burst, unsigned int latency)
{
	return cg_env_set_io(ctid, "iolimit", speed, burst, latency);
}

int cg_env_set_iopslimit(const char *ctid, unsigned int speed,
		unsigned int burst, unsigned int latency)
{
	return cg_env_set_io(ctid, "iopslimit", speed, burst, latency);
}

int cg_env_get_memory(const char *ctid, const char *name, unsigned long *value)
{
	return cg_get_ul(ctid, CG_MEMORY, name, value);
}

int cg_env_set_net_classid(const char *ctid, unsigned int classid)
{
	return cg_set_ul(ctid, CG_NET_CLS, CG_NET_CLASSID, classid);
}

static int cg_env_check_init_pid(const char *ctid, pid_t pid)
{
	int ret;
	FILE *fp;
	char buf[4096];

	snprintf(buf, sizeof(buf), "/proc/%d/status", pid);
	fp = fopen(buf, "r");
	if (fp == NULL) {
		if (errno == ENOENT)
			return vzctl_err(-1, 0, "Init pid %d is invalid:"
					" no such task", pid);
		return vzctl_err(-1, errno, "Unable to open %s", buf);
	}

	ret = 1;
	while (fgets(buf, sizeof(buf), fp)) {
		if (sscanf(buf, "envID:  %s", buf) != 1)
			continue;

		if (!strcmp(ctid, buf))
			ret = 0;
		break;
	}
	fclose(fp);

	if (ret)
		return vzctl_err(1, 0, "Init pid %d is invalid", pid);

	return 0;
}

int cg_env_get_init_pid(const char *ctid, pid_t *pid)
{
	int ret;

	if ((ret = read_init_pid(ctid, pid)))
		return ret;

	if ((ret = cg_env_check_init_pid(ctid, *pid))) {
		*pid = 0;
		return ret;
	}

	return 0;
}

int cg_env_get_ve_state(const char *ctid)
{
	int ret;
	char buf[64] = "";
	char path[PATH_MAX];

	ret = cg_get_path(ctid, CG_VE, "ve.state", path, sizeof(path));
	if (ret)
		return ret;

	if (access(path, F_OK))
		return 0;

	cg_read(path, buf, sizeof(buf));

	return (strcmp(buf, "STOPPED") && strcmp(buf, "STOPPING"));
}

int cg_env_get_pids(const char *ctid, list_head_t *list)
{
	FILE *fp;
	char path[PATH_MAX];
	char *str;
	size_t len;
	char *p;
	int n, ret = 0;

	ret = cg_get_path(ctid, CG_VE, "tasks", path, sizeof(path));
	if (ret)
		return ret;

	if ((fp = fopen(path, "r")) == NULL)
		return vzctl_err(-1, errno, "Unable to open %s", path);

	len = 10;
	str = malloc(len + 1);
	do {
		errno = 0;
		n = getline(&str, &len, fp);
		if (n == -1) {
			if (errno == 0)
				break;
			vzctl_err(-1, errno, "Failed to read %s", path);
			ret = -1;
			break;
		}

		str[n] = '\0';
		p = strrchr(str, '\n');
		if (p != NULL)
			*p = '\0';

		if (add_str_param(list, str) == NULL) {
			free_str(list);
			ret = -1;
			break;
		}
	} while (n > 0);

	free(str);
	fclose(fp);

	return ret;
}

int cg_get_legacy_veid(const char *ctid, unsigned long *value)
{
	return cg_get_ul(ctid, CG_VE, "ve.legacy_veid", value);
}

int cg_add_veip(const char *ctid, const char *ip)
{
	if (cg_set_param(ctid, CG_VE,
			is_ip6(ip) ? "ve.ip6_allow" : "ve.ip_allow", ip))
	{
		int rc = errno == EADDRINUSE ? VZCTL_E_IP_INUSE : VZCTL_E_CANT_ADDIP;
		return vzctl_err(rc, errno,
				"Unable to add ip %s", ip);
	}
	return 0;
}

int cg_del_veip(const char *ctid, const char *ip)
{
	if (cg_set_param(ctid, CG_VE,
				is_ip6(ip) ? "ve.ip6_deny" : "ve.ip_deny", ip))
		return vzctl_err(VZCTL_E_SYSTEM, errno,
				"Unable to del ip %s", ip);
	return 0;
}

static int get_veip(const char *path, list_head_t *list)
{
	FILE *fp;
	char str[64];
	char ip_str[65];
	char *p;
	int ret = 0;

	if ((fp = fopen(path, "r")) == NULL) {
		if (errno == ENOENT)
			return 0;
		return vzctl_err(-1, errno, "Unable to open %s", path);
	}

	while (!feof(fp)) {
		if (fgets(str, sizeof(str), fp) == NULL)
			break;

		p = strrchr(str, '\n');
		if (p != NULL)
			*p = '\0';

		ret = get_ip_name(str, ip_str, sizeof(ip_str));
		if (ret)
			break;

		if (add_ip_param_str(list, ip_str) == NULL) {
			free_ip(list);
			ret = -1;
			break;
		}
	}

	fclose(fp);

	return ret;
}

int cg_get_veip(const char *ctid, list_head_t *list)
{
	int ret;
	char path[PATH_MAX];

	ret = cg_get_path(ctid, CG_VE, "ve.ip_list", path, sizeof(path));
	if (ret)
		return ret;

	ret = get_veip(path, list);
	if (ret)
		return ret;

	ret = cg_get_path(ctid, CG_VE, "ve.ip6_list", path, sizeof(path));
	if (ret)
		return ret;

	ret = get_veip(path, list);
	if (ret)
		return ret;

	return 0;
}

static int do_bindmount(const char *src, const char *dst, int mnt_flags)
{

	if (access(dst, F_OK) && make_dir(dst, 1))
		return vzctl_err(VZCTL_E_RESOURCE, errno,
				"Can't create %s", dst);

	if (access(src, F_OK) && make_dir(src, 1))
		return vzctl_err(VZCTL_E_RESOURCE, errno,
				"Can't create %s", src);

	logger(5, 0, "bindmount %s -> %s", src, dst);
	if (mount(src, dst, NULL, mnt_flags, NULL))
		return vzctl_err(VZCTL_E_RESOURCE, errno,
				"Can't bindmount %s -> %s", src, dst);
	return 0;
}

/* For multiple cg mounted like 'cpu,cpuacct' create per ctl symlink PSBM-38634
 *
 * ln -s cpu,cpuacct /sys/fs/cgroup/cpu
 * ln -s cpu,cpuacct /sys/fs/cgroup/cpuacct
 *
 */
static int create_perctl_symlink(const char *root, const char *path)
{
	int ret = 0;
	char newpath[PATH_MAX];
	char buf[STR_SIZE];
	char oldpath[STR_SIZE];
	char *p, *name;
	int last = 0;

	p = strrchr(path, '/');
	if (p == NULL)
		return 0;

	snprintf(buf, sizeof(buf), "%s", p + 1);
	snprintf(oldpath, sizeof(oldpath), "%s", p + 1);
	p = strchr(buf, ',');
	if (p == NULL)
		return 0;

	name = buf;
	*p = '\0'; p++;
	while (1) {
		snprintf(newpath, sizeof(newpath), "%s/%s/../%s",
			root, path, name);
		logger(10, 0, "Create symlink %s -> %s", oldpath, name);
		unlink(newpath);
		if (symlink(oldpath, newpath) && errno != EEXIST) {
			ret = vzctl_err(-1, errno,
					"Cant create symlink %s -> %s",
					path, name);
			break;
		}
		if (last)
			break;

		name = p;
		p = strchr(p, ',');
		if (p != NULL) {
			*p = '\0'; p++;
		} else
			last = 1;
	}

	return ret;
}

static int get_cgroups(list_head_t *head)
{
       int ret = 0;
       FILE *fp;
       char buf[STR_SIZE];

       fp = fopen("/proc/cgroups", "r");
       if (fp == NULL)
               return vzctl_err(VZCTL_E_SYSTEM, errno,
                               "Unable to open /proc/cgroups");

       while (fgets(buf, sizeof(buf), fp)) {
               if (sscanf(buf, "%s", buf) != 1)
                       continue;

               if (buf[0] == '#')
                       continue;

               if (add_str_param(head, buf) == NULL) {
                       ret = VZCTL_E_NOMEM;
                       break;
               }
       }
       fclose(fp);

       if (ret)
               free_str(head);

       return ret;
}

static int cg_bindmount_cgroup(struct vzctl_env_handle *h, list_head_t *head)
{
	int ret = 0, i;
	char s[PATH_MAX], d[PATH_MAX];
	char *ve_root = h->env_param->fs->ve_root;
	struct vzctl_str_param *it;
	const char *mnt;
	struct cg_ctl *ctl;
	int flags;
	LIST_HEAD(cgroups);

	snprintf(s, sizeof(s), "%s/sys", ve_root);
	if (access(s, F_OK) && make_dir(s, 1))
		return vzctl_err(VZCTL_E_CREATE_DIR, errno,
				"Can't create %s", s);
	if (mount(NULL, s, "sysfs", 0, NULL))
		return vzctl_err(VZCTL_E_RESOURCE, errno,
				"Can't pre-mount sysfs in %s", s);

	snprintf(s, sizeof(s), "%s/sys/fs/cgroup", ve_root);
	if (access(s, F_OK) && make_dir(s, 1))
		return vzctl_err(VZCTL_E_RESOURCE, errno,
				"Can't pre-mount tmpfs in %s", s);
	if (mount(NULL, s, "tmpfs", 0, NULL))
		return vzctl_err(VZCTL_E_RESOURCE, errno,
				"Can't pre-mount tmpfs in %s", s);

	ret = get_cgroups(&cgroups);
	if (ret)
		return ret;

	for (i = 0; i < sizeof(cg_ctl_map)/sizeof(cg_ctl_map[0]); i++) {
		ret = cg_get_ctl(cg_ctl_map[i].subsys, &ctl);
		if (ret == -1)
			goto err;
		if (ctl->is_prvt)
			continue;
		if (!cg_is_systemd(ctl->subsys) &&
				find_str(&cgroups, ctl->subsys) == NULL)
			continue;
		if (find_str(head, ctl->mount_path) != NULL)
			continue;

		mnt = ctl->mount_path;
		if (mount(NULL, mnt, NULL, MS_SLAVE, NULL)) {
			ret =  vzctl_err(VZCTL_E_SYSTEM, errno,
					"Remounting cgroup %s as slaves failed",
					mnt);
			goto err;
		}

		if (add_str_param(head, ctl->mount_path) == NULL) {
			ret = VZCTL_E_NOMEM;
			break;
		}

		snprintf(d, sizeof(d), "%s%s", ve_root, mnt);
		get_cgroup_name(EID(h), ctl, s, sizeof(s));
		flags = MS_BIND;
		if (!cg_is_systemd(ctl->subsys))
			flags |= MS_PRIVATE;

		ret = do_bindmount(s, d, flags);
		if (ret)
			goto err;

		ret = create_perctl_symlink(ve_root, mnt);
		if (ret)
			goto err;
	}
err:
	if (ret) {
		list_for_each(it, head, list) {
			snprintf(s, sizeof(s), "%s/%s",
					ve_root, it->str);
			umount(s);
		}

		snprintf(s, sizeof(s), "%s/sys/fs/cgroup", ve_root);
		umount(s);

		snprintf(s, sizeof(s), "%s/sys", ve_root);
		umount(s);
	}

	free_str(&cgroups);

	return ret;
}

int bindmount_env_cgroup(struct vzctl_env_handle *h)
{
	int ret;
	LIST_HEAD(head);

	ret = cg_bindmount_cgroup(h, &head);
	free_str(&head);

	return ret;
}

int cg_set_veid(const char *ctid, int veid)
{
	char path[PATH_MAX];
	char id[12];
	int ret;

	ret = cg_get_path(ctid, CG_VE, "ve.veid", path, sizeof(path));
	if (ret)
		return ret;

	if (access(path, F_OK))
		return 0;

	sprintf(id, "%d", veid);
	return write_data(path, id);
}

static int cg_set_freezer_state(const char *ctid, const char *state)
{
	int ret;
	char buf[STR_SIZE];
	int len;

	ret = cg_set_param(ctid, CG_FREEZER, "freezer.state", state);
	if (ret)
		return ret;

	len = strlen(state);
	while (1) {
		ret = cg_get_param(ctid, CG_FREEZER, "freezer.state",
				buf, sizeof(buf));
		if (ret)
			return ret;

		if (strncmp(buf, state, len) == 0)
			break;

		sleep(1);
	}

	return 0;
}

int cg_freezer_cmd(const char *ctid, int cmd)
{
	if (cmd == VZCTL_CMD_RESUME) {
		logger(0, 0, "\tunfreeze");
		return cg_set_freezer_state(ctid, "THAWED");
	} else if (cmd == VZCTL_CMD_SUSPEND) {
		logger(0, 0, "\tfreeze");
		return cg_set_freezer_state(ctid, "FROZEN");
	}
	return vzctl_err(-1, 0, "Unsupported freezer command %d", cmd);
}
