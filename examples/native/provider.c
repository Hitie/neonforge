#include "math_service.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const nf_api *api;
static nf_runtime *runtime;
static nf_context context;
static nf_str text(const char *s)
{
    nf_str v = {s, strlen(s)};
    return v;
}
static nf_status NF_CALL add(int32_t a, int32_t b, int32_t *out)
{
    int64_t v = (int64_t)a + b;
    if (!out)
        return NF_E_ARGUMENT;
    if (v < INT32_MIN || v > INT32_MAX)
        return NF_E_LIMIT;
    *out = (int32_t)v;
    return NF_OK;
}
static const nf_example_math service = {sizeof(nf_example_math), add};
static void NF_CALL ready(nf_context c, const nf_event *event, void *user)
{
    (void)event;
    (void)user;
    api->log(runtime, c, 1, text("Original C plugin received nf.host.ready"));
}
__declspec(dllexport) nf_status NF_CALL nf_plugin_query(nf_plugin_info *out)
{
    if (!out || out->struct_size != sizeof(*out))
        return NF_E_ABI;
    out->abi_version = 1;
    out->id = text("org.neonforge.example");
    out->version_major = 0;
    out->version_minor = 1;
    out->version_patch = 0;
    out->reserved = 0;
    return NF_OK;
}
__declspec(dllexport) nf_status NF_CALL nf_plugin_start(const nf_api *a, nf_runtime *r,
                                                        nf_context c, nf_plugin_lifecycle *out)
{
    uint64_t needed = 0;
    char data[256];
    unsigned long boots = 0;
    nf_token t = 0;
    nf_status status;
    if (!a || a->struct_size != sizeof(*a) || a->abi_version != 1 || !out ||
        out->struct_size != sizeof(*out))
        return NF_E_ABI;
    api = a;
    runtime = r;
    context = c;
    status = a->settings_read(r, c, text("counter"), data, sizeof(data), &needed);
    if (status == NF_OK)
    {
        const char *p = strstr(data, "\"boots\":");
        if (!p)
            return NF_E_ARGUMENT;
        boots = strtoul(p + 8, NULL, 10);
        if (boots >= 1000000)
            return NF_E_LIMIT;
    }
    else if (status != NF_E_NOT_FOUND)
        return status;
    snprintf(data, sizeof(data), "{\"schema_version\":1,\"boots\":%lu}", boots + 1);
    status = a->settings_write(r, c, text("counter"), text(data));
    if (status)
        return status;
    status = a->service_publish(r, c, text("org.neonforge.example.math"), 1, &service,
                                sizeof(service), &t);
    if (status)
        return status;
    return a->event_subscribe(r, c, text("nf.host.ready"), ready, NULL, &t);
}
