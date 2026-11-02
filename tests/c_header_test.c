#include <neonforge/api.h>
_Static_assert(sizeof(nf_context) == 8, "context ABI width");
int nf_c_header_compiles(const nf_api *api) { return api && api->abi_version == NF_ABI_VERSION; }
