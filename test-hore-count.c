/*
 * Copyright 2020-2022 Andrew Sharp andy@tigerand.com, All Rights Reserved
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define FERR(FMT, ...) fprintf(stderr, FMT "\n", ##__VA_ARGS__)

#define SYS_CPU_FILE "/sys/devices/system/cpu/online"

 int
get_core_count(void)
{
	int fd;
	char online[512];
	int c_hores;
	int chars_read;
	char *tok;           /* token pointer */
	char *prog;          /* progress pointer */
	int begin_cpu;       /* the cpu num at the beginning of a range */
	int end_cpu;         /* the cpu num at the end of a range */

	online[0] = '\0';
	c_hores = 0;

	fd = open(SYS_CPU_FILE, O_RDONLY);
	if (fd < 0) {
		FERR("Problem opening %s for core count.  errno = %d", SYS_CPU_FILE,
			errno);
		return 0;
	}
//	fd = 0;
	chars_read = read(fd, &online[0], sizeof(online) - 1);
	if (chars_read < 1) {
		FERR("Problem reading %s for core count.  errno = %d", SYS_CPU_FILE,
			errno);
		return 0;
	}
	online[chars_read] = '\0';

	/*
	 * what a holey pain in the
	 */
	prog = &online[0];
	while ((tok = strpbrk(prog, ",-")) != NULL) {
		if (*tok == ',') {
			c_hores++;
		} else if (*tok == '-') {
			*tok = '\0';
			sscanf(prog, "%d", &begin_cpu);
			prog = tok + 1;
			tok = strpbrk(prog, ",");
			if (tok) {
				*tok = '\0';
			}
			sscanf(prog, "%d", &end_cpu);
			c_hores = c_hores + (end_cpu - begin_cpu + 1);
			if (tok == NULL) {
				prog = tok;
				break;
			}
		}
		prog = tok + 1;
	}
	if (prog && sscanf(prog, "%d", &end_cpu) == 1) {
		c_hores++;
	}

	return c_hores;
}

 void
main(int argc, char **argv) {
	int hores;

	hores = get_core_count();
	printf("hores=%d\n", hores);
}
