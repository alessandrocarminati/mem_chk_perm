#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_REGIONS 1024

#if __SIZEOF_POINTER__ == 8
#define ADDRESS_SPACE_END 0xFFFFFFFFFFFFFFFF
#else
#define ADDRESS_SPACE_END 0xFFFFFFFF
#endif

#define WRITEOK 1
#define WRITEKO 2
#define ANSWER_TO_THE_ULTIMATE_QUESTION 42


typedef struct {
	pid_t pid;
	int status;
	unsigned long start;
	unsigned long end;
	char perms[5];
	char name[256];
} MemRegion;

const char *reasons[3]={ "untested", "written ok", "segfault" };
MemRegion regions[MAX_REGIONS];
int region_count = 0;

void parse_proc_maps() {
	FILE *fp = fopen("/proc/self/maps", "r");
	if (!fp) {
		perror("Failed to open /proc/self/maps");
		exit(EXIT_FAILURE);
	}

	unsigned long prev_end = 0;
	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		MemRegion region;
		char perms[5], dev[12], mapname[256] = "";
		unsigned long offset, inode;

		if (sscanf(line, "%lx-%lx %4s %lx %s %lu %255[^\n]s",
				   &region.start, &region.end, perms, &offset, dev, &inode, mapname) >= 6) {
			strncpy(region.perms, perms, 4);
			region.perms[4] = '\0';
			strncpy(region.name, mapname, 255);
			region.name[255] = '\0';

			if (region_count == 0 && region.start > 0) {
				regions[region_count].start = 0;
				regions[region_count].end = region.start;
				strcpy(regions[region_count].perms, "----");
				strcpy(regions[region_count].name, "[unmapped]");
				region_count++;
			}

			if (prev_end != 0 && region.start > prev_end) {
				regions[region_count].start = prev_end;
				regions[region_count].end = region.start;
				strcpy(regions[region_count].perms, "----");
				strcpy(regions[region_count].name, "[unmapped]");
				region_count++;
			}

			regions[region_count++] = region;
			prev_end = region.end;
		}
	}
	fclose(fp);

	if (prev_end < ADDRESS_SPACE_END) {
		regions[region_count].start = prev_end;
		regions[region_count].end = ADDRESS_SPACE_END;
		strcpy(regions[region_count].perms, "----");
		strcpy(regions[region_count].name, "[unmapped]");
		region_count++;
	}
}
void print_memory_map() {
	printf("Memory Layout:\n");
	printf("+-----------------------------------------+------+------------------------------------------+--------+----------------------+\n");
	printf("| Memory Region                           | Perms| content                                  | TestPID| Result               |\n");
	for (int i = 0; i < region_count; i++) {

		printf("+-----------------------------------------+------+------------------------------------------+--------+----------------------+\n");
		printf("| 0x%016lx - 0x%016lx | %s | %-40s | %-6d | %-20s |\n",
			   regions[i].start, regions[i].end, regions[i].perms, regions[i].name[0] ? regions[i].name : "[anonymous]", regions[i].pid, 
			   reasons[regions[i].status]);
	}
	printf("+-----------------------------------------+------+------------------------------------------+--------+----------------------+\n");
}

void handle_child_exit(pid_t pid, int status) {
	int i = 0;

	while (i < region_count && regions[i].pid != pid) {
		i++;
	}
	if (regions[i].pid == pid) {
		if (WIFEXITED(status)) {
			regions[i].status = WRITEOK;
		}
		if (WIFSIGNALED(status)) {
			regions[i].status = WRITEKO;
		}
	}

}

void child_process(MemRegion region, int mode) {
	int modebit = 1;
	pid_t pid = getpid();

	prctl(PR_SET_DUMPABLE, 0);
	unsigned long addresses[3] = {region.start, (region.start + region.end) / 2, region.end - sizeof(int)};
	for (int i = 0; i < 3; i++) {
		printf("[%d] mode=%d, mask=%d res=%d\n", pid, mode, modebit << i, (mode & (modebit << i)));
		if (mode & (modebit << i)) {
			int *ptr = (int *)addresses[i];
			printf("[%d] Testing %p\n", pid, ptr);
			*ptr = ANSWER_TO_THE_ULTIMATE_QUESTION;
		}
	}
	exit(8);
}

int main(int argc, char *argv[]) {
	int mode;

	if (argc != 2) {
		printf("%s <mode>\n", argv[0]);
		return 1;
	}
	mode = atoi(argv[1]);
	if ((mode <=0)||(mode>=8)) return 2;
	memset(regions,0, sizeof(regions));
	parse_proc_maps();
	for (int i = 0; i < region_count; i++) {
		pid_t pid = fork();
		if (pid < 0) {
			perror("Fork failed");
			exit(EXIT_FAILURE);
		} else if (pid == 0) {
			child_process(regions[i], mode);
		} else {
			regions[i].pid = pid;
		}
	}

	for (int i = 0; i < region_count; i++) {
		int status;
		pid_t pid = wait(&status);
		handle_child_exit(pid, status);
	}

	print_memory_map();

	return 0;
}

