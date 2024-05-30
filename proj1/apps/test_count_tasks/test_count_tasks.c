#include <stdio.h>
#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(){
	int count_rt = 0;
	int status = syscall(451, &count_rt);
	printf(" Status: %d\n", status);
	printf("number of tasks : %d\n", count_rt);
	return 0;

}