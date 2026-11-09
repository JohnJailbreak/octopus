#include "../oct_native_dataset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void string(uint8_t *p,size_t *n,const char *s){size_t length=strlen(s);p[(*n)++]=(uint8_t)(length+1);for(size_t i=0;i<=length;i++){put16(p+*n,(uint8_t)s[i]);*n+=2;}}
int main(void){
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
