#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_native_io.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool valid_name(const char *name) { return name && (!strcmp(name,"lease") || !strcmp(name,"status")); }
static int open_directory(const char *path, bool create)
{
    if (!path) { errno=EINVAL; return -1; }
    if (create && mkdir(path,0700)<0 && errno!=EEXIST) return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    struct stat st;
    if (fstat(fd,&st)<0 || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        close(fd); errno=EPERM; return -1;
    }
    return fd;
}
oct_status oct_native_io_read(const char *directory, const char *name, void *bytes, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!valid_name(name) || !bytes || !length || capacity > 65536) {
        errno = EINVAL;
        return OCT_E_INVALID_ARGUMENT;
    }
    int dir = open_directory(directory, false);
    if (dir < 0) return OCT_E_IO;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno;
    close(dir);
    errno = saved;
    if (fd < 0) return OCT_E_IO;
    struct stat st;
    oct_status result = OCT_E_IO;
    if (fstat(fd, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1) {
            errno = EPERM;
        } else if (st.st_size < 0 || (uint64_t)st.st_size > capacity) {
            errno = EOVERFLOW;
        } else {
            size_t pos = 0;
            result = OCT_OK;
            while (pos < (size_t)st.st_size) {
                ssize_t n = read(fd, (char *)bytes + pos, (size_t)st.st_size - pos);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { if (!n) errno = EIO; result = OCT_E_IO; break; }
                pos += (size_t)n;
            }
            /* Concurrent growth is rejected; writers normally publish by rename. */
            if (result == OCT_OK) {
                char extra;
                ssize_t n;
                do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
                if (n != 0) { if (n > 0) errno = EOVERFLOW; result = OCT_E_IO; }
            }
            if (result == OCT_OK) *length = pos;
        }
    }
    saved = errno;
    close(fd);
    errno = saved;
    return result;
}
oct_status oct_native_io_write(const char *directory,const char *name,const void *bytes,size_t length)
{
    if(!valid_name(name)||(!bytes&&length)||length>65536){errno=EINVAL;return OCT_E_INVALID_ARGUMENT;}
    int dir=open_directory(directory,true);if(dir<0)return OCT_E_IO;
    char temporary[64];snprintf(temporary,sizeof temporary,".%s.%ld.tmp",name,(long)getpid());
    int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){int saved=errno;close(dir);errno=saved;return OCT_E_IO;}
    size_t pos=0;oct_status result=OCT_OK;
    while(pos<length){ssize_t n=write(fd,(const char*)bytes+pos,length-pos);if(n<0&&errno==EINTR)continue;if(n<=0){if(!n)errno=EIO;result=OCT_E_IO;break;}pos+=(size_t)n;}
    if(result==OCT_OK&&fsync(fd)<0)result=OCT_E_IO;
    if(close(fd)<0)result=OCT_E_IO;
    if(result==OCT_OK&&renameat(dir,temporary,dir,name)<0)result=OCT_E_IO;
    int saved=errno;if(result!=OCT_OK)unlinkat(dir,temporary,0);close(dir);errno=saved;return result;
}
oct_status oct_native_io_remove_lease(const char *directory)
{
    int dir=open_directory(directory,false);if(dir<0)return errno==ENOENT?OCT_OK:OCT_E_IO;
    int rc=unlinkat(dir,"lease",0);int saved=errno;close(dir);errno=saved;
    return rc==0||errno==ENOENT?OCT_OK:OCT_E_IO;
}
static bool valid_token(const char *token)
{
    if(!token||strlen(token)!=32)return false;
    for(size_t i=0;i<32;i++)if(!((token[i]>='0'&&token[i]<='9')||(token[i]>='a'&&token[i]<='f')))return false;
    return true;
}
oct_status oct_native_write_lease(const char *directory,const char *token,unsigned mode,uint64_t now_ms)
{
    if(!valid_token(token)||mode<1||mode>2||now_ms>UINT64_MAX-OCT_NATIVE_LEASE_MS)return OCT_E_INVALID_ARGUMENT;
    char bytes[96];int n=snprintf(bytes,sizeof bytes,"OCTN1 %" PRIu64 " %u %s\n",now_ms+OCT_NATIVE_LEASE_MS,mode,token);
    if(n<0||(size_t)n>=sizeof bytes)return OCT_E_IO;
    return oct_native_io_write(directory,"lease",bytes,(size_t)n);
}
/* Exactly canonical decimal, with explicit overflow checks. scanf's unsigned
 * conversions accept signs and have undefined behavior on unrepresentable input. */
static bool decimal(const char **cursor, const char *end, uint64_t *value)
{
    const char *p = *cursor;
    uint64_t result = 0;
    if (p == end || *p < '0' || *p > '9') return false;
    if (*p == '0' && p + 1 < end && p[1] >= '0' && p[1] <= '9') return false;
    while (p < end && *p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
        p++;
    }
    *cursor = p;
    *value = result;
    return true;
}

unsigned oct_native_parse_lease(const void *bytes, size_t length, uint64_t now_ms, uint64_t *deadline, char token[33])
{
    if (deadline) *deadline = 0;
    if (token) token[0] = 0;
    if (!deadline || !token || !bytes || length > 96) { errno = EINVAL; return 0; }
    char parsed[33];
    uint64_t expires = 0, mode = 0;
    const char *p = bytes, *end = p + length;
    if (length < 6 || memcmp(p, "OCTN1 ", 6)) { errno = EINVAL; return 0; }
    p += 6;
    if (!decimal(&p, end, &expires) || p == end || *p++ != ' ' ||
        !decimal(&p, end, &mode) || p == end || *p++ != ' ' ||
        end - p != 33 || p[32] != '\n') { errno = EINVAL; return 0; }
    memcpy(parsed, p, 32);
    parsed[32] = 0;
    if (!valid_token(parsed) || mode < 1 || mode > 2 || expires <= now_ms ||
        expires - now_ms > OCT_NATIVE_LEASE_MS) { errno = EINVAL; return 0; }
    *deadline = expires;
    memcpy(token, parsed, 33);
    return (unsigned)mode;
}

unsigned oct_native_read_lease(const char *directory, uint64_t now_ms, uint64_t *deadline, char token[33])
{
    if (deadline) *deadline = 0;
    if (token) token[0] = 0;
    if (!deadline || !token) { errno = EINVAL; return 0; }
    char bytes[96];
    size_t length = 0;
    if (oct_native_io_read(directory, "lease", bytes, sizeof bytes, &length) != OCT_OK) return 0;
    return oct_native_parse_lease(bytes, length, now_ms, deadline, token);
}
