#include "audio_ring.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t storage[11] = {0};
    uint8_t readback[32] = {0};
    uint8_t sequence[32];
    for (size_t i = 0; i < sizeof(sequence); ++i) sequence[i] = (uint8_t)(i + 1);

    audio_ring_t ring;
    audio_ring_init(&ring, storage, sizeof(storage));
    assert(audio_ring_available(&ring) == 0);
    assert(audio_ring_space(&ring) == 11);
    assert(audio_ring_read(&ring, readback, 5) == 0);

    assert(audio_ring_write(&ring, sequence, 9) == 9);
    assert(audio_ring_available(&ring) == 9);
    assert(audio_ring_read(&ring, readback, 7) == 7);
    assert(memcmp(readback, sequence, 7) == 0);
    assert(audio_ring_write(&ring, sequence + 9, 9) == 9);
    assert(audio_ring_space(&ring) == 0);
    assert(audio_ring_write(&ring, sequence, 1) == 0);
    assert(audio_ring_read(&ring, readback, 11) == 11);
    assert(readback[0] == 8 && readback[1] == 9);
    assert(memcmp(readback + 2, sequence + 9, 9) == 0);
    assert(audio_ring_available(&ring) == 0);

    /* Exercise repeated wraparound with a reference byte queue. */
    uint8_t reference[11] = {0};
    size_t used = 0;
    for (int step = 0; step < 5000; ++step) {
        uint8_t input[7];
        for (size_t i = 0; i < sizeof(input); ++i)
            input[i] = (uint8_t)(step + (int)i);
        size_t request = (size_t)((step * 7) % 8);
        if (step % 3 != 0) {
            size_t expected = request < sizeof(reference) - used ? request : sizeof(reference) - used;
            size_t actual = audio_ring_write(&ring, input, request);
            assert(actual == expected);
            memcpy(reference + used, input, actual);
            used += actual;
        } else {
            size_t expected = request < used ? request : used;
            size_t actual = audio_ring_read(&ring, readback, request);
            assert(actual == expected);
            assert(memcmp(readback, reference, actual) == 0);
            memmove(reference, reference + actual, used - actual);
            used -= actual;
        }
        assert(audio_ring_available(&ring) == used);
        assert(audio_ring_space(&ring) + used == sizeof(reference));
    }
    puts("Audio ring tests: OK");
    return 0;
}
