#define NF_BUILD_CORE
#include "kernel.hpp"
#include <neonforge/api.h>
extern "C" NF_EXPORT nf_status NF_CALL nf_get_api(uint32_t abi, uint32_t size, const nf_api **out)
{
    if (!out)
        return NF_E_ARGUMENT;
    *out = nullptr;
    if (abi != NF_ABI_VERSION || size != sizeof(nf_api))
        return NF_E_ABI;
    *out = &nf::kernel_api();
    return NF_OK;
}
extern "C" NF_EXPORT nf_status NF_CALL nf_get_typed_api(uint32_t abi, uint32_t size,
                                                        const nf_typed_api **out)
{
    if (!out)
        return NF_E_ARGUMENT;
    *out = nullptr;
    if (abi != NF_TYPED_ABI_VERSION || size != sizeof(nf_typed_api))
        return NF_E_ABI;
    *out = &nf::typed_api();
    return NF_OK;
}
