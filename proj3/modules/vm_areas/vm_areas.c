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
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <linux/uaccess.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

void display_memory_areas(int);

int follow(struct mm_struct *mm, unsigned long address, pte_t **ptepp)
{
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *ptep;
         
        pgd = pgd_offset(mm, address);
        if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
            goto out;
        p4d = p4d_offset(pgd, address);
        if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
            goto out;
        
        pud = pud_offset(p4d, address);
        if (pud_none(*pud) || unlikely(pud_bad(*pud)))
            goto out;
        
        pmd = pmd_offset(pud, address);
        VM_BUG_ON(pmd_trans_huge(*pmd));
     
        if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
            goto out;
        
        ptep = pte_offset_map(pmd, address);
        if (!pte_present(*ptep))
            return 1;
        *ptepp = ptep;
       
        return 0;
        
    out:
        
        return -EINVAL;
}

int vm_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "vm: open\n");
    return 0;
}
int vm_close(struct inode *inodep, struct file *filep)
{
    printk(KERN_INFO "vm: close\n");
    return 0;
}
ssize_t vm_read(struct file *filep, char __user *buf, size_t count, loff_t *f_pos)
{
    printk(KERN_INFO "vm: read\n");
    return 0;
}
static ssize_t vm_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
    printk(KERN_INFO "vm: write \n");

    char kbuf[30];
    int usr_pid;
    if(copy_from_user(kbuf,ubuf,len)==0)
    {
        kbuf[len] = '\0';
        if(strlen(kbuf)>20)
        {
            printk(KERN_INFO "vm_areas: error : input greater than 20 characters\n");
            return -1;
        }
        else
        {
            printk(KERN_INFO "vm_areas: write: pid=%s",kbuf);
            const char *p;
            p = strstrip(kbuf); // strstrip() removes unnecessary empty spaces and line breaks;
            // returns the starting address of the cleaned-up string
            if(kstrtoint(p, 0, &usr_pid)) 
            { // kstrtoint() converts p to int and stores in val
                printk(KERN_INFO "Error while converting\n");
                return len;
            }
            display_memory_areas(usr_pid);
        }
    }
    return len;
}

void display_memory_areas(int pid)
{
    struct task_struct *prsnt_task;
    printk(KERN_INFO "In display function pid received = %d",pid);
    prsnt_task = pid_task(find_vpid((pid_t)pid), PIDTYPE_PID);
    if(prsnt_task!=NULL)
    {
        if(prsnt_task->mm == NULL)
        {
            printk(KERN_INFO "vm_areas: error: task does not have any virtual address space\n");
        }
        else
        {
            int i;
            pte_t temp;
            pte_t *ptepp = &temp;
            struct vm_area_struct *vma;
            //for follow_pte
            struct vm_area_struct *pte_vma;
            printk(KERN_INFO "[Memory-mapped areas of process %d]",pid);
            MA_STATE(mas, &prsnt_task->mm->mm_mt, 0, 0);
            mas_for_each(&mas, vma, ULONG_MAX) 
            {
                // now 'vma' points to each vm_area_struct, e.g., vma->vm_start
                int locked = 0, size=0, pages=0, KB=0 , pages_divided = 0;
                if(vma!=NULL)
                {
                    //printk(KERN_INFO "For memory starting at %08lx these are the pages present : \n",vma->vm_start);
                    size = vma->vm_end-vma->vm_start;
                    for( unsigned long j = vma->vm_start;j< vma->vm_end;j+=(0x1000 + 1))
                    {
                        pages_divided += 1;
                        //printk(KERN_INFO "Page %d : %08lx - %08lx",pages_divided,j,j+(0x1000 + 1));
                        i = follow(prsnt_task->mm,j,&ptepp);
                        if(i == 0)
                        {
                            pages += 1;                        
                        }
                        
                    }
                    KB = pages * 4;           
                    
                    printk(KERN_INFO "%08lx - %08lx: (%d) bytes %s, %d pages (%d KB) in phymem",vma->vm_start,vma->vm_end,size, (locked?"[L]":" "),pages,KB);
                
                    //locked = 0;
                   
                    
                }
                else{                    
                    return;
                }
            }
        }
    }
    else{
        printk(KERN_INFO "vm_areas: error: task does not exist\n");
    }
}


struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = vm_read,
    .open = vm_open,
    .write = vm_write,
    .release = vm_close,
};
struct miscdevice vm_areas_device ={
    .minor = MISC_DYNAMIC_MINOR,
    .name = "vm_areas",
    .fops = &fops,
};
int vm_areas_init(void)
{
    int error;
    error = misc_register(&vm_areas_device);
    if(error) {
        printk(KERN_INFO "vm_areas_device:misc_register failed \n");
        return error;
    }
    printk(KERN_INFO "vm_areas_device : registered\n");
    return 0;
}

void vm_areas_exit(void)
{
    misc_deregister(&vm_areas_device);
    printk(KERN_INFO "vm_areas_device : de-registered\n");
}

module_init(vm_areas_init);
module_exit(vm_areas_exit);
MODULE_LICENSE("GPL");