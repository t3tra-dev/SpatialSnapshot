#include "bmff.h"

/* HEIF Exif offsets are relative to the byte following the four-byte offset field.
 * Only IFD0 describes the primary raster; IFD1 can describe an oriented thumbnail. */
ss_status_t bm_exif_orientation(bm_context *c, const uint8_t *p, size_t n, uint32_t id) {
    if (n < 4 || bm_u32(p) > n - 4)
        return bm_host(c, 0, "invalid HEIF Exif TIFF offset");
    size_t offset = (size_t)bm_u32(p) + 4;
    p += offset;
    n -= offset;
    if (n < 8)
        return bm_host(c, 0, "truncated TIFF header");
    bool le = p[0] == 'I' && p[1] == 'I', be = p[0] == 'M' && p[1] == 'M';
    if ((!le && !be) || (le ? ssi_u16(p + 2) : bm_u16(p + 2)) != 42)
        return bm_host(c, 0, "invalid TIFF byte order or magic");
    uint32_t ifd = le ? ssi_u32(p + 4) : bm_u32(p + 4);
    if (!ifd)
        return SS_OK;
    if (ifd > n || n - ifd < 2)
        return bm_host(c, 0, "TIFF IFD0 offset outside item");
    uint32_t count = le ? ssi_u16(p + ifd) : bm_u16(p + ifd);
    if ((uint64_t)count * 12 + 6 > n - ifd)
        return bm_host(c, 0, "truncated TIFF IFD0");
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = p + ifd + 2 + i * 12;
        uint16_t tag = le ? ssi_u16(e) : bm_u16(e);
        if (tag != 0x0112)
            continue;
        uint16_t type = le ? ssi_u16(e + 2) : bm_u16(e + 2);
        uint32_t values = le ? ssi_u32(e + 4) : bm_u32(e + 4);
        if (type != 3 || values != 1)
            return bm_host(c, 0, "invalid Exif Orientation type/count");
        uint16_t orientation = le ? ssi_u16(e + 8) : bm_u16(e + 8);
        bm_check(c, orientation == 1, 0, id, "primary Exif Orientation is not 1");
    }
    return SS_OK;
}

typedef struct xml_slice {
    const uint8_t *p;
    size_t n;
} xml_slice;
typedef struct xml_ns {
    xml_slice prefix, uri;
    size_t depth;
} xml_ns;
typedef struct xml_attr {
    xml_slice name, value;
} xml_attr;
typedef struct xml_element {
    xml_slice name;
    size_t ns_start;
    bool orientation, value, has_value;
    char scalar[64];
    size_t length;
} xml_element;
static bool xml_space(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}
static bool xml_equal(xml_slice s, const char *v) {
    return s.n == strlen(v) && !memcmp(s.p, v, s.n);
}
static bool xml_match(const uint8_t *p, size_t n, const char *s) {
    size_t len = strlen(s);
    return n >= len && !memcmp(p, s, len);
}
static bool xml_name(const uint8_t **p, const uint8_t *end, xml_slice *name) {
    const uint8_t *start = *p;
    while (*p < end) {
        uint8_t ch = **p;
        if (!(ch >= 128 || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == ':' || ch == '_' || ch == '-' || ch == '.'))
            break;
        ++*p;
    }
    *name = (xml_slice){start, (size_t)(*p - start)};
    return name->n != 0;
}
/* Decode XML predefined/numeric entities without a DTD, external I/O, or entity expansion. */
static bool xml_ascii(xml_slice s, char *out, size_t capacity, size_t *written) {
    size_t n = 0;
    for (size_t i = 0; i < s.n; ++i) {
        uint32_t ch = s.p[i];
        if (ch == '&') {
            size_t j = i + 1;
            while (j < s.n && s.p[j] != ';' && j - i <= 16)
                ++j;
            if (j == s.n || s.p[j] != ';')
                return false;
            xml_slice entity = {s.p + i + 1, j - i - 1};
            if (xml_equal(entity, "amp"))
                ch = '&';
            else if (xml_equal(entity, "lt"))
                ch = '<';
            else if (xml_equal(entity, "gt"))
                ch = '>';
            else if (xml_equal(entity, "quot"))
                ch = '"';
            else if (xml_equal(entity, "apos"))
                ch = '\'';
            else if (entity.n >= 2 && entity.p[0] == '#') {
                unsigned base = entity.p[1] == 'x' ? 16 : 10;
                size_t k = base == 16 ? 2 : 1;
                ch = 0;
                if (k == entity.n)
                    return false;
                for (; k < entity.n; ++k) {
                    uint8_t v = entity.p[k];
                    unsigned d;
                    if (v >= '0' && v <= '9')
                        d = (unsigned)(v - '0');
                    else if (v >= 'a' && v <= 'f')
                        d = (unsigned)(v - 'a') + 10;
                    else if (v >= 'A' && v <= 'F')
                        d = (unsigned)(v - 'A') + 10;
                    else
                        return false;
                    if (d >= base || ch > (127u - d) / base)
                        return false;
                    ch = ch * base + d;
                }
            } else
                return false;
            i = j;
        }
        if (!ch || ch > 127 || n + 1 >= capacity)
            return false;
        out[n++] = (char)ch;
    }
    out[n] = 0;
    *written = n;
    return true;
}
static bool xml_uri_equal(xml_slice s, const char *uri) {
    char value[128];
    size_t n;
    return xml_ascii(s, value, sizeof(value), &n) && n == strlen(uri) && !memcmp(value, uri, n);
}
static bool xml_resolves(xml_slice name, bool attribute, const xml_ns *ns, size_t count,
                         const char *uri, const char *local) {
    size_t colon = 0;
    while (colon < name.n && name.p[colon] != ':')
        ++colon;
    xml_slice prefix = {name.p, colon == name.n ? 0 : colon};
    xml_slice part = colon == name.n ? name : (xml_slice){name.p + colon + 1, name.n - colon - 1};
    if (!xml_equal(part, local) || (attribute && !prefix.n))
        return false;
    while (count) {
        const xml_ns *entry = &ns[--count];
        if (entry->prefix.n == prefix.n && !memcmp(entry->prefix.p, prefix.p, prefix.n))
            return xml_uri_equal(entry->uri, uri);
    }
    return false;
}
static void xml_orientation(bm_context *c, xml_slice value, uint32_t id) {
    char decoded[64];
    size_t n;
    bool valid = xml_ascii(value, decoded, sizeof(decoded), &n);
    uint32_t integer = 0;
    size_t i = 0;
    if (valid) {
        while (i < n && xml_space((uint8_t)decoded[i]))
            ++i;
        if (i < n && decoded[i] == '+')
            ++i;
        size_t begin = i;
        while (i < n && decoded[i] >= '0' && decoded[i] <= '9') {
            if (integer > 100) {
                valid = false;
                break;
            }
            integer = integer * 10 + (uint32_t)(decoded[i++] - '0');
        }
        valid &= i != begin;
        while (i < n && xml_space((uint8_t)decoded[i]))
            ++i;
        valid &= i == n && integer == 1;
    }
    bm_check(c, valid, 0, id, "primary XMP TIFF Orientation is not integer 1");
}
static ss_status_t xml_parse(bm_context *c, const uint8_t *p, size_t n, uint32_t id,
                             xml_ns **namespace_storage, xml_attr **attribute_storage) {
    static const char *tiff = "http://ns.adobe.com/tiff/1.0/",
                      *rdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
    const uint8_t *end = p + n;
    xml_element elements[64];
    size_t depth = 0, ns_count = 0, ns_capacity = 0, attr_capacity = 0;
    bool root_seen = false;
    if (n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
        p += 3;
    while (p < end) {
        if (*p != '<') {
            const uint8_t *start = p;
            while (p < end && *p != '<')
                ++p;
            if (depth && (elements[depth - 1].orientation || elements[depth - 1].value)) {
                xml_element *el = &elements[depth - 1];
                size_t size = (size_t)(p - start);
                if (size >= sizeof(el->scalar) - el->length)
                    return bm_limit(c, 0, "XMP orientation scalar size limit");
                memcpy(el->scalar + el->length, start, size);
                el->length += size;
            } else if (!depth)
                for (const uint8_t *s = start; s < p; ++s)
                    if (!xml_space(*s))
                        return bm_host(c, 0, "text outside XMP root element");
            continue;
        }
        if (xml_match(p, (size_t)(end - p), "<!--") || xml_match(p, (size_t)(end - p), "<?") ||
            xml_match(p, (size_t)(end - p), "<![CDATA[")) {
            bool cd = p[1] == '!' && p[2] == '[';
            const char *close = cd ? "]]>" : p[1] == '?' ? "?>" : "-->";
            size_t skip = cd ? 9 : p[1] == '?' ? 2 : 4;
            p += skip;
            const uint8_t *text = p;
            while (p < end && !xml_match(p, (size_t)(end - p), close))
                ++p;
            if (p == end)
                return bm_host(c, 0, "unterminated XMP comment/PI/CDATA");
            if (cd && depth && (elements[depth - 1].orientation || elements[depth - 1].value)) {
                xml_element *el = &elements[depth - 1];
                size_t size = (size_t)(p - text);
                bm_check(c, memchr(text, '&', size) == NULL, 0, id,
                         "XMP CDATA Orientation is not an integer");
                if (size >= sizeof(el->scalar) - el->length)
                    return bm_limit(c, 0, "XMP orientation scalar size limit");
                memcpy(el->scalar + el->length, text, size);
                el->length += size;
            }
            p += strlen(close);
            continue;
        }
        if (xml_match(p, (size_t)(end - p), "<!"))
            return bm_error(c, SS_DOMAIN_UNSUPPORTED_BINDING, SS_UNSUPPORTED, 0, id,
                            "XMP DTD/entity declarations are unsupported");
        ++p;
        bool closing = p < end && *p == '/';
        if (closing)
            ++p;
        xml_slice name;
        if (!xml_name(&p, end, &name))
            return bm_host(c, 0, "invalid XMP element name");
        if (closing) {
            while (p < end && xml_space(*p))
                ++p;
            if (p == end || *p++ != '>' || !depth || elements[depth - 1].name.n != name.n ||
                memcmp(elements[depth - 1].name.p, name.p, name.n))
                return bm_host(c, 0, "mismatched XMP closing tag");
            xml_element *el = &elements[--depth];
            if ((el->orientation && !el->has_value) || el->value) {
                if (el->value && depth && elements[depth - 1].orientation) {
                    /* RDF qualified property: rdf:value holds the scalar. */
                    elements[depth - 1].has_value = true;
                }
                xml_orientation(c, (xml_slice){(const uint8_t *)el->scalar, el->length}, id);
            }
            ns_count = el->ns_start;
            continue;
        }
        if (depth == 64)
            return bm_limit(c, 0, "XMP nesting depth exceeds 64");
        if (!depth && root_seen)
            return bm_host(c, 0, "multiple XMP roots");
        if (!depth)
            root_seen = true;
        xml_element *el = &elements[depth];
        memset(el, 0, sizeof(*el));
        el->name = name;
        el->ns_start = ns_count;
        size_t attrs = 0;
        bool empty = false;
        for (;;) {
            while (p < end && xml_space(*p))
                ++p;
            if (p == end)
                return bm_host(c, 0, "truncated XMP start tag");
            if (*p == '>') {
                ++p;
                break;
            }
            if (*p == '/' && end - p >= 2 && p[1] == '>') {
                p += 2;
                empty = true;
                break;
            }
            xml_attr a;
            if (!xml_name(&p, end, &a.name))
                return bm_host(c, 0, "invalid XMP attribute");
            while (p < end && xml_space(*p))
                ++p;
            if (p == end || *p++ != '=')
                return bm_host(c, 0, "XMP attribute lacks equals");
            while (p < end && xml_space(*p))
                ++p;
            if (p == end || (*p != '\'' && *p != '"'))
                return bm_host(c, 0, "XMP attribute lacks quote");
            uint8_t quote = *p++;
            const uint8_t *start = p;
            while (p < end && *p != quote && *p != '<')
                ++p;
            if (p == end || *p != quote)
                return bm_host(c, 0, "unterminated XMP attribute");
            a.value = (xml_slice){start, (size_t)(p++ - start)};
            if (attrs >= 4096)
                return bm_limit(c, 0, "XMP attributes per element limit");
            BM_TRY(ssi_grow(c->memory, (void **)attribute_storage, &attr_capacity, attrs + 1,
                            sizeof(xml_attr)));
            for (size_t i = 0; i < attrs; ++i)
                if ((*attribute_storage)[i].name.n == a.name.n &&
                    !memcmp((*attribute_storage)[i].name.p, a.name.p, a.name.n))
                    return bm_host(c, 0, "duplicate XMP attribute");
            (*attribute_storage)[attrs++] = a;
            if (xml_equal(a.name, "xmlns") || xml_match(a.name.p, a.name.n, "xmlns:")) {
                BM_TRY(ssi_grow(c->memory, (void **)namespace_storage, &ns_capacity, ns_count + 1,
                                sizeof(xml_ns)));
                xml_slice prefix = a.name.n == 5 ? (xml_slice){a.name.p, 0}
                                                 : (xml_slice){a.name.p + 6, a.name.n - 6};
                (*namespace_storage)[ns_count++] = (xml_ns){prefix, a.value, depth};
            }
        }
        el->orientation =
            xml_resolves(name, false, *namespace_storage, ns_count, tiff, "Orientation");
        el->value = depth && elements[depth - 1].orientation &&
                    xml_resolves(name, false, *namespace_storage, ns_count, rdf, "value");
        for (size_t i = 0; i < attrs; ++i) {
            xml_attr a = (*attribute_storage)[i];
            if (xml_resolves(a.name, true, *namespace_storage, ns_count, tiff, "Orientation"))
                xml_orientation(c, a.value, id);
        }
        if (empty) {
            if (el->orientation || el->value)
                xml_orientation(c, (xml_slice){(const uint8_t *)"", 0}, id);
            ns_count = el->ns_start;
        } else
            ++depth;
    }
    if (depth || !root_seen)
        return bm_host(c, 0, "incomplete XMP document");
    return SS_OK;
}
ss_status_t bm_xmp_orientation(bm_context *c, const uint8_t *p, size_t n, uint32_t id) {
    xml_ns *ns = NULL;
    xml_attr *attrs = NULL;
    uint8_t *converted = NULL;
    /* XML's BOM/initial-byte encoding detection. No locale-dependent conversion. */
    unsigned unit = 1;
    size_t bom = 0;
    bool le = false;
    if (n >= 4 && !memcmp(p, "\xff\xfe\0\0", 4)) {
        unit = 4;
        bom = 4;
        le = true;
    } else if (n >= 4 && !memcmp(p, "\0\0\xfe\xff", 4)) {
        unit = 4;
        bom = 4;
    } else if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) {
        unit = 2;
        bom = 2;
        le = true;
    } else if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) {
        unit = 2;
        bom = 2;
    } else if (n >= 4 && p[0] == '<' && !p[1] && !p[2] && !p[3]) {
        unit = 4;
        le = true;
    } else if (n >= 4 && !p[0] && !p[1] && !p[2] && p[3] == '<')
        unit = 4;
    else if (n >= 4 && p[0] == '<' && !p[1]) {
        unit = 2;
        le = true;
    } else if (n >= 4 && !p[0] && p[1] == '<')
        unit = 2;
    if (unit != 1) {
        if (n % unit)
            return bm_host(c, 0, "incomplete UTF-16/32 XMP code unit");
        size_t capacity;
        if (!ssi_multiply(n, 2, &capacity))
            return bm_limit(c, 0, "UTF-16 conversion size overflow");
        converted = ssi_alloc(c->memory, capacity);
        if (!converted)
            return c->memory->failure;
        size_t out = 0;
        for (size_t i = bom; i < n; i += unit) {
            uint32_t ch = unit == 2 ? (le ? ssi_u16(p + i) : bm_u16(p + i))
                                    : (le ? ssi_u32(p + i) : bm_u32(p + i));
            if (unit == 2 && ch >= 0xd800 && ch <= 0xdbff && n - i >= 4) {
                uint32_t low = le ? ssi_u16(p + i + 2) : bm_u16(p + i + 2);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    ch = 0x10000 + ((ch - 0xd800) << 10) + low - 0xdc00;
                    i += 2;
                }
            }
            if (!ch || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) {
                ssi_free(converted);
                return bm_host(c, 0, "invalid Unicode code point in XMP");
            }
            if (ch < 128)
                converted[out++] = (uint8_t)ch;
            else if (ch < 2048) {
                converted[out++] = (uint8_t)(0xc0 | (ch >> 6));
                converted[out++] = (uint8_t)(0x80 | (ch & 63));
            } else if (ch < 65536) {
                converted[out++] = (uint8_t)(0xe0 | (ch >> 12));
                converted[out++] = (uint8_t)(0x80 | ((ch >> 6) & 63));
                converted[out++] = (uint8_t)(0x80 | (ch & 63));
            } else {
                converted[out++] = (uint8_t)(0xf0 | (ch >> 18));
                converted[out++] = (uint8_t)(0x80 | ((ch >> 12) & 63));
                converted[out++] = (uint8_t)(0x80 | ((ch >> 6) & 63));
                converted[out++] = (uint8_t)(0x80 | (ch & 63));
            }
        }
        p = converted;
        n = out;
    }
    ss_status_t s = xml_parse(c, p, n, id, &ns, &attrs);
    ssi_free(ns);
    ssi_free(attrs);
    ssi_free(converted);
    return s;
}
