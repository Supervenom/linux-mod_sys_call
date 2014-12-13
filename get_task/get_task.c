#include <linux/linkage.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
asmlinkage long sys_get_task(char __user *data, int len)
{
    char msg [200];
    if(strlen_user(data) > 200)
        return -EINVAL;
    copy_from_user(msg, data, strlen_user(data));
    printk("mysyscall: %s\n", msg);
    copy_to_user(data, "hello world", strlen("hello world")+1);
    return strlen("hello world")+1;
}
