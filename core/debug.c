// SPDX-License-Identifier: GPL-2.0
#include "debug.h"
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>

#include <asm/pgtable.h>
#define KERNEL_PGD_START 256

static u32 target_pid = 1;

static struct dentry *debug_dir;

static int pgd_show(struct seq_file *m, void *v)
{
	struct task_struct *task;
	struct mm_struct *mm;
	pgd_t *pgd;
	int i;
	int start;

	rcu_read_lock();
	task = pid_task(find_vpid(target_pid), PIDTYPE_PID);
	if (!task) {
		rcu_read_unlock();
		seq_printf(m, "error: PID %u not found\n", target_pid);
		return 0;
	}

	get_task_struct(task);
	rcu_read_unlock();

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm) {
		seq_printf(
			m,
			"error: PID %d has no mm_struct (likely a kernel thread)\n",
			target_pid);
		return 0;
	}

	pgd = mm->pgd;

	seq_printf(m, "#PID=%d\n", target_pid);
	seq_puts(m, "index,value\n");

	if ((unsigned long)m->private)
		start = KERNEL_PGD_START;
	else
		start = 0;

	for (i = 0; i < 256; ++i) {
		int index;
		pgd_t entry;

		index = start + i;
		entry = pgd[index];
		if ((pgd_val(entry) & 0b1) != 0) {
			seq_printf(m, "%3d,0x%016lx\n", index,
				   (unsigned long)pgd_val(entry));
		}
	}

	mmput(mm);
	return 0;
}

static int debug_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, pgd_show, inode->i_private);
}

static const struct file_operations debug_fops = {
	.owner = THIS_MODULE,
	.open = debug_file_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int debug_init(void)
{
	debug_dir = debugfs_create_dir("ss_dbg", NULL);
	if (IS_ERR(debug_dir)) {
		pr_debug("cannot create debugfs directory: %ld\n",
			 PTR_ERR(debug_dir));
	}

	debugfs_create_u32("target_pid", 0644, debug_dir, &target_pid);
	debugfs_create_file("pgd_hi", 0400, debug_dir, (void *)1, &debug_fops);
	debugfs_create_file("pgd_lo", 0400, debug_dir, (void *)0, &debug_fops);
	return 0;
}

void debug_exit(void)
{
	debugfs_remove_recursive(debug_dir);
}
