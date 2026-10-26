/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SUSFS_OR_IDENT_H
#define __SUSFS_OR_IDENT_H

#include <linux/types.h>

struct file;
struct seq_file;

/* keep_ident (this port only): the files a keep_ident open_redirect rule served, each with the
 * identity of the file the caller asked for.  The stat layer asks this table about the file behind
 * an fd and reports that identity (st_dev, st_ino) instead of the redirected file's.
 *
 * Every listed file is held by a reference of the table's own, so its address cannot be reused by
 * another open while it is listed.  An entry is dropped when the table finds it is the file's only
 * holder left (checked on every track), or when its rule goes away. */

/* Called by the name-layer hook for a file it just opened for a keep_ident rule, before the fd is
 * installed.  A full table leaves that file unlisted (counted as "full"). */
void susfs_or_ident_track(struct file *file, unsigned long ino, dev_t dev);

/* The original identity of a listed file; false when the file is not listed. */
bool susfs_or_ident_lookup(const struct file *file, unsigned long *ino, dev_t *dev);

/* Drop the entries served for one rule target (rule deleted or replaced), or all of them. */
void susfs_or_ident_forget(unsigned long ino, dev_t dev);
void susfs_or_ident_clear(void);

/* Lockless "anything listed": the stat layer's fast path. */
bool susfs_or_ident_active(void);

/* One line of counters for /proc/susfs_open_redirect. */
void susfs_or_ident_show(struct seq_file *m);

#endif /* __SUSFS_OR_IDENT_H */
