#include <neonforge/api.h>
#include <string.h>
static nf_str text(const char *s)
{
    nf_str v = {s, strlen(s)};
    return v;
}
static void NF_CALL event(nf_context c, const nf_event *e, void *u)
{
    (void)c;
    (void)e;
    (void)u;
}
static void NF_CALL task(nf_context c, void *u)
{
    (void)c;
    (void)u;
}
__declspec(dllexport) nf_status NF_CALL nf_plugin_query(nf_plugin_info *out)
{
    out->abi_version = 1;
    out->id = text("org.neonforge.example");
    out->version_minor = 1;
    return NF_OK;
}
__declspec(dllexport) nf_status NF_CALL nf_plugin_start(const nf_api *a, nf_runtime *r,
                                                        nf_context c, nf_plugin_lifecycle *out)
{
    nf_token t = 0;
    (void)out;
    a->event_subscribe(r, c, text("nf.host.ready"), event, 0, &t);
    a->task_enqueue(r, c, task, 0, &t);
    return NF_E_STATE;
}
