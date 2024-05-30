#include<stdio.h>
#include<string.h>
#include <stdlib.h>
#define MAX_TASKS 10000
#define MAX_CPUS 100

struct task_list {
    //struct task_list *next;
    char task_name[20];
    int C,T;
    float U;
};
typedef struct task_list *task;
struct cpu_utilization {
    float utilized;
    task *tasks_in_cpu;
    int no_tasks;

};
typedef struct cpu_utilization *cpu;

void print_tasks(cpu *arr , int n){
    printf("Success\n");
    for (int i = 0; i < n; i++) {
        printf("CPU%d", i); 
        for (int j = 0; j < arr[i]->no_tasks; j++) {
            printf(",%s", arr[i]->tasks_in_cpu[j]->task_name);
        }
        printf("\n");
    }

}

void bubble_sort_desc(task *arr, int n) {
    int i, j;
    for (i = 0; i < n-1; i++) {
        for (j = 0; j < n-i-1; j++) {
            if (arr[j]->U < arr[j+1]->U) {
                // Swap the tasks
                task temp = arr[j];
                arr[j] = arr[j+1];
                arr[j+1] = temp;
            }
        }
    }
}
int main()
{
    FILE *input;
    // typedef struct task_list *task;
    task *task_array;
    cpu *cpu_array;
    int i=0;
    input = fopen("input1.txt","r");
    if(input)
    {
        char str[50];
        int line = 1;
        int CPUs,N=0;
        char heuristic[3];
        while(fgets(str,50,input)!=NULL)
        {
            char substring[20];
            if(line==1)
            {
                line++;
                strncpy(substring,str+(0),1);
                CPUs = atoi(substring);
                if(CPUs > MAX_CPUS){
                    printf("Too many CPU's\n");
                    return -1;
                }
                cpu_array = (cpu *)malloc(CPUs*sizeof(cpu));
                strncpy(heuristic,str+(2),3);
                for(int i = 0; i < CPUs;i++){
                    cpu_array[i] = malloc(sizeof(struct cpu_utilization));
                    cpu_array[i]->tasks_in_cpu = (task *)malloc(MAX_TASKS * sizeof(task)); // Allocate memory for tasks_in_cpu
                    cpu_array[i]->no_tasks = 0;
                    cpu_array[i]->utilized = 0.0f;
                }
            }
            else if(line==2)
            {
                line++;
                N = atoi(str);
                if(N> MAX_TASKS){
                    printf("Too many tasks\n");
                    return -1;
                }
                task_array = (task *)malloc(N*sizeof(task));
                printf("The number of tasks are stored %d\n",N);
            }
            else{
                line++;
                char c[10],t[10];
                //curr_task = (task)malloc(sizeof(struct task_list));
                task_array[i] = malloc(sizeof(struct task_list));
                sscanf(str, "%[^,],%[^,],%s", task_array[i]->task_name,c,t);
                task_array[i]->C = atoi(c);
                task_array[i]->T = atoi(t);
                task_array[i]->U = (float)task_array[i]->C/task_array[i]->T;
                i++;               

            }
        }
        
        bubble_sort_desc(task_array, N);

        for(int j = 0; j<N; j++)
        {
        int cpu_ass = -1;
        float available_space = 1.0f;
        float worst_space = -1.0f;
        switch(heuristic[0])
        {
            case 'B':
            {
                
                for(int k = 0; k<CPUs; k++){
                
                   float with_task = cpu_array[k]->utilized + task_array[j]->U;
                   float cpu_cond = 1.0f - with_task;
                   if(cpu_cond >= 0 && cpu_cond < available_space){
                          available_space = cpu_cond;
                          cpu_ass = k;
                   }
                }
                           
                break;
            }
            case 'W':
            {   
                for(int k = 0; k<CPUs; k++){
                    
                   float with_task = cpu_array[k]->utilized + task_array[j]->U;
                   float cpu_cond = 1.0f - with_task;
                   if(cpu_cond >= 0 && cpu_cond > worst_space)
                   {
                          worst_space = cpu_cond;
                          cpu_ass = k;
                   }
                
                }
                break;
            }
            case 'F':
            {
                for(int k = 0; k<CPUs; k++){
                    
                   float with_task = cpu_array[k]->utilized + task_array[j]->U;
                   if(with_task < 1.0f)
                   {
                        cpu_ass = k;
                        break;
                   }
                
                }
                break;
            }
            default:
            {
            printf("Unknown heuristic given\n");
            return 0;
            }
        } 
        if(cpu_ass != -1)
        {
            cpu_array[cpu_ass]->tasks_in_cpu[cpu_array[cpu_ass]->no_tasks++] = task_array[j];
            cpu_array[cpu_ass]->utilized += task_array[j]->U;
        }

        else
        {
            printf("Failure\n");
            return 0;
        }

    }
    print_tasks(cpu_array,CPUs);
    return 0;
}
}