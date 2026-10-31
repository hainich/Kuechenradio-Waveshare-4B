#pragma once
/*
 * Small byte FIFO used by the internet radio.
 * Storage is allocated separately in internal SRAM.
 * NOT thread-safe: callers must hold the pipe mutex.
 * Pure C so the wraparound logic can be tested without ESP-IDF.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t used;
} audio_ring_t;

static inline void audio_ring_init(audio_ring_t *ring, uint8_t *storage, size_t capacity)
{
    ring->data = storage;
    ring->capacity = capacity;
    ring->head = 0;
    ring->tail = 0;
    ring->used = 0;
}

static inline size_t audio_ring_available(const audio_ring_t *ring)
{
    return ring->used;
}

static inline size_t audio_ring_space(const audio_ring_t *ring)
{
    return ring->capacity - ring->used;
}

static inline size_t audio_ring_write(audio_ring_t *ring, const uint8_t *source, size_t length)
{
    if (length > audio_ring_space(ring)) length = audio_ring_space(ring);
    if (!length) return 0;
    size_t first = ring->capacity - ring->head;
    if (first > length) first = length;
    memcpy(ring->data + ring->head, source, first);
    memcpy(ring->data, source + first, length - first);
    ring->head = (ring->head + length) % ring->capacity;
    ring->used += length;
    return length;
}

static inline size_t audio_ring_read(audio_ring_t *ring, uint8_t *dest, size_t length)
{
    if (length > ring->used) length = ring->used;
    if (!length) return 0;
    size_t first = ring->capacity - ring->tail;
    if (first > length) first = length;
    memcpy(dest, ring->data + ring->tail, first);
    memcpy(dest + first, ring->data, length - first);
    ring->tail = (ring->tail + length) % ring->capacity;
    ring->used -= length;
    return length;
}
