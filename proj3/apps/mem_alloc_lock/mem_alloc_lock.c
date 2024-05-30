#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
int main(int argc, char *argv[]){
    
    int n;
    struct timespec t1,t2;
    if(argc>1){
        n = atoi(argv[1]);
    }
    else{
        printf("Give the number of bytes you want to allocate\n");
        return 0;
    }
    char *buf = (char *) malloc(n);
    if(buf == NULL){
        printf("Memory allocation failed\n");
        return 0;
    }
    int check_lock = mlock(buf,n);
    if(check_lock == -1)
    {
        printf("%s\n",strerror(errno));
    }
    else{
        printf("memory was locked\n");
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < n; i += 4096) 
        buf[i] = 1;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    
    long diff = (t2.tv_sec - t1.tv_sec) * 1000000000L;
    diff+= (t2.tv_nsec - t1.tv_nsec);
    
    int pid = getpid();
    printf("PID %d, %ld ns\n",pid,diff);
    pause();

    return 0;
}