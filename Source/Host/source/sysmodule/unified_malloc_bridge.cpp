// Route newlib allocations (including Opus) through the shared libstratosphere heap.
#include <stratosphere.hpp>

extern "C"
{

    void *__libnx_alloc(size_t size)
    {
        return ::ams::init::GetAllocator()->Allocate(size);
    }

    void *__libnx_aligned_alloc(size_t alignment, size_t size)
    {
        return ::ams::init::GetAllocator()->Allocate(size, alignment);
    }

    void __libnx_free(void *p)
    {
        ::ams::init::GetAllocator()->Free(p);
    }
}
