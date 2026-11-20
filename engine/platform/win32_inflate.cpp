// Replacement for Inf32.asm (Inf32BufSize/Inf32Decode): raw DEFLATE (RFC 1951) decoding for rge_fio.cpp.
// rge_read decodes one file at a time in 64 KB output steps; the whole stream is inflated on the first
// call (offset 0) and handed out in the requested chunk sizes.
#include "port_prefix.h"
#include <vector>

namespace {

struct Bits {
    const unsigned char* data;
    size_t size, pos;
    unsigned value, count;
    bool error;
    Bits(const unsigned char* d, size_t n) : data(d), size(n), pos(0), value(0), count(0), error(false) {}
    unsigned get(unsigned n) {
        while (count < n) {
            if (pos >= size) { error = true; return 0; }
            value |= (unsigned) data[pos++] << count;
            count += 8;
        }
        unsigned v = value & ((1u << n) - 1);
        value >>= n;
        count -= n;
        return v;
    }
    void align() { value = 0; count = 0; }
};

struct Huffman {
    unsigned short counts[16], symbols[320];
    bool build(const unsigned char* lengths, int n) {
        unsigned short offsets[16];
        for (int i = 0; i < 16; i++) counts[i] = 0;
        for (int i = 0; i < n; i++) counts[lengths[i]]++;
        counts[0] = 0;
        offsets[1] = 0;
        for (int i = 1; i < 15; i++) offsets[i + 1] = offsets[i] + counts[i];
        for (int i = 0; i < n; i++) if (lengths[i]) symbols[offsets[lengths[i]]++] = (unsigned short) i;
        return true;
    }
    int decode(Bits& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int) in.get(1);
            int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
            if (in.error) return -1;
        }
        return -1;
    }
};

const unsigned short length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const unsigned char length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned short dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const unsigned char dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& in, std::vector<unsigned char>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.decode(in);
        if (sym < 0 || in.error) return false;
        if (sym < 256) { out.push_back((unsigned char) sym); continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        unsigned len = length_base[sym] + in.get(length_extra[sym]);
        int d = dist.decode(in);
        if (d < 0 || d >= 30) return false;
        size_t back = dist_base[d] + in.get(dist_extra[d]);
        if (in.error || back > out.size()) return false;
        size_t from = out.size() - back;
        for (unsigned i = 0; i < len; i++) out.push_back(out[from + i]);
    }
}

bool inflate(const unsigned char* data, size_t size, std::vector<unsigned char>& out, size_t* consumed) {
    Bits in(data, size);
    int last;
    do {
        last = in.get(1);
        int type = in.get(2);
        if (in.error) return false;
        if (type == 0) {
            in.align();
            if (in.pos + 4 > size) return false;
            unsigned len = data[in.pos] | data[in.pos + 1] << 8, nlen = data[in.pos + 2] | data[in.pos + 3] << 8;
            in.pos += 4;
            if ((len ^ 0xffff) != nlen || in.pos + len > size) return false;
            out.insert(out.end(), data + in.pos, data + in.pos + len);
            in.pos += len;
        } else if (type == 1) {
            unsigned char lengths[320];
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            Huffman lit, dist;
            lit.build(lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            dist.build(lengths, 30);
            if (!codes(in, out, lit, dist)) return false;
        } else if (type == 2) {
            int nlen = in.get(5) + 257, ndist = in.get(5) + 1, ncode = in.get(4) + 4;
            static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            unsigned char lengths[320] = {0};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (unsigned char) in.get(3);
            Huffman lencode;
            lencode.build(lengths, 19);
            unsigned char all[320] = {0};
            for (int i = 0; i < nlen + ndist;) {
                int sym = lencode.decode(in);
                if (sym < 0 || in.error) return false;
                if (sym < 16) { all[i++] = (unsigned char) sym; continue; }
                int repeat, value = 0;
                if (sym == 16) { if (!i) return false; value = all[i - 1]; repeat = 3 + in.get(2); }
                else if (sym == 17) repeat = 3 + in.get(3);
                else repeat = 11 + in.get(7);
                if (i + repeat > nlen + ndist) return false;
                while (repeat--) all[i++] = (unsigned char) value;
            }
            Huffman lit, dist;
            lit.build(all, nlen);
            dist.build(all + nlen, ndist);
            if (!codes(in, out, lit, dist)) return false;
        } else return false;
    } while (!last);
    *consumed = in.pos;
    return true;
}

std::vector<unsigned char> decoded;
size_t cursor;

}

extern "C" {

int Inf32BufSize(void) { return 16; }

// compressed+offset: input; *in_size: input size (in) / bytes consumed (out);
// buffer: output; *out_size: capacity (in) / bytes produced (out). Returns 0 on success.
int Inf32Decode(void* compressed, int offset, void* in_size, void* buffer, int, void* out_size, void*, int) {
    int* in_bytes = (int*) in_size;
    int* out_bytes = (int*) out_size;
    if (offset == 0) {
        decoded.clear();
        cursor = 0;
        size_t consumed = 0;
        if (!inflate((const unsigned char*) compressed, (size_t) *in_bytes, decoded, &consumed)) {
            printf("[aoe] Inf32Decode: corrupt DEFLATE stream (%u bytes decoded)\n", (unsigned) decoded.size());
        }
        *in_bytes = (int) consumed;
    } else *in_bytes = 0;
    size_t n = decoded.size() - cursor;
    if (n > (size_t) *out_bytes) n = (size_t) *out_bytes;
    if (n) memcpy(buffer, &decoded[cursor], n);
    cursor += n;
    *out_bytes = (int) n;
    return 0;
}

}
