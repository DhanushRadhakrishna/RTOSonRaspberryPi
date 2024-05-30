#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/time.h>
#include <linux/syscalls.h>



SYSCALL_DEFINE1(count_rt_tasks, int *, result){
	
    struct task_struct *task;
    struct task_struct *thread;
    printk(KERN_INFO "I have entered the syscall");
    int count_val=0;
    
    
    if(result == NULL) return 3;
     
     for_each_process(task) {
        if(task->rt_priority <=0) continue;
	for_each_thread(task, thread){
		count_val++; 
}
    }


    printk(KERN_INFO "number of tasks: %d\n", count_val);
    if(copy_to_user(result, &count_val, sizeof(count_val))!=0) return 2;
    
    return 0;
}