// SPDX-License-Identifier: GPL-2.0
/*
 * sus_unix.c - hide abstract AF_UNIX listeners from app connect().
 *
 * An abstract unix socket has no inode, so sus_path cannot hide it: the name lives only in the
 * kernel's unix socket hash.  Apps can still prove such a listener exists by connecting to it.
 * Example: adbd binds @jdwp-control, and an untrusted app's connect() to it succeeds while adbd
 * runs, which detectors use as ADB liveness evidence.  SELinux cannot help: a denial is EACCES,
 * and since the LSM check runs after the name lookup, EACCES also proves the listener exists.
 *
 * This layer replaces the connect() table entry (native and AArch32).  For a caller that
 * sus_path hides from (susfs_is_current_proc_umounted_app()), a connect to a listed abstract
 * name on an AF_UNIX socket answers -ECONNREFUSED, which is what unix_find_other() answers for an
 * abstract name nobody has bound.  Every other call goes to the original entry unchanged, so the
 * kernel validates arguments (EFAULT, EINVAL, EBADF, ...) the way it always does.
 *
 * Names are managed by root through /proc/susfs_unix:
 *
 *     echo "add jdwp-control" > /proc/susfs_unix
 *     echo "del jdwp-control" > /proc/susfs_unix
 *     echo clear              > /proc/susfs_unix
 *     cat /proc/susfs_unix    # names and counters
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/cred.h>
#include <net/sock.h>
#include <asm/ptrace.h>
#include <asm/unistd.h>

#include "susfs.h"
#include "susfs_log.h"
#include "susfs_fp_hook.h"
#include "ksu_umount_gate.h"
#include "ksu_allow_gate.h"

/* arch/arm/tools/syscall.tbl: 283 common connect.  The compat table has its own numbering. */
#define SUS_UNIX_COMPAT_NR_CONNECT	283

#define SUS_UNIX_MAX		16
/* An abstract name is sun_path minus its leading NUL. */
#define SUS_UNIX_NAME_MAX	(UNIX_PATH_MAX - 1)

struct sus_unix_name {
	int len;
	char name[SUS_UNIX_NAME_MAX];
};

static struct sus_unix_name sus_unix_names[SUS_UNIX_MAX];
static int sus_unix_count;
static DEFINE_SPINLOCK(sus_unix_lock);

static atomic_t n_checked = ATOMIC_INIT(0);
static atomic_t n_hidden = ATOMIC_INIT(0);

static susfs_syscall_fn_t connect_orig;
static susfs_syscall_fn_t connect_orig_compat;

static bool sus_unix_name_hidden(const char *name, int len)
{
	bool hidden = false;
	int i;

	spin_lock(&sus_unix_lock);
	for (i = 0; i < sus_unix_count; i++) {
		if (sus_unix_names[i].len == len &&
		    !memcmp(sus_unix_names[i].name, name, len)) {
			hidden = true;
			break;
		}
	}
	spin_unlock(&sus_unix_lock);
	return hidden;
}

/* 0 = not ours, pass the call on; -ECONNREFUSED = hide this listener. */
static long sus_unix_check(int fd, void __user *uaddr, int addrlen)
{
	struct sockaddr_un sun;
	struct socket *sock;
	bool is_unix;
	int name_len, err;

	if (!READ_ONCE(sus_unix_count))
		return 0;
	if (!susfs_is_current_proc_umounted_app())
		return 0;
	/* sun_family plus the leading NUL plus at least one name byte; anything else, including a
	 * length the kernel would reject, is left for the original to answer. */
	if (addrlen <= (int)offsetof(struct sockaddr_un, sun_path) + 1 ||
	    addrlen > (int)sizeof(sun))
		return 0;
	if (copy_from_user(&sun, uaddr, addrlen))
		return 0;
	if (sun.sun_family != AF_UNIX || sun.sun_path[0] != '\0')
		return 0;

	atomic_inc(&n_checked);
	name_len = addrlen - (int)offsetof(struct sockaddr_un, sun_path) - 1;
	if (!sus_unix_name_hidden(sun.sun_path + 1, name_len))
		return 0;

	/* Only a real AF_UNIX socket gets the "no such listener" answer: on any other fd the
	 * kernel's own error (EBADF, ENOTSOCK, EAFNOSUPPORT, ...) must come back unchanged. */
	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return 0;
	is_unix = sock->sk && sock->sk->sk_family == AF_UNIX;
	sockfd_put(sock);
	if (!is_unix)
		return 0;

	atomic_inc(&n_hidden);
	return -ECONNREFUSED;
}

static long __nocfi sus_unix_connect(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(connect_orig);
	long ret;

	ret = sus_unix_check((int)regs->regs[0], (void __user *)regs->regs[1],
			     (int)regs->regs[2]);
	if (ret)
		return ret;
	return orig ? orig(regs) : -ENOSYS;
}

static long __nocfi sus_unix_connect_compat(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(connect_orig_compat);
	long ret;

	ret = sus_unix_check((int)regs->regs[0], compat_ptr((u32)regs->regs[1]),
			     (int)regs->regs[2]);
	if (ret)
		return ret;
	return orig ? orig(regs) : -ENOSYS;
}

static struct susfs_fp_hook connect_hook = {
	.nr = __NR_connect,
	.nr_compat = SUS_UNIX_COMPAT_NR_CONNECT,
	.name = "sus_unix/connect",
	.sym = "__arm64_sys_connect",
	.wrapper = sus_unix_connect,
	.wrapper_compat = sus_unix_connect_compat,
	.orig_slot = &connect_orig,
	.orig_slot_compat = &connect_orig_compat,
};

/* ---- /proc/susfs_unix ---- */

static int sus_unix_add(const char *name, int len)
{
	int i, ret = 0;

	if (len <= 0 || len > SUS_UNIX_NAME_MAX)
		return -EINVAL;

	spin_lock(&sus_unix_lock);
	for (i = 0; i < sus_unix_count; i++) {
		if (sus_unix_names[i].len == len &&
		    !memcmp(sus_unix_names[i].name, name, len))
			goto out;	/* already listed */
	}
	if (sus_unix_count >= SUS_UNIX_MAX) {
		ret = -ENOSPC;
		goto out;
	}
	memcpy(sus_unix_names[sus_unix_count].name, name, len);
	sus_unix_names[sus_unix_count].len = len;
	WRITE_ONCE(sus_unix_count, sus_unix_count + 1);
out:
	spin_unlock(&sus_unix_lock);
	return ret;
}

static int sus_unix_del(const char *name, int len)
{
	int i, ret = -ENOENT;

	spin_lock(&sus_unix_lock);
	for (i = 0; i < sus_unix_count; i++) {
		if (sus_unix_names[i].len == len &&
		    !memcmp(sus_unix_names[i].name, name, len)) {
			sus_unix_names[i] = sus_unix_names[sus_unix_count - 1];
			WRITE_ONCE(sus_unix_count, sus_unix_count - 1);
			ret = 0;
			break;
		}
	}
	spin_unlock(&sus_unix_lock);
	return ret;
}

static void sus_unix_clear(void)
{
	spin_lock(&sus_unix_lock);
	WRITE_ONCE(sus_unix_count, 0);
	spin_unlock(&sus_unix_lock);
}

static int sus_unix_show(struct seq_file *m, void *v)
{
	char name[SUS_UNIX_NAME_MAX + 1];
	int i;

	seq_printf(m, "# abstract names: %d/%d, hook=%d, checked=%d, hidden=%d\n",
		   READ_ONCE(sus_unix_count), SUS_UNIX_MAX, (int)connect_hook.installed,
		   atomic_read(&n_checked), atomic_read(&n_hidden));
	spin_lock(&sus_unix_lock);
	for (i = 0; i < sus_unix_count; i++) {
		memcpy(name, sus_unix_names[i].name, sus_unix_names[i].len);
		name[sus_unix_names[i].len] = '\0';
		seq_printf(m, "@%s\n", name);
	}
	spin_unlock(&sus_unix_lock);
	return 0;
}

static int sus_unix_open(struct inode *inode, struct file *file)
{
	if (!susfs_control_visible())
		return -ENOENT;
	return single_open(file, sus_unix_show, NULL);
}

static ssize_t sus_unix_write(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *off)
{
	char buf[16 + SUS_UNIX_NAME_MAX];
	char *name;
	size_t n;
	int ret;

	if (!susfs_control_visible())
		return -ENOENT;
	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	n = strcspn(buf, "\n");
	buf[n] = '\0';

	if (!strcmp(buf, "clear")) {
		sus_unix_clear();
		return len;
	}
	if (!strncmp(buf, "add ", 4)) {
		name = buf + 4;
		if (*name == '@')
			name++;
		ret = sus_unix_add(name, strlen(name));
	} else if (!strncmp(buf, "del ", 4)) {
		name = buf + 4;
		if (*name == '@')
			name++;
		ret = sus_unix_del(name, strlen(name));
	} else {
		ret = -EINVAL;
	}
	return ret ? ret : len;
}

static const struct proc_ops sus_unix_proc_ops = {
	.proc_open = sus_unix_open,
	.proc_read = seq_read,
	.proc_write = sus_unix_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_dir_entry *sus_unix_proc;

int susfs_sus_unix_init(void)
{
	int rc;

	rc = susfs_fp_install(&connect_hook);
	if (rc) {
		pr_warn("sus_unix: connect entry not installed (%d) - abstract listeners stay visible\n",
			rc);
		return rc;
	}

	if (susfs_control_node_allowed()) {
		sus_unix_proc = proc_create("susfs_unix", 0777, NULL, &sus_unix_proc_ops);
		if (!sus_unix_proc)
			pr_warn("proc_create(susfs_unix) failed\n");
	}
	SUSFS_LOGI("sus_unix: armed (connect native+compat, proc=%d)\n", sus_unix_proc ? 1 : 0);
	return 0;
}

void susfs_sus_unix_exit(void)
{
	if (sus_unix_proc) {
		proc_remove(sus_unix_proc);
		sus_unix_proc = NULL;
	}
	if (connect_hook.installed) {
		susfs_fp_remove(&connect_hook);
		susfs_fp_drain();
	}
	pr_info("sus_unix: checked=%d hidden=%d\n",
		atomic_read(&n_checked), atomic_read(&n_hidden));
	sus_unix_clear();
}
