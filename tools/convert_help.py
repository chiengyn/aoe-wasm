#!/usr/bin/env python3
"""Convert a WinHelp (.HLP) file into static HTML pages for the browser build.

The game opens its help with WinHelp: HELP_KEY (a keyword, from Help buttons) and HELP_FINDER/CONTENTS.
This reads the file's internal file system and writes, into the output folder:
  index.html            contents (the file's contents topic, then every topic by title)
  t<N>.html             one page per topic, with its text, fonts, jumps/popups as links, and pictures
  bm<N>.bmp             the help file's pictures (SHG/MRB device-independent bitmaps) as BMP files
  help.json             {"title", "contents", "keywords": {keyword: "t<N>.html"}, "topics": [...]}

Format references: Manfred Winterhoff's "Windows Help file format" (helpfile.txt) and helpdeco.
Supported: WinHelp 3.1/4.0, LZ77 topic and picture compression, phrase and Hall (PhrIndex/PhrImage) text
compression, text/table records, jumps and popups by context hash, keywords, titles, DIB/DDB pictures.
Picture hotspots become image maps. Macros map to page actions where the browser has one (Back, Search,
Print, JumpId/PopupId, returning to the game); others (AoEHlp.dll sound/video calls) are left inert.
Not converted: metafile pictures.
"""
from pathlib import Path
import argparse, html, json, re, struct


# ---------------------------------------------------------------- low-level helpers
def lz77(data, limit=None):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        flags = data[i]; i += 1
        for bit in range(8):
            if i >= n or (limit and len(out) >= limit):
                break
            if flags & (1 << bit):
                if i + 1 >= n:
                    i = n; break
                w = data[i] | data[i + 1] << 8; i += 2
                length, dist = (w >> 12) + 3, (w & 0xfff) + 1
                for _ in range(length):
                    out.append(out[-dist] if dist <= len(out) else 0)
            else:
                out.append(data[i]); i += 1
    return bytes(out)


def rle(data):
    out = bytearray()
    i = 0
    while i < len(data):
        n = data[i]; i += 1
        if n & 0x80:
            out += data[i:i + (n & 0x7f)]; i += n & 0x7f
        else:
            out += bytes([data[i]]) * n if i < len(data) else b''; i += 1
    return bytes(out)


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos
    def u8(self):
        v = self.d[self.p]; self.p += 1; return v
    def s16(self):
        v = struct.unpack_from('<h', self.d, self.p)[0]; self.p += 2; return v
    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]; self.p += 2; return v
    def s32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]; self.p += 4; return v
    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v
    def cword(self):   # compressed unsigned short
        if self.d[self.p] & 1: return self.u16() >> 1
        return self.u8() >> 1
    def cint(self):    # compressed signed short
        if self.d[self.p] & 1: return (self.u16() >> 1) - 0x4000
        return (self.u8() >> 1) - 0x40
    def clong(self):   # compressed unsigned long
        if self.d[self.p] & 1: return self.u32() >> 1
        return self.u16() >> 1
    def cslong(self):  # compressed signed long (biased)
        if self.d[self.p] & 1: return (self.u32() >> 1) - 0x40000000
        return (self.u16() >> 1) - 0x4000
    def sz(self):
        end = self.d.index(b'\0', self.p); v = self.d[self.p:end]; self.p = end + 1; return v


# ---------------------------------------------------------------- the help file
class HelpFile:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        magic, dirstart = struct.unpack_from('<Ii', self.d, 0)
        if magic != 0x35f3f:
            raise SystemExit('Not a WinHelp file: ' + str(path))
        self.files = dict(self.btree(dirstart, lambda r: (r.sz().decode('latin1'), r.s32())))

    def internal(self, name):
        off = self.files.get(name)
        if off is None:
            return None
        reserved, used, flags = struct.unpack_from('<IIB', self.d, off)
        return self.d[off + 9:off + 9 + used]

    def btree(self, offset, parse):
        """Leaf entries of a B+ tree stored as an internal file at `offset` (or named)."""
        d = self.d
        h = offset + 9
        magic, flags, pagesize = struct.unpack_from('<HHH', d, h)
        root, _, totalpages, nlevels, total = struct.unpack_from('<hhhhi', d, h + 26)
        pages = h + 38
        page = root
        for _ in range(nlevels - 1):
            page = struct.unpack_from('<h', d, pages + page * pagesize + 4)[0]
        out = []
        while page != -1:
            p = pages + page * pagesize
            _, count, prev, nxt = struct.unpack_from('<hhhh', d, p)
            r = Reader(d, p + 8)
            for _ in range(count):
                out.append(parse(r))
            page = nxt
        return out

    def tree(self, name, parse):
        return self.btree(self.files[name], parse) if name in self.files else []

    # ---- |SYSTEM
    def read_system(self):
        r = Reader(self.internal('|SYSTEM'))
        magic, self.minor, self.major = r.u16(), r.u16(), r.u16()
        r.u32()
        flags = r.u16()
        self.title, self.contents_offset = '', None
        if self.minor <= 16:
            self.compressed, self.block = False, 2048
            self.title = r.sz().decode('cp1252', 'replace')
            return
        self.compressed = flags in (4, 8)
        self.block = 2048 if flags == 8 else 4096
        while r.p + 4 <= len(r.d):
            kind, size = r.u16(), r.u16()
            data = r.d[r.p:r.p + size]; r.p += size
            if kind == 1:
                self.title = data.split(b'\0')[0].decode('cp1252', 'replace')
            elif kind == 3 and size >= 4:
                self.contents_offset = struct.unpack_from('<i', data)[0]

    # ---- phrases
    def read_phrases(self):
        self.phrases = []
        index, image = self.internal('|PhrIndex'), self.internal('|PhrImage')
        if index and image:   # Hall compression (WinHelp 4)
            r = Reader(index)
            r.u32(); count = r.u32(); r.u32(); image_size = r.u32(); image_comp = r.u32(); r.u32()
            bits = r.u16() & 0xf; r.u16()
            stream, bitpos = index[r.p:], 0
            def bit():
                nonlocal bitpos
                word = struct.unpack_from('<I', stream, (bitpos >> 5) * 4)[0] if (bitpos >> 5) * 4 + 4 <= len(stream) else 0
                b = (word >> (bitpos & 31)) & 1; bitpos += 1; return b
            if image_size != image_comp:
                image = lz77(image, image_size)
            offsets = [0]
            for _ in range(count):
                n = 1
                while bit():
                    n += 1 << bits
                if bit(): n += 1
                if bits > 1 and bit(): n += 2
                if bits > 2 and bit(): n += 4
                if bits > 3 and bit(): n += 8
                if bits > 4 and bit(): n += 16
                offsets.append(offsets[-1] + n)
            self.phrases = [image[offsets[i]:offsets[i + 1]] for i in range(count)]
            self.hall = True
            return
        self.hall = False
        data = self.internal('|Phrases')
        if not data:
            return
        r = Reader(data)
        count = r.u16(); r.u16()
        if self.minor > 16:
            size = r.u32()
        offsets = [r.u16() for _ in range(count + 1)]
        body = data[r.p:]
        base = offsets[0]
        if self.minor > 16:
            body = lz77(body, size)
        self.phrases = [body[offsets[i] - base:offsets[i + 1] - base] for i in range(count)]

    def expand(self, data, size):
        """Decompress phrase/Hall compressed LinkData2."""
        out = bytearray()
        i = 0
        if self.hall:
            while i < len(data) and len(out) < size:
                ch = data[i]; i += 1
                if ch & 1 == 0:
                    out += self.phrases[ch >> 1]
                elif ch & 3 == 1:
                    out += self.phrases[128 + (ch >> 2) * 256 + data[i]]; i += 1
                elif ch & 7 == 3:
                    n = (ch >> 3) + 1; out += data[i:i + n]; i += n
                elif ch & 15 == 7:
                    out += b' ' * ((ch >> 4) + 1)
                else:
                    out += b'\0' * ((ch >> 4) + 1)
        else:
            while i < len(data) and len(out) < size:
                ch = data[i]; i += 1
                if 0 < ch < 16 and i < len(data):
                    n = (ch - 1) * 256 + data[i]; i += 1
                    out += self.phrases[n >> 1] + (b' ' if n & 1 else b'')
                else:
                    out.append(ch)
        return bytes(out[:size])

    # ---- |TOPIC: the decompressed stream of topic links
    def read_topics(self):
        raw = self.internal('|TOPIC')
        decompress = 0x4000 if self.compressed else self.block - 12
        self.stream = bytearray()
        self.block_base = []
        for b in range(0, len(raw), self.block):
            chunk = raw[b + 12:b + self.block]
            self.block_base.append(len(self.stream))
            self.stream += lz77(chunk, decompress) if self.compressed else chunk
        self.decompress = decompress

    def topicpos(self, pos):
        block, offset = divmod(pos, self.decompress) if self.compressed else divmod(pos, self.block)
        if block >= len(self.block_base):
            return None
        return self.block_base[block] + offset - 12

    def links(self):
        """Yields (record_type, linkdata1, linkdata2) in file order."""
        first = struct.unpack_from('<i', self.internal('|TOPIC'), 4)[0]
        pos = self.topicpos(first)
        s = self.stream
        seen = set()
        while pos is not None and 0 <= pos < len(s) - 21 and pos not in seen:
            seen.add(pos)
            size, len2, prev, nxt, len1 = struct.unpack_from('<iiiii', s, pos)
            kind = s[pos + 20]
            if size <= 0:
                break
            d1 = bytes(s[pos + 21:pos + len1])
            d2 = bytes(s[pos + len1:pos + size])
            if len2 > size - len1:
                d2 = self.expand(d2, len2)
            yield kind, d1, d2
            if self.minor <= 16:
                pos += size
            else:
                pos = self.topicpos(nxt) if nxt > 0 else None

    # ---- |FONT
    def read_fonts(self):
        self.fonts = []
        data = self.internal('|FONT')
        if not data:
            return
        r = Reader(data)
        nnames, ndesc, names_off, desc_off = r.u16(), r.u16(), r.u16(), r.u16()
        name_len = (desc_off - names_off) // nnames if nnames else 32
        names = [data[names_off + i * name_len:names_off + (i + 1) * name_len].split(b'\0')[0].decode('latin1') for i in range(nnames)]
        for i in range(ndesc):
            p = desc_off + i * 11
            if p + 11 > len(data):
                break
            attr, half, family, name = data[p], data[p + 1], data[p + 2], struct.unpack_from('<H', data, p + 3)[0]
            color = data[p + 5:p + 8]
            self.fonts.append({'bold': bool(attr & 1), 'italic': bool(attr & 2), 'underline': bool(attr & 4),
                               'size': half / 2 if half else 10, 'face': names[name] if name < len(names) else '',
                               'color': '#%02x%02x%02x' % tuple(color)})

    # ---- pictures
    def picture(self, name):
        data = self.internal(name)
        if not data or len(data) < 8:
            return None
        r = Reader(data)
        magic, count = r.u16(), r.u16()
        if magic not in (0x506c, 0x706c) or count < 1:
            return None
        offsets = [r.u32() for _ in range(count)]
        # Prefer the first device-independent picture.
        for off in offsets:
            r = Reader(data, off)
            kind, packing = r.u8(), r.u8()
            if kind not in (5, 6):
                continue
            xdpi, ydpi = r.clong(), r.clong()
            planes, bitcount = r.cword(), r.cword()
            width, height, used, important = r.clong(), r.clong(), r.clong(), r.clong()
            comp_size, hot_size = r.clong(), r.clong()
            comp_off, hot_off = r.u32(), r.u32()
            colors = used or (1 << bitcount if bitcount <= 8 else 0)
            if kind == 6:
                palette = data[r.p:r.p + colors * 4]
            else:
                palette = b'\0\0\0\0\xff\xff\xff\0' if bitcount == 1 else b''
                colors = len(palette) // 4
            bits = data[off + comp_off:off + comp_off + comp_size]
            if packing == 1: bits = rle(bits)
            elif packing == 2: bits = lz77(bits)
            elif packing == 3: bits = rle(lz77(bits))
            if kind == 5:   # DDB rows are padded to 2 bytes and top-down: make a DIB
                src_stride = ((width * bitcount + 15) // 16) * 2
                dst_stride = ((width * bitcount + 31) // 32) * 4
                rows = [bits[y * src_stride:(y + 1) * src_stride].ljust(dst_stride, b'\0')[:dst_stride] for y in range(height)]
                bits = b''.join(reversed(rows))
            header = struct.pack('<IiiHHIIiiII', 40, width, height, 1, bitcount, 0, len(bits), 3780, 3780, colors, 0)
            body = header + palette + bits
            bmp = b'BM' + struct.pack('<IHHI', 14 + len(body), 0, 0, 14 + 40 + len(palette)) + body
            return bmp, self.hotspots(data, off + hot_off, hot_size) if hot_size else []
        return None

    def hotspots(self, data, offset, size):
        """[(kind, x, y, w, h, hash, target)] of a picture: kind 0xE2/E6 popup, 0xE3/E7 jump, 0xC8/CC macro."""
        r = Reader(data, offset)
        r.u8(); count = r.u16(); macro_size = r.u32()
        base = r.p
        spots = [(data[base + i * 15], *struct.unpack_from('<HHHHi', data, base + i * 15 + 3)) for i in range(count)]
        r.p = base + count * 15 + macro_size
        out = []
        for spot in spots:
            r.sz()   # hotspot name
            out.append((*spot, r.sz().decode('cp1252', 'replace')))
        return out


# ---------------------------------------------------------------- conversion
def context_hash(name):
    """WinHelp context-name hash (as stored in |CONTEXT and in jump commands)."""
    h = 0
    for ch in name:
        c = ch.upper()
        if 'A' <= c <= 'Z': v = ord(c) - ord('A') + 17
        elif '1' <= c <= '9': v = ord(c) - ord('0')
        elif c == '0': v = 10
        elif c == '.': v = 12
        elif c == '_': v = 13
        else: v = 0
        h = (h * 43 + v) & 0xffffffff
    return h - (1 << 32) if h >= 1 << 31 else h


def macro_href(macro, hash_to_topic, extras=None):
    """A link target for a WinHelp macro string, or None when the browser has no equivalent."""
    m = macro.strip()
    if re.search(r'ActivateTribeGame|CloseWindow|Exit\(', m, re.I):
        return "javascript:parent.postMessage('aoe-help-close','*')"
    if re.match(r'(Back|BK)\(\)', m, re.I): return 'javascript:history.back()'
    if re.match(r'(Search|Finder|Contents|JumpContents)\(', m, re.I): return 'index.html'
    if re.match(r'Print\(\)', m, re.I): return 'javascript:print()'
    # AoEHlp.dll PlayBMP(hwnd, "file.bmp", "title"): a picture from the game's help/ folder, when present.
    b = re.match(r'PlayBMP\([^,]*,\s*"([^"]+)"', m, re.I)
    if b and extras and b.group(1).lower() in extras:
        return extras[b.group(1).lower()]
    j = re.match(r'(?:JumpId|JI|PopupId|PI)\(\s*[`"\']?[^,]*?[\'"]?\s*,\s*[`"\']?([^\'"`)]+)', m, re.I)
    if j and context_hash(j.group(1).strip()) in hash_to_topic:
        return f't{hash_to_topic[context_hash(j.group(1).strip())]}.html'
    return None


def convert(hlp_path, out_dir, extras_url='../native/help/'):
    hf = HelpFile(hlp_path)
    hf.read_system(); hf.read_phrases(); hf.read_topics(); hf.read_fonts()
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    # Files of the game's help/ folder (pictures for PlayBMP), linked where the prepared game folder serves them.
    help_dir = Path(hlp_path).parent / 'help'
    extras = {f.name.lower(): extras_url + f.name for f in help_dir.iterdir() if f.is_file()} if help_dir.is_dir() else {}
    for old in out.glob('*'):
        old.unlink()

    # Topic titles by TOPICOFFSET (one per topic, in order) and context hashes -> TOPICOFFSET.
    titles = hf.tree('|TTLBTREE', lambda r: (r.s32(), r.sz().decode('cp1252', 'replace')))
    titles.sort()
    contexts = dict(hf.tree('|CONTEXT', lambda r: (r.s32(), r.s32())))
    kwdata = hf.internal('|KWDATA') or b''
    keywords = hf.tree('|KWBTREE', lambda r: (r.sz().decode('cp1252', 'replace'), r.s16(), r.s32()))

    # Walk the topic links: group display records under their topic header.
    topics = []
    for kind, d1, d2 in hf.links():
        if kind == 2:
            title = d2.split(b'\0')[0].decode('cp1252', 'replace') if d2 else ''
            topics.append({'title': title, 'records': []})
        elif kind in (0x20, 0x23, 0x01) and topics:
            topics[-1]['records'].append((kind, d1, d2))
    if not topics:
        raise SystemExit('No topics found')
    # TOPICOFFSET of topic i = i-th title entry (both in file order); map offsets to topic numbers.
    offsets = [t[0] for t in titles][:len(topics)]
    for i, t in enumerate(topics):
        if not t['title'] and i < len(titles):
            t['title'] = titles[i][1]
    def topic_for_offset(off):
        best = 0
        for i, o in enumerate(offsets):
            if o <= off: best = i
            else: break
        return best
    hash_to_topic = {h: topic_for_offset(o) for h, o in contexts.items()}

    pictures = {}
    def picture_url(num):
        """(url, image-map HTML or '') for picture |bm<num>."""
        if num not in pictures:
            result = hf.picture(f'|bm{num}')
            pictures[num] = (None, '')
            if result:
                data, spots = result
                (out / f'bm{num}.bmp').write_bytes(data)
                areas = []
                for kind, x, y, w, h, value, target in spots:
                    if kind in (0xc8, 0xcc):
                        href = macro_href(target, hash_to_topic, extras)
                    else:
                        topic = hash_to_topic.get(value)
                        href = f't{topic}.html' if topic is not None else None
                    if href:
                        areas.append(f'<area shape="rect" coords="{x},{y},{x + w},{y + h}" href="{html.escape(href)}" alt="">')
                pictures[num] = (f'bm{num}.bmp', f'<map name="m{num}">{"".join(areas)}</map>' if areas else '')
        return pictures[num]

    for number, topic in enumerate(topics):
        body = render_topic(hf, topic, hash_to_topic, picture_url)
        title = html.escape(topic['title'] or hf.title or f'Topic {number}')
        (out / f't{number}.html').write_text(page(title, hf.title, body), encoding='utf-8')

    keyword_map = {}
    for word, count, offset in keywords:
        if count > 0 and offset + 4 <= len(kwdata):
            target = struct.unpack_from('<i', kwdata, offset)[0]
            keyword_map.setdefault(word.lower(), f't{topic_for_offset(target)}.html')

    contents = topic_for_offset(hf.contents_offset) if hf.contents_offset is not None else 0
    listing = ''.join(f'<li><a href="t{i}.html">{html.escape(t["title"] or f"Topic {i}")}</a></li>'
                      for i, t in enumerate(topics) if t['title'])
    index_body = (f'<p class="lead"><a href="t{contents}.html">{html.escape(topics[contents]["title"] or "Contents")}</a></p>'
                  f'<h2>All topics</h2><ul class="topics">{listing}</ul>')
    (out / 'index.html').write_text(page('Contents', hf.title, index_body, is_index=True), encoding='utf-8')
    (out / 'help.json').write_text(json.dumps({'title': hf.title, 'contents': f't{contents}.html', 'keywords': keyword_map,
                                               'topics': [t['title'] for t in topics]}, indent=1) + '\n')
    return {'title': hf.title, 'topics': len(topics), 'keywords': len(keyword_map),
            'pictures': sum(1 for v in pictures.values() if v[0]), 'contexts': len(contexts)}


def render_topic(hf, topic, hash_to_topic, picture_url):
    parts = []
    for kind, d1, d2 in topic['records']:
        try:
            parts.append(render_record(hf, kind, d1, d2, hash_to_topic, picture_url))
        except (IndexError, struct.error, ValueError):
            text = re.sub(rb'\0+', b' ', d2).decode('cp1252', 'replace').strip()
            if text:
                parts.append(f'<p>{html.escape(text)}</p>')
    return '\n'.join(parts)


def render_record(hf, kind, d1, d2, hash_to_topic, picture_url):
    r = Reader(d1)
    strings = d2.split(b'\0')
    si = 0
    def text():
        nonlocal si
        s = strings[si] if si < len(strings) else b''
        si += 1
        return html.escape(s.decode('cp1252', 'replace'))
    r.clong()
    if kind in (0x20, 0x23):
        r.cword()
    table = kind == 0x23
    if table:
        cols, ttype = r.u8(), r.u8()
        if ttype in (0, 2):
            r.s16()
        for _ in range(cols):
            r.s16(); r.s16()
    out = ['<table class="help-table"><tr>' if table else '']
    cells = 0
    while r.p < len(d1):
        if table:
            column = r.s16()
            if column == -1:
                break
            r.s16(); r.u8()
            out.append('<td>')
            cells += 1
        r.u8(); r.u8(); r.s16()
        attrs = r.u16()
        if attrs & 1: r.clong()
        for bit in (2, 4, 8, 0x10, 0x20, 0x40):
            if attrs & bit: r.cint()
        if attrs & 0x100: r.u8(); r.s16()
        if attrs & 0x200:
            for _ in range(r.cint()):
                if r.cword() & 0x4000: r.cword()
        align = ' style="text-align:right"' if attrs & 0x400 else ' style="text-align:center"' if attrs & 0x800 else ''
        para, spans, link_open = [f'<p{align}>'], 0, False
        while True:
            para.append(text())
            if r.p >= len(d1):
                break
            c = r.u8()
            if c == 0xff:
                break
            if c == 0x20: r.p += 4
            elif c == 0x21: r.p += 2
            elif c == 0x80:
                font = r.u16()
                para.append('</span>' * spans); spans = 0
                f = hf.fonts[font] if font < len(hf.fonts) else None
                if f:
                    style = []
                    if f['bold']: style.append('font-weight:bold')
                    if f['italic']: style.append('font-style:italic')
                    if f['underline'] and not link_open: style.append('text-decoration:underline')
                    if f['size'] >= 12: style.append(f'font-size:{f["size"] / 10:.2f}em')
                    if f['color'] not in ('#000000',): style.append(f'color:{f["color"]}')
                    para.append(f'<span style="{";".join(style)}">'); spans = 1
            elif c == 0x81: para.append('<br>')
            elif c == 0x82:
                para.append('</span>' * spans + '</p>'); spans = 0
                out.append(''.join(para)); para = [f'<p{align}>']
            elif c == 0x83: para.append(' &emsp; ')
            elif c in (0x86, 0x87, 0x88):
                ptype = r.u8(); size = r.cslong()
                if ptype == 0x22: r.cword()
                start = r.p
                if ptype in (0x03, 0x22):
                    r.s16(); num = r.s16()
                    url, image_map = picture_url(num)
                    cls = {0x86: 'inline', 0x87: 'left', 0x88: 'right'}[c]
                    if url:
                        use = f' usemap="#m{num}"' if image_map else ''
                        para.append(f'<img class="{cls}" src="{url}"{use} alt="">{image_map}')
                r.p = start + size
            elif c == 0x89:
                if link_open: para.append('</a>'); link_open = False
            elif c in (0x8b,): para.append('&nbsp;')
            elif c in (0x8c,): para.append('&#8209;')
            elif c in (0xc8, 0xcc):
                n = r.s16(); macro = r.d[r.p:r.p + n].split(b'\0')[0].decode('cp1252', 'replace'); r.p += n
                href = macro_href(macro, hash_to_topic, extras)
                para.append(f'<a href="{html.escape(href)}">' if href else '<a class="macro">'); link_open = True
            elif c in (0xe0, 0xe1):
                r.s32(); para.append('<a class="missing">'); link_open = True
            elif c in (0xe2, 0xe3, 0xe6, 0xe7):
                h = r.s32()
                target = hash_to_topic.get(h)
                popup = c in (0xe2, 0xe6)
                cls = 'popup' if popup else 'jump'
                para.append(f'<a class="{cls}" href="t{target}.html">' if target is not None else '<a class="missing">')
                link_open = True
            elif c in (0xea, 0xeb, 0xee, 0xef):
                n = r.s16(); start = r.p; r.p = start + n
                para.append('<a class="missing">'); link_open = True
            # Unknown commands carry no operands in the files we read.
        if link_open: para.append('</a>')
        para.append('</span>' * spans)
        if len(para) > 1:
            out.append(''.join(para) + '</p>')
        if table:
            out.append('</td>')
        else:
            break
    if table:
        out.append('</tr></table>')
    return ''.join(out)


def page(title, help_title, body, is_index=False):
    nav = '' if is_index else '<a href="index.html">Contents</a> · <a href="javascript:history.back()">Back</a>'
    return f'''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title><style>
:root{{--bg:#fff;--fg:#000;--link:#008000;--muted:#555;color-scheme:light}}
body{{margin:0;padding:12px 16px;background:var(--bg);color:var(--fg);font:15px/1.45 Arial,Helvetica,sans-serif}}
nav{{font-size:13px;color:var(--muted);margin-bottom:8px}}a{{color:var(--link)}}a.popup{{text-decoration:underline dotted}}
a.missing,a.macro{{color:inherit;text-decoration:none}}img{{image-rendering:auto;vertical-align:top}}img.left{{float:left;margin:0 10px 6px 0}}
img.right{{float:right;margin:0 0 6px 10px}}p{{margin:0 0 .5em}}table.help-table{{border-collapse:collapse;margin:4px 0}}
table.help-table td{{vertical-align:top;padding:2px 8px 2px 0}}ul.topics{{columns:2 280px}}h1{{font-size:18px;margin:0 0 8px}}
</style></head><body><nav>{html.escape(help_title)}{" · " + nav if nav else ""}</nav><h1>{title}</h1>
{body}
</body></html>
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('hlp', type=Path)
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    print(json.dumps(convert(args.hlp, args.out)))


if __name__ == '__main__':
    main()
