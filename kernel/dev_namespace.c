/*
 * kernel/dev_namespace.c
 *
 * Copyright (c) 2011-2014 Cellrox Ltd. Certain portions are copyrighted by
 * Columbia University. This program is free software licensed under the GNU
 * General Public License Version 2 (GPL 2). You can distribute it and/or
 * modify it under the terms of the GPL 2.
 *
 * Device namespaces:
 *
 * The idea with a device namespace comes from the Android-Cells project:
 * namespaces are utilized to create a container-like environment on Linux,
 * and there is a notion of an 'active' namespace while other namespaces are
 * non-active. In such a case only processes residing within the active device
 * namespace should communicate with actual devices, where processes inside
 * non-active containers should be able to communicate gracefully with the
 * device driver, but not the device.
 *
 * The device namespace allows a device driver to register itself and pass a
 * pointer to its device specific namespace structure and register notifiers
 * which are called when the active namepace becomes non-active and when an
 * non-active namespace becomes active.
 *
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 * The full GPL 2 License is included in this distribution in the file called
 * COPYING
 *
 * Cellrox can be contacted at oss@cellrox.com
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/pid_namespace.h>
#include <linux/dev_namespace.h>
#include <linux/wakelock.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

/* protects active namespace and switches */
static DECLARE_RWSEM(global_dev_ns_lock);

struct dev_namespace init_dev_ns = {
	/* device namespace core */
	.active = true,
	.count = ATOMIC_INIT(2),  /* extra reference for active_dev_ns */
	.pid_ns = &init_pid_ns,
	.notifiers = BLOCKING_NOTIFIER_INIT(init_dev_ns.notifiers),
	.timestamp = 0,
	/* device namespace desc */
	.mutex = __MUTEX_INITIALIZER(init_dev_ns.mutex),
 };
EXPORT_SYMBOL_GPL(init_dev_ns);

#ifdef CONFIG_DEV_NS

struct dev_namespace *active_dev_ns = &init_dev_ns;

static void dev_ns_lock(struct dev_namespace *dev_ns)
{
	mutex_lock(&dev_ns->mutex);
}

static void dev_ns_unlock(struct dev_namespace *dev_ns)
{
	mutex_unlock(&dev_ns->mutex);
}

static struct dev_namespace *create_dev_ns(struct task_struct *task)
{
	struct dev_namespace *dev_ns;

	dev_ns = kzalloc(sizeof(struct dev_namespace), GFP_KERNEL);
	if (!dev_ns)
		return ERR_PTR(-ENOMEM);

	atomic_set(&dev_ns->count, 1);
	BLOCKING_INIT_NOTIFIER_HEAD(&dev_ns->notifiers);
	mutex_init(&dev_ns->mutex);

	dev_ns->pid_ns = get_pid_ns(task->nsproxy->pid_ns);

	return dev_ns;
}

struct dev_namespace *copy_dev_ns(unsigned long flags,
				  struct task_struct *task)
{
	struct dev_namespace *dev_ns = task->nsproxy->dev_ns;

	/* couple device namespace semantics with pid-namespace */
	if (!(flags & CLONE_NEWPID))
		return get_dev_ns(dev_ns);
	else
		return create_dev_ns(task);
}

#if defined(CONFIG_PM_WAKELOCK) || defined(CONFIG_USER_WAKELOCK)
extern void destroy_wakelocks_in_dev_ns(struct dev_namespace *dev_ns);
#endif

void __put_dev_ns(struct dev_namespace *dev_ns)
{
	if (dev_ns) {
#if defined(CONFIG_PM_WAKELOCK) || defined(CONFIG_USER_WAKELOCK)
		destroy_wakelocks_in_dev_ns(dev_ns);
#endif
		put_pid_ns(dev_ns->pid_ns);
		kfree(dev_ns);
	}
}

struct dev_namespace *get_dev_ns_by_task(struct task_struct *task)
{
	struct dev_namespace *dev_ns = NULL;
	struct nsproxy *nsproxy;

	rcu_read_lock();
	nsproxy = task_nsproxy(task);
	if (nsproxy)
		dev_ns = get_dev_ns(nsproxy->dev_ns);
	rcu_read_unlock();

	return dev_ns;
}

struct dev_namespace *get_dev_ns_by_vpid(pid_t vpid)
{
	struct dev_namespace *dev_ns = NULL;
	struct task_struct *task;
	struct nsproxy *nsproxy;

	rcu_read_lock();
	task = find_task_by_pid_ns(vpid, &init_pid_ns);
	if (task) {
		nsproxy = task_nsproxy(task);
		if (nsproxy)
			dev_ns = get_dev_ns(nsproxy->dev_ns);
	}
	rcu_read_unlock();

	return dev_ns;
}

/**
 * notifications: activate/deactive device namespace
 */
static BLOCKING_NOTIFIER_HEAD(dev_ns_notifiers);
void dev_ns_register_notify(struct dev_namespace *dev_ns,
			    struct notifier_block *nb)
{
	if (dev_ns != NULL)
		blocking_notifier_chain_register(&dev_ns->notifiers, nb);
	else
		blocking_notifier_chain_register(&dev_ns_notifiers, nb);
}

void dev_ns_unregister_notify(struct dev_namespace *dev_ns,
			      struct notifier_block *nb)
{
	if (dev_ns != NULL)
		blocking_notifier_chain_unregister(&dev_ns->notifiers, nb);
	else
		blocking_notifier_chain_unregister(&dev_ns_notifiers, nb);
}

/*
 * Helpers for device namespace subsytem logic
 *
 * Drivers should embed 'struct dev_ns_info' in driver-specific,
 * per-device-namespace data, e.g.:
 *
 *   struct xxxx_namespace {
 *     ... (data specific to xxxx)
 *     struct dev_ns_info devns_info;
 *   };
 *
 * Drivers should register a 'struct dev_ns_ops' with ->create()
 * and ->release() methods, and keep an identifier (dev_ns_xxx_id),
 * for use by device namespace generic code
 *
 * Drivers can get and put 'struct dev_ns_info' objects using:
 *   struct dev_ns_info *get_dev_ns_info(int ns_id, struct task_struct *task);
 *   void put_dev_ns_info(int ns_id, struct dev_ns_info *dev_ns_info);
 * And iterate over per-driver device namespace data of all cells:
 *   void loop_dev_ns_info(int dev_ns_id, void *ptr,
 * 		void (*func)(struct dev_ns_info *dev_ns_info, void *ptr))
 */

struct dev_ns_desc {
	char *name;
	struct dev_ns_ops *ops;
	struct list_head head;
};

static struct dev_ns_desc dev_ns_desc[DEV_NS_DESC_MAX];
static DEFINE_SPINLOCK(dev_ns_desc_lock);

int register_dev_ns_ops(char *name, struct dev_ns_ops *ops)
{
	int ns_id, ret = -ENOSPC;

	spin_lock(&dev_ns_desc_lock);
	for (ns_id = 0; ns_id < DEV_NS_DESC_MAX; ns_id++) {
		if (!dev_ns_desc[ns_id].name) {
			pr_info("dev_ns: register info %s\n", name);
			dev_ns_desc[ns_id].name = name;
			dev_ns_desc[ns_id].ops = ops;
			INIT_LIST_HEAD(&dev_ns_desc[ns_id].head);
			ret = ns_id;
			break;
		}
	}
	spin_unlock(&dev_ns_desc_lock);

	return ret;
}

void unregister_dev_ns_ops(int dev_ns_id)
{
	struct dev_ns_desc *desc = &dev_ns_desc[dev_ns_id];

	spin_lock(&dev_ns_desc_lock);
	pr_info("dev_ns: unregister desc %s\n", desc->name);
	memset(&dev_ns_desc[dev_ns_id], 0, sizeof(*desc));
	spin_unlock(&dev_ns_desc_lock);
}

static struct dev_ns_info *new_dev_ns_info(int dev_ns_id,
					   struct dev_namespace *dev_ns)
{
	struct dev_ns_desc *desc = &dev_ns_desc[dev_ns_id];
	struct dev_ns_info *dev_ns_info;

	pr_debug("dev_ns: [0x%p] new info %s\n", dev_ns, desc->name);

	dev_ns_info = desc->ops->create(dev_ns);
	if (!dev_ns_info)
		return NULL;

	pr_debug("dev_ns: [0x%p] got info 0x%p\n", dev_ns, dev_ns_info);

	dev_ns->info[dev_ns_id] = dev_ns_info;
	dev_ns_info->dev_ns = get_dev_ns(dev_ns);
	atomic_set(&dev_ns_info->count, 0);

	spin_lock(&dev_ns_desc_lock);
	list_add(&dev_ns_info->list, &desc->head);
	spin_unlock(&dev_ns_desc_lock);

	return dev_ns_info;
}

static void del_dev_ns_info(int dev_ns_id, struct dev_ns_info *dev_ns_info)
{
	struct dev_namespace *dev_ns = dev_ns_info->dev_ns;

	pr_debug("dev_ns: [0x%p] destory info 0x%p\n", dev_ns, dev_ns_info);

	spin_lock(&dev_ns_desc_lock);
	list_del(&dev_ns_info->list);
	dev_ns->info[dev_ns_id] = NULL;
	spin_unlock(&dev_ns_desc_lock);

	dev_ns_desc[dev_ns_id].ops->release(dev_ns_info);
	put_dev_ns(dev_ns);
}

struct dev_ns_info *get_dev_ns_info(int dev_ns_id,
				    struct dev_namespace *dev_ns,
				    int lock, int create)
{
	struct dev_ns_info *dev_ns_info;

	if (lock) {
		down_read(&global_dev_ns_lock);
		dev_ns_lock(dev_ns);
	}

	dev_ns_info = dev_ns->info[dev_ns_id];

	if (!dev_ns_info && create)
		dev_ns_info = new_dev_ns_info(dev_ns_id, dev_ns);

	if (dev_ns_info) {
		pr_debug("dev_ns: [0x%p] get info 0x%p count %d+\n", dev_ns,
			 dev_ns_info, atomic_read(&dev_ns_info->count));
		atomic_inc(&dev_ns_info->count);
	}

	if (lock) {
		dev_ns_unlock(dev_ns);
		up_read(&global_dev_ns_lock);
	}

	return dev_ns_info;
}

struct dev_ns_info *get_dev_ns_info_task(int dev_ns_id, struct task_struct *task)
{
	struct dev_ns_info *dev_ns_info;
	struct dev_namespace *dev_ns;

	dev_ns = get_dev_ns_by_task(task);
	dev_ns_info = dev_ns ? get_dev_ns_info(dev_ns_id, dev_ns, 1, 1) : NULL;
	put_dev_ns(dev_ns);

	return dev_ns_info;
}

void put_dev_ns_info(int dev_ns_id, struct dev_ns_info *dev_ns_info, int lock)
{
	struct dev_namespace *dev_ns;

	/*
	 * keep extra reference, or else the concluding dev_ns_unlock()
	 * could theoretically execute after the last dev_ns_put()..
	 */
	dev_ns = get_dev_ns(dev_ns_info->dev_ns);

	if (lock) {
		down_read(&global_dev_ns_lock);
		dev_ns_lock(dev_ns);
	}

	pr_debug("dev_ns: [0x%p] put info 0x%p count %d-\n", dev_ns,
		dev_ns_info, atomic_read(&dev_ns_info->count));
	if (atomic_dec_and_test(&dev_ns_info->count))
		del_dev_ns_info(dev_ns_id, dev_ns_info);

	if (lock){
		dev_ns_unlock(dev_ns);
		up_read(&global_dev_ns_lock);
	}

        put_dev_ns(dev_ns);
}

/*
 * @dev_ns_id: id of device namespace subsystem
 * @ptr: data pointer to be passed to callback
 * @func: callback for each device namespace (atomic, must not sleep)
 */
void loop_dev_ns_info(int dev_ns_id, void *ptr,
		      void (*func)(struct dev_ns_info *dev_ns_info, void *ptr))
{
	struct dev_ns_desc *desc;
	struct dev_ns_info *dev_ns_info;

	spin_lock(&dev_ns_desc_lock);
	desc = &dev_ns_desc[dev_ns_id];
	list_for_each_entry(dev_ns_info, &desc->head, list) {
		pr_debug("dev_ns: loop info 0x%p (dev_ns 0x%p) of %s\n",
			 dev_ns_info, dev_ns_info->dev_ns, desc->name);
		(*func)(dev_ns_info, ptr);
	}
	spin_unlock(&dev_ns_desc_lock);
}

/**
 * Set the active device namespace (will call registered notifiers to
 * allow device drivers to make device specific context store/restore)
 *
 * @dev_ns: The new active device namespace
 */
void set_active_dev_ns(struct dev_namespace *next_ns)
{
	struct dev_namespace *prev_ns;

	BUG_ON(next_ns == NULL);

	down_write(&global_dev_ns_lock);

	if (next_ns == active_dev_ns)
		goto done;

	pr_info("dev_ns: activate 0x%p\n", next_ns);

	prev_ns = active_dev_ns;

	dev_ns_lock(prev_ns);
	prev_ns->active = false;
	prev_ns->timestamp = jiffies;
	active_dev_ns = NULL;

	(void) blocking_notifier_call_chain(&prev_ns->notifiers,
					    DEV_NS_EVENT_DEACTIVATE, prev_ns);
	(void) blocking_notifier_call_chain(&dev_ns_notifiers,
					    DEV_NS_EVENT_DEACTIVATE, prev_ns);
	dev_ns_unlock(prev_ns);

	dev_ns_lock(next_ns);

	next_ns->active = true;
	next_ns->timestamp = jiffies;

	active_dev_ns = next_ns;

	(void) blocking_notifier_call_chain(&next_ns->notifiers,
					    DEV_NS_EVENT_ACTIVATE, next_ns);
	(void) blocking_notifier_call_chain(&dev_ns_notifiers,
					    DEV_NS_EVENT_ACTIVATE, next_ns);

	dev_ns_unlock(next_ns);

	get_dev_ns(next_ns);
	put_dev_ns(prev_ns);

	pr_info("dev_ns: activate 0x%p done\n", next_ns);
 done:
	up_write(&global_dev_ns_lock);
}

/**
 * Setup for /proc/dev_ns
 */

static struct proc_dir_entry *proc_dev_ns_dir;

struct proc_dir_entry *
create_dev_ns_proc(const char *name, const struct file_operations *fops)
{
	struct proc_dir_entry *entry;
	entry = proc_create(name, 0, proc_dev_ns_dir, fops);
	return entry;
}

static int proc_active_ns_show(struct seq_file *seq, void *offset)
{
	/* TODO: use rwsem or RCU to avoid this lock */
	down_read(&global_dev_ns_lock);
	seq_printf(seq, "%d\n", dev_ns_init_pid(active_dev_ns));
	up_read(&global_dev_ns_lock);
	return 0;
}

static int proc_ns_tag_show(struct seq_file *seq, void *offset)
{
	/* TODO: use rwsem or RCU to avoid this lock */
	down_read(&global_dev_ns_lock);
	seq_printf(seq, "active: %d timestamp: %ld tag: %s\n",
		   dev_ns_init_pid(active_dev_ns),
		   active_dev_ns->timestamp,
		   active_dev_ns->tag);
	up_read(&global_dev_ns_lock);
	return 0;
}

static bool dev_ns_proc_permission(void)
{
	return current_dev_ns() == &init_dev_ns;
}

static int proc_active_ns_open(struct inode *inode, struct file *file)
{
	if (!dev_ns_proc_permission())
		return -EPERM;
	return single_open(file, proc_active_ns_show, PDE(inode)->data);
}

static int proc_ns_tag_open(struct inode *inode, struct file *file)
{
	if (!dev_ns_proc_permission())
		return -EPERM;
	return single_open(file, proc_ns_tag_show, PDE(inode)->data);
}

static ssize_t dev_ns_proc_write(struct file *file, const char __user * buffer,
				    size_t count, loff_t * ppos, int setactive)
{
	struct dev_namespace *dev_ns;
	char strbuf[16]; /* 10 chars for 32-bit pid + ':' + 4 chars for tag */
	char *new_str;
	pid_t new_pid = 0;

	/* only init ns may change active ns */
	if (!dev_ns_proc_permission())
		return -EPERM;

	if (count >= sizeof(strbuf) || count == 0)
		return -EFAULT;

	if (copy_from_user(strbuf, buffer, count))
		return -EFAULT;

	strbuf[count] = '\0';
	new_str = strim(strbuf);
	new_pid = simple_strtoul(new_str, &new_str, 10);
	if (!new_pid) {
		pr_warning("dev_ns: bad PID format '%s'\n", new_str);
		return -EINVAL;
	}

	dev_ns = get_dev_ns_by_vpid(new_pid);
	if (!dev_ns) {
		pr_warning("dev_ns: non-existing PID %d\n", new_pid);
		return -EINVAL;
	}

	if (setactive) {
		set_active_dev_ns(dev_ns);
	} else if (new_str[0] == ':') {
		/* set dev_ns tag if format was <pid>:<tag> */
		/* (safe: last byte of tag always remains NULL) */
		strncpy(dev_ns->tag, new_str + 1, DEV_NS_TAG_LEN);
	} else {
		pr_warning("dev_ns: bad PID:tag format '%s'\n", new_str);
		count = -EINVAL;
	}

	put_dev_ns(dev_ns);
	return count;
}

static ssize_t proc_active_ns_write(struct file *file,
				    const char __user * buffer,
				    size_t count, loff_t * ppos)
{
	return dev_ns_proc_write(file, buffer, count, ppos, 1);
}

static ssize_t proc_ns_tag_write(struct file *file,
				 const char __user * buffer,
				 size_t count, loff_t * ppos)
{
	return dev_ns_proc_write(file, buffer, count, ppos, 0);
}

static const struct file_operations active_ns_fileops = {
	.owner = THIS_MODULE,
	.open = proc_active_ns_open,
	.read = seq_read,
	.write = proc_active_ns_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations ns_tag_fileops = {
	.owner = THIS_MODULE,
	.open = proc_ns_tag_open,
	.read = seq_read,
	.write = proc_ns_tag_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static __init int dev_namespace_init(void)
{
	struct proc_dir_entry *entry;

	proc_dev_ns_dir = proc_mkdir("dev_ns", NULL);
	if (!proc_dev_ns_dir)
		return -ENOMEM;

	entry = proc_create("active_ns_pid", 0644, proc_dev_ns_dir,
			    &active_ns_fileops);
	if (!entry)
		goto out_fail_active_ns;

	entry = proc_create("ns_tag", 0644, proc_dev_ns_dir,
			    &ns_tag_fileops);
	if (!entry)
		goto out_fail_ns_tag;

	return 0;

out_fail_ns_tag:
	remove_proc_entry("active_ns_pid", proc_dev_ns_dir);
out_fail_active_ns:
	remove_proc_entry("dev_ns", NULL);
	return -ENOMEM;
}

__initcall(dev_namespace_init);

#endif /* CONFIG_DEV_NS */

