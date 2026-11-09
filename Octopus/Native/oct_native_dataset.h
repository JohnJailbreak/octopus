#ifndef OCT_NATIVE_DATASET_H
#define OCT_NATIVE_DATASET_H
#include <stddef.h>
#include <stdint.h>

/* Bound the native DeviceInfo copy; reject extensions we do not understand.
 * Returns 1 when appended, 0 when already present, -1 for invalid input,
 * -2 for insufficient output. On 1, output is a complete dataset and length
 * is set. Input and output must not overlap. On other returns output is
 * untouched; length is zero. No operations or other capabilities are added. */
#define OCT_NATIVE_DEVICE_INFO_MAX 65536u
int oct_native_device_info_add_format(const uint8_t *input, size_t length,
                                     uint16_t format, uint8_t *output,
                                     size_t capacity, size_t *output_length);
#endif
