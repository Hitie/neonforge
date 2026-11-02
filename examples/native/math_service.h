#ifndef NF_EXAMPLE_MATH_H
#define NF_EXAMPLE_MATH_H
#include <neonforge/api.h>
typedef struct nf_example_math
{
    uint32_t struct_size;
    nf_status(NF_CALL *add)(int32_t, int32_t, int32_t *);
} nf_example_math;
#endif
