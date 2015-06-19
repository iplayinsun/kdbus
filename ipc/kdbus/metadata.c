/*
 * Copyright (C) 2013-2015 Kay Sievers
 * Copyright (C) 2013-2015 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013-2015 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013-2015 David Herrmann <dh.herrmann@gmail.com>
 * Copyright (C) 2013-2015 Linux Foundation
 * Copyright (C) 2014-2015 Djalal Harouni <tixxdz@opendz.org>
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/uio.h>
#include <linux/user_namespace.h>

#include "bus.h"
#include "connection.h"
#include "endpoint.h"
#include "item.h"
#include "message.h"
#include "metadata.h"
#include "names.h"

/**
 * struct kdbus_meta_proc - Process metadata
 * @kref:		Reference counting
 * @lock:		Object lock
 * @collected:		Bitmask of collected items
 * @valid:		Bitmask of collected and valid items
 * @cred:		Credentials
 * @pid:		PID of process
 * @tgid:		TGID of process
 * @ppid:		PPID of process
 * @tid_comm:		TID comm line
 * @pid_comm:		PID comm line
 * @exe_path:		Executable path
 * @root_path:		Root-FS path
 * @cmdline:		Command-line
 * @cgroup:		Full cgroup path
 * @seclabel:		Seclabel
 * @audit_loginuid:	Audit login-UID
 * @audit_sessionid:	Audit session-ID
 */
struct kdbus_meta_proc {
	struct kref kref;
	struct mutex lock;
	u64 collected;
	u64 valid;

	/* KDBUS_ITEM_CREDS */
	/* KDBUS_ITEM_AUXGROUPS */
	/* KDBUS_ITEM_CAPS */
	const struct cred *cred;

	/* KDBUS_ITEM_PIDS */
	struct pid *pid;
	struct pid *tgid;
	struct pid *ppid;

	/* KDBUS_ITEM_TID_COMM */
	char tid_comm[TASK_COMM_LEN];
	/* KDBUS_ITEM_PID_COMM */
	char pid_comm[TASK_COMM_LEN];

	/* KDBUS_ITEM_EXE */
	struct path exe_path;
	struct path root_path;

	/* KDBUS_ITEM_CMDLINE */
	char *cmdline;

	/* KDBUS_ITEM_CGROUP */
	char *cgroup;

	/* KDBUS_ITEM_SECLABEL */
	char *seclabel;

	/* KDBUS_ITEM_AUDIT */
	kuid_t audit_loginuid;
	unsigned int audit_sessionid;
};

/**
 * struct kdbus_meta_conn
 * @kref:		Reference counting
 * @lock:		Object lock
 * @collected:		Bitmask of collected items
 * @valid:		Bitmask of collected and valid items
 * @ts:			Timestamp values
 * @owned_names_items:	Serialized items for owned names
 * @owned_names_size:	Size of @owned_names_items
 * @conn_description:	Connection description
 */
struct kdbus_meta_conn {
	struct kref kref;
	struct mutex lock;
	u64 collected;
	u64 valid;

	/* KDBUS_ITEM_TIMESTAMP */
	struct kdbus_timestamp ts;

	/* KDBUS_ITEM_OWNED_NAME */
	struct kdbus_item *owned_names_items;
	size_t owned_names_size;

	/* KDBUS_ITEM_CONN_DESCRIPTION */
	char *conn_description;
};

/* fixed size equivalent of "kdbus_caps" */
struct kdbus_meta_caps {
	u32 last_cap;
	struct {
		u32 caps[_KERNEL_CAPABILITY_U32S];
	} set[4];
};

/**
 * kdbus_meta_proc_new() - Create process metadata object
 *
 * Return: Pointer to new object on success, ERR_PTR on failure.
 */
struct kdbus_meta_proc *kdbus_meta_proc_new(void)
{
	struct kdbus_meta_proc *mp;

	mp = kzalloc(sizeof(*mp), GFP_KERNEL);
	if (!mp)
		return ERR_PTR(-ENOMEM);

	kref_init(&mp->kref);
	mutex_init(&mp->lock);

	return mp;
}

static void kdbus_meta_proc_free(struct kref *kref)
{
	struct kdbus_meta_proc *mp = container_of(kref, struct kdbus_meta_proc,
						  kref);

	path_put(&mp->exe_path);
	path_put(&mp->root_path);
	if (mp->cred)
		put_cred(mp->cred);
	put_pid(mp->ppid);
	put_pid(mp->tgid);
	put_pid(mp->pid);

	kfree(mp->seclabel);
	kfree(mp->cmdline);
	kfree(mp->cgroup);
	kfree(mp);
}

/**
 * kdbus_meta_proc_ref() - Gain reference
 * @mp:		Process metadata object
 *
 * Return: @mp is returned
 */
struct kdbus_meta_proc *kdbus_meta_proc_ref(struct kdbus_meta_proc *mp)
{
	if (mp)
		kref_get(&mp->kref);
	return mp;
}

/**
 * kdbus_meta_proc_unref() - Drop reference
 * @mp:		Process metadata object
 *
 * Return: NULL
 */
struct kdbus_meta_proc *kdbus_meta_proc_unref(struct kdbus_meta_proc *mp)
{
	if (mp)
		kref_put(&mp->kref, kdbus_meta_proc_free);
	return NULL;
}

static void kdbus_meta_proc_collect_pids(struct kdbus_meta_proc *mp)
{
	struct task_struct *parent;

	mp->pid = get_pid(task_pid(current));
	mp->tgid = get_pid(task_tgid(current));

	rcu_read_lock();
	parent = rcu_dereference(current->real_parent);
	mp->ppid = get_pid(task_tgid(parent));
	rcu_read_unlock();

	mp->valid |= KDBUS_ATTACH_PIDS;
}

static void kdbus_meta_proc_collect_tid_comm(struct kdbus_meta_proc *mp)
{
	get_task_comm(mp->tid_comm, current);
	mp->valid |= KDBUS_ATTACH_TID_COMM;
}

static void kdbus_meta_proc_collect_pid_comm(struct kdbus_meta_proc *mp)
{
	get_task_comm(mp->pid_comm, current->group_leader);
	mp->valid |= KDBUS_ATTACH_PID_COMM;
}

static void kdbus_meta_proc_collect_exe(struct kdbus_meta_proc *mp)
{
	struct file *exe_file;

	rcu_read_lock();
	exe_file = rcu_dereference(current->mm->exe_file);
	if (exe_file) {
		mp->exe_path = exe_file->f_path;
		path_get(&mp->exe_path);
		get_fs_root(current->fs, &mp->root_path);
		mp->valid |= KDBUS_ATTACH_EXE;
	}
	rcu_read_unlock();
}

static int kdbus_meta_proc_collect_cmdline(struct kdbus_meta_proc *mp)
{
	struct mm_struct *mm = current->mm;
	char *cmdline;

	if (!mm->arg_end)
		return 0;

	cmdline = strndup_user((const char __user *)mm->arg_start,
			       mm->arg_end - mm->arg_start);
	if (IS_ERR(cmdline))
		return PTR_ERR(cmdline);

	mp->cmdline = cmdline;
	mp->valid |= KDBUS_ATTACH_CMDLINE;

	return 0;
}

static int kdbus_meta_proc_collect_cgroup(struct kdbus_meta_proc *mp)
{
#ifdef CONFIG_CGROUPS
	void *page;
	char *s;

	page = (void *)__get_free_page(GFP_TEMPORARY);
	if (!page)
		return -ENOMEM;

	s = task_cgroup_path(current, page, PAGE_SIZE);
	if (s) {
		mp->cgroup = kstrdup(s, GFP_KERNEL);
		if (!mp->cgroup) {
			free_page((unsigned long)page);
			return -ENOMEM;
		}
	}

	free_page((unsigned long)page);
	mp->valid |= KDBUS_ATTACH_CGROUP;
#endif

	return 0;
}

static int kdbus_meta_proc_collect_seclabel(struct kdbus_meta_proc *mp)
{
#ifdef CONFIG_SECURITY
	char *ctx = NULL;
	u32 sid, len;
	int ret;

	security_task_getsecid(current, &sid);
	ret = security_secid_to_secctx(sid, &ctx, &len);
	if (ret < 0) {
		/*
		 * EOPNOTSUPP means no security module is active,
		 * lets skip adding the seclabel then. This effectively
		 * drops the SECLABEL item.
		 */
		return (ret == -EOPNOTSUPP) ? 0 : ret;
	}

	mp->seclabel = kstrdup(ctx, GFP_KERNEL);
	security_release_secctx(ctx, len);
	if (!mp->seclabel)
		return -ENOMEM;

	mp->valid |= KDBUS_ATTACH_SECLABEL;
#endif

	return 0;
}

static void kdbus_meta_proc_collect_audit(struct kdbus_meta_proc *mp)
{
#ifdef CONFIG_AUDITSYSCALL
	mp->audit_loginuid = audit_get_loginuid(current);
	mp->audit_sessionid = audit_get_sessionid(current);
	mp->valid |= KDBUS_ATTACH_AUDIT;
#endif
}

/**
 * kdbus_meta_proc_collect() - Collect process metadata
 * @mp:		Process metadata object
 * @what:	Attach flags to collect
 *
 * This collects process metadata from current and saves it in @mp.
 *
 * Return: 0 on success, negative error code on failure.
 */
int kdbus_meta_proc_collect(struct kdbus_meta_proc *mp, u64 what)
{
	int ret;

	if (!mp || !(what & (KDBUS_ATTACH_CREDS |
			     KDBUS_ATTACH_PIDS |
			     KDBUS_ATTACH_AUXGROUPS |
			     KDBUS_ATTACH_TID_COMM |
			     KDBUS_ATTACH_PID_COMM |
			     KDBUS_ATTACH_EXE |
			     KDBUS_ATTACH_CMDLINE |
			     KDBUS_ATTACH_CGROUP |
			     KDBUS_ATTACH_CAPS |
			     KDBUS_ATTACH_SECLABEL |
			     KDBUS_ATTACH_AUDIT)))
		return 0;

	mutex_lock(&mp->lock);

	/* creds, auxgrps and caps share "struct cred" as context */
	{
		const u64 m_cred = KDBUS_ATTACH_CREDS |
				   KDBUS_ATTACH_AUXGROUPS |
				   KDBUS_ATTACH_CAPS;

		if ((what & m_cred) && !(mp->collected & m_cred)) {
			mp->cred = get_current_cred();
			mp->valid |= m_cred;
			mp->collected |= m_cred;
		}
	}

	if ((what & KDBUS_ATTACH_PIDS) &&
	    !(mp->collected & KDBUS_ATTACH_PIDS)) {
		kdbus_meta_proc_collect_pids(mp);
		mp->collected |= KDBUS_ATTACH_PIDS;
	}

	if ((what & KDBUS_ATTACH_TID_COMM) &&
	    !(mp->collected & KDBUS_ATTACH_TID_COMM)) {
		kdbus_meta_proc_collect_tid_comm(mp);
		mp->collected |= KDBUS_ATTACH_TID_COMM;
	}

	if ((what & KDBUS_ATTACH_PID_COMM) &&
	    !(mp->collected & KDBUS_ATTACH_PID_COMM)) {
		kdbus_meta_proc_collect_pid_comm(mp);
		mp->collected |= KDBUS_ATTACH_PID_COMM;
	}

	if ((what & KDBUS_ATTACH_EXE) &&
	    !(mp->collected & KDBUS_ATTACH_EXE)) {
		kdbus_meta_proc_collect_exe(mp);
		mp->collected |= KDBUS_ATTACH_EXE;
	}

	if ((what & KDBUS_ATTACH_CMDLINE) &&
	    !(mp->collected & KDBUS_ATTACH_CMDLINE)) {
		ret = kdbus_meta_proc_collect_cmdline(mp);
		if (ret < 0)
			goto exit_unlock;
		mp->collected |= KDBUS_ATTACH_CMDLINE;
	}

	if ((what & KDBUS_ATTACH_CGROUP) &&
	    !(mp->collected & KDBUS_ATTACH_CGROUP)) {
		ret = kdbus_meta_proc_collect_cgroup(mp);
		if (ret < 0)
			goto exit_unlock;
		mp->collected |= KDBUS_ATTACH_CGROUP;
	}

	if ((what & KDBUS_ATTACH_SECLABEL) &&
	    !(mp->collected & KDBUS_ATTACH_SECLABEL)) {
		ret = kdbus_meta_proc_collect_seclabel(mp);
		if (ret < 0)
			goto exit_unlock;
		mp->collected |= KDBUS_ATTACH_SECLABEL;
	}

	if ((what & KDBUS_ATTACH_AUDIT) &&
	    !(mp->collected & KDBUS_ATTACH_AUDIT)) {
		kdbus_meta_proc_collect_audit(mp);
		mp->collected |= KDBUS_ATTACH_AUDIT;
	}

	ret = 0;

exit_unlock:
	mutex_unlock(&mp->lock);
	return ret;
}

/**
 * kdbus_meta_fake_new() - Create fake metadata object
 *
 * Return: Pointer to new object on success, ERR_PTR on failure.
 */
struct kdbus_meta_fake *kdbus_meta_fake_new(void)
{
	struct kdbus_meta_fake *mf;

	mf = kzalloc(sizeof(*mf), GFP_KERNEL);
	if (!mf)
		return ERR_PTR(-ENOMEM);

	return mf;
}

/**
 * kdbus_meta_fake_free() - Free fake metadata object
 * @mf:		Fake metadata object
 *
 * Return: NULL
 */
struct kdbus_meta_fake *kdbus_meta_fake_free(struct kdbus_meta_fake *mf)
{
	if (mf) {
		put_pid(mf->ppid);
		put_pid(mf->tgid);
		put_pid(mf->pid);
		kfree(mf->seclabel);
		kfree(mf);
	}

	return NULL;
}

/**
 * kdbus_meta_fake_collect() - Fill fake metadata from faked credentials
 * @mf:		Fake metadata object
 * @creds:	Creds to set, may be %NULL
 * @pids:	PIDs to set, may be %NULL
 * @seclabel:	Seclabel to set, may be %NULL
 *
 * This function takes information stored in @creds, @pids and @seclabel and
 * resolves them to kernel-representations, if possible. This call uses the
 * current task's namespaces to resolve the given information.
 *
 * Return: 0 on success, negative error code on failure.
 */
int kdbus_meta_fake_collect(struct kdbus_meta_fake *mf,
			    const struct kdbus_creds *creds,
			    const struct kdbus_pids *pids,
			    const char *seclabel)
{
	if (mf->valid)
		return -EALREADY;

	if (creds) {
		struct user_namespace *ns = current_user_ns();

		mf->uid		= make_kuid(ns, creds->uid);
		mf->euid	= make_kuid(ns, creds->euid);
		mf->suid	= make_kuid(ns, creds->suid);
		mf->fsuid	= make_kuid(ns, creds->fsuid);

		mf->gid		= make_kgid(ns, creds->gid);
		mf->egid	= make_kgid(ns, creds->egid);
		mf->sgid	= make_kgid(ns, creds->sgid);
		mf->fsgid	= make_kgid(ns, creds->fsgid);

		if ((creds->uid   != (uid_t)-1 && !uid_valid(mf->uid))   ||
		    (creds->euid  != (uid_t)-1 && !uid_valid(mf->euid))  ||
		    (creds->suid  != (uid_t)-1 && !uid_valid(mf->suid))  ||
		    (creds->fsuid != (uid_t)-1 && !uid_valid(mf->fsuid)) ||
		    (creds->gid   != (gid_t)-1 && !gid_valid(mf->gid))   ||
		    (creds->egid  != (gid_t)-1 && !gid_valid(mf->egid))  ||
		    (creds->sgid  != (gid_t)-1 && !gid_valid(mf->sgid))  ||
		    (creds->fsgid != (gid_t)-1 && !gid_valid(mf->fsgid)))
			return -EINVAL;

		mf->valid |= KDBUS_ATTACH_CREDS;
	}

	if (pids) {
		mf->pid = get_pid(find_vpid(pids->tid));
		mf->tgid = get_pid(find_vpid(pids->pid));
		mf->ppid = get_pid(find_vpid(pids->ppid));

		if ((pids->tid != 0 && !mf->pid) ||
		    (pids->pid != 0 && !mf->tgid) ||
		    (pids->ppid != 0 && !mf->ppid)) {
			put_pid(mf->pid);
			put_pid(mf->tgid);
			put_pid(mf->ppid);
			mf->pid = NULL;
			mf->tgid = NULL;
			mf->ppid = NULL;
			return -EINVAL;
		}

		mf->valid |= KDBUS_ATTACH_PIDS;
	}

	if (seclabel) {
		mf->seclabel = kstrdup(seclabel, GFP_KERNEL);
		if (!mf->seclabel)
			return -ENOMEM;

		mf->valid |= KDBUS_ATTACH_SECLABEL;
	}

	return 0;
}

/**
 * kdbus_meta_conn_new() - Create connection metadata object
 *
 * Return: Pointer to new object on success, ERR_PTR on failure.
 */
struct kdbus_meta_conn *kdbus_meta_conn_new(void)
{
	struct kdbus_meta_conn *mc;

	mc = kzalloc(sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return ERR_PTR(-ENOMEM);

	kref_init(&mc->kref);
	mutex_init(&mc->lock);

	return mc;
}

static void kdbus_meta_conn_free(struct kref *kref)
{
	struct kdbus_meta_conn *mc =
		container_of(kref, struct kdbus_meta_conn, kref);

	kfree(mc->conn_description);
	kfree(mc->owned_names_items);
	kfree(mc);
}

/**
 * kdbus_meta_conn_ref() - Gain reference
 * @mc:		Connection metadata object
 */
struct kdbus_meta_conn *kdbus_meta_conn_ref(struct kdbus_meta_conn *mc)
{
	if (mc)
		kref_get(&mc->kref);
	return mc;
}

/**
 * kdbus_meta_conn_unref() - Drop reference
 * @mc:		Connection metadata object
 */
struct kdbus_meta_conn *kdbus_meta_conn_unref(struct kdbus_meta_conn *mc)
{
	if (mc)
		kref_put(&mc->kref, kdbus_meta_conn_free);
	return NULL;
}

static void kdbus_meta_conn_collect_timestamp(struct kdbus_meta_conn *mc,
					      u64 msg_seqnum)
{
	mc->ts.monotonic_ns = ktime_get_ns();
	mc->ts.realtime_ns = ktime_get_real_ns();

	if (msg_seqnum)
		mc->ts.seqnum = msg_seqnum;

	mc->valid |= KDBUS_ATTACH_TIMESTAMP;
}

static int kdbus_meta_conn_collect_names(struct kdbus_meta_conn *mc,
					 struct kdbus_conn *conn)
{
	const struct kdbus_name_entry *e;
	struct kdbus_item *item;
	size_t slen, size;

	lockdep_assert_held(&conn->ep->bus->name_registry->rwlock);

	size = 0;
	/* open-code length calculation to avoid final padding */
	list_for_each_entry(e, &conn->names_list, conn_entry)
		size = KDBUS_ALIGN8(size) + KDBUS_ITEM_HEADER_SIZE +
			sizeof(struct kdbus_name) + strlen(e->name) + 1;

	if (!size)
		return 0;

	/* make sure we include zeroed padding for convenience helpers */
	item = kmalloc(KDBUS_ALIGN8(size), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	mc->owned_names_items = item;
	mc->owned_names_size = size;

	list_for_each_entry(e, &conn->names_list, conn_entry) {
		slen = strlen(e->name) + 1;
		kdbus_item_set(item, KDBUS_ITEM_OWNED_NAME, NULL,
			       sizeof(struct kdbus_name) + slen);
		item->name.flags = e->flags;
		memcpy(item->name.name, e->name, slen);
		item = KDBUS_ITEM_NEXT(item);
	}

	/* sanity check: the buffer should be completely written now */
	WARN_ON((u8 *)item !=
			(u8 *)mc->owned_names_items + KDBUS_ALIGN8(size));

	mc->valid |= KDBUS_ATTACH_NAMES;
	return 0;
}

static int kdbus_meta_conn_collect_description(struct kdbus_meta_conn *mc,
					       struct kdbus_conn *conn)
{
	if (!conn->description)
		return 0;

	mc->conn_description = kstrdup(conn->description, GFP_KERNEL);
	if (!mc->conn_description)
		return -ENOMEM;

	mc->valid |= KDBUS_ATTACH_CONN_DESCRIPTION;
	return 0;
}

/**
 * kdbus_meta_conn_collect() - Collect connection metadata
 * @mc:		Message metadata object
 * @conn:	Connection to collect data from
 * @msg_seqnum:	Sequence number of the message to send
 * @what:	Attach flags to collect
 *
 * This collects connection metadata from @msg_seqnum and @conn and saves it
 * in @mc.
 *
 * If KDBUS_ATTACH_NAMES is set in @what and @conn is non-NULL, the caller must
 * hold the name-registry read-lock of conn->ep->bus->registry.
 *
 * Return: 0 on success, negative error code on failure.
 */
int kdbus_meta_conn_collect(struct kdbus_meta_conn *mc,
			    struct kdbus_conn *conn,
			    u64 msg_seqnum, u64 what)
{
	int ret;

	if (!mc || !(what & (KDBUS_ATTACH_TIMESTAMP |
			     KDBUS_ATTACH_NAMES |
			     KDBUS_ATTACH_CONN_DESCRIPTION)))
		return 0;

	mutex_lock(&mc->lock);

	if (msg_seqnum && (what & KDBUS_ATTACH_TIMESTAMP) &&
	    !(mc->collected & KDBUS_ATTACH_TIMESTAMP)) {
		kdbus_meta_conn_collect_timestamp(mc, msg_seqnum);
		mc->collected |= KDBUS_ATTACH_TIMESTAMP;
	}

	if (conn && (what & KDBUS_ATTACH_NAMES) &&
	    !(mc->collected & KDBUS_ATTACH_NAMES)) {
		ret = kdbus_meta_conn_collect_names(mc, conn);
		if (ret < 0)
			goto exit_unlock;
		mc->collected |= KDBUS_ATTACH_NAMES;
	}

	if (conn && (what & KDBUS_ATTACH_CONN_DESCRIPTION) &&
	    !(mc->collected & KDBUS_ATTACH_CONN_DESCRIPTION)) {
		ret = kdbus_meta_conn_collect_description(mc, conn);
		if (ret < 0)
			goto exit_unlock;
		mc->collected |= KDBUS_ATTACH_CONN_DESCRIPTION;
	}

	ret = 0;

exit_unlock:
	mutex_unlock(&mc->lock);
	return ret;
}

/*
 * kdbus_meta_export_prepare() - Prepare metadata for export
 * @mp:		Process metadata, or NULL
 * @mf:		Fake metadata, or NULL
 * @mc:		Connection metadata, or NULL
 * @mask:	Pointer to mask of KDBUS_ATTACH_* flags to export
 * @sz:		Pointer to return the size needed by the metadata
 *
 * Does a conservative calculation of how much space metadata information
 * will take up during export. It is 'conservative' because for string
 * translations in namespaces, it will use the kernel namespaces, which is
 * the longest possible version.
 *
 * The actual size consumed by kdbus_meta_export() may hence vary from the
 * one reported here, but it is guaranteed never to be greater.
 *
 * Return: 0 on success, negative error number otherwise.
 */
int kdbus_meta_export_prepare(struct kdbus_meta_proc *mp,
			      struct kdbus_meta_fake *mf,
			      struct kdbus_meta_conn *mc,
			      u64 *mask, size_t *sz)
{
	char *exe_pathname = NULL;
	void *exe_page = NULL;
	size_t size = 0;
	u64 valid = 0;
	int ret = 0;

	if (WARN_ON(mf && mp))
		mp = NULL;

	if (mf)
		valid |= mf->valid;

	if (mp) {
		mutex_lock(&mp->lock);
		valid |= mp->valid;
		mutex_unlock(&mp->lock);
	}

	if (mc) {
		mutex_lock(&mc->lock);
		valid |= mc->valid;
		mutex_unlock(&mc->lock);
	}

	*mask &= valid;

	if (!*mask)
		goto exit;

	/* process metadata */

	if ((mp || mf) && (*mask & KDBUS_ATTACH_CREDS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_creds));

	if ((mp || mf) && (*mask & KDBUS_ATTACH_PIDS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_pids));

	if (mp && (*mask & KDBUS_ATTACH_AUXGROUPS))
		size += KDBUS_ITEM_SIZE(mp->cred->group_info->ngroups *
					sizeof(u64));

	if (mp && (*mask & KDBUS_ATTACH_TID_COMM))
		size += KDBUS_ITEM_SIZE(strlen(mp->tid_comm) + 1);

	if (mp && (*mask & KDBUS_ATTACH_PID_COMM))
		size += KDBUS_ITEM_SIZE(strlen(mp->pid_comm) + 1);

	if (mp && (*mask & KDBUS_ATTACH_EXE)) {
		exe_page = (void *)__get_free_page(GFP_TEMPORARY);
		if (!exe_page) {
			ret = -ENOMEM;
			goto exit;
		}

		exe_pathname = d_path(&mp->exe_path, exe_page, PAGE_SIZE);
		if (IS_ERR(exe_pathname)) {
			ret = PTR_ERR(exe_pathname);
			goto exit;
		}

		size += KDBUS_ITEM_SIZE(strlen(exe_pathname) + 1);
		free_page((unsigned long)exe_page);
	}

	if (mp && (*mask & KDBUS_ATTACH_CMDLINE))
		size += KDBUS_ITEM_SIZE(strlen(mp->cmdline) + 1);

	if (mp && (*mask & KDBUS_ATTACH_CGROUP))
		size += KDBUS_ITEM_SIZE(strlen(mp->cgroup) + 1);

	if (mp && (*mask & KDBUS_ATTACH_CAPS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_meta_caps));

	if ((mp || mf) && (*mask & KDBUS_ATTACH_SECLABEL))
		size += KDBUS_ITEM_SIZE(strlen(mp ? mp->seclabel
						  : mf->seclabel) + 1);

	if (mp && (*mask & KDBUS_ATTACH_AUDIT))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_audit));

	/* connection metadata */

	if (mc && (*mask & KDBUS_ATTACH_NAMES))
		size += KDBUS_ALIGN8(mc->owned_names_size);

	if (mc && (*mask & KDBUS_ATTACH_CONN_DESCRIPTION))
		size += KDBUS_ITEM_SIZE(strlen(mc->conn_description) + 1);

	if (mc && (*mask & KDBUS_ATTACH_TIMESTAMP))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_timestamp));

exit:
	*sz = size;

	return ret;
}

static int kdbus_meta_push_kvec(struct kvec *kvec,
				struct kdbus_item_header *hdr,
				u64 type, void *payload,
				size_t payload_size, u64 *size)
{
	hdr->type = type;
	hdr->size = KDBUS_ITEM_HEADER_SIZE + payload_size;
	kdbus_kvec_set(kvec++, hdr, sizeof(*hdr), size);
	kdbus_kvec_set(kvec++, payload, payload_size, size);
	return 2 + !!kdbus_kvec_pad(kvec++, size);
}

static void kdbus_meta_export_caps(struct kdbus_meta_caps *out,
				   const struct kdbus_meta_proc *mp,
				   struct user_namespace *user_ns)
{
	struct user_namespace *iter;
	const struct cred *cred = mp->cred;
	bool parent = false, owner = false;
	int i;

	/*
	 * This translates the effective capabilities of 'cred' into the given
	 * user-namespace. If the given user-namespace is a child-namespace of
	 * the user-namespace of 'cred', the mask can be copied verbatim. If
	 * not, the mask is cleared.
	 * There's one exception: If 'cred' is the owner of any user-namespace
	 * in the path between the given user-namespace and the user-namespace
	 * of 'cred', then it has all effective capabilities set. This means,
	 * the user who created a user-namespace always has all effective
	 * capabilities in any child namespaces. Note that this is based on the
	 * uid of the namespace creator, not the task hierarchy.
	 */
	for (iter = user_ns; iter; iter = iter->parent) {
		if (iter == cred->user_ns) {
			parent = true;
			break;
		}

		if (iter == &init_user_ns)
			break;

		if ((iter->parent == cred->user_ns) &&
		    uid_eq(iter->owner, cred->euid)) {
			owner = true;
			break;
		}
	}

	out->last_cap = CAP_LAST_CAP;

	CAP_FOR_EACH_U32(i) {
		if (parent) {
			out->set[0].caps[i] = cred->cap_inheritable.cap[i];
			out->set[1].caps[i] = cred->cap_permitted.cap[i];
			out->set[2].caps[i] = cred->cap_effective.cap[i];
			out->set[3].caps[i] = cred->cap_bset.cap[i];
		} else if (owner) {
			out->set[0].caps[i] = 0U;
			out->set[1].caps[i] = ~0U;
			out->set[2].caps[i] = ~0U;
			out->set[3].caps[i] = ~0U;
		} else {
			out->set[0].caps[i] = 0U;
			out->set[1].caps[i] = 0U;
			out->set[2].caps[i] = 0U;
			out->set[3].caps[i] = 0U;
		}
	}

	/* clear unused bits */
	for (i = 0; i < 4; i++)
		out->set[i].caps[CAP_TO_INDEX(CAP_LAST_CAP)] &=
					CAP_LAST_U32_VALID_MASK;
}

/* This is equivalent to from_kuid_munged(), but maps INVALID_UID to itself */
static uid_t kdbus_from_kuid_keep(struct user_namespace *ns, kuid_t uid)
{
	return uid_valid(uid) ? from_kuid_munged(ns, uid) : ((uid_t)-1);
}

/* This is equivalent to from_kgid_munged(), but maps INVALID_GID to itself */
static gid_t kdbus_from_kgid_keep(struct user_namespace *ns, kgid_t gid)
{
	return gid_valid(gid) ? from_kgid_munged(ns, gid) : ((gid_t)-1);
}

/**
 * kdbus_meta_export() - export information from metadata into a slice
 * @mp:		Process metadata, or NULL
 * @mf:		Fake metadata, or NULL
 * @mc:		Connection metadata, or NULL
 * @conn:	Target connection to translate metadata into
 * @mask:	Mask of KDBUS_ATTACH_* flags to export
 * @slice:	The slice to export to
 * @offset:	The offset inside @slice to write to
 * @real_size:	The real size the metadata consumed
 *
 * This function exports information from metadata into @slice at offset
 * @offset inside that slice. Only information that is requested in @mask
 * and that has been collected before is exported.
 *
 * In order to make sure not to write out of bounds, @mask must be the same
 * value that was previously returned from kdbus_meta_export_prepare(). The
 * function will, however, not necessarily write as many bytes as returned by
 * kdbus_meta_export_prepare(); depending on the namespaces in question, it
 * might use up less than that.
 *
 * All information will be translated using the namespaces of @conn.
 *
 * Return: 0 on success, negative error number otherwise.
 */
int kdbus_meta_export(struct kdbus_meta_proc *mp,
		      struct kdbus_meta_fake *mf,
		      struct kdbus_meta_conn *mc,
		      struct kdbus_conn *conn,
		      u64 mask,
		      struct kdbus_pool_slice *slice,
		      off_t offset,
		      size_t *real_size)
{
	struct user_namespace *user_ns = conn->user_ns;
	struct kdbus_item_header item_hdr[13], *hdr;
	char *exe_pathname = NULL;
	struct kdbus_creds creds;
	struct kdbus_pids pids;
	void *exe_page = NULL;
	struct kvec kvec[40];
	u64 *auxgrps = NULL;
	size_t cnt = 0;
	u64 size = 0;
	int ret = 0;

	if (WARN_ON(mf && mp))
		mp = NULL;

	hdr = &item_hdr[0];

	if (mask == 0) {
		*real_size = 0;
		return 0;
	}

	/* process metadata */

	if (mf && (mask & KDBUS_ATTACH_CREDS)) {
		creds.uid	= kdbus_from_kuid_keep(user_ns, mf->uid);
		creds.euid	= kdbus_from_kuid_keep(user_ns, mf->euid);
		creds.suid	= kdbus_from_kuid_keep(user_ns, mf->suid);
		creds.fsuid	= kdbus_from_kuid_keep(user_ns, mf->fsuid);
		creds.gid	= kdbus_from_kgid_keep(user_ns, mf->gid);
		creds.egid	= kdbus_from_kgid_keep(user_ns, mf->egid);
		creds.sgid	= kdbus_from_kgid_keep(user_ns, mf->sgid);
		creds.fsgid	= kdbus_from_kgid_keep(user_ns, mf->fsgid);

		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++, KDBUS_ITEM_CREDS,
					    &creds, sizeof(creds), &size);
	} else if (mp && (mask & KDBUS_ATTACH_CREDS)) {
		const struct cred *c = mp->cred;

		creds.uid	= kdbus_from_kuid_keep(user_ns, c->uid);
		creds.euid	= kdbus_from_kuid_keep(user_ns, c->euid);
		creds.suid	= kdbus_from_kuid_keep(user_ns, c->suid);
		creds.fsuid	= kdbus_from_kuid_keep(user_ns, c->fsuid);
		creds.gid	= kdbus_from_kgid_keep(user_ns, c->gid);
		creds.egid	= kdbus_from_kgid_keep(user_ns, c->egid);
		creds.sgid	= kdbus_from_kgid_keep(user_ns, c->sgid);
		creds.fsgid	= kdbus_from_kgid_keep(user_ns, c->fsgid);

		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++, KDBUS_ITEM_CREDS,
					    &creds, sizeof(creds), &size);
	}

	if (mf && (mask & KDBUS_ATTACH_PIDS)) {
		pids.pid = pid_nr_ns(mf->tgid, conn->pid_ns);
		pids.tid = pid_nr_ns(mf->pid, conn->pid_ns);
		pids.ppid = pid_nr_ns(mf->ppid, conn->pid_ns);

		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++, KDBUS_ITEM_PIDS,
					    &pids, sizeof(pids), &size);
	} else if (mp && (mask & KDBUS_ATTACH_PIDS)) {
		pids.pid = pid_nr_ns(mp->tgid, conn->pid_ns);
		pids.tid = pid_nr_ns(mp->pid, conn->pid_ns);
		pids.ppid = pid_nr_ns(mp->ppid, conn->pid_ns);

		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++, KDBUS_ITEM_PIDS,
					    &pids, sizeof(pids), &size);
	}

	if (mp && (mask & KDBUS_ATTACH_AUXGROUPS)) {
		const struct group_info *info = mp->cred->group_info;
		size_t i, n, payload_size;

		n = info->ngroups;
		payload_size = n * sizeof(u64);
		auxgrps = kmalloc(payload_size, GFP_KERNEL);
		if (!auxgrps) {
			ret = -ENOMEM;
			goto exit;
		}

		for (i = 0; i < n; ++i)
			auxgrps[i] = from_kgid_munged(user_ns,
						      GROUP_AT(info, i));

		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_AUXGROUPS,
					    auxgrps, payload_size, &size);
	}

	if (mp && (mask & KDBUS_ATTACH_TID_COMM))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_TID_COMM, mp->tid_comm,
					    strlen(mp->tid_comm) + 1, &size);

	if (mp && (mask & KDBUS_ATTACH_PID_COMM))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_PID_COMM, mp->pid_comm,
					    strlen(mp->pid_comm) + 1, &size);

	if (mp && (mask & KDBUS_ATTACH_EXE)) {
		struct path p;

		/*
		 * TODO: We need access to __d_path() so we can write the path
		 * relative to conn->root_path. Once upstream, we need
		 * EXPORT_SYMBOL(__d_path) or an equivalent of d_path() that
		 * takes the root path directly. Until then, we drop this item
		 * if the root-paths differ.
		 */

		get_fs_root(current->fs, &p);
		if (path_equal(&p, &mp->root_path) &&
		    path_equal(&p, &conn->root_path)) {
			exe_page = (void *)__get_free_page(GFP_TEMPORARY);
			if (!exe_page) {
				path_put(&p);
				ret = -ENOMEM;
				goto exit;
			}

			exe_pathname = d_path(&mp->exe_path, exe_page,
					      PAGE_SIZE);
			if (IS_ERR(exe_pathname)) {
				path_put(&p);
				ret = PTR_ERR(exe_pathname);
				goto exit;
			}

			cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
						    KDBUS_ITEM_EXE,
						    exe_pathname,
						    strlen(exe_pathname) + 1,
						    &size);
		}
		path_put(&p);
	}

	if (mp && (mask & KDBUS_ATTACH_CMDLINE))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_CMDLINE, mp->cmdline,
					    strlen(mp->cmdline) + 1, &size);

	if (mp && (mask & KDBUS_ATTACH_CGROUP))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_CGROUP, mp->cgroup,
					    strlen(mp->cgroup) + 1, &size);

	if (mp && (mask & KDBUS_ATTACH_CAPS)) {
		struct kdbus_meta_caps caps = {};

		kdbus_meta_export_caps(&caps, mp, user_ns);
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_CAPS, &caps,
					    sizeof(caps), &size);
	}

	if (mf && (mask & KDBUS_ATTACH_SECLABEL))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_SECLABEL, mf->seclabel,
					    strlen(mf->seclabel) + 1, &size);
	else if (mp && (mask & KDBUS_ATTACH_SECLABEL))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_SECLABEL, mp->seclabel,
					    strlen(mp->seclabel) + 1, &size);

	if (mp && (mask & KDBUS_ATTACH_AUDIT)) {
		struct kdbus_audit a = {
			.loginuid = from_kuid(user_ns, mp->audit_loginuid),
			.sessionid = mp->audit_sessionid,
		};

		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++, KDBUS_ITEM_AUDIT,
					    &a, sizeof(a), &size);
	}

	/* connection metadata */

	if (mc && (mask & KDBUS_ATTACH_NAMES))
		kdbus_kvec_set(&kvec[cnt++], mc->owned_names_items,
			       KDBUS_ALIGN8(mc->owned_names_size), &size);

	if (mc && (mask & KDBUS_ATTACH_CONN_DESCRIPTION))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_CONN_DESCRIPTION,
					    mc->conn_description,
					    strlen(mc->conn_description) + 1,
					    &size);

	if (mc && (mask & KDBUS_ATTACH_TIMESTAMP))
		cnt += kdbus_meta_push_kvec(kvec + cnt, hdr++,
					    KDBUS_ITEM_TIMESTAMP, &mc->ts,
					    sizeof(mc->ts), &size);

	ret = kdbus_pool_slice_copy_kvec(slice, offset, kvec, cnt, size);
	*real_size = size;

exit:
	kfree(auxgrps);

	if (exe_page)
		free_page((unsigned long)exe_page);

	return ret;
}

struct kdbus_meta_staging {
	const struct kdbus_meta_proc *mp;
	const struct kdbus_meta_fake *mf;
	const struct kdbus_meta_conn *mc;
	const struct kdbus_conn *conn;
	u64 mask;

	void *exe;
	const char *exe_path;
};

static size_t kdbus_meta_measure(struct kdbus_meta_staging *staging)
{
	const struct kdbus_meta_proc *mp = staging->mp;
	const struct kdbus_meta_fake *mf = staging->mf;
	const struct kdbus_meta_conn *mc = staging->mc;
	const u64 mask = staging->mask;
	size_t size = 0;

	/* process metadata */

	if (mf && (mask & KDBUS_ATTACH_CREDS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_creds));
	else if (mp && (mask & KDBUS_ATTACH_CREDS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_creds));

	if (mf && (mask & KDBUS_ATTACH_PIDS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_pids));
	else if (mp && (mask & KDBUS_ATTACH_PIDS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_pids));

	if (mp && (mask & KDBUS_ATTACH_AUXGROUPS))
		size += KDBUS_ITEM_SIZE(mp->cred->group_info->ngroups *
					sizeof(u64));

	if (mp && (mask & KDBUS_ATTACH_TID_COMM))
		size += KDBUS_ITEM_SIZE(strlen(mp->tid_comm) + 1);

	if (mp && (mask & KDBUS_ATTACH_PID_COMM))
		size += KDBUS_ITEM_SIZE(strlen(mp->pid_comm) + 1);

	if (staging->exe_path && (mask & KDBUS_ATTACH_EXE))
		size += KDBUS_ITEM_SIZE(strlen(staging->exe_path) + 1);

	if (mp && (mask & KDBUS_ATTACH_CMDLINE))
		size += KDBUS_ITEM_SIZE(strlen(mp->cmdline) + 1);

	if (mp && (mask & KDBUS_ATTACH_CGROUP))
		size += KDBUS_ITEM_SIZE(strlen(mp->cgroup) + 1);

	if (mp && (mask & KDBUS_ATTACH_CAPS))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_meta_caps));

	if (mf && (mask & KDBUS_ATTACH_SECLABEL))
		size += KDBUS_ITEM_SIZE(strlen(mf->seclabel) + 1);
	else if (mp && (mask & KDBUS_ATTACH_SECLABEL))
		size += KDBUS_ITEM_SIZE(strlen(mp->seclabel) + 1);

	if (mp && (mask & KDBUS_ATTACH_AUDIT))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_audit));

	/* connection metadata */

	if (mc && (mask & KDBUS_ATTACH_NAMES))
		size += KDBUS_ALIGN8(mc->owned_names_size);

	if (mc && (mask & KDBUS_ATTACH_CONN_DESCRIPTION))
		size += KDBUS_ITEM_SIZE(strlen(mc->conn_description) + 1);

	if (mc && (mask & KDBUS_ATTACH_TIMESTAMP))
		size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_timestamp));

	return size;
}

static struct kdbus_item *kdbus_write_head(struct kdbus_item **iter,
					   u64 type, u64 size)
{
	struct kdbus_item *item = *iter;
	size_t padding;

	item->type = type;
	item->size = KDBUS_ITEM_HEADER_SIZE + size;

	/* clear padding */
	padding = KDBUS_ALIGN8(item->size) - item->size;
	if (padding)
		memset(item->data + size, 0, padding);

	*iter = KDBUS_ITEM_NEXT(item);
	return item;
}

static struct kdbus_item *kdbus_write_full(struct kdbus_item **iter,
					   u64 type, u64 size, const void *data)
{
	struct kdbus_item *item;

	item = kdbus_write_head(iter, type, size);
	memcpy(item->data, data, size);
	return item;
}

static size_t kdbus_meta_write(struct kdbus_meta_staging *staging, void *mem,
			       size_t size)
{
	struct user_namespace *user_ns = staging->conn->user_ns;
	struct pid_namespace *pid_ns = staging->conn->pid_ns;
	struct kdbus_item *item = NULL, *items = mem;
	u8 *end, *owned_names_end = NULL;

	/* process metadata */

	if (staging->mf && (staging->mask & KDBUS_ATTACH_CREDS)) {
		const struct kdbus_meta_fake *mf = staging->mf;

		item = kdbus_write_head(&items, KDBUS_ITEM_CREDS,
					sizeof(struct kdbus_creds));
		item->creds = (struct kdbus_creds){
			.uid	= kdbus_from_kuid_keep(user_ns, mf->uid),
			.euid	= kdbus_from_kuid_keep(user_ns, mf->euid),
			.suid	= kdbus_from_kuid_keep(user_ns, mf->suid),
			.fsuid	= kdbus_from_kuid_keep(user_ns, mf->fsuid),
			.gid	= kdbus_from_kgid_keep(user_ns, mf->gid),
			.egid	= kdbus_from_kgid_keep(user_ns, mf->egid),
			.sgid	= kdbus_from_kgid_keep(user_ns, mf->sgid),
			.fsgid	= kdbus_from_kgid_keep(user_ns, mf->fsgid),
		};
	} else if (staging->mp && (staging->mask & KDBUS_ATTACH_CREDS)) {
		const struct cred *c = staging->mp->cred;

		item = kdbus_write_head(&items, KDBUS_ITEM_CREDS,
					sizeof(struct kdbus_creds));
		item->creds = (struct kdbus_creds){
			.uid	= kdbus_from_kuid_keep(user_ns, c->uid),
			.euid	= kdbus_from_kuid_keep(user_ns, c->euid),
			.suid	= kdbus_from_kuid_keep(user_ns, c->suid),
			.fsuid	= kdbus_from_kuid_keep(user_ns, c->fsuid),
			.gid	= kdbus_from_kgid_keep(user_ns, c->gid),
			.egid	= kdbus_from_kgid_keep(user_ns, c->egid),
			.sgid	= kdbus_from_kgid_keep(user_ns, c->sgid),
			.fsgid	= kdbus_from_kgid_keep(user_ns, c->fsgid),
		};
	}

	if (staging->mf && (staging->mask & KDBUS_ATTACH_PIDS)) {
		item = kdbus_write_head(&items, KDBUS_ITEM_PIDS,
					sizeof(struct kdbus_pids));
		item->pids = (struct kdbus_pids){
			.pid = pid_nr_ns(staging->mf->tgid, pid_ns),
			.tid = pid_nr_ns(staging->mf->pid, pid_ns),
			.ppid = pid_nr_ns(staging->mf->ppid, pid_ns),
		};
	} else if (staging->mp && (staging->mask & KDBUS_ATTACH_PIDS)) {
		item = kdbus_write_head(&items, KDBUS_ITEM_PIDS,
					sizeof(struct kdbus_pids));
		item->pids = (struct kdbus_pids){
			.pid = pid_nr_ns(staging->mp->tgid, pid_ns),
			.tid = pid_nr_ns(staging->mp->pid, pid_ns),
			.ppid = pid_nr_ns(staging->mp->ppid, pid_ns),
		};
	}

	if (staging->mp && (staging->mask & KDBUS_ATTACH_AUXGROUPS)) {
		const struct group_info *info = staging->mp->cred->group_info;
		size_t i;

		item = kdbus_write_head(&items, KDBUS_ITEM_AUXGROUPS,
					info->ngroups * sizeof(u64));
		for (i = 0; i < info->ngroups; ++i)
			item->data64[i] = from_kgid_munged(user_ns,
							   GROUP_AT(info, i));
	}

	if (staging->mp && (staging->mask & KDBUS_ATTACH_TID_COMM))
		item = kdbus_write_full(&items, KDBUS_ITEM_TID_COMM,
					strlen(staging->mp->tid_comm) + 1,
					staging->mp->tid_comm);

	if (staging->mp && (staging->mask & KDBUS_ATTACH_PID_COMM))
		item = kdbus_write_full(&items, KDBUS_ITEM_PID_COMM,
					strlen(staging->mp->pid_comm) + 1,
					staging->mp->pid_comm);

	if (staging->exe_path && (staging->mask & KDBUS_ATTACH_EXE))
		item = kdbus_write_full(&items, KDBUS_ITEM_EXE,
					strlen(staging->exe_path) + 1,
					staging->exe_path);

	if (staging->mp && (staging->mask & KDBUS_ATTACH_CMDLINE))
		item = kdbus_write_full(&items, KDBUS_ITEM_CMDLINE,
					strlen(staging->mp->cmdline) + 1,
					staging->mp->cmdline);

	if (staging->mp && (staging->mask & KDBUS_ATTACH_CGROUP))
		item = kdbus_write_full(&items, KDBUS_ITEM_CGROUP,
					strlen(staging->mp->cgroup) + 1,
					staging->mp->cgroup);

	if (staging->mp && (staging->mask & KDBUS_ATTACH_CAPS)) {
		item = kdbus_write_head(&items, KDBUS_ITEM_CAPS,
					sizeof(struct kdbus_meta_caps));
		kdbus_meta_export_caps((void*)&item->caps, staging->mp,
				       user_ns);
	}

	if (staging->mf && (staging->mask & KDBUS_ATTACH_SECLABEL))
		item = kdbus_write_full(&items, KDBUS_ITEM_SECLABEL,
					strlen(staging->mf->seclabel) + 1,
					staging->mf->seclabel);
	else if (staging->mp && (staging->mask & KDBUS_ATTACH_SECLABEL))
		item = kdbus_write_full(&items, KDBUS_ITEM_SECLABEL,
					strlen(staging->mp->seclabel) + 1,
					staging->mp->seclabel);

	if (staging->mp && (staging->mask & KDBUS_ATTACH_AUDIT)) {
		item = kdbus_write_head(&items, KDBUS_ITEM_AUDIT,
					sizeof(struct kdbus_audit));
		item->audit = (struct kdbus_audit){
			.loginuid = from_kuid(user_ns,
					      staging->mp->audit_loginuid),
			.sessionid = staging->mp->audit_sessionid,
		};
	}

	/* connection metadata */

	if (staging->mc && (staging->mask & KDBUS_ATTACH_NAMES)) {
		memcpy(items, staging->mc->owned_names_items,
		       KDBUS_ALIGN8(staging->mc->owned_names_size));
		owned_names_end = (u8 *)items + staging->mc->owned_names_size;
		items = (void *)KDBUS_ALIGN8((unsigned long)owned_names_end);
	}

	if (staging->mc && (staging->mask & KDBUS_ATTACH_CONN_DESCRIPTION))
		item = kdbus_write_full(&items, KDBUS_ITEM_CONN_DESCRIPTION,
				strlen(staging->mc->conn_description) + 1,
				staging->mc->conn_description);

	if (staging->mc && (staging->mask & KDBUS_ATTACH_TIMESTAMP))
		item = kdbus_write_full(&items, KDBUS_ITEM_TIMESTAMP,
					sizeof(staging->mc->ts),
					&staging->mc->ts);

	/*
	 * Return real size (minus trailing padding). In case of 'owned_names'
	 * we cannot deduce it from item->size, so treat it special.
	 */

	if (items == (void *)KDBUS_ALIGN8((unsigned long)owned_names_end))
		end = owned_names_end;
	else if (item)
		end = (u8 *)item + item->size;
	else
		end = mem;

	WARN_ON((u8 *)items - (u8 *)mem != size);
	WARN_ON((void *)KDBUS_ALIGN8((unsigned long)end) != (void *)items);

	return end - (u8 *)mem;
}

int kdbus_meta_emit(struct kdbus_meta_proc *mp,
		    struct kdbus_meta_fake *mf,
		    struct kdbus_meta_conn *mc,
		    struct kdbus_conn *conn,
		    u64 mask,
		    struct kdbus_item **out_items,
		    size_t *out_size)
{
	struct kdbus_meta_staging staging = {};
	struct kdbus_item *items = NULL;
	size_t size = 0;
	int ret;

	if (WARN_ON(mf && mp))
		mp = NULL;

	staging.mp = mp;
	staging.mf = mf;
	staging.mc = mc;
	staging.conn = conn;

	/* get mask of valid items */
	if (mf)
		staging.mask |= mf->valid;
	if (mp) {
		mutex_lock(&mp->lock);
		staging.mask |= mp->valid;
		mutex_unlock(&mp->lock);
	}
	if (mc) {
		mutex_lock(&mc->lock);
		staging.mask |= mc->valid;
		mutex_unlock(&mc->lock);
	}

	staging.mask &= mask;

	if (!staging.mask) { /* bail out if nothing to do */
		ret = 0;
		goto exit;
	}

	/* EXE is special as it needs a temporary page to assemble */
	if (mp && (staging.mask & KDBUS_ATTACH_EXE)) {
		struct path p;

		/*
		 * XXX: We need access to __d_path() so we can write the path
		 * relative to conn->root_path. Once upstream, we need
		 * EXPORT_SYMBOL(__d_path) or an equivalent of d_path() that
		 * takes the root path directly. Until then, we drop this item
		 * if the root-paths differ.
		 */

		get_fs_root(current->fs, &p);
		if (path_equal(&p, &conn->root_path)) {
			staging.exe = (void *)__get_free_page(GFP_TEMPORARY);
			if (!staging.exe) {
				path_put(&p);
				ret = -ENOMEM;
				goto exit;
			}

			staging.exe_path = d_path(&mp->exe_path, staging.exe,
						  PAGE_SIZE);
			if (IS_ERR(staging.exe_path)) {
				path_put(&p);
				ret = PTR_ERR(staging.exe_path);
				goto exit;
			}
		}
		path_put(&p);
	}

	size = kdbus_meta_measure(&staging);
	if (!size) { /* bail out if nothing to do */
		ret = 0;
		goto exit;
	}

	items = kmalloc(size, GFP_KERNEL);
	if (!items) {
		ret = -ENOMEM;
		goto exit;
	}

	size = kdbus_meta_write(&staging, items, size);
	if (!size) {
		kfree(items);
		items = NULL;
	}

	ret = 0;

exit:
	if (staging.exe)
		free_page((unsigned long)staging.exe);
	if (ret >= 0) {
		*out_items = items;
		*out_size = size;
	}
	return ret;
}

/**
 * kdbus_meta_calc_attach_flags() - calculate attach flags for a sender
 *				    and a receiver
 * @sender:		Sending connection
 * @receiver:		Receiving connection
 *
 * Return: the attach flags both the sender and the receiver have opted-in
 * for.
 */
u64 kdbus_meta_calc_attach_flags(const struct kdbus_conn *sender,
				 const struct kdbus_conn *receiver)
{
	return atomic64_read(&sender->attach_flags_send) &
	       atomic64_read(&receiver->attach_flags_recv);
}
