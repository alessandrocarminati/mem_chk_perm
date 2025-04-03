#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_REGIONS 256

#if __SIZEOF_POINTER__ == 8
#define ADDRESS_SPACE_END 0xFFFFFFFFFFFFFFFF
#else
#define ADDRESS_SPACE_END 0xFFFFFFFF
#endif

#define WRITEOK 1
#define WRITEKO 2
#define ANSWER_TO_THE_ULTIMATE_QUESTION 42

#define LINE_FORMAT_REGULAR  "| 0x%016lx - 0x%016lx | %s | %-40s | %-29s | %-29s | %-29s |\n"
#define LINE_FORMAT_MODIFIED "|*0x%016lx - 0x%016lx*| %s | %-40s | %-29s | %-29s | %-29s |\n"


typedef struct {
	pid_t pid[3];
	int status[3];
	unsigned long start;
	unsigned long end;
	char perms[5];
	char name[256];
} MemRegion;

typedef struct {
	MemRegion regions[MAX_REGIONS];
	int region_count;
} MemMap;


const char *reasons[3]={ "\033[33muntested\033[0m", "\033[32mwritten ok\033[0m", "\033[31msegfault\033[0m" };

void parse_proc_maps(MemMap *m) {
	FILE *fp = fopen("/proc/self/maps", "r");
	MemRegion *region = malloc(sizeof(MemRegion));

	if (!fp) {
		perror("Failed to open /proc/self/maps");
		exit(EXIT_FAILURE);
	}

	unsigned long prev_end = 0;
	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		char perms[5], dev[12], mapname[256] = "";
		unsigned long offset, inode;

		if (sscanf(line, "%lx-%lx %4s %lx %s %lu %255[^\n]s",
				   &(region->start), &(region->end), perms, &offset, dev, &inode, mapname) >= 6) {
			strncpy(region->perms, perms, 4);
			region->perms[4] = '\0';
			strncpy(region->name, mapname, 255);
			region->name[255] = '\0';

			if (m->region_count == 0 && region->start > 0) {
				m->regions[m->region_count].start = 0;
				m->regions[m->region_count].end = region->start;
				strcpy(m->regions[m->region_count].perms, "----");
				strcpy(m->regions[m->region_count].name, "[unmapped]");
				m->region_count++;
			}

			if (prev_end != 0 && region->start > prev_end) {
				m->regions[m->region_count].start = prev_end;
				m->regions[m->region_count].end = region->start;
				strcpy(m->regions[m->region_count].perms, "----");
				strcpy(m->regions[m->region_count].name, "[unmapped]");
				m->region_count++;
			}

			memcpy(&(m->regions[m->region_count++]), region, sizeof(MemRegion));
			prev_end = region->end;
		}
	memset(m->regions[m->region_count-1].status, 0, sizeof(m->regions[m->region_count-1].status));
	memset(m->regions[m->region_count-1].pid, 0, sizeof(m->regions[m->region_count-1].pid));
	printf("%lx-%lx %s %s [%d,%d,%d] [%d,%d,%d]\n", 
			m->regions[m->region_count-1].start, 
			m->regions[m->region_count-1].end, 
			m->regions[m->region_count-1].perms, 
			m->regions[m->region_count-1].name, 
			m->regions[m->region_count-1].pid[0], 
			m->regions[m->region_count-1].pid[1], 
			m->regions[m->region_count-1].pid[2], 
			m->regions[m->region_count-1].status[0], 
			m->regions[m->region_count-1].status[1], 
			m->regions[m->region_count-1].status[2]);

	}
	fclose(fp);
	free(region);

	if (prev_end < ADDRESS_SPACE_END) {
		m->regions[m->region_count].start = prev_end;
		m->regions[m->region_count].end = ADDRESS_SPACE_END;
		strcpy(m->regions[m->region_count].perms, "----");
		strcpy(m->regions[m->region_count].name, "[unmapped]");
		m->region_count++;
	}
}
void print_memory_map(MemMap *m, MemMap *m2) {
	printf("Memory Layout:\n");
	printf("+-----------------------------------------+------+------------------------------------------+----------------------+----------------------+----------------------+\n");
	printf("| Memory Region                           | Perms| content                                  | Result Bottom        | Result middle        | Result upper         |\n");
	for (int i = 0; i < m->region_count; i++) {

		printf("+-----------------------------------------+------+------------------------------------------+----------------------+----------------------+----------------------+\n");
		printf( ((m->regions[i].start == m2->regions[i].start) && (m->regions[i].end == m2->regions[i].end)) ? LINE_FORMAT_REGULAR : LINE_FORMAT_MODIFIED,
//		printf( LINE_FORMAT_REGULAR,
			   m->regions[i].start, m->regions[i].end, m->regions[i].perms, m->regions[i].name[0] ? m->regions[i].name : "[anonymous]",
			   reasons[m->regions[i].status[0]], reasons[m->regions[i].status[1]], reasons[m->regions[i].status[2]] );
//			   reasons[0], reasons[0], reasons[0] );
	}
	printf("+-----------------------------------------+------+------------------------------------------+----------------------+----------------------+----------------------+\n");
}

void handle_child_exit(MemMap *m, pid_t pid, int status) {
	int i,j;

	for (j = 0; j < 3; j++) {
		for (i = 0; i < m->region_count; i++) {
			if (m->regions[i].pid[j] == pid) {
				if (WIFEXITED(status)) {
					m->regions[i].status[j] = WRITEOK;
				}
				if (WIFSIGNALED(status)) {
					m->regions[i].status[j] = WRITEKO;
				}
				return;
			}
		}
	}
	printf("[handle_child_exit]unknown pid (%d) exit\n", pid);
}

void child_process(MemRegion *region, int test_type) {
	pid_t pid = getpid();
	int *ptr;
	int old;
	int i;

	prctl(PR_SET_DUMPABLE, 0);

	unsigned long addresses[3] = {region->start, (region->start + region->end) / 2, region->end - 2 * sizeof(int)};
	ptr = (int *)addresses[test_type];
	printf("[%d] read %p region=%s test_type=%d\n", pid, ptr, region->name, test_type);
	old = *ptr; // save old
	printf("[%d] read %p -> %d\n", pid, ptr, old);

	*ptr = ANSWER_TO_THE_ULTIMATE_QUESTION;
	printf("[%d] read %p <- %d\n", pid, ptr, ANSWER_TO_THE_ULTIMATE_QUESTION);

	if (*ptr != ANSWER_TO_THE_ULTIMATE_QUESTION) {
		printf("the write at %p was not successful (%d)\n", ptr, *ptr);
		exit(2);
	}
	printf("[%d] read %p <- %d\n", pid, ptr, old);
	*ptr = old;
	exit(0);
}

int main(int argc, char *argv[]) {
	void *rsp;

	MemMap *m, *m2;

	m = malloc(sizeof(MemMap));
	m2 = malloc(sizeof(MemMap));
	getchar();
	memset(m, 0, sizeof(m));
	memset(m2, 0, sizeof(m2));
	__asm__("mov %%rsp, %0" : "=r"(rsp));printf("Stack Pointer (RSP): %p\n", rsp);
 	parse_proc_maps(m);
	getchar();
//        print_memory_map(&m, &m2);
	for (int j = 0; j<3; j++){
		for (int i = 0; i < m->region_count; i++) {
			pid_t pid = fork();
			if (pid < 0) {
				perror("Fork failed");
				exit(EXIT_FAILURE);
			} else if (pid == 0) {
				child_process(&(m->regions[i]), j);
			} else {
				m->regions[i].pid[j] = pid;
			}
		}
	}

	for (int j = 0; j<3; j++){
		for (int i = 0; i < m->region_count; i++) {
			int status;
			pid_t pid = wait(&status);
			handle_child_exit(m, pid, status);
		}
	}

	parse_proc_maps(m2);
	print_memory_map(m, m2);
	free(m);
	free(m2);

	sleep(300);
	return 0;
}

