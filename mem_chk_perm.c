#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <setjmp.h>

#define MAX_REGIONS 256
//#define DEBUG

#if __SIZEOF_POINTER__ == 8
#define ADDRESS_SPACE_END 0xFFFFFFFFFFFFFFFF
#else
#define ADDRESS_SPACE_END 0xFFFFFFFF
#endif

#define WRITEOK 1
#define WRITEKO 2
#define ANSWER_TO_THE_ULTIMATE_QUESTION 42

#ifdef DEBUG
#define LINE_FORMAT_REGULAR  "| 0x%016lx - 0x%016lx | %s | %-40s | %-29s | %-29s | %-29s | %-21s | %d %d %d %d %d\n"
#define LINE_FORMAT_MODIFIED "|*\033[33m0x%016lx - 0x%016lx\033[0m*| %s | %-40s | %-29s | %-29s | %-29s | %-21s | %d %d %d %d %d\n"
#else
#define LINE_FORMAT_REGULAR  "| 0x%016lx - 0x%016lx | %s | %-40s | %-29s | %-29s | %-29s | %-21s |\n"
#define LINE_FORMAT_MODIFIED "|*\033[33m0x%016lx - 0x%016lx\033[0m*| %s | %-40s | %-29s | %-29s | %-29s | %-21s |\n"
#endif


typedef struct {
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


const char *reasons[3]={ "\033[33muntested\033[0m", "\033[32mwritten ok\033[0m", "\033[31mdenied\033[0m" };

static sigjmp_buf jump_buffer;

void signal_handler(int signum) {
	(void)signum;
	siglongjmp(jump_buffer, 1);
}

int safe_write(void *addr, int value) {
	struct sigaction sa, old_sa_segv, old_sa_bus;
	int old;

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGSEGV, &sa, &old_sa_segv);
	sigaction(SIGBUS, &sa, &old_sa_bus);

	if (sigsetjmp(jump_buffer, 1) == 0) {
		volatile int *volatile_addr = (volatile int *)addr;
		old = *volatile_addr;
		*volatile_addr = value;
		if (*volatile_addr != value) {
			printf("weird happened");
			exit(2);
		}
		*volatile_addr = old;

		sigaction(SIGSEGV, &old_sa_segv, NULL);
		sigaction(SIGBUS, &old_sa_bus, NULL);
		return WRITEOK;
	}

	sigaction(SIGSEGV, &old_sa_segv, NULL);
	sigaction(SIGBUS, &old_sa_bus, NULL);
	return WRITEKO;
}


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
#ifdef DEBUG
	printf("%lx-%lx %s %s [%d,%d,%d]\n", 
			m->regions[m->region_count-1].start, 
			m->regions[m->region_count-1].end, 
			m->regions[m->region_count-1].perms, 
			m->regions[m->region_count-1].name, 
			m->regions[m->region_count-1].status[0], 
			m->regions[m->region_count-1].status[1], 
			m->regions[m->region_count-1].status[2]);
#endif

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
	printf("+-----------------------------------------+------+------------------------------------------+----------------------+----------------------+----------------------+---------+\n");
	printf("| Memory Region                           | Perms| content                                  | Result Bottom        | Result middle        | Result upper         | Overall |\n");
	for (int i = 0; i < m->region_count; i++) {
		//y = BCDE + A'B'C'D'E' + AB'C'D'E
		int test_res = (
				((m->regions[i].perms[1] == 'w') && 
				 (m->regions[i].status[0] == WRITEOK) && 
				 (m->regions[i].status[1] == WRITEOK) && 
				 (m->regions[i].status[2] == WRITEOK)) ||
				(((m->regions[i].start == m2->regions[i].start) && (m->regions[i].end == m2->regions[i].end)) && 
				 !(m->regions[i].perms[1] == 'w') && 
				 !(m->regions[i].status[0] == WRITEOK) && 
				 !(m->regions[i].status[1] == WRITEOK) && 
				 !(m->regions[i].status[2] == WRITEOK)) ||
				(!((m->regions[i].start == m2->regions[i].start) && (m->regions[i].end == m2->regions[i].end)) && 
				 !(m->regions[i].perms[1] == 'w') && 
				 !(m->regions[i].status[0] == WRITEOK) && 
				 !(m->regions[i].status[1] == WRITEOK) && 
				 (m->regions[i].status[2] == WRITEOK))
				);
#ifdef DEBUG
		int a1 = !((m->regions[i].start == m2->regions[i].start) && (m->regions[i].end == m2->regions[i].end));
		int a2 = (m->regions[i].perms[1] == 'w');
		int a3 = (m->regions[i].status[0] == WRITEOK);
		int a4 = (m->regions[i].status[1] == WRITEOK);
		int a5 = (m->regions[i].status[2] == WRITEOK);
#endif
		printf("+-----------------------------------------+------+------------------------------------------+----------------------+----------------------+----------------------+---------+\n");
		printf( ((m->regions[i].start == m2->regions[i].start) && (m->regions[i].end == m2->regions[i].end)) ? LINE_FORMAT_REGULAR : LINE_FORMAT_MODIFIED,
			m->regions[i].start, m->regions[i].end, m->regions[i].perms, m->regions[i].name[0] ? m->regions[i].name : "[anonymous]",
			reasons[m->regions[i].status[0]], reasons[m->regions[i].status[1]], reasons[m->regions[i].status[2]], test_res ? "\033[42m\x1B[30mPASS\033[0m":"\033[41m\x1B[30mFAILED\033[0m"
#ifdef DEBUG
			, a1 ,a2 ,a3, a4, a5
#endif
			);
	}
	printf("+-----------------------------------------+------+------------------------------------------+----------------------+----------------------+----------------------+---------+\n");
}

int main(int argc, char *argv[]) {
	MemMap *m, *m2;

	m = malloc(sizeof(MemMap));
	m2 = malloc(sizeof(MemMap));
	memset(m, 0, sizeof(m));
	memset(m2, 0, sizeof(m2));
 	parse_proc_maps(m);
	for (int i = 0; i < m->region_count; i++) {
		unsigned long addresses[3] = {m->regions[i].start, (m->regions[i].start + m->regions[i].end) / 2, m->regions[i].end - 2 * sizeof(int)};
		for (int j = 0; j<3; j++){
#ifdef DEBUG
			printf("Attempt write %s at %p\n", m->regions[i].name, (int *) addresses[j]);
#endif
			m->regions[i].status[j] = safe_write((int *) addresses[j], ANSWER_TO_THE_ULTIMATE_QUESTION);
		}
	}

	parse_proc_maps(m2);
	print_memory_map(m, m2);
	free(m);
	free(m2);

	return 0;
}

