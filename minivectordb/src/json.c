/* Minimal JSON reader/writer for the JSON-Lines protocol.
 *
 * The reader accepts the subset the judge sends: one object per line with
 * string, number, boolean and null members. Numbers keep their integer-ness
 * so a request id can be echoed back with the same JSON type.
 */
#include "db.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    char *errbuf;
    size_t errlen;
    int failed;
} JsonParser;

static void json_fail(JsonParser *jp, const char *msg) {
    if (!jp->failed && jp->errbuf && jp->errlen) snprintf(jp->errbuf, jp->errlen, "%s", msg);
    jp->failed = 1;
}

static void json_skip_ws(JsonParser *jp) {
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') jp->p++;
        else break;
    }
}

static MvJson *json_new(MvJsonType t) {
    MvJson *j = mv_xmalloc(sizeof(MvJson));
    memset(j, 0, sizeof(*j));
    j->type = t;
    return j;
}

void mv_json_free(MvJson *j) {
    if (!j) return;
    for (size_t i = 0; i < j->count; i++) {
        if (j->items) mv_json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->sval);
    free(j);
}

static int json_hex4(JsonParser *jp, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (jp->p >= jp->end) return MV_ERR;
        char c = *jp->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return MV_ERR;
    }
    *out = v;
    return MV_OK;
}

static void json_put_utf8(MvBuf *b, unsigned cp) {
    char tmp[4];
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        mv_buf_append(b, tmp, 1);
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        mv_buf_append(b, tmp, 2);
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        mv_buf_append(b, tmp, 3);
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        mv_buf_append(b, tmp, 4);
    }
}

static int json_parse_string(JsonParser *jp, char **out, size_t *outlen) {
    if (jp->p >= jp->end || *jp->p != '"') {
        json_fail(jp, "expected string");
        return MV_ERR;
    }
    jp->p++;
    MvBuf b;
    mv_buf_init(&b);
    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '"') {
            *out = b.data ? b.data : mv_strdup("");
            *outlen = b.len;
            return MV_OK;
        }
        if (c == '\\') {
            if (jp->p >= jp->end) break;
            char e = *jp->p++;
            switch (e) {
                case '"': mv_buf_append(&b, "\"", 1); break;
                case '\\': mv_buf_append(&b, "\\", 1); break;
                case '/': mv_buf_append(&b, "/", 1); break;
                case 'b': mv_buf_append(&b, "\b", 1); break;
                case 'f': mv_buf_append(&b, "\f", 1); break;
                case 'n': mv_buf_append(&b, "\n", 1); break;
                case 'r': mv_buf_append(&b, "\r", 1); break;
                case 't': mv_buf_append(&b, "\t", 1); break;
                case 'u': {
                    unsigned cp = 0;
                    if (json_hex4(jp, &cp) != MV_OK) {
                        mv_buf_free(&b);
                        json_fail(jp, "bad \\u escape");
                        return MV_ERR;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && jp->p + 1 < jp->end && jp->p[0] == '\\' &&
                        jp->p[1] == 'u') {
                        jp->p += 2;
                        unsigned lo = 0;
                        if (json_hex4(jp, &lo) == MV_OK && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    json_put_utf8(&b, cp);
                    break;
                }
                default:
                    mv_buf_free(&b);
                    json_fail(jp, "bad escape");
                    return MV_ERR;
            }
            continue;
        }
        if (c < 0x20) {
            mv_buf_free(&b);
            json_fail(jp, "control character in string");
            return MV_ERR;
        }
        char ch = (char)c;
        mv_buf_append(&b, &ch, 1);
    }
    mv_buf_free(&b);
    json_fail(jp, "unterminated string");
    return MV_ERR;
}

static MvJson *json_parse_value(JsonParser *jp);

static MvJson *json_parse_number(JsonParser *jp) {
    const char *start = jp->p;
    if (jp->p < jp->end && (*jp->p == '-' || *jp->p == '+')) jp->p++;
    int is_double = 0;
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c >= '0' && c <= '9') {
            jp->p++;
        } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            is_double = 1;
            jp->p++;
        } else {
            break;
        }
    }
    if (jp->p == start) {
        json_fail(jp, "bad number");
        return NULL;
    }
    size_t n = (size_t)(jp->p - start);
    char tmp[64];
    if (n >= sizeof(tmp)) {
        json_fail(jp, "number too long");
        return NULL;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    if (!is_double) {
        char *endp = NULL;
        long long v = strtoll(tmp, &endp, 10);
        if (endp && *endp == '\0') {
            MvJson *j = json_new(MV_JSON_INT);
            j->ival = (int64_t)v;
            j->dval = (double)v;
            return j;
        }
    }
    char *endp = NULL;
    double d = strtod(tmp, &endp);
    if (!endp || *endp != '\0' || !isfinite(d)) {
        json_fail(jp, "bad number");
        return NULL;
    }
    MvJson *j = json_new(MV_JSON_DOUBLE);
    j->dval = d;
    j->ival = (int64_t)d;
    return j;
}

static MvJson *json_parse_value(JsonParser *jp) {
    json_skip_ws(jp);
    if (jp->p >= jp->end) {
        json_fail(jp, "unexpected end of input");
        return NULL;
    }
    char c = *jp->p;
    if (c == '{') {
        jp->p++;
        MvJson *obj = json_new(MV_JSON_OBJECT);
        size_t cap = 0;
        json_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == '}') {
            jp->p++;
            return obj;
        }
        for (;;) {
            json_skip_ws(jp);
            char *key = NULL;
            size_t klen = 0;
            if (json_parse_string(jp, &key, &klen) != MV_OK) {
                mv_json_free(obj);
                return NULL;
            }
            json_skip_ws(jp);
            if (jp->p >= jp->end || *jp->p != ':') {
                free(key);
                mv_json_free(obj);
                json_fail(jp, "expected ':'");
                return NULL;
            }
            jp->p++;
            MvJson *val = json_parse_value(jp);
            if (!val) {
                free(key);
                mv_json_free(obj);
                return NULL;
            }
            if (obj->count == cap) {
                cap = cap ? cap * 2 : 8;
                obj->items = mv_xrealloc(obj->items, cap * sizeof(MvJson *));
                obj->keys = mv_xrealloc(obj->keys, cap * sizeof(char *));
            }
            obj->keys[obj->count] = key;
            obj->items[obj->count] = val;
            obj->count++;
            json_skip_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
                continue;
            }
            if (jp->p < jp->end && *jp->p == '}') {
                jp->p++;
                return obj;
            }
            mv_json_free(obj);
            json_fail(jp, "expected ',' or '}'");
            return NULL;
        }
    }
    if (c == '[') {
        jp->p++;
        MvJson *arr = json_new(MV_JSON_ARRAY);
        size_t cap = 0;
        json_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ']') {
            jp->p++;
            return arr;
        }
        for (;;) {
            MvJson *val = json_parse_value(jp);
            if (!val) {
                mv_json_free(arr);
                return NULL;
            }
            if (arr->count == cap) {
                cap = cap ? cap * 2 : 8;
                arr->items = mv_xrealloc(arr->items, cap * sizeof(MvJson *));
            }
            arr->items[arr->count++] = val;
            json_skip_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
                continue;
            }
            if (jp->p < jp->end && *jp->p == ']') {
                jp->p++;
                return arr;
            }
            mv_json_free(arr);
            json_fail(jp, "expected ',' or ']'");
            return NULL;
        }
    }
    if (c == '"') {
        char *s = NULL;
        size_t slen = 0;
        if (json_parse_string(jp, &s, &slen) != MV_OK) return NULL;
        MvJson *j = json_new(MV_JSON_STRING);
        j->sval = s;
        j->slen = slen;
        return j;
    }
    if ((size_t)(jp->end - jp->p) >= 4 && memcmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        MvJson *j = json_new(MV_JSON_BOOL);
        j->bval = 1;
        return j;
    }
    if ((size_t)(jp->end - jp->p) >= 5 && memcmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        MvJson *j = json_new(MV_JSON_BOOL);
        j->bval = 0;
        return j;
    }
    if ((size_t)(jp->end - jp->p) >= 4 && memcmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        return json_new(MV_JSON_NULL);
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return json_parse_number(jp);
    json_fail(jp, "unexpected token");
    return NULL;
}

MvJson *mv_json_parse(const char *text, size_t len, char *errbuf, size_t errlen) {
    JsonParser jp;
    jp.p = text;
    jp.end = text + len;
    jp.errbuf = errbuf;
    jp.errlen = errlen;
    jp.failed = 0;
    MvJson *v = json_parse_value(&jp);
    if (!v) return NULL;
    json_skip_ws(&jp);
    if (jp.p != jp.end) {
        mv_json_free(v);
        if (errbuf && errlen) snprintf(errbuf, errlen, "trailing content");
        return NULL;
    }
    return v;
}

const MvJson *mv_json_get(const MvJson *obj, const char *key) {
    if (!obj || obj->type != MV_JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

void mv_json_escape(MvBuf *out, const char *s, size_t len) {
    mv_buf_append(out, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': mv_buf_append(out, "\\\"", 2); break;
            case '\\': mv_buf_append(out, "\\\\", 2); break;
            case '\n': mv_buf_append(out, "\\n", 2); break;
            case '\r': mv_buf_append(out, "\\r", 2); break;
            case '\t': mv_buf_append(out, "\\t", 2); break;
            case '\b': mv_buf_append(out, "\\b", 2); break;
            case '\f': mv_buf_append(out, "\\f", 2); break;
            default:
                if (c < 0x20) mv_buf_printf(out, "\\u%04x", c);
                else mv_buf_append(out, &s[i], 1);
        }
    }
    mv_buf_append(out, "\"", 1);
}

void mv_json_double(MvBuf *out, double v) {
    if (!isfinite(v)) {
        /* The protocol forbids NaN/Infinity; clamp to a finite fallback. */
        mv_buf_append(out, "0", 1);
        return;
    }
    char tmp[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
        if (strtod(tmp, NULL) == v) break;
    }
    mv_buf_append(out, tmp, strlen(tmp));
}
