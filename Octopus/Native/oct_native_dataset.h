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

/* ---- Reporting a format a host will accept -------------------------------------------------
 *
 * In a PTP ObjectInfo dataset the ObjectFormat is a 16-bit field at offset 4, straight after the
 * 32-bit StorageID. This rewrites it, and only it, when it holds `from`.
 *
 * Why: on 10 November Windows asked for ObjectInfo on all five objects in the root and in
 * Documents, then displayed three. The two it dropped are the ones whose format is 0x3004, MTP's
 * Text; the one it kept alongside the folders is 0x3000, Undefined. Everything else in those
 * datasets is identical in shape, the format was advertised to the host in GetDeviceInfo (the hook
 * ran and the append succeeded, both confirmed on the device), and the object it did accept
 * downloaded with a matching SHA-256. So the host accepts Undefined here and not Text.
 *
 * 0x3000 means "a file whose format the device does not classify", which is true of these objects
 * and is what the host demonstrably displays. The store keeps calling them text, because they are;
 * only what is reported over the wire changes.
 *
 * Returns 1 when the field was rewritten, 0 when it held something else, -1 on a short or absent
 * dataset. */
int oct_native_object_info_set_format(unsigned char *bytes, size_t length, uint16_t from, uint16_t to);


/* ---- Advertising an operation -------------------------------------------------------------
 *
 * Appends an operation code to OperationsSupported, the first array in a PTPDeviceInfoDataset,
 * exactly as add_format appends to the playback formats array: same arguments, same returns, same
 * refusal to touch a dataset whose layout is not completely understood.
 *
 * A host reads that array to decide which operations to offer. Without DeleteObject in it Explorer
 * will not offer to delete, however willing the device is. */
int oct_native_device_info_add_operation(const uint8_t *input, size_t length,
                                         uint16_t operation, uint8_t *output,
                                         size_t capacity, size_t *output_length);

#endif