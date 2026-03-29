/* IronKernel v0.04 — test/hi.c */
void _start(void)
{
    __asm__ volatile("mov $1,%%rax; int $0x80":::"rax");
    __builtin_unreachable();
}
