# RTOSonRaspberryPi
This project is part of my Lab work for my course at University of California, Riverside CS215 under Prof. Hoyesung Kim.< br / >
It includes three main folders where the most of the development was done. Namely Proj1, Proj2, Proj3.
In Proj1 -> adding custom system call by editing system call table of the Linux Kernel. The system call outputs the number of tasks running on the Raspberry PI 
In Proj2 -> Developed Loadable Kernel Modules (ioctl) to extend functionalities of I/O devices etc. The module loads a task to the raspberry pi and starts the timer using the hrtimer of the Linux kernel. It then restarts the task  for a fixed time interval and suspends it unless the user kills the task using CNTRL+C. After which the task is removed from the list of tasks. The user is allowed to add mutiple tasks at a time (using different terminals) and the user can kills any process using the pid. The module is able to handles any scenarios.
In Proj3 -> Developed a Misc Device, (Linux treats I/O devices as files). There are two parts to this sub project i.segment info ii.vmareas (The misc device is a just code and can be called using a test function, no device is required).
segment info - here the device functionality is to display the address, size of the code segment and the data segment of the current process. User can view this performing a write ooperation to the device.
vmareas - here the device functionality is to display the addresses of each page in the page table using the vm_struct struct of the lnux kernel. It also displays which page is currently locked for the process.
