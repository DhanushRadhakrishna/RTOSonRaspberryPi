#include <linux/init.h>
#include <linux/module.h>
#include <linux/syscalls.h>

int hello_init(void)
{
    printk(KERN_ALERT "Hello World! team06 in kernel space\n");
    return 0;
}
void helli_exit(void)
{
    printk(KERN_ALERT "Goodbye\n");
}
module_init(hello_init);
module_exit(helli_exit);
MODULE_LICENSE("GPL");