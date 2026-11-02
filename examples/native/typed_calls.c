#include "neonforge/typed.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static nf_str text(const char *s)
{
    nf_str value = {s, strlen(s)};
    return value;
}

static nf_status NF_CALL add(nf_context provider, nf_context caller, const nf_value *arguments,
                             uint32_t count, nf_value *result, void *user)
{
    int64_t a, b;
    (void)provider;
    (void)caller;
    (void)user;
    if (count != 2)
        return NF_E_ARGUMENT;
    a = arguments[0].as.i64;
    b = arguments[1].as.i64;
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return NF_E_LIMIT;
    result->kind = NF_VALUE_I64;
    result->as.i64 = a + b;
    return NF_OK;
}

int main(void)
{
    const nf_api *core = NULL;
    const nf_typed_api *typed = NULL;
    nf_runtime *runtime = NULL;
    nf_context provider = 0, consumer = 0;
    nf_token function = 0, handle = 0;
    nf_status status;
    nf_runtime_options options = {sizeof(options), 0, {"data/typed-demo", 15}};
    uint32_t kinds[2] = {NF_VALUE_I64, NF_VALUE_I64}, resolved[2] = {0};
    nf_signature signature = {sizeof(signature), NF_VALUE_I64, 2, 0, kinds};
    nf_function_info info = {0};
    nf_value arguments[2] = {0}, result = {0};
    nf_typed_stats stats = {0};
    uint64_t needed = 0;
    int64_t answer = 0;
    char json[64];
    int length;

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        status = (expression);                                                                     \
        if (status != NF_OK)                                                                       \
            goto cleanup;                                                                          \
    } while (0)
    CHECK(nf_get_api(NF_ABI_VERSION, sizeof(nf_api), &core));
    CHECK(nf_get_typed_api(NF_TYPED_ABI_VERSION, sizeof(nf_typed_api), &typed));
    if (!core || core->struct_size != sizeof(*core) || core->abi_version != NF_ABI_VERSION ||
        !typed || typed->struct_size != sizeof(*typed) ||
        typed->abi_version != NF_TYPED_ABI_VERSION)
    {
        status = NF_E_ABI;
        goto cleanup;
    }
    CHECK(core->runtime_create(&options, &runtime));
    CHECK(core->context_create(runtime, text("org.neonforge.typed.provider"), &provider));
    CHECK(core->context_create(runtime, text("org.neonforge.typed.consumer"), &consumer));
    CHECK(typed->function_register(runtime, provider, text("demo.math.add"), &signature, add, NULL,
                                   &function));
    info.struct_size = sizeof(info);
    CHECK(typed->function_resolve(runtime, consumer, text("demo.math.add"), &info, resolved, 2));
    if (info.result_kind != NF_VALUE_I64 || info.argument_count != 2 ||
        resolved[0] != NF_VALUE_I64 || resolved[1] != NF_VALUE_I64)
    {
        status = NF_E_ABI;
        goto cleanup;
    }
    arguments[0].struct_size = arguments[1].struct_size = sizeof(nf_value);
    arguments[0].kind = arguments[1].kind = NF_VALUE_I64;
    arguments[0].as.i64 = 20;
    arguments[1].as.i64 = 22;
    CHECK(typed->function_invoke(runtime, consumer, info.function, arguments, 2, &handle));
    result.struct_size = sizeof(result);
    CHECK(typed->result_read(runtime, consumer, handle, &result, NULL, 0, &needed));
    if (result.kind != NF_VALUE_I64 || needed != 0)
    {
        status = NF_E_ABI;
        goto cleanup;
    }
    answer = result.as.i64;
    CHECK(typed->result_release(runtime, consumer, handle));
    handle = 0;
    length = snprintf(json, sizeof(json), "{\"answer\":%" PRId64 "}", answer);
    if (length < 0 || (size_t)length >= sizeof(json))
    {
        status = NF_E_LIMIT;
        goto cleanup;
    }
    CHECK(core->settings_write(runtime, consumer, text("result"), text(json)));

cleanup:
    if (runtime)
    {
        nf_status cleaned;
        if (consumer)
        {
            cleaned = core->context_close(runtime, consumer);
            if (!status)
                status = cleaned;
            cleaned = core->context_release(runtime, consumer);
            if (!status)
                status = cleaned;
        }
        if (provider)
        {
            cleaned = core->context_close(runtime, provider);
            if (!status)
                status = cleaned;
            cleaned = core->context_release(runtime, provider);
            if (!status)
                status = cleaned;
        }
        stats.struct_size = sizeof(stats);
        cleaned = typed->stats(runtime, &stats);
        if (!status)
            status = cleaned;
        if (!status && (stats.functions || stats.results || stats.retained_payload_bytes))
            status = NF_E_STATE;
        cleaned = core->runtime_destroy(runtime);
        if (!status)
            status = cleaned;
    }
    if (status)
    {
        fprintf(stderr, "NEONFORGE typed demo failed: %d\n", (int)status);
        return 1;
    }
    printf("{\"answer\":%" PRId64 "}\n", answer);
    return 0;
#undef CHECK
}
