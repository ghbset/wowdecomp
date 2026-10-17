// Link stubs for smooth_check: storm allocator on the CRT; the rest is never called by the check.
#include <cstdlib>
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C33Matrix.h"

void* STORMAPI SMemAlloc(size_t bytes, const char*, int32_t, uint32_t flags) { return flags & 8 ? calloc(1, bytes) : malloc(bytes); }
void* STORMAPI SMemReAlloc(void* ptr, size_t bytes, const char*, int32_t, uint32_t) { return realloc(ptr, bytes); }
int32_t STORMAPI SMemFree(void* ptr, const char*, int32_t, uint32_t) { free(ptr); return 1; }
C33Matrix::C33Matrix(const C44Matrix&) { abort(); }
CDataStore& CDataStore::GetDataInSitu(void*&, uint32_t) { abort(); }
CDataStore& CDataStore::Get(uint8_t&) { abort(); }
CDataStore& CDataStore::Get(uint32_t&) { abort(); }
