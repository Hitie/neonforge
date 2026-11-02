#include "math_service.h"
#include <cstring>
#include <new>
namespace
{
nf_str text(const char *s) { return {s, std::strlen(s)}; }
struct State
{
    const nf_api *api;
    nf_runtime *r;
    nf_context c;
    int32_t answer = 0;
};
void NF_CALL stop(void *user) { delete static_cast<State *>(user); }
void NF_CALL work(nf_context c, void *user)
{
    auto *s = static_cast<State *>(user);
    if (s->answer == 42)
        s->api->settings_write(s->r, c, text("result"),
                               text("{\"schema_version\":1,\"answer\":42}"));
}
} // namespace
extern "C" __declspec(dllexport) nf_status NF_CALL nf_plugin_query(nf_plugin_info *out)
{
    if (!out || out->struct_size != sizeof(*out))
        return NF_E_ABI;
    out->abi_version = 1;
    out->id = text("org.neonforge.consumer");
    out->version_major = 0;
    out->version_minor = 1;
    out->version_patch = 0;
    return NF_OK;
}
extern "C" __declspec(dllexport) nf_status NF_CALL nf_plugin_start(const nf_api *a, nf_runtime *r,
                                                                   nf_context c,
                                                                   nf_plugin_lifecycle *out)
{
    if (!a || a->struct_size != sizeof(*a) || a->abi_version != 1 || !out ||
        out->struct_size != sizeof(*out))
        return NF_E_ABI;
    auto *s = new (std::nothrow) State{a, r, c};
    if (!s)
        return NF_E_LIMIT;
    out->user = s;
    out->stop = stop;
    nf_service lease{};
    auto status = a->service_acquire(r, c, text("org.neonforge.example.math"), 1,
                                     sizeof(nf_example_math), &lease);
    if (status)
        return status;
    auto *math = static_cast<const nf_example_math *>(lease.table);
    status = math->add(20, 22, &s->answer);
    if (status)
        return status;
    nf_token task = 0;
    return a->task_enqueue(r, c, work, s, &task);
}
