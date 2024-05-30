#include<linux/kernel.h>
#include<linux/fs.h>
#include<linux/miscdevice.h>
#include<linux/uaccess.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/list.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>



void display_memory_info(int pid)
{
    struct task_struct *prsnt_task;
    printk(KERN_INFO "In display function pid received = %d",pid);
    prsnt_task = pid_task(find_vpid((pid_t)pid), PIDTYPE_PID);
    if(prsnt_task == NULL)
    {
        printk(KERN_INFO " segment_info: error : task does not exist\n");
    }
    else{
        if(prsnt_task->mm == NULL)
        {
            printk(KERN_INFO "segment_info: error : mm field is null\n");
        }
        else{
            printk(KERN_INFO "[Memory segment addresses of process %d]",pid);
            printk(KERN_INFO "%08lx - %08lx: code segment (%d) bytes",prsnt_task->mm->start_code,prsnt_task->mm->end_code,(prsnt_task->mm->end_code-prsnt_task->mm->start_code));
            printk(KERN_INFO "%08lx - %08lx: data segment (%d) bytes",prsnt_task->mm->start_data,prsnt_task->mm->end_data,(prsnt_task->mm->end_data-prsnt_task->mm->start_data));   
        }
    }
}

int segment_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "segment_info: open\n");
    return 0;
}

int segment_close(struct inode *inodep, struct file *filep)
{
    printk(KERN_INFO "segment_info: close\n");

    return 0;
}
ssize_t segment_read(struct file *filep, char __user *buf, size_t count, loff_t *f_pos)
{
    printk(KERN_INFO "segment_info: read\n");
    return 0;
}

static ssize_t segment_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
    printk(KERN_INFO "segment_info: write\n");
    
    char kbuf[50];
    int usr_pid;
    if(copy_from_user(kbuf,ubuf,len)==0)
    {
        kbuf[len] = '\0';
        if(strlen(kbuf)>20)
        {
            printk(KERN_INFO "segment_info: error : input greater than 20 characters\n");
            return len;
        }
        else{
                printk(KERN_INFO "segment_info : write : pid=%s",kbuf);
                const char *p;
                p = strstrip(kbuf); // strstrip() removes unnecessary empty spaces and line breaks;
                            // returns the starting address of the cleaned-up string
                if (kstrtoint(p, 0, &usr_pid)) { // kstrtoint() converts p to int and stores in val
                    printk(KERN_INFO "Error while converting\n");
                    return len;
                }
                display_memory_info(usr_pid);
            }
    }
    else{
        printk(KERN_INFO "Segment_info : write : Error while copy_from_user\n");
    }
    
    return len; 
}


struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = segment_read,
    .open = segment_open,
    .write = segment_write,
    .release = segment_close,
};
struct miscdevice segment_info_device ={
    .minor = MISC_DYNAMIC_MINOR,
    .name = "segment_info",
    .fops = &fops,

};

int segment_info_init(void)
{
    int error;
    misc_register(&segment_info_device);
    if(error) {
        printk(KERN_INFO "segment_info_device:misc_register failed \n");
        return error;
    }
    printk(KERN_INFO "segment_info_device : registered\n");
    return 0;
}

void segment_info_exit(void)
{
    misc_deregister(&segment_info_device);
    printk(KERN_INFO "segment_info_device : de-registered\n");
}

module_init(segment_info_init);
module_exit(segment_info_exit);
MODULE_LICENSE("GPL");