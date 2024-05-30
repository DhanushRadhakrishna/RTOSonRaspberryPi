/* 
 * 
 * CS251/EE255 Real-Time Embedded Systems at UC Riverside 
 *
 * : User-level periodic task sample program
 *
 */

#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void timespec_add_ms(struct timespec *t, long ms)
{
	t->tv_nsec += ms * 1000000;
	if (t->tv_nsec > 1000000000) {
		t->tv_nsec = t->tv_nsec - 1000000000;
		t->tv_sec += 1;
	}
}

void timespec_init(struct timespec *t)
{
	t->tv_sec = 0;
	t->tv_nsec = 0;
}

double timespec_to_ms(struct timespec t)
{
	return (t.tv_sec * 1000000000 + t.tv_nsec) / 1000000.;
}

long long timespec_to_ns(struct timespec t)
{
	return (t.tv_sec * 1000000000 + t.tv_nsec);
}

int main(int argc, char *argv[])
{
	long C_ms, T_ms;
	int cpuid;
	struct timespec C, T, next;
	cpu_set_t set;

	if (argc < 4) {
		printf("%s: <C in ms> <T in ms> <CPUID>\n", argv[0]);
		return -1;
	}

	C_ms = atoi(argv[1]);
	T_ms = atoi(argv[2]);
	cpuid = atoi(argv[3]);

	if (C_ms <= 0 || C_ms > 10000 || T_ms <= 0 || T_ms > 10000 || C_ms > T_ms || cpuid < 0) {
		printf("invalid range: C %ld, D %ld, CPUID %d\n", C_ms, T_ms, cpuid);
		return -1;
	}

	CPU_ZERO(&set);
	CPU_SET(cpuid, &set);
	if (sched_setaffinity(getpid(), sizeof(set), &set) == -1) {
		perror("sched_setaffinity error");
		return -1;
	}			
	
	printf("PID: %d, C: %ld, T: %ld, CPUID: %d\n", getpid(), C_ms, T_ms, cpuid);

	timespec_init(&C);
	timespec_init(&T);
	timespec_add_ms(&C, C_ms);
	timespec_add_ms(&T, T_ms);

	clock_gettime(CLOCK_MONOTONIC, &next);

	while (1) {
		struct timespec t1, t2;
		double t1_ms, t2_ms;
		double delta = 0.5; // error margin; 0.5 ms
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
		t1_ms = timespec_to_ms(t1); 
		do {
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t2);
			t2_ms = timespec_to_ms(t2);
		} while ((t2_ms - t1_ms + delta) < C_ms);

		timespec_add_ms(&next, T_ms);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return 0;
}
