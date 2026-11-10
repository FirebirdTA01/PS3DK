"""Extract three frozen emitter bodies verbatim; no target compilation."""
import hashlib,re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h'
SOURCE_LF_SHA256='9c522540894a0521ee52aac209ed71ea19bdc6820605d4e43deea6b9b530dcce'

def extract(text,signature):
    assert text.count(signature)==1,signature
    start=text.index(signature);brace=text.index('{',start);depth=0
    # Strings and comments are tokens so their braces do not change depth.
    tokens=re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',text[brace:])
    for token in tokens:
        if token[0]=='{':depth+=1
        elif token[0]=='}':
            depth-=1
            if depth==0:return text[start:brace+token.end()]
    raise ValueError('unterminated '+signature)

def write(dest):
    text=SOURCE.read_text(encoding='utf-8')
    if hashlib.sha256(text.encode('utf-8')).hexdigest()!=SOURCE_LF_SHA256:
        raise ValueError('emitter fixture differs from frozen 60fe5924 source')
    signatures=['static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(DrawVertexArray)',
                'static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(DrawIndexArray)',
                'void RSX_FUNC(ClearSurface)']
    bodies=[extract(text,s) for s in signatures]
    pre='''#include <assert.h>
#include <stdint.h>
#include <rsx/nv40.h>
typedef uint8_t u8; typedef uint32_t u32;
typedef struct { u32 *current, *end; } gcmContextData;
typedef struct { u32 *start; u32 count; } fixture_group;
static fixture_group fixture_groups[64];
static unsigned fixture_group_count;
static void fixture_reserve(gcmContextData *context,u32 n) {
    assert((size_t)(context->end-context->current)>=(size_t)n);
    assert(fixture_group_count<64);
    fixture_groups[fixture_group_count++]=(fixture_group){context->current,n};
}
#define RSX_FUNC_INTERNAL(n) fixture_##n
#define RSX_FUNC(n) fixture_##n
#define GCM_INDEX_TYPE_32B 0
#define RSX_MAX_METHOD_COUNT 0x7ff
#define RSX_CONTEXT_CURRENTP (context->current)
#define RSX_CONTEXT_CURRENT_END(n) context->current += (n)
#define RSX_CONTEXT_CURRENT_BEGIN(n) fixture_reserve(context,(n))
#define RSX_METHOD(m,n) (((uint32_t)(n)<<18)|(m))
#define RSX_METHOD_NI(m,n) (RSX_METHOD(m,n)|0x40000000u)
'''
    dest.write_bytes((pre+'\n\n'.join(bodies)+'\n').encode())
    return {'source':str(SOURCE.relative_to(ROOT)),'sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'lf_sha256':SOURCE_LF_SHA256,
            'bodies':{s:hashlib.sha256(b.encode()).hexdigest() for s,b in zip(signatures,bodies)}}
