void pointer_cycle(unsigned *slot) {
  unsigned middle = *(unsigned *) *slot;
  *slot = *(unsigned *) middle;
}
