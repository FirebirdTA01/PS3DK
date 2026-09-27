#include <stdio.h>
#include "abi.h"

_Static_assert(sizeof(unsigned int)==4, "requires 32-bit int");
_Static_assert(sizeof(unsigned long long)==8, "requires 64-bit long long");
_Static_assert((unsigned int)-15720400==0xff102030u, "signed test constant");

/* Compile this judge at O0. Volatile is additional protection against the
   compiler defect under test; expected values are a literal table. */
static volatile int checks, failures;
static void check(const char *family, const char *op, int row, int got, int want)
{
    ++checks;
    if (got != want) {
        ++failures;
        printf("PPU_COMPARE_CARRY mismatch family=%s op=%s row=%d got=%d expected=%d\n",
               family, op, row, got, want);
    }
}

#define DECLARE(P,T) \
extern int P##_plus_eq(const T*,int); extern int P##_plus_ne(const T*,int); \
extern int P##_minus_eq(const T*,int); extern int P##_minus_ne(const T*,int); \
extern int P##_neg_eq(const T*,int); extern int P##_neg_ne(const T*,int); \
extern int P##_ne(const T*,int);
DECLARE(u32_high,unsigned int)
DECLARE(i32_high,int)
DECLARE(u32_low,unsigned int)
DECLARE(u64_high,unsigned long long)
extern int register_ne(const unsigned int*,const unsigned int*,int);

/* Rows: all equal, all different, two equal/two different, empty input.
   Columns: +eq, +ne, -eq, -ne, neg-eq OR, neg-ne OR, ne OR. */
static const int expected[4][7] = {
    {4,0,-4,0,-1,0,0}, {0,4,0,-4,0,-1,1},
    {2,2,-2,-2,-1,-1,1}, {0,0,0,0,0,0,0}
};
#define RUN(P,T,K) do { \
    const T values[4][4] = {{K,K,K,K},{0,1,2,3},{K,0,K,1},{K,K,K,K}}; \
    for(int row=0;row<4;++row) { \
        int n=row==3?0:4; \
        check(#P,"plus_eq",row,P##_plus_eq(values[row],n),expected[row][0]); \
        check(#P,"plus_ne",row,P##_plus_ne(values[row],n),expected[row][1]); \
        check(#P,"minus_eq",row,P##_minus_eq(values[row],n),expected[row][2]); \
        check(#P,"minus_ne",row,P##_minus_ne(values[row],n),expected[row][3]); \
        check(#P,"neg_eq",row,P##_neg_eq(values[row],n),expected[row][4]); \
        check(#P,"neg_ne",row,P##_neg_ne(values[row],n),expected[row][5]); \
        check(#P,"ne",row,P##_ne(values[row],n),expected[row][6]); \
    } \
} while(0)

int main(void)
{
    printf("PPU_COMPARE_CARRY ABI pointer=%u long=%u int=%u\n",
           (unsigned)sizeof(void*), (unsigned)sizeof(long), (unsigned)sizeof(int));
    RUN(u32_high,unsigned int,0xff102030u);
    RUN(i32_high,int,-15720400);
    RUN(u32_low,unsigned int,0x102030u);
    RUN(u64_high,unsigned long long,0x12345678ff102030ull);
    const unsigned int a[4]={0xff102030u,0,1,0x80000000u};
    const unsigned int b[4]={0xff102030u,1,1,0};
    check("register","ne",0,register_ne(a,a,4),0);
    check("register","ne",1,register_ne(a,b,4),2);
    check("register","ne",2,register_ne(a,b,0),0);
    printf("PPU_COMPARE_CARRY checks=%d failures=%d\n",checks,failures);
    if(checks!=115) return 2;
    puts(failures ? "PPU_COMPARE_CARRY_FAIL" : "PPU_COMPARE_CARRY_OK");
    return failures ? 1 : 0;
}
