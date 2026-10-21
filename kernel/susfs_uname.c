// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_uname.c - spoof uname release/version (SUSFS SPOOF_UNAME).  Upstream patches the body of
 * SYSCALL_DEFINE1(newuname) between the memcpy(utsname) and copy_to_user; the LKM equivalent hooks
 * __arm64_sys_newuname with a kretprobe and rewrites the release/version fields of the user buffer just before
 * the syscall returns - the target pages were just faulted in by the original copy_to_user, so the handler's
 * copy_to_user cannot fault, and "default" copies the device's CURRENT utsname()->release/version at runtime.
 * Upstream semantics: OFF by default, enabled by CMD_SUSFS_SET_UNAME alone, kretprobe registered lazily.
 *
 * Machine spoof (this port only): the same probe also serves /proc/susfs_uname_machine, which rewrites
 * utsname.machine for processes that run translated arm64 code.  A translator passes uname() straight to
 * this kernel, so native code in an arm64 app reads the host's machine while Java's os.arch reads aarch64.
 * Such a process is recognised by a VMA backed by the marker file's inode (the guest linker), which no
 * x86_64 app, root or adb shell maps.  A kretprobe ret handler cannot walk the VMAs (atomic context, no
 * mmap_lock), so it queues a task_work item that walks them in process context before the syscall
 * returns.  The release/version half is independent and keeps its behaviour.
 */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/string.h>
#include <linux/mutex.h>	/* uname_ctl_lock: serialises the arm/disarm pair */
#include <linux/version.h>	/* LINUX_VERSION_CODE: mm->mmap vs the vma iterator (6.1) */
#include <linux/sched.h>	/* current->mm: is this process translated? */
#include <linux/mm.h>		/* struct vm_area_struct, VMA_ITERATOR */
#include <linux/mm_types.h>	/* the vma iterator's own types, as sus_map.c does */
#include <linux/mmap_lock.h>	/* mmap_read_lock: the VMA walk of the work callback */
#include <linux/fs.h>		/* file_inode(), on vma->vm_file */
#include <linux/dcache.h>	/* d_backing_inode: the marker's inode at set time */
#include <linux/atomic.h>	/* the spoofed counter */
#include <linux/namei.h>	/* kern_path: the marker is resolved at set time */
#include <linux/slab.h>		/* kzalloc(GFP_ATOMIC), the work item */
#include <linux/task_work.h>	/* task_work_add(TWA_RESUME): the deferral used by the supercall too */
#include <linux/spinlock.h>	/* the ret handler snapshots the setting in atomic context */
#include <linux/proc_fs.h>	/* the control node */
#include <linux/seq_file.h>
#include "susfs_abi.h"
#include "susfs_log.h"
#include "susfs.h"	/* susfs_abi_path_ok, susfs_control_node_allowed */
#include "ksu_allow_gate.h"	/* susfs_control_visible: the control node gate */
#include "susfs_kprobe.h"

#define SUSFS_UNAME_LEN (__NEW_UTS_LEN + 1)
/* The marker path is stored only to print it back; the spoof matches by (ino, dev).  256 matches the
 * rule pathnames elsewhere and keeps the whole write command in one small buffer. */
#define SUSFS_UNAME_MARKER_MAX 256

static char fake_release[SUSFS_UNAME_LEN];
static char fake_version[SUSFS_UNAME_LEN];
static bool uname_spoof_enabled;

/* Machine spoof state.  A queued work item keeps what it needs, so a set/clear that lands after the
 * item was queued does not change the call it was queued for. */
static char fake_machine[SUSFS_UNAME_LEN];
static char machine_marker_path[SUSFS_UNAME_MARKER_MAX];
static unsigned long machine_marker_ino;
static dev_t machine_marker_dev;
static bool machine_spoof_enabled;
static atomic_t machine_spoofed = ATOMIC_INIT(0);
/* The ret handler runs in atomic context and must not sleep; set/clear run in process context.  This
 * lock is what lets the handler take one consistent snapshot of the setting. */
static DEFINE_SPINLOCK(machine_lock);
/* Serialises the three state changes (release set, machine set, machine clear) so "is any spoof on"
 * and the probe registration are decided together.  Without it a clear that read release as off could
 * unregister a probe that a concurrent release set had just armed, leaving that spoof listed as on
 * with no hook.  uname_ctl_lock only covers one arm/disarm. */
static DEFINE_MUTEX(uname_state_lock);

/* optional insmod-time override; supercall is the primary interface */
module_param_string(release, fake_release, sizeof(fake_release), 0644);
module_param_string(version, fake_version, sizeof(fake_version), 0644);
module_param(uname_spoof_enabled, bool, 0644);
MODULE_PARM_DESC(release, "fake uname release (insmod override)");
MODULE_PARM_DESC(version, "fake uname version (insmod override)");
MODULE_PARM_DESC(uname_spoof_enabled, "enable uname spoofing (insmod override)");

struct uname_args {
    struct new_utsname __user *name;
};

static int kr_newuname_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct uname_args *a = (struct uname_args *)ri->data;
    struct pt_regs *user = (struct pt_regs *)susfs_sc_arg(regs, 0);

    /* this GKI kernel does NOT auto-adjust syscall-wrapper probe regs:
     * susfs_sc_arg(regs, 0) is the struct pt_regs* argument, not the user arg */
    if (!user)
        return 1;   /* no return handler: ri->data would be left stale */

    a->name = (struct new_utsname __user *)susfs_sc_arg(user, 0);
    return 0;
}

/* Queued from the ret handler; runs from task_work in process context before the syscall returns to
 * user space, where walking the mm is allowed. */
struct uname_machine_args {
    struct callback_head cb;
    struct new_utsname __user *name;	/* the buffer the syscall filled, still the caller's */
    unsigned long ino;			/* the marker's identity, snapshotted at queue time */
    dev_t dev;
    char machine[SUSFS_UNAME_LEN];
};

/* Is the marker mapped into @mm?  Only a process running translated arm64 code maps the guest linker;
 * an x86_64 app process, root and adb shell do not.  Identity is (i_ino, s_dev), so a marker deleted and
 * recreated matches only if its filesystem reuses both numbers.
 *   <= 6.0  mm_struct.mmap heads a list, walked with vma->vm_next
 *   >= 6.1  that list is the mm_mt maple tree, walked with the vma iterator (mm.h)
 * Both spellings are header-only, so neither adds a kernel symbol.  The caller holds mmap_read_lock. */
static bool uname_machine_mapped(struct mm_struct *mm, unsigned long ino, dev_t dev)
{
    struct vm_area_struct *vma;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    VMA_ITERATOR(vmi, mm, 0);

    for_each_vma(vmi, vma) {
#else
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
        struct inode *inode;

        if (!vma->vm_file)
            continue;
        inode = file_inode(vma->vm_file);
        if (inode && inode->i_ino == ino && inode->i_sb->s_dev == dev)
            return true;
    }
    return false;
}

static void uname_machine_tw(struct callback_head *cb)
{
    struct uname_machine_args *a = container_of(cb, struct uname_machine_args, cb);
    struct mm_struct *mm = current->mm;
    bool hit = false;

    if (mm && a->name) {
        mmap_read_lock(mm);
        hit = uname_machine_mapped(mm, a->ino, a->dev);
        mmap_read_unlock(mm);
    }
    /* The copy stays outside the lock: copy_to_user may fault, and the fault path takes the same lock
     * for read.  The syscall has not returned to user space yet, so the buffer is still its own. */
    if (hit &&
        !copy_to_user((char __user *)a->name + offsetof(struct new_utsname, machine),
                      a->machine, SUSFS_UNAME_LEN))
        atomic_inc(&machine_spoofed);
    kfree(a);
    module_put(THIS_MODULE);
}

static void uname_machine_queue(struct new_utsname __user *name)
{
    struct uname_machine_args *a;
    unsigned long flags;

    if (!READ_ONCE(machine_spoof_enabled) || !current->mm)
        return;
    /* The callback runs from this module's text after the task returns to user space, and
     * unregister_kretprobe() cannot cancel a task_work that is already queued: pin the module until
     * the callback has run.  Same reason and same order as susfs_supercall.c. */
    if (!try_module_get(THIS_MODULE))
        return;
    a = kzalloc(sizeof(*a), GFP_ATOMIC);
    if (!a) {
        module_put(THIS_MODULE);
        pr_warn_ratelimited("uname machine: no memory for the work item, this call is not spoofed\n");
        return;
    }

    /* One snapshot under the lock, and the setting is re-checked here: a clear that raced the branch
     * above means there is nothing to spoof any more. */
    spin_lock_irqsave(&machine_lock, flags);
    if (!machine_spoof_enabled) {
        spin_unlock_irqrestore(&machine_lock, flags);
        kfree(a);
        module_put(THIS_MODULE);
        return;
    }
    a->name = name;
    a->ino = machine_marker_ino;
    a->dev = machine_marker_dev;
    memcpy(a->machine, fake_machine, sizeof(a->machine));
    spin_unlock_irqrestore(&machine_lock, flags);

    a->cb.func = uname_machine_tw;
    if (task_work_add(current, &a->cb, TWA_RESUME)) {
        kfree(a);
        module_put(THIS_MODULE);
        pr_warn_ratelimited("uname machine: task_work_add failed, this call is not spoofed\n");
    }
}

static int kr_newuname_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct uname_args *a = (struct uname_args *)ri->data;

    if (regs_return_value(regs) != 0)
        return 0;
    if (!a->name)
        return 0;

    /* The two spoofs are independent: each one is on or off on its own, and the probe stays registered
     * while either is on. */
    if (uname_spoof_enabled) {
        if (copy_to_user((char __user *)a->name + offsetof(struct new_utsname, release),
                         fake_release, SUSFS_UNAME_LEN))
            return 0;
        if (copy_to_user((char __user *)a->name + offsetof(struct new_utsname, version),
                         fake_version, SUSFS_UNAME_LEN))
            return 0;
    }
    /* The machine half needs the mm, which cannot be walked here. */
    uname_machine_queue(a->name);
    return 0;
}

static struct kretprobe krp = {
    .kp.symbol_name = SUSFS_SC_WRAPPER_NAME(newuname),
    .entry_handler = kr_newuname_entry,
    .handler = kr_newuname_ret,
    .data_size = sizeof(struct uname_args),
    .maxactive = 32,
};

static bool uname_registered;
/* Set by susfs_uname_exit().  The /proc node holds no module reference (proc_create(..., NULL, ...)
 * takes no owner), so a write that is already inside the handler could call uname_register() after
 * the probe was taken down; registration refuses afterwards. */
static bool uname_exiting;

/* Serialises the arm/disarm pair - the full reasoning is in susfs_avc_spoof.c's avc_ctl_lock:
 * register_kprobe() answers -EINVAL once another task has resolved .addr, and the failure branch
 * below clears .addr - which, without this lock, disarms a probe that is LIVE without ever
 * unregistering it, leaving its breakpoint installed after the module is freed.  Two tasks can
 * send CMD_SUSFS_SET_UNAME at the same time (each supercall runs in its own task_work), and the
 * machine spoof adds a second caller. */
static DEFINE_MUTEX(uname_ctl_lock);

static int uname_register(void)
{
    int rc;

    mutex_lock(&uname_ctl_lock);
    if (uname_exiting) {
        mutex_unlock(&uname_ctl_lock);
        return -ENODEV;
    }
    if (uname_registered) {
        mutex_unlock(&uname_ctl_lock);
        return 0;
    }
    rc = register_kretprobe(&krp);
    if (rc) {
        /* CMD_SUSFS_SET_UNAME can be sent again after a failure, and the address the kernel
         * resolved is already in the struct: without this the retry is refused with -EINVAL
         * and the spoof can never be turned on - see susfs_kprobe.h.  Safe under the lock. */
        susfs_krp_forget_addr(&krp);
        mutex_unlock(&uname_ctl_lock);
        return rc;
    }
    uname_registered = true;
    mutex_unlock(&uname_ctl_lock);
    /* The probe serves both spoofs, so a half that is off shows as "-" rather than an empty value. */
    SUSFS_LOGI("uname spoof armed: release=%s version=%s\n",
            fake_release[0] ? fake_release : "-",
            fake_version[0] ? fake_version : "-");
    return 0;
}

static void uname_unregister(void)
{
    mutex_lock(&uname_ctl_lock);
    if (!uname_registered) {
        mutex_unlock(&uname_ctl_lock);
        return;
    }
    unregister_kretprobe(&krp);
    susfs_krp_forget_addr(&krp);
    uname_registered = false;
    mutex_unlock(&uname_ctl_lock);
}

/* The insmod/sysfs parameter path does not go through the supercall's strscpy_pad(): the
 * kernel's own param_set_copystring() strcpy()s into the buffer and leaves the rest of it
 * alone, so a value that arrived that way needs the same padding before it is handed out. */
static void uname_pad(char *s, size_t size)
{
    size_t n = strnlen(s, size);

    if (n < size)
        memset(s + n, 0, size - n);
}

/* ---- machine spoof: /proc/susfs_uname_machine ---- */

static bool uname_machine_ok(const char *machine)
{
    size_t i;

    /* new_utsname.machine is char[65]; the string has to leave room for the NUL.  Whitespace and
     * control characters are refused: split_ws() already cut the token at one, so a leftover can only
     * be a character that would make the listing unreadable. */
    if (!machine[0] || strlen(machine) >= SUSFS_UNAME_LEN)
        return false;
    for (i = 0; machine[i]; i++) {
        unsigned char c = (unsigned char)machine[i];

        if (c <= ' ' || c == 0x7f)
            return false;
    }
    return true;
}

static int uname_machine_set(const char *machine, const char *marker)
{
    struct path p;
    struct inode *inode;
    unsigned long flags, ino;
    dev_t dev;
    int rc;

    if (!uname_machine_ok(machine))
        return -EINVAL;
    /* The path is stored to print it back; one that cannot fit is refused like one that does not
     * resolve. */
    if (strlen(marker) >= SUSFS_UNAME_MARKER_MAX)
        return -ENOENT;

    /* Resolved now, and only the numbers are kept: nothing pins the linker's mount. */
    rc = kern_path(marker, LOOKUP_FOLLOW, &p);
    if (rc)
        return -ENOENT;
    inode = d_backing_inode(p.dentry);
    if (!inode) {
        path_put(&p);
        return -ENOENT;
    }
    ino = inode->i_ino;
    dev = inode->i_sb->s_dev;
    path_put(&p);

    mutex_lock(&uname_state_lock);
    /* Arm first: a setting that is listed but cannot fire is worse than no setting.  When the
     * release spoof already holds the probe this only confirms it. */
    rc = uname_register();
    if (rc) {
        mutex_unlock(&uname_state_lock);
        return rc;
    }

    spin_lock_irqsave(&machine_lock, flags);
    strscpy_pad(fake_machine, machine, sizeof(fake_machine));
    strscpy(machine_marker_path, marker, sizeof(machine_marker_path));
    machine_marker_ino = ino;
    machine_marker_dev = dev;
    WRITE_ONCE(machine_spoof_enabled, true);
    spin_unlock_irqrestore(&machine_lock, flags);
    mutex_unlock(&uname_state_lock);

    SUSFS_LOGI("susfs_uname_machine: machine=%s marker=%s (ino=%lu dev=%lu)\n",
            machine, marker, ino, (unsigned long)dev);
    return 0;
}

static void uname_machine_clear(void)
{
    unsigned long flags;
    bool was_on;

    mutex_lock(&uname_state_lock);
    spin_lock_irqsave(&machine_lock, flags);
    was_on = machine_spoof_enabled;
    WRITE_ONCE(machine_spoof_enabled, false);
    machine_marker_path[0] = '\0';
    spin_unlock_irqrestore(&machine_lock, flags);
    /* The probe is shared with the release spoof, which has no off switch: it comes down only when
     * nothing needs it any more. */
    if (!uname_spoof_enabled)
        uname_unregister();
    mutex_unlock(&uname_state_lock);
    if (was_on)
        SUSFS_LOGI("susfs_uname_machine: off\n");
}

static int machine_proc_show(struct seq_file *m, void *v)
{
    unsigned long flags;
    char machine[SUSFS_UNAME_LEN];
    char marker[SUSFS_UNAME_MARKER_MAX];
    unsigned long ino;
    dev_t dev;
    bool on;

    /* The lock only covers taking the snapshot: seq_printf() may sleep. */
    spin_lock_irqsave(&machine_lock, flags);
    on = machine_spoof_enabled;
    memcpy(machine, fake_machine, sizeof(machine));
    memcpy(marker, machine_marker_path, sizeof(marker));
    ino = machine_marker_ino;
    dev = machine_marker_dev;
    spin_unlock_irqrestore(&machine_lock, flags);

    if (!on) {
        seq_puts(m, "machine spoof: off\n");
        return 0;
    }
    seq_printf(m, "machine=%s marker=%s (ino=%lu dev=%lu) spoofed=%d\n",
               machine, marker, ino, (unsigned long)dev,
               atomic_read(&machine_spoofed));
    return 0;
}

static int machine_proc_open(struct inode *inode, struct file *file)
{
    /* 0777 is deliberate (the ENOENT contract comes from sus_path's hidden set, not from the mode), so
     * refuse non-root callers here too - see the note in susfs_enable_log.c's log_proc_open(). */
    if (!susfs_control_visible())
        return -ENOENT;
    return single_open(file, machine_proc_show, NULL);
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

static ssize_t machine_proc_write(struct file *file, const char __user *buf,
                                  size_t len, loff_t *off)
{
    char cmd[640];
    char *argv[4];
    int argc, rc;

    /* Same gate as machine_proc_open(): an fd opened before the opener dropped privileges must not
     * become a way in. */
    if (!susfs_control_visible())
        return -ENOENT;

    if (len >= sizeof(cmd))
        len = sizeof(cmd) - 1;
    if (copy_from_user(cmd, buf, len))
        return -EFAULT;
    cmd[len] = 0;

    /* Four slots, so a fourth token is seen and refused instead of being cut off. */
    argc = split_ws(cmd, argv, 4);
    if (argc == 0)
        return -EINVAL;
    if (!strcmp(argv[0], "clear") && argc == 1) {
        uname_machine_clear();
        return len;
    }
    if (!strcmp(argv[0], "set") && argc == 3) {
        rc = uname_machine_set(argv[1], argv[2]);
        if (rc) {
            pr_warn("susfs_uname_machine: set refused %d\n", rc);
            return rc;
        }
        return len;
    }
    return -EINVAL;
}

static const struct proc_ops machine_proc_ops = {
    .proc_open = machine_proc_open,
    .proc_read = seq_read,
    .proc_write = machine_proc_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static struct proc_dir_entry *machine_proc_entry;

int susfs_uname_init(void)
{
    uname_pad(fake_release, sizeof(fake_release));
    uname_pad(fake_version, sizeof(fake_version));

    if (uname_spoof_enabled && fake_release[0] && fake_version[0]) {
        int rc = uname_register();

        if (rc)
            pr_warn("register_kretprobe(newuname) failed %d\n", rc);
    } else {
        SUSFS_LOGI("uname spoof: disabled (enable via CMD_SUSFS_SET_UNAME)\n");
    }

    /* Same gate as the other control nodes: 0777 only makes sense under sus_path's LSM layer, and
     * expose_proc=0 removes the node entirely.  This gate must not return early and skip the work
     * above. */
    if (susfs_control_node_allowed()) {
        machine_proc_entry = proc_create("susfs_uname_machine", 0777, NULL,
                                         &machine_proc_ops);
        if (!machine_proc_entry)
            pr_warn("proc_create(susfs_uname_machine) failed\n");
    } else {
        SUSFS_LOGI("susfs_uname_machine: /proc node not created (expose_proc=%d lsm=%d)\n",
                (int)susfs_expose_proc, (int)sus_path_lsm_active());
    }
    return 0;
}

void susfs_uname_exit(void)
{
    /* Set before the node and the probe go: a write that is already inside machine_proc_write()
     * must not arm the probe again after uname_unregister() cleared it. */
    mutex_lock(&uname_ctl_lock);
    uname_exiting = true;
    mutex_unlock(&uname_ctl_lock);

    if (machine_proc_entry) {
        proc_remove(machine_proc_entry);
        machine_proc_entry = NULL;
    }
    uname_unregister();
}

/* supercall: CMD_SUSFS_SET_UNAME */
void susfs_uname_supercall(void __user **arg)
{
    struct st_susfs_uname info = {0};
    int rc;

    if (copy_from_user(&info, (void __user *)*arg, sizeof(info))) {
        info.err = -EFAULT;
        goto out;
    }
    if (*info.release == '\0' || *info.version == '\0') {
        /* -EINVAL: the buffer came across fine, the VALUE is what is wrong. */
        info.err = -EINVAL;
        goto out;
    }
    /* char[65] ABI fields the caller need not terminate; strcmp() and strscpy() below would run past the end. */
    if (!susfs_abi_path_ok(info.release, sizeof(info.release)) ||
        !susfs_abi_path_ok(info.version, sizeof(info.version))) {
        info.err = -ENAMETOOLONG;
        goto out;
    }

    /* Held across the state writes and the registration, the same section that machine set/clear take:
     * that is what lets "machine off" see this spoof as on and leave the shared probe alone. */
    mutex_lock(&uname_state_lock);

    /* "default" copies the device's CURRENT uname at runtime */
    /* strscpy_pad(), not strscpy(): the kernel's own utsname fields are zero-padded to their
     * full width, so an unpadded copy hands the caller the tail of whatever longer value was
     * set before - residue no genuine uname(2) can produce. */
    if (!strcmp(info.release, "default"))
        strscpy_pad(fake_release, utsname()->release, sizeof(fake_release));
    else
        strscpy_pad(fake_release, info.release, sizeof(fake_release));
    if (!strcmp(info.version, "default"))
        strscpy_pad(fake_version, utsname()->version, sizeof(fake_version));
    else
        strscpy_pad(fake_version, info.version, sizeof(fake_version));

    uname_spoof_enabled = true;
    rc = uname_register();
    mutex_unlock(&uname_state_lock);
    if (rc) {
        info.err = rc;
        goto out;
    }
    info.err = 0;
    SUSFS_LOGI("uname spoof set: release=%s version=%s\n", fake_release, fake_version);
out:
    /* upstream writes back only ->err for input-type commands */
    if (copy_to_user(&((struct st_susfs_uname __user *)*arg)->err,
                     &info.err, sizeof(info.err)))
        pr_warn("uname supercall copy_to_user failed\n");
}
