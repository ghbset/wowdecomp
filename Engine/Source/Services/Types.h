#ifndef ENGINE_SOURCE_SERVICES_TYPES_H
#define ENGINE_SOURCE_SERVICES_TYPES_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Storm/H/stpl.h"

class CAsyncObject;
class CAsyncQueue;
class SFileImpl;

class CAsyncObject {
    public:
    /* 0000 */ SFileImpl* m_file;
    /* 0004 */ void* m_buffer;
    /* 0008 */ uint32_t m_size;
    /* 000C */ void* m_userArg;
    /* 0010 */ void* m_userPostloadCallback;
    /* 0014 */ void* m_userPostUnloadCallback;
    /* 0018 */ CAsyncQueue* m_queue;
    /* 001C */ uint32_t m_queuedFrame;
    /* 0020 */ uint8_t m_priority;
    /* 0021 */ int8_t m_isProcessed;
    /* 0022 */ int8_t m_isRead;
    /* 0023 */ int8_t m_isCurrent;
    /* 0024 */ int8_t m_char24;
    /* 0025 */ int8_t m_char25;
    /* 0026 */ uint16_t m_padding;
    /* 0028 */ TSLink<CAsyncObject> m_link;
    // Not stock (worldLoadSort): world x/y the load is for, so the disk readers can take the nearest one first.
    float m_loadX;
    float m_loadY;
    uint32_t m_linkFrame;
    bool m_hasLoadPos;
#if defined(WOWDECOMP_ARCH_64)
    // MODIFIED: performance. Runs on the reader thread right after a successful read, before the object is posted
    // to the main thread. It may only touch m_buffer (which outlives a Cancel until the post-unload callback runs)
    // and the fields below, never the owner, which a Cancel can free while it runs
    void (*m_prepareCallback)(CAsyncObject* object);
    // the prepare callback's result for the post-load callback: 0 none ran, > 0 succeeded, < 0 failed
    int32_t m_prepared;
    // ASYNC_KIND_*, for the load statistics
    uint32_t m_kind;
#endif

    int32_t IsPending();
};

// MODIFIED: kinds of asynchronous loads, for GetLoadTimings()
enum {
    ASYNC_KIND_OTHER,
    ASYNC_KIND_TEXTURE,
    ASYNC_KIND_MODEL,
    ASYNC_KIND_SKIN,
    ASYNC_KIND_ANIM,
    ASYNC_KIND_ADT,
    ASYNC_KIND_WMO,
    ASYNC_KIND_WMOGROUP,
    ASYNC_KIND_COUNT
};

#if defined(WOWDECOMP_ARCH_64)
#define ASYNC_SET_KIND(object, kind) ((object)->m_kind = (kind))
#else
#define ASYNC_SET_KIND(object, kind) ((void)0)
#endif

struct HTEXTBLOCK__ {
    /* 0000 */ int32_t m_unused;
};

typedef HTEXTBLOCK__* HTEXTBLOCK;

struct HTEXTFONT__ {
    /* 0000 */ int32_t m_unused;
};

typedef HTEXTFONT__* HTEXTFONT;

enum PIXEL_FORMAT {
    PIXEL_DXT1 = 0,
    PIXEL_DXT3 = 1,
    PIXEL_ARGB8888 = 2,
    PIXEL_ARGB1555 = 3,
    PIXEL_ARGB4444 = 4,
    PIXEL_RGB565 = 5,
    PIXEL_A8 = 6,
    PIXEL_DXT5 = 7,
    PIXEL_UNSPECIFIED = 8,
    PIXEL_ARGB2565 = 9,
    NUM_PIXEL_FORMATS = 10,
};

#endif
