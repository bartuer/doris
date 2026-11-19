// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// cell_of() is the per-row path at load time: it must not allocate.  Counts
// every heap entry point (audit round 2 #2): scalar and aligned operator new,
// and malloc/calloc/realloc/posix_memalign/aligned_alloc, including calls from
// inside libs2.  Linux/glibc only (it interposes glibc's __libc_* functions);
// a positive control proves each path is counted before cell_of() is measured.

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "geo_index/cell.h"

static long g_allocs = 0;

extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);

void* malloc(size_t size) {
    ++g_allocs;
    return __libc_malloc(size);
}
void* calloc(size_t n, size_t size) {
    ++g_allocs;
    return __libc_calloc(n, size);
}
void* realloc(void* p, size_t size) {
    ++g_allocs;
    return __libc_realloc(p, size);
}
int posix_memalign(void** out, size_t align, size_t size) {
    ++g_allocs;
    *out = __libc_memalign(align, size);
    return *out == nullptr ? 12 /* ENOMEM */ : 0;
}
void* aligned_alloc(size_t align, size_t size) {
    ++g_allocs;
    return __libc_memalign(align, size);
}
} // extern "C"

// operator new (scalar, array, aligned, nothrow) all end in malloc or
// posix_memalign in libstdc++, so the C hooks above count them.

int main() {
    // Positive control: every monitored path increments the counter.
    long c0 = g_allocs;
    void* p1 = std::malloc(16);
    void* p2 = std::calloc(1, 16);
    p1 = std::realloc(p1, 64);
    void* p3 = nullptr;
    int rc = posix_memalign(&p3, 64, 64);
    void* p4 = aligned_alloc(64, 64);
    auto* p5 = new int(1);
    auto* p6 = new std::vector<int>(4);
    struct alignas(64) Wide {
        char c[64];
    };
    auto* p7 = new Wide();
    long control = g_allocs - c0;
    std::free(p1);
    std::free(p2);
    std::free(p3);
    std::free(p4);
    delete p5;
    delete p6;
    delete p7;
    // malloc, calloc, realloc, posix_memalign, aligned_alloc, new int,
    // new vector + its buffer, aligned new: >= 9 counted calls.
    if (rc != 0 || control < 9) {
        std::printf("positive control failed: %ld counted calls (want >= 9)\n", control);
        return 2;
    }

    volatile int64_t sink = 0;
    long before = g_allocs;
    for (int i = 0; i < 1000000; ++i) {
        sink = sink ^ geo_index::cell_of(-89.9 + (i % 1799) * 0.1, -179.9 + (i % 3599) * 0.1, 16);
    }
    long allocations = g_allocs - before;
    std::printf("cell_of: positive control %ld/9 counted; 1000000 calls, %ld allocations\n", control,
                allocations);
    return allocations == 0 ? 0 : 1;
}
