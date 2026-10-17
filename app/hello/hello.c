#include <stdio.h>
#include <sys/utsname.h>
int main(void){ struct utsname u; uname(&u); printf("hello from ap80 app: %s %s\n", u.sysname, u.machine); return 0; }
