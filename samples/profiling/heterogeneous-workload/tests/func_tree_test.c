#include <stdint.h>
#include <stdio.h>
#include "../source/func_workload.h"

static unsigned depth, entries[5], exits[5], errors;
static void *stack[8];
static int identify(void *fn)
{
    if(fn == (void *)sample_func_root)return 0;
    if(fn == (void *)sample_func_left)return 1;
    if(fn == (void *)sample_func_right)return 2;
    if(fn == (void *)sample_func_leaf)return depth>1 && stack[depth-1]==(void *)sample_func_left ? 3 : 4;
    return -1;
}
void __cyg_profile_func_enter(void *fn,void *site)
{
    (void)site; int k=identify(fn);
    if(k<0 || depth>=8){++errors;return;}
    if((k==0 && depth!=0) || ((k==1 || k==2) && (depth!=1 || stack[0]!=(void *)sample_func_root)) ||
       (k>=3 && (depth!=2 || (stack[1]!=(void *)sample_func_left && stack[1]!=(void *)sample_func_right))))++errors;
    ++entries[k];stack[depth++]=fn;
}
void __cyg_profile_func_exit(void *fn,void *site)
{
    (void)site;
    if(!depth || stack[depth-1]!=fn){++errors;return;}
    --depth;int k=identify(fn);
    if(k<0){++errors;return;}++exits[k];
}
int main(void)
{
    uint32_t sum=0;
    for(unsigned i=0;i<64;++i)sum+=sample_func_root(i);
    unsigned expected[5]={64,64,64,128,192};
    /* Independent closed-form result: root(i) = 15*i + 51. */
    if(sum!=33504 || depth)++errors;
    for(unsigned k=0;k<5;++k)if(entries[k]!=expected[k] || exits[k]!=expected[k])++errors;
    printf("func-tree errors=%u sum=%u entries=%u/%u/%u/%u/%u\n",errors,sum,entries[0],entries[1],entries[2],entries[3],entries[4]);
    return errors?1:0;
}
