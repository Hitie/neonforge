#include <neonforge/api.h>
#include <neonforge/typed.h>
_Static_assert(sizeof(nf_context) == 8, "context ABI width");
_Static_assert(sizeof(nf_value) == 24, "typed value ABI width");
_Static_assert(sizeof(nf_signature) == 24, "typed signature ABI width");
int nf_c_header_compiles(const nf_api *api) { return api && api->abi_version == NF_ABI_VERSION; }
