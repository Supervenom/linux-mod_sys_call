#include <linux/linkage.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/sched.h>    /* Needed for task_struct*/

void get_pid2(struct task_struct *tsk, char* str);
void get_comm(struct task_struct *tsk, char* str);

asmlinkage long sys_get_task(char __user *data, int len)
{
    //char msg [200];
    char str [16]; 
    struct task_struct *cur_task_p;
    cur_task_p = get_current();
    if(strlen_user(data) > 200)
        return -EINVAL;
    //copy_from_user(msg, data, strlen_user(data));
    //printk("mysyscall: %s\n", msg);
    if (len==0) get_pid2(cur_task_p, str);
    
    if (len==1) get_comm(cur_task_p, str);

    copy_to_user(data, str, 16);
    
    return strlen("hello world")+1;
}

void get_pid2(struct task_struct *tsk, char* str)
{
	sprintf(str, "%d", tsk->pid);
}

void get_comm(struct task_struct *tsk, char* str)
{
	sprintf(str, "%s", tsk->comm);
}