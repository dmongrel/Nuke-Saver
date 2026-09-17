// The single translation unit that instantiates VMA. Kept apart from everything else so the
// header's large implementation is compiled exactly once, and so its warnings do not have to
// be reconciled with the project's -Wall -Wextra.
//
// VMA takes its entry points from volk rather than linking them (spec 12: nothing static-imports
// vulkan-1.dll), which is what the two VULKAN_FUNCTIONS switches below select.

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#include <vk_mem_alloc.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
