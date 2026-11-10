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


int main(void){
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
