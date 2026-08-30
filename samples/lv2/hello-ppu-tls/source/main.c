#include <stdio.h>

__thread int tls_counter = 41;
static __thread struct {
    int a;
    int b[4];
} tls_agg = {1, {2, 3, 4, 5}};

int main(void)
{
    tls_counter++;
    printf("TLS_TEST: counter=%d agg.a=%d agg.b3=%d\n",
           tls_counter, tls_agg.a, tls_agg.b[3]);
    printf((tls_counter == 42 && tls_agg.a == 1 && tls_agg.b[3] == 5)
               ? "TLS_OK\n"
               : "TLS_BAD\n");
    return 0;
}
