#include "oct_native_dataset.h"
#include <stdbool.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static bool skip_string(const uint8_t *p,size_t length,size_t *offset) {
    if (*offset >= length) return false;
    size_t units=p[(*offset)++], bytes=units*2;
    if (bytes > length-*offset) return false;
    if (units && (p[*offset+bytes-2] || p[*offset+bytes-1])) return false;
    *offset+=bytes;return true;
}
int oct_native_device_info_add_format(const uint8_t *input,size_t length,
                                     uint16_t format,uint8_t *output,
                                     size_t capacity,size_t *output_length) {
    if (!output_length) return -1;
    *output_length=0;
    if (!input || length<8 || length>OCT_NATIVE_DEVICE_INFO_MAX) return -1;
    /* standardVersion/u16, vendorExtensionID/u32, extensionVersion/u16 */
    size_t offset=8;
    if (!skip_string(input,length,&offset) || length-offset<2) return -1;
    offset+=2; /* functionalMode */
    size_t format_count_offset=0,format_end=0;uint32_t formats=0;bool present=false;
    for (unsigned array=0;array<5;array++) {
        if (length-offset<4) return -1;
        size_t count_offset=offset;uint32_t count=u32(input+offset);offset+=4;
        if (count>(length-offset)/2) return -1;
        if (array==4) {
            format_count_offset=count_offset;formats=count;
            for (uint32_t i=0;i<count;i++) {
                const uint8_t *item=input+offset+(size_t)i*2;
                if (((uint16_t)item[0] | (uint16_t)item[1]<<8)==format) present=true;
            }
        }
        offset+=(size_t)count*2;
        if (array==4) format_end=offset;
    }
    /* Manufacturer, model, device version and serial number. Require the
     * complete known layout before changing even one advertised format. */
    for(unsigned i=0;i<4;i++)if(!skip_string(input,length,&offset))return -1;
    if (offset!=length) return -1;
    if (present) return 0;
    if (length>OCT_NATIVE_DEVICE_INFO_MAX-2 || !output || capacity<length+2) return -2;
    memcpy(output,input,format_end);
    output[format_end]=(uint8_t)format;output[format_end+1]=(uint8_t)(format>>8);
    memcpy(output+format_end+2,input+format_end,length-format_end);
    uint32_t count=formats+1;
    for(unsigned i=0;i<4;i++)output[format_count_offset+i]=(uint8_t)(count>>(i*8));
    *output_length=length+2;return 1;
}

int oct_native_object_info_set_format(unsigned char *bytes, size_t length, uint16_t from, uint16_t to)
{
    /* StorageID occupies the first four bytes; ObjectFormat is the next two, little-endian. */
    if (!bytes || length < 6) return -1;
    uint16_t current = (uint16_t)(bytes[4] | ((uint16_t)bytes[5] << 8));
    if (current != from) return 0;
    bytes[4] = (unsigned char)(to & 0xFFu);
    bytes[5] = (unsigned char)((to >> 8) & 0xFFu);
    return 1;
}
