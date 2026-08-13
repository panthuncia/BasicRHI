#ifndef BASICRHI_C_H
#define BASICRHI_C_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define BASICRHI_C_CALL __cdecl
#else
#define BASICRHI_C_CALL
#endif

#define BASICRHI_C_ABI_VERSION_1 1u
#define BASICRHI_C_INVALID_INDEX UINT32_MAX

typedef enum basicrhi_c_result {
    BASICRHI_C_OK = 0,
    BASICRHI_C_INVALID_ARGUMENT = 1,
    BASICRHI_C_STALE_BORROW = 2,
    BASICRHI_C_UNSUPPORTED = 3,
    BASICRHI_C_BACKEND_ERROR = 4
} basicrhi_c_result;

typedef struct basicrhi_c_handle {
    uint32_t index;
    uint32_t generation;
} basicrhi_c_handle;

typedef basicrhi_c_handle basicrhi_c_resource;
typedef basicrhi_c_handle basicrhi_c_view;
typedef basicrhi_c_handle basicrhi_c_sampler;
typedef basicrhi_c_handle basicrhi_c_pipeline;
typedef basicrhi_c_handle basicrhi_c_pipeline_layout;
typedef basicrhi_c_handle basicrhi_c_command_signature;
typedef basicrhi_c_handle basicrhi_c_descriptor_heap;

typedef struct basicrhi_c_byte_span {
    const void* data;
    size_t size_bytes;
} basicrhi_c_byte_span;

/* Valid only during one contributor execute callback. No end/reset/submit entry points exist. */
typedef struct basicrhi_c_borrowed_recorder_v1 {
    uint32_t structure_size;
    uint32_t abi_version;
    void* context;
    uint64_t borrow_token;
    basicrhi_c_result (BASICRHI_C_CALL *bind_layout)(void*, uint64_t, basicrhi_c_pipeline_layout);
    basicrhi_c_result (BASICRHI_C_CALL *bind_pipeline)(void*, uint64_t, basicrhi_c_pipeline);
    basicrhi_c_result (BASICRHI_C_CALL *set_descriptor_heaps)(void*, uint64_t, basicrhi_c_descriptor_heap, basicrhi_c_descriptor_heap);
    basicrhi_c_result (BASICRHI_C_CALL *push_constants)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, basicrhi_c_byte_span);
    basicrhi_c_result (BASICRHI_C_CALL *draw)(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *draw_indexed)(void*, uint64_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *dispatch)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *dispatch_mesh)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *execute_indirect)(void*, uint64_t, basicrhi_c_command_signature,
        basicrhi_c_resource, uint64_t, basicrhi_c_resource, uint64_t, uint32_t);
    basicrhi_c_result (BASICRHI_C_CALL *copy_buffer)(void*, uint64_t, basicrhi_c_resource, uint64_t,
        basicrhi_c_resource, uint64_t, uint64_t);
} basicrhi_c_borrowed_recorder_v1;

typedef struct basicrhi_c_device_api_v1 {
    uint32_t structure_size;
    uint32_t abi_version;
    void* context;
    basicrhi_c_result (BASICRHI_C_CALL *query_capability)(void*, uint32_t, void*, size_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_pipeline_layout)(void*, basicrhi_c_byte_span, basicrhi_c_pipeline_layout*);
    void (BASICRHI_C_CALL *destroy_pipeline_layout)(void*, basicrhi_c_pipeline_layout, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_pipeline)(void*, basicrhi_c_byte_span, basicrhi_c_pipeline*);
    void (BASICRHI_C_CALL *destroy_pipeline)(void*, basicrhi_c_pipeline, uint64_t);
    basicrhi_c_result (BASICRHI_C_CALL *create_command_signature)(void*, basicrhi_c_byte_span, basicrhi_c_command_signature*);
    void (BASICRHI_C_CALL *destroy_command_signature)(void*, basicrhi_c_command_signature, uint64_t);
} basicrhi_c_device_api_v1;

#if defined(__cplusplus)
static_assert(sizeof(basicrhi_c_handle) == 8, "BasicRHI C handle ABI drift");
#endif

#endif
