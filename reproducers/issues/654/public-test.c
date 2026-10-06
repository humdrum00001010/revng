__declspec(dllexport) __attribute__((naked)) unsigned entry_stack(void) {
  __asm__("movl %esp, %eax\n\tretl");
}
__declspec(dllexport) __attribute__((naked)) unsigned entry_relative(void) {
  __asm__("leal 8(%esp), %eax\n\tretl");
}
__declspec(dllexport) __attribute__((naked)) unsigned temporary_stack(void) {
  __asm__("subl $16, %esp\n\tmovl %esp, %eax\n\taddl $16, %esp\n\tretl");
}
