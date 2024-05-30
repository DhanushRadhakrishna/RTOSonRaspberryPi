#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/fs.h>
#include<linux/miscdevice.h>
#include<linux/uaccess.h>
#include<linux/time.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/list.h>
#include <linux/vmalloc.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#define SET_RTMON _IO(0, 100)
#define CANCEL_RTMON _IO(0, 101)
#define WAIT_FOR_NEXT_PERIOD _IO(0,102)


struct rtmon_param {
    int pid;
    int C, T;
};

struct proc {
    struct list_head h;
    int pid;
    int C, T;
    struct hrtimer rt_timer; 
};


struct proc *itr;
ktime_t time_period;
LIST_HEAD(proc_list);

//hrtimer calback definition
enum hrtimer_restart my_hrtimer_callback(struct hrtimer *timer)
{

    printk(KERN_INFO "timer expired\n");
    struct pid *pid_struct;
    struct task_struct *task;
    struct proc *driver_task = container_of(timer, struct proc, rt_timer);
    printk(KERN_INFO "pid retrieved %d",driver_task->pid);

    pid_struct = find_get_pid(driver_task->pid);
    task = pid_task(pid_struct,PIDTYPE_PID);
    if(task!=NULL)
    {
        printk(KERN_INFO "inside if state=%d\n",task->__state);
    
        // if(task->__state == TASK_STOPPED)
        // {
        //     printk(KERN_INFO "task is stopped\n");
        //     return HRTIMER_NORESTART;
            
        // }
        // else if(task->__state ==  TASK_INTERRUPTIBLE)
        // {
            printk(KERN_INFO "was suspended by wait_for_next_period\n");
            wake_up_process(task);
            hrtimer_forward_now(timer,time_period); 
            return HRTIMER_RESTART;
        // }
    }
    else
    {
        printk(KERN_INFO "cancelling the timer ctrl+c was pressed\n");
        return HRTIMER_NORESTART;
    }

    // return HRTIMER_NORESTART;
}

long my_ioctl(struct file *file,unsigned int cmd,unsigned long arg){
    struct rtmon_param var;

    unsigned long pid_rem;  
    struct proc *new_proc;

    switch(cmd)
    {
    case SET_RTMON:
    {   
        if(copy_from_user(&var, (struct rtmon_param*)arg,sizeof(struct rtmon_param)) == 0)
        {
            //Checking if the values are clean
            if(var.C > 10000 || var.C < 1)
            {
            return -1;
            }
            if(var.T > 10000 || var.T < 1)
            {
            return -1;
            }
            if(pid_task(find_vpid(var.pid), PIDTYPE_PID)== NULL){
                return -1;
            }       
                
            list_for_each_entry(itr, &proc_list, h) {
                if(itr->pid == var.pid){
                    return -1;
                }
            }

            
            new_proc = vmalloc(sizeof(struct proc));
            new_proc->pid = var.pid;
            new_proc->C = var.C;
            new_proc->T = var.T;
            list_add(&new_proc->h,&proc_list);
            printk(KERN_INFO "Added PID %d to my_list\n",new_proc->pid);
            //initializing the  time period (converting ms to ns)
            time_period = ms_to_ktime((unsigned long)new_proc->T);
            //initialize the timer
            hrtimer_init(&new_proc->rt_timer , CLOCK_MONOTONIC, HRTIMER_MODE_REL);
            new_proc->rt_timer.function = &my_hrtimer_callback;
            //starting the hrtimer
            printk(KERN_INFO "starting timer\n");
            //  hrtimer_start( &hr_timer, ktime, HRTIMER_MODE_REL );
            hrtimer_start(&new_proc->rt_timer, time_period, HRTIMER_MODE_REL);
            return 0;
        }
        else{
            printk(KERN_INFO "error while copying\n");
            return -1;
        }
    }
    break;
    case CANCEL_RTMON:
    {
            if(copy_from_user(&pid_rem, (unsigned long*)arg,sizeof(unsigned long)) == 0)
            {
                //printk("Entered the function");
                //stopping proc_list and removing from proc_list
                struct proc *r, *temp;
                list_for_each_entry_safe(r, temp, &proc_list, h) 
                {   //printk(KERN_INFO "Inside the loop");
                    if(r->pid == (int)pid_rem)
                    {
                        //printk(KERN_INFO "Inside the if condition");
                        int pid = r->pid;
                        int ret;
                        ret = hrtimer_cancel(&r->rt_timer);
                        list_del(&r->h);
                        vfree(r);
                        printk(KERN_INFO "Entry for PID %d removed\n", pid);
                        return 0;
                        
                    }
                }
                return -1;
            } 
            else{
                printk(KERN_INFO "Error while copying in CANCEL_RTMON\n");
                return -1;
            }      
    }
    break;

    case WAIT_FOR_NEXT_PERIOD:
    {
            printk(KERN_INFO "Entered Wait_for_next_period\n");
            bool found = false;
            list_for_each_entry(itr,&proc_list,h){
                if(current->pid == itr->pid){
                   found = true;
                   break;
                }
            }
            if(!found)
            {
                printk(KERN_INFO "Did not find the pid in our list\n");
                return -1;
            }
            //suspending the task
            set_current_state(TASK_INTERRUPTIBLE);
            set_tsk_need_resched(current);
            return 0;
    }
    break;

    default:
        return -EINVAL;
    }

       return 0;
    }

int my_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "my_device:open\n");
    return 0;
}

int my_close(struct inode *inodep, struct file *filep)
{
    printk(KERN_INFO "my_device:close\n");

    return 0;
}
ssize_t my_read(struct file *filep, char __user *buf, size_t count, loff_t *f_pos)
{
    struct pid *pid_struct;
    struct task_struct *task;
    list_for_each_entry(itr,&proc_list,h)
    {   
        pid_struct = find_get_pid(itr->pid);
        task = pid_task(pid_struct,PIDTYPE_PID);
        if(task!=NULL)
        {
            printk(KERN_INFO "print_rtmon: PID %d, C %d, T %d   \n", itr->pid,itr->C,itr->T);
        }
    
    }
    return 0;
}

struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = my_read,
    .open = my_open,
    .release = my_close,
    .unlocked_ioctl = my_ioctl,
};
struct miscdevice my_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "rtmon",
    .fops = &fops,
    
};

int my_device_init(void)
{
    int error;

    error = misc_register(&my_device);
    if(error) {
        printk(KERN_INFO "my_device:misc_register failed \n");
        return error;
    }
    printk(KERN_INFO "my_device : registered\n");
    return 0;

}
void my_device_exit(void)
{

    //freeing proc_list
    if(list_empty(&proc_list)!=0)
    {
        list_for_each_entry(itr, &proc_list, h) 
        {
            int ret;
            ret = hrtimer_cancel(&itr->rt_timer);
            vfree(itr);
        }
    }
    misc_deregister(&my_device);
    printk(KERN_INFO "my_device : deregistered\n");

}

module_init(my_device_init);
module_exit(my_device_exit);
MODULE_LICENSE("GPL");