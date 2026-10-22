// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_prop_view.c - per-app property view (SPIKE).
 *
 * bionic maps the Android property area files (/dev/__properties__/u:object_r:<ctx>:s0,
 * 131072 bytes each) MAP_SHARED PROT_READ in zygote before fork, so every app inherits the
 * same mappings.  For an app process this layer replaces such a mapping with a mapping of a
 * fabricated copy file at the SAME address, so the app reads the copy and everything else
 * keeps the real area.  Root, adb shell and every non-app process are untouched.
 *
 * The trigger is the setresuid a zygote child calls when it becomes an app.  A kretprobe
 * ret handler cannot walk the VMAs (atomic context), so it queues a task_work item that runs
 * in process context before the syscall returns to user space.
 *
 * /proc/susfs_prop_view:
 *   add <real_area_path> <copy_path>
 *   clear
 * A read prints one "real copy" line per entry, then the swapped and failed counters.
 */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/version.h>	/* LINUX_VERSION_CODE: mm->mmap vs the vma iterator (6.1) */
#include <linux/fs.h>		/* struct file, filp_open, file_inode, i_size_read */
#include <linux/fcntl.h>	/* O_RDONLY */
#include <linux/file.h>		/* fput */
#include <linux/namei.h>	/* kern_path */
#include <linux/path.h>		/* path_put */
#include <linux/dcache.h>	/* d_backing_inode */
#include <linux/cred.h>		/* get_current_cred, put_cred, override_creds */
#include <linux/sched.h>	/* current, current_uid */
#include <linux/mm.h>		/* struct vm_area_struct, VMA_ITERATOR, vm_mmap */
#include <linux/mm_types.h>	/* the vma iterator's own types */
#include <linux/mman.h>		/* PROT_READ, MAP_SHARED, MAP_FIXED */
#include <linux/mmap_lock.h>	/* mmap_read_lock */
#include <linux/atomic.h>	/* the counters */
#include <linux/slab.h>		/* kzalloc(GFP_ATOMIC), the work item */
#include <linux/task_work.h>	/* task_work_add(TWA_RESUME) */
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include "susfs.h"
#include "susfs_log.h"
#include "ksu_allow_gate.h"	/* susfs_control_visible, susfs_control_node_allowed */
#include "ksu_umount_gate.h"	/* susfs_is_current_proc_umounted_app */
#include "susfs_kprobe.h"

#define SUSFS_PROP_VIEW_MAX 16
#define SUSFS_PROP_VIEW_PATH_MAX 256
/* One area is normally mapped once per process.  A matched VMA beyond this many is counted
 * as failed rather than silently skipped. */
#define SUSFS_PROP_VIEW_HITS 32

struct prop_view_entry {
    char real_path[SUSFS_PROP_VIEW_PATH_MAX];
    char copy_path[SUSFS_PROP_VIEW_PATH_MAX];
    unsigned long ino;
    dev_t dev;
    loff_t size;
    struct file *copy;		/* kept open until clear/exit */
    const struct cred *owner;	/* the writer's creds, used for the swap */
};

struct prop_view_hit {
    unsigned long start;
    unsigned long len;
    int idx;
};

struct prop_view_args {
    struct callback_head cb;
};

static int pv_setresuid_ret(struct kretprobe_instance *ri, struct pt_regs *regs);
static void pv_swap_current(void);

static struct prop_view_entry pv_entries[SUSFS_PROP_VIEW_MAX];
static int pv_n;
/* Read by the ret handler (atomic context), written under pv_lock. */
static bool pv_enabled;
static atomic_t pv_swapped = ATOMIC_INIT(0);
static atomic_t pv_failed = ATOMIC_INIT(0);

/* Protects the entry table and pv_exiting.  The work callback holds it across its swaps,
 * which is the simple correct choice: clear/exit cannot free a copy file or a cred that a
 * swap is still using. */
static DEFINE_MUTEX(pv_lock);
/* Serialises the arm/disarm pair, like uname_ctl_lock. */
static DEFINE_MUTEX(pv_ctl_lock);
static bool pv_registered;
static bool pv_exiting;

static struct kretprobe pv_krp = {
    .kp.symbol_name = SUSFS_SC_WRAPPER_NAME(setresuid),
    .handler = pv_setresuid_ret,
    .maxactive = 32,
};

static int pv_register(void)
{
    int rc;

    mutex_lock(&pv_ctl_lock);
    if (pv_registered) {
        mutex_unlock(&pv_ctl_lock);
        return 0;
    }
    rc = register_kretprobe(&pv_krp);
    if (rc) {
        /* A retry after a failed registration is refused without this. */
        susfs_krp_forget_addr(&pv_krp);
        mutex_unlock(&pv_ctl_lock);
        return rc;
    }
    pv_registered = true;
    mutex_unlock(&pv_ctl_lock);
    SUSFS_LOGI("prop_view: setresuid probe armed\n");
    return 0;
}

static void pv_unregister(void)
{
    mutex_lock(&pv_ctl_lock);
    if (!pv_registered) {
        mutex_unlock(&pv_ctl_lock);
        return;
    }
    unregister_kretprobe(&pv_krp);
    susfs_krp_forget_addr(&pv_krp);
    pv_registered = false;
    mutex_unlock(&pv_ctl_lock);
    SUSFS_LOGI("prop_view: setresuid probe disarmed\n");
}

static int pv_find_index(unsigned long ino, dev_t dev)
{
    int i;

    for (i = 0; i < pv_n; i++)
        if (pv_entries[i].ino == ino && pv_entries[i].dev == dev)
            return i;
    return -1;
}

/* Only the numbers and the size are kept; nothing pins the real area's mount. */
static int pv_resolve_real(const char *real, unsigned long *ino, dev_t *dev,
                           loff_t *size)
{
    struct path p;
    struct inode *inode;
    int rc;

    rc = kern_path(real, LOOKUP_FOLLOW, &p);
    if (rc) {
        pr_info("prop_view: cannot resolve %s: %d\n", real, rc);
        return rc;
    }
    inode = d_backing_inode(p.dentry);
    if (!inode) {
        path_put(&p);
        pr_info("prop_view: no inode for %s\n", real);
        return -ENOENT;
    }
    *ino = inode->i_ino;
    *dev = inode->i_sb->s_dev;
    *size = i_size_read(inode);
    path_put(&p);
    if (*size <= 0) {
        pr_info("prop_view: %s has size %lld\n", real, (long long)*size);
        return -EINVAL;
    }
    return 0;
}

static int pv_open_copy(const char *copy, loff_t size, struct file **out)
{
    struct file *f;
    struct inode *inode;
    int rc;

    f = filp_open(copy, O_RDONLY, 0);
    if (IS_ERR(f)) {
        rc = PTR_ERR(f);
        pr_info("prop_view: cannot open %s: %d\n", copy, rc);
        return rc;
    }
    inode = file_inode(f);
    if (!inode || i_size_read(inode) != size) {
        pr_info("prop_view: %s is %lld bytes, the real area is %lld\n", copy,
                inode ? (long long)i_size_read(inode) : -1LL,
                (long long)size);
        fput(f);
        return -EINVAL;
    }
    *out = f;
    return 0;
}

static int pv_add(const char *real, const char *copy)
{
    struct file *f;
    const struct cred *owner;
    unsigned long ino;
    dev_t dev;
    loff_t size;
    int rc;

    if (strlen(real) >= SUSFS_PROP_VIEW_PATH_MAX ||
        strlen(copy) >= SUSFS_PROP_VIEW_PATH_MAX) {
        pr_info("prop_view: path too long\n");
        return -ENAMETOOLONG;
    }
    rc = pv_resolve_real(real, &ino, &dev, &size);
    if (rc)
        return rc;
    rc = pv_open_copy(copy, size, &f);
    if (rc)
        return rc;

    owner = get_current_cred();

    mutex_lock(&pv_lock);
    if (pv_exiting) {
        rc = -ENODEV;
        goto out_free;
    }
    if (pv_find_index(ino, dev) >= 0) {
        pr_info("prop_view: %s is already added\n", real);
        rc = -EEXIST;
        goto out_free;
    }
    if (pv_n >= SUSFS_PROP_VIEW_MAX) {
        pr_info("prop_view: table full (%d entries)\n", SUSFS_PROP_VIEW_MAX);
        rc = -E2BIG;
        goto out_free;
    }
    if (!pv_n) {
        rc = pv_register();
        if (rc) {
            pr_info("prop_view: probe registration failed %d\n", rc);
            goto out_free;
        }
    }

    strscpy(pv_entries[pv_n].real_path, real, SUSFS_PROP_VIEW_PATH_MAX);
    strscpy(pv_entries[pv_n].copy_path, copy, SUSFS_PROP_VIEW_PATH_MAX);
    pv_entries[pv_n].ino = ino;
    pv_entries[pv_n].dev = dev;
    pv_entries[pv_n].size = size;
    pv_entries[pv_n].copy = f;
    pv_entries[pv_n].owner = owner;
    pv_n++;
    WRITE_ONCE(pv_enabled, true);
    mutex_unlock(&pv_lock);

    SUSFS_LOGI("prop_view: %s -> %s (ino=%lu dev=%lu size=%lld)\n",
               real, copy, ino, (unsigned long)dev, (long long)size);
    return 0;

out_free:
    mutex_unlock(&pv_lock);
    put_cred(owner);
    fput(f);
    return rc;
}

/* The probe stays registered: pv_enabled is what turns the swap off.  Measured on the AVD
 * (android15-6.6): after clear unregistered it and the next add registered it again, the add
 * logged "probe armed" but the handler never ran, and swapped stayed put until a reboot.
 * The other layers register once at init and unregister only at exit; this one now does the
 * same, arming on the first add and disarming in susfs_prop_view_exit(). */
static void pv_clear_entries(void)
{
    int i;

    mutex_lock(&pv_lock);
    WRITE_ONCE(pv_enabled, false);
    for (i = 0; i < pv_n; i++) {
        fput(pv_entries[i].copy);
        put_cred(pv_entries[i].owner);
    }
    pv_n = 0;
    mutex_unlock(&pv_lock);
    SUSFS_LOGI("prop_view: cleared\n");
}

/* Queued from the ret handler; runs from task_work in process context before the syscall
 * returns to user space, where walking the mm and vm_mmap are allowed. */
static void pv_task_work(struct callback_head *cb)
{
    kfree(container_of(cb, struct prop_view_args, cb));
    pv_swap_current();
    module_put(THIS_MODULE);
}

static int pv_setresuid_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct prop_view_args *a;

    (void)ri;
    if (regs_return_value(regs) != 0)
        return 0;
    /* Only an app uid, which is the set the trigger is for. */
    if (current_uid().val % 100000 < 10000)
        return 0;
    if (!READ_ONCE(pv_enabled) || !current->mm)
        return 0;
    /* unregister_kretprobe() cannot cancel a queued task_work, so pin the module until the
     * callback has run.  Same reason and same order as susfs_uname.c. */
    if (!try_module_get(THIS_MODULE))
        return 0;
    a = kzalloc(sizeof(*a), GFP_ATOMIC);
    if (!a) {
        module_put(THIS_MODULE);
        pr_warn_ratelimited("prop_view: no memory for the work item\n");
        return 0;
    }
    a->cb.func = pv_task_work;
    if (task_work_add(current, &a->cb, TWA_RESUME)) {
        kfree(a);
        module_put(THIS_MODULE);
        pr_warn_ratelimited("prop_view: task_work_add failed\n");
    }
    return 0;
}

/* Caller holds pv_lock.  Collects the matching VMAs under mmap_read_lock; the swap itself
 * runs after the mmap read lock is dropped, because vm_mmap takes the mmap lock for write. */
static int pv_collect_hits(struct mm_struct *mm, struct prop_view_hit *hits,
                           int max, int *overflow)
{
    struct vm_area_struct *vma;
    int n = 0;

    *overflow = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    VMA_ITERATOR(vmi, mm, 0);

    for_each_vma(vmi, vma) {
#else
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
        struct inode *inode;
        int idx;

        if (!vma->vm_file)
            continue;
        inode = file_inode(vma->vm_file);
        if (!inode)
            continue;
        idx = pv_find_index(inode->i_ino, inode->i_sb->s_dev);
        if (idx < 0)
            continue;
        if (n >= max) {
            (*overflow)++;
            continue;
        }
        hits[n].start = vma->vm_start;
        hits[n].len = vma->vm_end - vma->vm_start;
        hits[n].idx = idx;
        n++;
    }
    return n;
}

/* Caller holds pv_lock and the entry/copy stay valid for the call. */
static void pv_swap_one(struct prop_view_entry *e, const struct prop_view_hit *hit)
{
    const struct cred *saved;
    unsigned long addr;

    if (hit->len != (unsigned long)e->size) {
        atomic_inc(&pv_failed);
        pr_warn_ratelimited("prop_view: VMA %#lx length %lu, copy is %lld bytes\n",
                            hit->start, hit->len, (long long)e->size);
        return;
    }
    /* The app's SELinux domain may not be allowed to map the copy file's label. */
    saved = override_creds(e->owner);
    addr = vm_mmap(e->copy, hit->start, hit->len, PROT_READ,
                   MAP_SHARED | MAP_FIXED, 0);
    revert_creds(saved);
    if (addr == hit->start) {
        atomic_inc(&pv_swapped);
    } else {
        atomic_inc(&pv_failed);
        pr_warn_ratelimited("prop_view: vm_mmap at %#lx failed %ld\n",
                            hit->start, (long)addr);
    }
}

static void pv_swap_current(void)
{
    struct prop_view_hit hits[SUSFS_PROP_VIEW_HITS];
    struct mm_struct *mm = current->mm;
    int n, overflow, i;

    if (!mm)
        return;
    if (!susfs_is_current_proc_umounted_app())
        return;
    if (!susfs_current_exe_is_app_process())
        return;

    /* Choice for step 1: hold pv_lock across the swaps instead of snapshotting the copies
     * with get_file().  clear/exit then cannot free what a swap is using. */
    mutex_lock(&pv_lock);
    if (!READ_ONCE(pv_enabled) || !pv_n) {
        mutex_unlock(&pv_lock);
        return;
    }

    mmap_read_lock(mm);
    n = pv_collect_hits(mm, hits, ARRAY_SIZE(hits), &overflow);
    mmap_read_unlock(mm);
    if (overflow) {
        atomic_add(overflow, &pv_failed);
        pr_warn_ratelimited("prop_view: %d matching VMA(s) over the hit limit\n",
                            overflow);
    }

    for (i = 0; i < n; i++)
        pv_swap_one(&pv_entries[hits[i].idx], &hits[i]);
    mutex_unlock(&pv_lock);
}

/* ---- /proc/susfs_prop_view ---- */

static int pv_proc_show(struct seq_file *m, void *v)
{
    int i;

    mutex_lock(&pv_lock);
    for (i = 0; i < pv_n; i++)
        seq_printf(m, "%s %s\n", pv_entries[i].real_path,
                   pv_entries[i].copy_path);
    seq_printf(m, "swapped: %d\n", atomic_read(&pv_swapped));
    seq_printf(m, "failed: %d\n", atomic_read(&pv_failed));
    mutex_unlock(&pv_lock);
    return 0;
}

static int pv_proc_open(struct inode *inode, struct file *file)
{
    if (!susfs_control_visible())
        return -ENOENT;
    return single_open(file, pv_proc_show, NULL);
}

static int split_ws(char *buf, char **argv, int max)
{
    int argc = 0;
    char *p = buf;

    while (argc < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        if (*p)
            *p++ = '\0';
    }
    return argc;
}

static ssize_t pv_proc_write(struct file *file, const char __user *buf,
                             size_t len, loff_t *off)
{
    char cmd[640];
    char *argv[4];
    int argc, rc;

    if (!susfs_control_visible())
        return -ENOENT;

    /* Not truncated: a cut path could name a different area and still be accepted. */
    if (len >= sizeof(cmd))
        return -E2BIG;
    if (copy_from_user(cmd, buf, len))
        return -EFAULT;
    cmd[len] = 0;

    argc = split_ws(cmd, argv, 4);
    if (argc == 1 && !strcmp(argv[0], "clear")) {
        pv_clear_entries();
        return len;
    }
    if (argc == 3 && !strcmp(argv[0], "add")) {
        rc = pv_add(argv[1], argv[2]);
        if (rc) {
            pr_info("prop_view: add refused %d\n", rc);
            return rc;
        }
        return len;
    }
    return -EINVAL;
}

static const struct proc_ops pv_proc_ops = {
    .proc_open = pv_proc_open,
    .proc_read = seq_read,
    .proc_write = pv_proc_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static struct proc_dir_entry *pv_proc_entry;

int susfs_prop_view_init(void)
{
    if (susfs_control_node_allowed()) {
        pv_proc_entry = proc_create("susfs_prop_view", 0777, NULL,
                                    &pv_proc_ops);
        if (!pv_proc_entry)
            pr_warn("proc_create(susfs_prop_view) failed\n");
    } else {
        SUSFS_LOGI("susfs_prop_view: /proc node not created (expose_proc=%d lsm=%d)\n",
                   (int)susfs_expose_proc, (int)sus_path_lsm_active());
    }
    return 0;
}

void susfs_prop_view_exit(void)
{
    /* No drain: a queued task_work item holds a module reference until its callback has run,
     * so the kernel cannot reach this exit while one is pending.  Same as susfs_uname.c.
     * pv_exiting is set first, so a write already inside pv_add() refuses after the clear. */
    mutex_lock(&pv_lock);
    pv_exiting = true;
    mutex_unlock(&pv_lock);

    if (pv_proc_entry) {
        proc_remove(pv_proc_entry);
        pv_proc_entry = NULL;
    }
    pv_clear_entries();
    pv_unregister();
}
