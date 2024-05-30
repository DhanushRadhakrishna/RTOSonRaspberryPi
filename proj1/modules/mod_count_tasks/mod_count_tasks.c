#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/syscall.h>

MODULE_LICENSE("GPL");
extern syscall_fn_t sys_call_table[];
asmlinkage long (*count_rt_tasks)(int);
SYSCALL_DEFINE1(mod_count_rt_tasks, int *, result){

    struct task_struct *it;
    struct task_struct *thread;
    printk(KERN_INFO "I have entered the syscall");
    int mod_count=0;


    if(result == NULL) return 3;

     for_each_process_thread(it,thread) {
        if(thread->rt_priority>50)
        {
            mod_count++;
        }
    }
    printk(KERN_INFO "count_rt_tasks[MOD]%d\n", mod_count);
    if(copy_to_user(result, &mod_count, sizeof(mod_count))!=0) return 2;

    return 0;
}
int mod_count_init(void)
{
    printk("Entered LKM mod_count_tasks\n");
    count_rt_tasks = (void*)sys_call_table[__NR_count_rt_tasks];
    sys_call_table[__NR_count_rt_tasks] = (void *)__arm64_sys_mod_count_rt_tasks;
    return 0;

}

void mod_count_exit(void)
{
    sys_call_table[__NR_count_rt_tasks] = (void *)count_rt_tasks;
    printk("Exited from LKM mod_count_tasks.c\n");
}
module_init(mod_count_init);
module_exit(mod_count_exit);

