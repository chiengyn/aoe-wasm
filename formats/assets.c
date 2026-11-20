/* AoE/RoR asset decoding, compiled directly to wasm32. No x86/Windows runtime.
 * Format evidence: dcostap/aoe1-decomp @ 6d6d8b1, Res_file.h, Shape.h,
 * Asm_draw.asm; SFTtech/openage format documentation. See README.md.
 * The RGBA shadow representation is a preview approximation, not the original
 * palette-index destination transform. Unsupported SLP opcodes fail explicitly.
 */
#include <stdint.h>
#include <stddef.h>

#define API __attribute__((visibility("default")))
enum { BAD_HEADER=-1, BOUNDS=-2, BAD_INDEX=-3, BAD_DIMENSIONS=-4,
       OUTPUT_SMALL=-5, UNSUPPORTED=-6, BAD_ROW=-7, BAD_PALETTE=-8 };
static int span(uint32_t n, uint32_t at, uint32_t len) { return at<=n && len<=n-at; }
static int memory_ok(const void *p, uint32_t len) {
#ifdef __wasm__
    return (uint64_t)(uintptr_t)p + len <= (uint64_t)__builtin_wasm_memory_size(0)*65536;
#else
    return p != NULL;
#endif
}
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8; }

API int drs_count(const uint8_t *p, uint32_t len) {
    if (!memory_ok(p,len) || len<64) return BOUNDS;
    if (u32(p+40)!=0x30302e31u) return BAD_HEADER; /* 1.00 */
    uint32_t nt=u32(p+56), count=0;
    if (nt>(len-64)/12) return BOUNDS;
    for (uint32_t t=0;t<nt;t++) {
        const uint8_t *tab=p+64+t*12;
        uint32_t at=u32(tab+4), n=u32(tab+8);
        if (at>len || n>(len-at)/12 || n>0x7fffffffu-count) return BOUNDS;
        for (uint32_t i=0;i<n;i++) {
            const uint8_t *e=p+at+12*i;
            if (!span(len,u32(e+4),u32(e+8))) return BOUNDS;
        }
        count+=n;
    }
    return (int)count;
}
/* out: resource type (little endian integer), resource id, offset, size. */
API int drs_entry(const uint8_t *p,uint32_t len,uint32_t index,uint32_t *out) {
    int count=drs_count(p,len);
    if (count<0) return count;
    if (!memory_ok(out,16)) return BOUNDS;
    if (index>=(uint32_t)count) return BAD_INDEX;
    for (uint32_t t=0;t<u32(p+56);t++) {
        const uint8_t *tab=p+64+t*12;
        uint32_t n=u32(tab+8);
        if (index<n) {
            const uint8_t *e=p+u32(tab+4)+12*index;
            out[0]=u32(tab);out[1]=u32(e);out[2]=u32(e+4);out[3]=u32(e+8);
            return 0;
        }
        index-=n;
    }
    return BAD_INDEX;
}
/* out: width, height, signed hotspot x/y, total frames, properties. */
API int slp_info(const uint8_t *p,uint32_t len,uint32_t frame,int32_t *out) {
    if (!memory_ok(p,len) || !memory_ok(out,24) || len<32) return BOUNDS;
    if (u32(p)!=0x4e302e32u) return BAD_HEADER; /* 2.0N only */
    uint32_t frames=u32(p+4);
    if (frames>(len-32)/32) return BOUNDS;
    if (frame>=frames) return BAD_INDEX;
    const uint8_t *f=p+32+frame*32;
    uint32_t w=u32(f+16),h=u32(f+20);
    if (!w || !h || w>4096 || h>4096 || w*h>4194304) return BAD_DIMENSIONS;
    if (!span(len,u32(f),h*4) || !span(len,u32(f+4),h*4)) return BOUNDS;
    out[0]=(int32_t)w;out[1]=(int32_t)h;
    out[2]=(int32_t)u32(f+24);out[3]=(int32_t)u32(f+28);
    out[4]=(int32_t)frames;out[5]=(int32_t)u32(f+12);
    return 0;
}
static int number(const uint8_t *p,uint32_t len,uint32_t *pos) {
    while (*pos<len && (p[*pos]==' ' || p[*pos]=='\r' || p[*pos]=='\n' || p[*pos]=='\t')) ++*pos;
    uint32_t start=*pos, val=0;
    while (*pos<len && p[*pos]>='0' && p[*pos]<='9') {
        val=val*10+(p[(*pos)++]-'0');
        if (val>65535) return -1;
    }
    if (*pos==start || (*pos<len && p[*pos]!=' ' && p[*pos]!='\r' && p[*pos]!='\n' && p[*pos]!='\t')) return -1;
    return (int)val;
}
API int palette_decode(const uint8_t *p,uint32_t len,uint8_t *out,uint32_t outlen) {
    if (!memory_ok(p,len) || !memory_ok(out,outlen) || len<8) return BOUNDS;
    if (u32(p)!=0x4353414a || u32(p+4)!=0x4c41502d) return BAD_PALETTE;
    if (outlen<1024) return OUTPUT_SMALL;
    uint32_t pos=8;
    if (number(p,len,&pos)!=100 || number(p,len,&pos)!=256) return BAD_PALETTE;
    for (uint32_t i=0;i<256;i++) {
        for (uint32_t c=0;c<3;c++) {
            int v=number(p,len,&pos);
            if (v<0 || v>255) return BAD_PALETTE;
            out[i*4+c]=(uint8_t)v;
        }
        out[i*4+3]=255;
    }
    return 256;
}
static void pixel(uint8_t *out,uint32_t at,const uint8_t *pal,uint32_t color) {
    for (uint32_t c=0;c<4;c++) out[4*at+c]=pal[4*color+c];
}
/* player is 1..8. The caller owns disjoint input, palette and output spans.
 * Shadow: RGBA black/alpha128 for inspection, pending indexed compositing.
 * Nonzero return codes must discard partially decoded output.
 */
API int slp_decode(const uint8_t *p,uint32_t len,uint32_t frame,
                   const uint8_t *pal,uint32_t player,uint8_t *out,uint32_t outlen) {
    int32_t info[6];
    int r=slp_info(p,len,frame,info);
    if (r<0) return r;
    if (!memory_ok(pal,1024) || !memory_ok(out,outlen)) return BOUNDS;
    if (player<1 || player>8) return BAD_INDEX;
    uint32_t w=(uint32_t)info[0],h=(uint32_t)info[1],bytes=w*h*4;
    if (outlen<bytes) return OUTPUT_SMALL;
    for (uint32_t i=0;i<bytes;i++) out[i]=0;
    const uint8_t *f=p+32+frame*32;
    uint32_t commands=u32(f),edges=u32(f+4);
    for (uint32_t y=0;y<h;y++) {
        uint32_t left=u16(p+edges+y*4),right=u16(p+edges+y*4+2);
        if (left==0x8000 || right==0x8000) continue;
        if (left>w || right>w-left) return BAD_ROW;
        uint32_t x=left, end=w-right, pos=u32(p+commands+y*4);
        for (;;) {
            if (pos>=len) return BOUNDS;
            uint32_t cmd=p[pos++],op=cmd&15,n=0,mode=0;
            /* mode: 0 copy, 1 skip, 2 player-copy, 3 fill, 4 player-fill, 5 shadow */
            if (op==15) {
                if (cmd!=15 || x!=end) return BAD_ROW;
                break;
            }
            if ((op&3)==0) n=cmd>>2;
            else if ((op&3)==1) {n=cmd>>2;mode=1;}
            else if (op==2 || op==3) {
                if (pos>=len) return BOUNDS;
                n=((cmd&240)<<4)|p[pos++];mode=(op==3);
            } else if (op==6 || op==7 || op==10 || op==11) {
                n=cmd>>4;
                if (!n) { if (pos>=len) return BOUNDS; n=p[pos++]; }
                mode=op==6?2:op==7?3:op==10?4:5;
            } else return UNSUPPORTED;
            if (!n || n>end-x) return BAD_ROW;
            uint32_t payload=(mode==0 || mode==2)?n:(mode==3 || mode==4)?1:0;
            if (!span(len,pos,payload)) return BOUNDS;
            if (mode!=1) {
                for (uint32_t k=0;k<n;k++) {
                    uint32_t at=y*w+x+k;
                    if (mode==5) {out[4*at+3]=128;continue;}
                    uint32_t color=p[pos+((mode==0 || mode==2)?k:0)];
                    if (mode==2 || mode==4) {
                        /* AoE1 uses ten shades per player, at 16-byte intervals. */
                        if (color>9) return BAD_PALETTE;
                        color+=player*16;
                    }
                    pixel(out,at,pal,color);
                }
            }
            pos+=payload;x+=n;
        }
    }
    return (int)bytes;
}
