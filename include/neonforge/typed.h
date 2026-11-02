#ifndef NEONFORGE_TYPED_H
#define NEONFORGE_TYPED_H
#include "api.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define NF_TYPED_ABI_VERSION 1u
    enum nf_value_kind
    {
        NF_VALUE_VOID = 0,
        NF_VALUE_BOOL = 1,
        NF_VALUE_I64 = 2,
        NF_VALUE_U64 = 3,
        NF_VALUE_F64 = 4,
        NF_VALUE_UTF8 = 5,
        NF_VALUE_BYTES = 6
    };
    typedef struct nf_value
    {
        uint32_t struct_size;
        uint32_t kind;
        union
        {
            uint32_t boolean;
            int64_t i64;
            uint64_t u64;
            double f64;
            nf_str text;
            nf_bytes bytes;
        } as;
    } nf_value;
    typedef struct nf_signature
    {
        uint32_t struct_size, result_kind, argument_count, reserved;
        const uint32_t *argument_kinds;
    } nf_signature;
    typedef struct nf_function_info
    {
        uint32_t struct_size, result_kind, argument_count, reserved;
        nf_token function;
        nf_context owner;
    } nf_function_info;
    typedef nf_status(NF_CALL *nf_native_fn)(nf_context provider, nf_context caller,
                                             const nf_value *arguments, uint32_t count,
                                             nf_value *result, void *user);
    typedef struct nf_typed_stats
    {
        uint32_t struct_size, functions, results, reserved;
        uint64_t retained_payload_bytes;
    } nf_typed_stats;
    typedef struct nf_typed_api
    {
        uint32_t struct_size, abi_version;
        nf_status(NF_CALL *value_convert)(const nf_value *, uint32_t target_kind, nf_value *);
        nf_status(NF_CALL *function_register)(nf_runtime *, nf_context, nf_str,
                                              const nf_signature *, nf_native_fn, void *,
                                              nf_token *);
        nf_status(NF_CALL *function_unregister)(nf_runtime *, nf_context, nf_token);
        nf_status(NF_CALL *function_resolve)(nf_runtime *, nf_context, nf_str, nf_function_info *,
                                             uint32_t *argument_kinds, uint32_t capacity);
        nf_status(NF_CALL *function_invoke)(nf_runtime *, nf_context, nf_token, const nf_value *,
                                            uint32_t count, nf_token *result);
        nf_status(NF_CALL *result_read)(nf_runtime *, nf_context, nf_token, nf_value *,
                                        void *buffer, uint64_t capacity, uint64_t *needed);
        nf_status(NF_CALL *result_release)(nf_runtime *, nf_context, nf_token);
        nf_status(NF_CALL *stats)(nf_runtime *, nf_typed_stats *);
    } nf_typed_api;
    NF_EXPORT nf_status NF_CALL nf_get_typed_api(uint32_t version, uint32_t table_size,
                                                 const nf_typed_api **out);
#ifdef __cplusplus
}
#endif
#endif
