/* Read KGSL feature properties; no register writes or kernel modifications. */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "msm_kgsl.h"
int main(void) {
    int fd=open("/dev/kgsl-3d0",O_RDWR);
    if(fd<0) { perror("open KGSL"); return 2; }
    unsigned props[]={KGSL_PROP_IS_RAYTRACING_ENABLED,KGSL_PROP_IS_AQE_ENABLED};
    const char *names[]={"raytracing","aqe"};
    int status=0;
    for(unsigned i=0;i<2;i++) {
        uint32_t value=0;
        struct kgsl_device_getproperty p={.type=props[i],.value=&value,.sizebytes=sizeof(value)};
        int ret=ioctl(fd,IOCTL_KGSL_DEVICE_GETPROPERTY,&p);
        printf("%s property=0x%x value=%u ret=%d\n",names[i],props[i],value,ret);
        if(ret) { perror(names[i]); status=2; }
    }
    close(fd); return status;
}
