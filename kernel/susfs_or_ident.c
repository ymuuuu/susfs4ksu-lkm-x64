// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_or_ident.c - keep_ident: which files an open_redirect rule served, and whose identity they report.
 *
 * A redirected open hands the caller the redirected file, so fstat() on that fd reports the redirected file's
 * st_dev/st_ino while stat() on the path reports the original's.  A keep_ident rule records every file it served
 * here, with the original's (ino, dev), and the stat layer (susfs_kstat.c) rewrites those two fields for a call
 * whose file is listed.  Other opens of the redirected file are never listed, so they keep their own identity.
 *
 * Lifetime: struct file is recycled right after its last reference goes, so an address alone cannot tell a served
 * file from a later open of the same file.  The table therefore takes a reference of its own on every listed file:
 * while listed, the address cannot be reused.  An entry the table alone keeps alive (file_count() == 1: no fd, no
 * mapping, nothing in flight) is dropped on the next track, and the file's final release runs then.  That release,
 * and the IN_CLOSE_NOWRITE it sends, therefore comes later than the close() that ended the caller's use of it.
 *
 * Process context only: tracked from the name-layer hook, read from the stat layer's table entries, changed from
 * the rule-management paths.  fput() never sleeps, so it is called under the lock.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/seq_file.h>
#include "susfs_or_ident.h"

/* Live served files at the same time, not opens ever made: an entry goes once nothing but the table holds it. */
#define OR_IDENT_MAX 512

struct or_ident {
	struct file *file;	/* holds a reference */
	unsigned long ino;	/* the original's identity, as stat() on its path reports it */
	dev_t dev;
};

static struct or_ident or_idents[OR_IDENT_MAX];
static unsigned int n_idents;		/* dense: [0, n_idents) are live */
static DEFINE_SPINLOCK(or_ident_lock);

static atomic_t or_ident_tracked = ATOMIC_INIT(0);
static atomic_t or_ident_full = ATOMIC_INIT(0);
static atomic_t or_ident_hits = ATOMIC_INIT(0);

/* Lock held.  The last entry moves into the freed place, so the array stays dense. */
static void or_ident_drop(unsigned int i)
{
	fput(or_idents[i].file);
	or_idents[i] = or_idents[n_idents - 1];
	WRITE_ONCE(n_idents, n_idents - 1);
}

/* Lock held.  file_count() == 1 means the table's reference is the only one, and no new one can appear: the
 * file is in no fd table and no mapping, and this table hands out no pointers. */
static void or_ident_prune(void)
{
	unsigned int i = 0;

	while (i < n_idents) {
		if (file_count(or_idents[i].file) == 1)
			or_ident_drop(i);
		else
			i++;
	}
}

void susfs_or_ident_track(struct file *file, unsigned long ino, dev_t dev)
{
	spin_lock(&or_ident_lock);
	or_ident_prune();
	if (n_idents == OR_IDENT_MAX) {
		spin_unlock(&or_ident_lock);
		atomic_inc(&or_ident_full);
		return;
	}
	or_idents[n_idents].file = get_file(file);
	or_idents[n_idents].ino = ino;
	or_idents[n_idents].dev = dev;
	WRITE_ONCE(n_idents, n_idents + 1);
	spin_unlock(&or_ident_lock);
	atomic_inc(&or_ident_tracked);
}

bool susfs_or_ident_lookup(const struct file *file, unsigned long *ino, dev_t *dev)
{
	bool found = false;
	unsigned int i;

	spin_lock(&or_ident_lock);
	for (i = 0; i < n_idents; i++) {
		if (or_idents[i].file == file) {
			*ino = or_idents[i].ino;
			*dev = or_idents[i].dev;
			found = true;
			break;
		}
	}
	spin_unlock(&or_ident_lock);
	if (found)
		atomic_inc(&or_ident_hits);
	return found;
}

void susfs_or_ident_forget(unsigned long ino, dev_t dev)
{
	unsigned int i = 0;

	spin_lock(&or_ident_lock);
	while (i < n_idents) {
		if (or_idents[i].ino == ino && or_idents[i].dev == dev)
			or_ident_drop(i);
		else
			i++;
	}
	spin_unlock(&or_ident_lock);
}

void susfs_or_ident_clear(void)
{
	spin_lock(&or_ident_lock);
	while (n_idents)
		or_ident_drop(n_idents - 1);
	spin_unlock(&or_ident_lock);
}

bool susfs_or_ident_active(void)
{
	return READ_ONCE(n_idents) != 0;
}

void susfs_or_ident_show(struct seq_file *m)
{
	seq_printf(m, "keep_ident: listed=%u tracked=%d hits=%d full=%d\n",
		   READ_ONCE(n_idents), atomic_read(&or_ident_tracked),
		   atomic_read(&or_ident_hits), atomic_read(&or_ident_full));
}
