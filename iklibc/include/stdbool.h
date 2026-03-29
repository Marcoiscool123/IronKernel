/* IronKernel iklibc — stdbool.h */
#ifndef STDBOOL_H
#define STDBOOL_H

/* In C23+, bool/true/false are keywords — no typedef needed. */
#if __STDC_VERSION__ < 202311L
# ifndef __cplusplus
    typedef _Bool bool;
#   define true  1
#   define false 0
# endif
#endif

#define __bool_true_false_are_defined 1

#endif
