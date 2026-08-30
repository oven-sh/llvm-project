// RUN: %clang_asan -Wno-alloc-size -fsanitize-recover=address %s -o %t && %env_asan_opts=halt_on_error=0 %run %t 2>&1 | FileCheck %s --check-prefixes=CHECK%if target={{.*(linux-gnu|freebsd).*}} %{,CHECK-GLIBC%}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__) || defined(__FreeBSD__)
#include <malloc.h>
#endif

int main(int argc, char **argv) {
  {
    char *p1 = (char *)calloc(1, 0);
    printf("p1 is %p\n", p1);
    printf("Content of p1 is: %d\n", *p1);
    // CHECK: ERROR: AddressSanitizer: heap-buffer-overflow
    // CHECK: {{#0 0x.* in main .*zero_alloc.cpp:}}[[@LINE-2]]
    free(p1);
  }

  {
    char *p2 = (char *)calloc(0, 1);
    printf("p2 is %p\n", p2);
    printf("Content of p2 is: %d\n", *p2);
    // CHECK: ERROR: AddressSanitizer: heap-buffer-overflow
    // CHECK: {{#0 0x.* in main .*zero_alloc.cpp:}}[[@LINE-2]]
    free(p2);
  }

  {
    char *p3 = (char *)malloc(0);
    printf("p3 is %p\n", p3);
    printf("Content of p2 is: %d\n", *p3);
    // CHECK: ERROR: AddressSanitizer: heap-buffer-overflow
    // CHECK: {{#0 0x.* in main .*zero_alloc.cpp:}}[[@LINE-2]]
    free(p3);
  }

#if defined(__GLIBC__) || defined(__FreeBSD__)
  {
    // malloc_usable_size must not report the poisoned byte as usable.
    char *p4 = (char *)malloc(0);
    size_t n = malloc_usable_size(p4);
    printf("usable size of p4 is: %zu\n", n);
    // CHECK-GLIBC: usable size of p4 is: 0
    char buf[8];
    memcpy(buf, p4, n);
    free(p4);
    puts("p4 done");
    // CHECK-GLIBC-NEXT: p4 done
  }
#endif

  return 0;
}
