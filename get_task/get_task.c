#include <linux/linkage.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/sched.h>    /* Needed for task_struct*/
asmlinkage long sys_get_task(char __user *data, int len)
{
    char msg [200];
    char *str; 
    struct task_struct *cur_task_p;
    cur_task_p = get_current();
    if(strlen_user(data) > 200)
        return -EINVAL;
    copy_from_user(msg, data, strlen_user(data));
    printk("mysyscall: %s\n", msg);
    str = get_pid2(cur_task_p);
    copy_to_user(data, str, 15);
    return strlen("hello world")+1;
}

char *get_pid2(struct task_struct *tsk)
{
	char str[15];
	sprintf(str, "%d", tsk->pid);
	return str;
}
