#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* This control reserves host memory. It does not replace mmap or a rev.ng API. */
__attribute__((constructor)) static void occupy_low_page(void) {
  const struct rlimit no_core = {0, 0};
  const size_t bytes = 4096;
  void *occupied = (void *)(uintptr_t)0x8000;
  void *free_page = (void *)(uintptr_t)0x9000;
  const int flags = MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS;

  if (sysconf(_SC_PAGESIZE) != (long)bytes) {
    fputs("occupancy-control: requires a 4096-byte host page\n", stderr);
    _exit(125);
  }
  if (setrlimit(RLIMIT_CORE, &no_core) != 0) {
    perror("occupancy-control: setrlimit");
    _exit(125);
  }
  void *mapping = mmap(occupied, bytes, PROT_NONE, flags, -1, 0);
  if (mapping != occupied) {
    fprintf(stderr, "occupancy-control: cannot reserve 0x8000: %s\n",
            strerror(errno));
    _exit(125);
  }

  /* Establish that another low page remains available, then release that page. */
  mapping = mmap(free_page, bytes, PROT_NONE, flags, -1, 0);
  if (mapping != free_page || munmap(mapping, bytes) != 0) {
    fprintf(stderr, "occupancy-control: cannot verify free page 0x9000: %s\n",
            strerror(errno));
    _exit(125);
  }
  fputs("occupancy-control: reserved PROT_NONE page 0x8000; page 0x9000 is free\n",
        stderr);
  fflush(stderr);
  /* Keep only the 0x8000 reservation for the unmodified CLI in this process. */
}
