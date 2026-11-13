#include "../oct_native_dataset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void string(uint8_t *p,size_t *n,const char *s){size_t length=strlen(s);p[(*n)++]=(uint8_t)(length+1);for(size_t i=0;i<=length;i++){put16(p+*n,(uint8_t)s[i]);*n+=2;}}
/* Rewriting the ObjectInfo format field. On 10 November a host asked for ObjectInfo on all five
 * objects and displayed only those whose format was not 0x3004, so the provider reports text
 * objects as undefined; only the one 16-bit field at offset 4 may change. */
static void test_object_info_format(void)
{
    /* StorageID 0x4f430001, ObjectFormat 0x3004, then protection, size and trailing bytes. */
    unsigned char dataset[] = { 0x01,0x00,0x43,0x4f, 0x04,0x30, 0x00,0x00, 0x38,0x00,0x00,0x00, 0xAA,0xBB };
    unsigned char before[sizeof dataset];
    memcpy(before, dataset, sizeof dataset);
    assert(oct_native_object_info_set_format(dataset, sizeof dataset, 0x3004, 0x3000) == 1);
    assert(dataset[4] == 0x00);
    assert(dataset[5] == 0x30);
    /* Nothing else moved: StorageID, protection, size and the trailing bytes are untouched. */
    assert(memcmp(dataset, before, 4) == 0);
    assert(memcmp(dataset + 6, before + 6, sizeof dataset - 6) == 0);
    /* Applying it again reports no change rather than rewriting. */
    assert(oct_native_object_info_set_format(dataset, sizeof dataset, 0x3004, 0x3000) == 0);
    /* A folder, or anything already undefined, is left alone. */
    unsigned char folder[] = { 0,0,0,0, 0x01,0x30, 0,0 };
    assert(oct_native_object_info_set_format(folder, sizeof folder, 0x3004, 0x3000) == 0);
    assert(folder[4] == 0x01);
    /* Too short to hold the field, or absent. */
    unsigned char tiny[5] = { 0 };
    assert(oct_native_object_info_set_format(tiny, sizeof tiny, 0x3004, 0x3000) == -1);
    assert(oct_native_object_info_set_format(NULL, 16, 0x3004, 0x3000) == -1);
    /* Exactly six bytes is enough. */
    unsigned char exact[6] = { 0,0,0,0, 0x04,0x30 };
    assert(oct_native_object_info_set_format(exact, sizeof exact, 0x3004, 0x3000) == 1);
    assert(exact[4] == 0x00 && exact[5] == 0x30);
}


/* Appending an operation to OperationsSupported, the first array, where the format append puts a
 * value in the fifth. A host will not offer to delete unless DeleteObject is in this array. */
static void test_operation_append(void)
{
    /* standardVersion, vendorExtensionID, extensionVersion, an empty vendor string, functionalMode,
     * then five arrays: operations [0x1001, 0x1002], events, properties, capture, playback [0x3801],
     * then four empty strings. */
    const unsigned char input[] = {
        0x64,0x00, 0x00,0x00,0x00,0x00, 0x64,0x00, 0x00, 0x00,0x00,
        0x02,0x00,0x00,0x00, 0x01,0x10, 0x02,0x10,
        0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00, 0x01,0x38,
        0x00, 0x00, 0x00, 0x00,
    };
    unsigned char output[sizeof input + 8];
    size_t produced = 0;
    assert(oct_native_device_info_add_operation(input, sizeof input, 0x100B, output,
                                                sizeof output, &produced) == 1);
    assert(produced == sizeof input + 2);
    /* The count went from two to three, and the new code sits at the end of that array. */
    assert(output[11] == 0x03 && output[12] == 0 && output[13] == 0 && output[14] == 0);
    assert(output[19] == 0x0B && output[20] == 0x10);
    /* Everything after the operations array shifted by two and is otherwise unchanged. */
    assert(memcmp(output + 21, input + 19, sizeof input - 19) == 0);
    /* The first ten bytes, before the array, are untouched. */
    assert(memcmp(output, input, 11) == 0);

    /* An operation already advertised is not added twice. */
    produced = 12345;
    assert(oct_native_device_info_add_operation(input, sizeof input, 0x1001, output,
                                                sizeof output, &produced) == 0);
    assert(produced == 0);

    /* A buffer two bytes too small is refused rather than truncated. */
    assert(oct_native_device_info_add_operation(input, sizeof input, 0x100B, output,
                                                sizeof input, &produced) == -2);
    /* Malformed and absent inputs. */
    assert(oct_native_device_info_add_operation(NULL, sizeof input, 0x100B, output,
                                                sizeof output, &produced) == -1);
    assert(oct_native_device_info_add_operation(input, 4, 0x100B, output, sizeof output, &produced) == -1);
    assert(oct_native_device_info_add_operation(input, sizeof input, 0x100B, output, sizeof output, NULL) == -1);

    /* And the two appends stay independent: a format goes to the playback array, not this one. */
    assert(oct_native_device_info_add_format(input, sizeof input, 0x3004, output,
                                             sizeof output, &produced) == 1);
    assert(output[11] == 0x02); /* operations count unchanged */
}

/* Appending an event to EventsSupported, the second array. A host acts only on events it was told
 * to expect, so StoreAdded has to be there for an announcement of a storage change to mean
 * anything. The operations array, the first, must not move. */
static void test_event_append(void)
{
    const unsigned char input[] = {
        0x64,0x00, 0x00,0x00,0x00,0x00, 0x64,0x00, 0x00, 0x00,0x00,
        0x01,0x00,0x00,0x00, 0x01,0x10,                 /* operations: GetDeviceInfo */
        0x01,0x00,0x00,0x00, 0x05,0x40,                 /* events: StoreRemoved */
        0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00, 0x01,0x38,
        0x00, 0x00, 0x00, 0x00,
    };
    unsigned char output[sizeof input + 8];
    size_t produced = 0;
    assert(oct_native_device_info_add_event(input, sizeof input, 0x4004, output,
                                            sizeof output, &produced) == 1);
    assert(produced == sizeof input + 2);
    /* The events count went from one to two, with StoreAdded at the end of that array. */
    assert(output[17] == 0x02 && output[18] == 0 && output[19] == 0 && output[20] == 0);
    assert(output[23] == 0x04 && output[24] == 0x40);
    /* The operations array ahead of it is untouched. */
    assert(memcmp(output, input, 17) == 0);
    /* And everything after the events array shifted by two, otherwise unchanged. */
    assert(memcmp(output + 25, input + 23, sizeof input - 23) == 0);
    /* An event already advertised is not added twice. */
    assert(oct_native_device_info_add_event(input, sizeof input, 0x4005, output,
                                            sizeof output, &produced) == 0);
    /* And an operation still goes to the first array, not this one. */
    assert(oct_native_device_info_add_operation(input, sizeof input, 0x100B, output,
                                                sizeof output, &produced) == 1);
    assert(output[11] == 0x02);   /* operations count grew */
    assert(output[19] == 0x01);   /* events count did not */
}

/* The capture-format list, the fourth array, beside the playback list at the fifth. A host offers
 * New Folder and Paste only for formats it finds listed, so Association and Undefined go in both. */
static void test_capture_format_append(void)
{
    const unsigned char input[] = {
        0x64,0x00, 0x00,0x00,0x00,0x00, 0x64,0x00, 0x00, 0x00,0x00,
        0x00,0x00,0x00,0x00,                            /* operations: none */
        0x00,0x00,0x00,0x00,                            /* events: none */
        0x00,0x00,0x00,0x00,                            /* device properties: none */
        0x01,0x00,0x00,0x00, 0x01,0x38,                 /* capture: JPEG */
        0x01,0x00,0x00,0x00, 0x01,0x38,                 /* playback: JPEG */
        0x00, 0x00, 0x00, 0x00,
    };
    unsigned char output[sizeof input + 8];
    size_t produced = 0;
    assert(oct_native_device_info_add_capture_format(input, sizeof input, 0x3001, output,
                                                     sizeof output, &produced) == 1);
    assert(produced == sizeof input + 2);
    assert(output[23] == 0x02);                       /* capture count grew */
    assert(output[29] == 0x01 && output[30] == 0x30);  /* Association appended there */
    assert(output[31] == 0x01);                       /* playback count did not */
    assert(memcmp(output, input, 23) == 0);           /* everything before it untouched */
    /* And the playback append still goes to the fifth array, not this one. */
    assert(oct_native_device_info_add_format(input, sizeof input, 0x3000, output,
                                             sizeof output, &produced) == 1);
    assert(output[23] == 0x01 && output[29] == 0x02);
    /* A format already listed is not added twice. */
    assert(oct_native_device_info_add_capture_format(input, sizeof input, 0x3801, output,
                                                     sizeof output, &produced) == 0);
}

/* One array of a DeviceInfo dataset, found by walking everything before it, as the provider test
 * does, so a value is checked by the array it belongs to rather than by a byte position. */
static int info_has(const unsigned char *b, size_t n, unsigned index, unsigned value)
{
    size_t p = 2 + 4 + 2;
    if (p >= n) return 0;
    p += 1 + (size_t)b[p] * 2;
    p += 2;
    for (unsigned i = 0; ; i++) {
        if (p + 4 > n) return 0;
        size_t c = (size_t)b[p] | (size_t)b[p+1] << 8 | (size_t)b[p+2] << 16 | (size_t)b[p+3] << 24;
        if (i == index) {
            for (size_t k = 0; k < c && p + 4 + 2*k + 1 < n; k++)
                if ((unsigned)(b[p + 4 + 2*k] | b[p + 5 + 2*k] << 8) == value) return 1;
            return 0;
        }
        p += 4 + c * 2;
    }
}

/* The provider's whole append sequence on the provider test's stand-in, for a read-only store and
 * a writable one. The provider test pinned this dataset by length and byte offsets and broke when
 * events began to be advertised for every store, which moved the formats after them; it runs only
 * on macOS. This runs the same functions here and states what each array must hold. */
static void test_provider_sequence(void)
{
    const unsigned char apple[] = {0x64,0,6,0,0,0,0x64,0, 0, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
                                   1,0,0,0, 0x01,0x38, 0,0,0,0};
    unsigned char a[160], b[160];
    size_t n = sizeof apple, out = 0;
    memcpy(a, apple, n);
    assert(oct_native_device_info_add_format(a, n, 0x3004, b, sizeof b, &out) == 1);
    memcpy(a, b, out); n = out;
    const unsigned short events[] = {0x4004, 0x4005, 0x4008, 0x4002};
    for (int i = 0; i < 4; i++) {
        assert(oct_native_device_info_add_event(a, n, events[i], b, sizeof b, &out) == 1);
        memcpy(a, b, out); n = out;
    }
    /* Read-only: Apple's 37 bytes, the text format, four events, and nothing that writes. */
    assert(n == 37 + 2 + 8);
    assert(info_has(a, n, 4, 0x3801) && info_has(a, n, 4, 0x3004));
    for (int i = 0; i < 4; i++) assert(info_has(a, n, 1, events[i]));
    assert(!info_has(a, n, 0, 0x100C) && !info_has(a, n, 0, 0x100D) && !info_has(a, n, 0, 0x100B));
    assert(!info_has(a, n, 4, 0x3000) && !info_has(a, n, 4, 0x3001));
    /* Writable: the three operations, and Undefined and Association in both format lists. */
    const unsigned short ops[] = {0x100B, 0x100C, 0x100D};
    for (int i = 0; i < 3; i++) {
        assert(oct_native_device_info_add_operation(a, n, ops[i], b, sizeof b, &out) == 1);
        memcpy(a, b, out); n = out;
    }
    for (int i = 0; i < 2; i++) {
        unsigned short f = i ? 0x3001 : 0x3000;
        assert(oct_native_device_info_add_format(a, n, f, b, sizeof b, &out) == 1);
        memcpy(a, b, out); n = out;
        assert(oct_native_device_info_add_capture_format(a, n, f, b, sizeof b, &out) == 1);
        memcpy(a, b, out); n = out;
    }
    assert(n == 47 + 6 + 8);
    for (int i = 0; i < 3; i++) assert(info_has(a, n, 0, ops[i]));
    assert(info_has(a, n, 3, 0x3000) && info_has(a, n, 3, 0x3001));
    assert(info_has(a, n, 4, 0x3000) && info_has(a, n, 4, 0x3001));
    /* What was there before is still there, wherever it now sits. */
    assert(info_has(a, n, 4, 0x3801) && info_has(a, n, 4, 0x3004));
    for (int i = 0; i < 4; i++) assert(info_has(a, n, 1, events[i]));
}

int main(void){
    test_provider_sequence();
    test_capture_format_append();
    test_event_append();
    test_operation_append();
    test_object_info_format();
    uint8_t input[256]={0},backup[256],output[258];size_t n=8,out=0;
    put16(input,100);put32(input+2,6);put16(input+6,100);
    string(input,&n,"microsoft.com: 1.0;");put16(input+n,0);n+=2;
    for(unsigned i=0;i<4;i++){put32(input+n,1);n+=4;put16(input+n,(uint16_t)(0x1001+i));n+=2;}
    size_t countOffset=n;put32(input+n,2);n+=4;put16(input+n,0x3001);n+=2;put16(input+n,0x3801);n+=2;
    size_t insertion=n;
    string(input,&n,"Apple");string(input,&n,"iPhone");string(input,&n,"17.0");string(input,&n,"unchanged");
    memcpy(backup,input,sizeof(input));memset(output,0xa5,sizeof(output));
    assert(oct_native_device_info_add_format(input,n,0x3004,output,sizeof(output),&out)==1);
    assert(out==n+2 && output[countOffset]==3 && output[insertion]==4 && output[insertion+1]==0x30);
    assert(!memcmp(output,input,countOffset));assert(!memcmp(output+countOffset+4,input+countOffset+4,insertion-countOffset-4));
    assert(!memcmp(output+insertion+2,input+insertion,n-insertion));assert(!memcmp(input,backup,sizeof(input)));
    assert(output[out]==0xa5); /* output bound */
    uint8_t duplicate[258];memset(duplicate,0xc7,sizeof(duplicate));size_t ignored=99;
    assert(oct_native_device_info_add_format(output,out,0x3004,duplicate,sizeof(duplicate),&ignored)==0 && ignored==0);
    assert(duplicate[0]==0xc7);
    for(size_t cut=0;cut<n;cut++)assert(oct_native_device_info_add_format(input,cut,0x3004,output,sizeof(output),&out)==-1 && out==0);
    assert(oct_native_device_info_add_format(input,n,0x3004,output,n+1,&out)==-2 && out==0);
    assert(oct_native_device_info_add_format(input,n,0x3004,NULL,0,&out)==-2);
    assert(oct_native_device_info_add_format(NULL,n,0x3004,output,sizeof(output),&out)==-1);
    assert(oct_native_device_info_add_format(input,n,0x3004,output,sizeof(output),NULL)==-1);
    put32(input+countOffset,UINT32_MAX);assert(oct_native_device_info_add_format(input,n,0x3004,output,sizeof(output),&out)==-1);
    memcpy(input,backup,sizeof(input));input[n-1]=1;assert(oct_native_device_info_add_format(input,n,0x3004,output,sizeof(output),&out)==-1);
    memcpy(input,backup,sizeof(input));assert(oct_native_device_info_add_format(input,n+1,0x3004,output,sizeof(output),&out)==-1);
    assert(oct_native_device_info_add_format(input,OCT_NATIVE_DEVICE_INFO_MAX+1,0x3004,output,sizeof(output),&out)==-1);
    puts("PASS native DeviceInfo copy: format append, preservation, duplicates, every truncation, overflow, strings, capacity");
    return 0;
}
