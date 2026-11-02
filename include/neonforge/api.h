#ifndef NEONFORGE_API_H
#define NEONFORGE_API_H
#include <stdint.h>
#ifdef _WIN32
#define NF_CALL __cdecl
#ifdef NF_BUILD_CORE
#define NF_EXPORT __declspec(dllexport)
#else
#define NF_EXPORT __declspec(dllimport)
#endif
#else
#define NF_CALL
#define NF_EXPORT
#endif
#ifdef __cplusplus
extern "C"
{
#endif
#define NF_ABI_VERSION 1u
    typedef int32_t nf_status;
    enum
    {
        NF_OK = 0,
        NF_E_ARGUMENT = 1,
        NF_E_ABI = 2,
        NF_E_UNAVAILABLE = 3,
        NF_E_HANDLE = 4,
        NF_E_OWNER = 5,
        NF_E_THREAD = 6,
        NF_E_CONFLICT = 7,
        NF_E_STATE = 8,
        NF_E_LIMIT = 9,
        NF_E_BUFFER = 10,
        NF_E_IO = 11,
        NF_E_CALLBACK = 12,
        NF_E_BUSY = 13,
        NF_E_NOT_FOUND = 14
    };
    typedef struct nf_runtime nf_runtime;
    typedef uint64_t nf_context;
    typedef uint64_t nf_token;
    typedef struct nf_str
    {
        const char *data;
        uint64_t size;
    } nf_str;
    typedef struct nf_bytes
    {
        const void *data;
        uint64_t size;
    } nf_bytes;
    typedef struct nf_runtime_options
    {
        uint32_t struct_size;
        uint32_t reserved;
        nf_str data_root;
    } nf_runtime_options;
    typedef struct nf_event
    {
        nf_str name;
        nf_bytes payload;
        nf_context sender;
    } nf_event;
    typedef void(NF_CALL *nf_event_fn)(nf_context context, const nf_event *event, void *user);
    typedef void(NF_CALL *nf_task_fn)(nf_context context, void *user);
    typedef struct nf_service
    {
        nf_token lease;
        const void *table;
        uint32_t table_size;
        uint32_t version;
    } nf_service;
    typedef struct nf_stats
    {
        uint32_t struct_size;
        uint32_t contexts;
        uint32_t subscriptions;
        uint32_t queued_tasks;
        uint32_t services;
        uint32_t leases;
        uint64_t callbacks;
        uint64_t callback_errors;
        uint64_t callback_nanoseconds;
        uint64_t tasks_run;
        uint64_t log_lines;
    } nf_stats;
    typedef struct nf_context_stats
    {
        uint32_t struct_size;
        uint32_t closing;
        uint32_t subscriptions;
        uint32_t queued_tasks;
        uint32_t services;
        uint32_t leases;
        uint64_t callbacks;
        uint64_t callback_errors;
        uint64_t callback_nanoseconds;
    } nf_context_stats;
    typedef struct nf_capability
    {
        uint32_t struct_size;
        uint32_t version;
        uint32_t implemented;
        uint32_t offline_tested;
        uint32_t game_tested;
        uint32_t reserved;
    } nf_capability;
    typedef struct nf_api
    {
        uint32_t struct_size;
        uint32_t abi_version;
        uint32_t version_major, version_minor, version_patch, reserved;
        nf_status(NF_CALL *runtime_create)(const nf_runtime_options *, nf_runtime **);
        nf_status(NF_CALL *runtime_destroy)(nf_runtime *);
        nf_status(NF_CALL *context_create)(nf_runtime *, nf_str, nf_context *);
        nf_status(NF_CALL *context_close)(nf_runtime *, nf_context);
        nf_status(NF_CALL *context_release)(nf_runtime *, nf_context);
        nf_status(NF_CALL *capability_query)(nf_runtime *, nf_str, nf_capability *);
        nf_status(NF_CALL *event_subscribe)(nf_runtime *, nf_context, nf_str, nf_event_fn, void *,
                                            nf_token *);
        nf_status(NF_CALL *event_unsubscribe)(nf_runtime *, nf_context, nf_token);
        nf_status(NF_CALL *event_emit)(nf_runtime *, nf_context, nf_str, nf_bytes);
        nf_status(NF_CALL *task_enqueue)(nf_runtime *, nf_context, nf_task_fn, void *, nf_token *);
        nf_status(NF_CALL *task_cancel)(nf_runtime *, nf_context, nf_token);
        nf_status(NF_CALL *task_pump)(nf_runtime *, uint32_t, uint32_t *);
        nf_status(NF_CALL *service_publish)(nf_runtime *, nf_context, nf_str, uint32_t,
                                            const void *, uint32_t, nf_token *);
        nf_status(NF_CALL *service_acquire)(nf_runtime *, nf_context, nf_str, uint32_t, uint32_t,
                                            nf_service *);
        nf_status(NF_CALL *service_release)(nf_runtime *, nf_context, nf_token);
        nf_status(NF_CALL *settings_write)(nf_runtime *, nf_context, nf_str, nf_str);
        nf_status(NF_CALL *settings_read)(nf_runtime *, nf_context, nf_str, char *, uint64_t,
                                          uint64_t *);
        nf_status(NF_CALL *log)(nf_runtime *, nf_context, uint32_t, nf_str);
        nf_status(NF_CALL *stats)(nf_runtime *, nf_stats *);
        nf_status(NF_CALL *last_error)(nf_runtime *, char *, uint64_t, uint64_t *);
        nf_status(NF_CALL *context_stats)(nf_runtime *, nf_context, nf_context_stats *);
    } nf_api;
    NF_EXPORT nf_status NF_CALL nf_get_api(uint32_t abi_version, uint32_t table_size,
                                           const nf_api **out);
    typedef struct nf_plugin_info
    {
        uint32_t struct_size;
        uint32_t abi_version;
        nf_str id;
        uint32_t version_major, version_minor, version_patch, reserved;
    } nf_plugin_info;
    typedef struct nf_plugin_lifecycle
    {
        uint32_t struct_size;
        uint32_t reserved;
        void *user;
        void(NF_CALL *stop)(void *user);
    } nf_plugin_lifecycle;
    typedef nf_status(NF_CALL *nf_plugin_query_fn)(nf_plugin_info *);
    typedef nf_status(NF_CALL *nf_plugin_start_fn)(const nf_api *, nf_runtime *, nf_context,
                                                   nf_plugin_lifecycle *);
#ifdef __cplusplus
}
#endif
#endif
