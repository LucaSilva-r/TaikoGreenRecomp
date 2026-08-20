/*
 * ps3recomp - cellHttpUtil HLE implementation
 *
 * URL parsing, percent-encoding, and Base64 codec. Pure C, no
 * external dependencies.
 */

#include "cellHttpUtil.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static const char s_hex[] = "0123456789ABCDEF";

/* Is a character "unreserved" per RFC 3986? */
static int is_unreserved(u8 c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    return (c == '-' || c == '_' || c == '.' || c == '~');
}

/* ---------------------------------------------------------------------------
 * URL parsing
 * -----------------------------------------------------------------------*/

/* cellHttpUtilParseUri(uri, str, pool, poolSize, required)
 *
 * Every argument is a guest effective address, and the guest CellHttpUri is
 * *pointers into the caller's pool*, not inline buffers:
 *
 *     struct CellHttpUri { be32 scheme, hostname, username, password, path;
 *                          be32 port; u8 reserved[4]; };   // 28 bytes
 *
 * The previous version took the arguments as host pointers and memset the
 * guest address directly, which segfaults the moment the title parses the URL
 * ALL.Net hands back. `uri` null means "only tell me how much pool I need".
 */
s32 cellHttpUtilParseUri(CellHttpUtilUri* uri, const char* url,
                          void* pool, u32 poolSize, u32* required)
{
    const uint32_t uri_ea      = (uint32_t)(uintptr_t)uri;
    const uint32_t url_ea      = (uint32_t)(uintptr_t)url;
    const uint32_t pool_ea     = (uint32_t)(uintptr_t)pool;
    const uint32_t required_ea = (uint32_t)(uintptr_t)required;

    if (!url_ea)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    const char* text = (const char*)vm_translate(url_ea);
    const char* p = text;

    /* scheme://[user[:pass]@]host[:port][/path...] */
    const char* scheme = NULL; u32 scheme_len = 0;
    const char* colon = strstr(p, "://");
    if (colon) {
        scheme = p;
        scheme_len = (u32)(colon - p);
        p = colon + 3;
    }

    const char* username = NULL; u32 username_len = 0;
    const char* password = NULL; u32 password_len = 0;
    const char* slash = strchr(p, '/');
    const char* at = strchr(p, '@');
    if (at && (!slash || at < slash)) {
        const char* sep = memchr(p, ':', (size_t)(at - p));
        username = p;
        username_len = (u32)((sep ? sep : at) - p);
        if (sep) {
            password = sep + 1;
            password_len = (u32)(at - sep - 1);
        }
        p = at + 1;
    }

    const char* host_end = p;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;

    const char* port_colon = NULL;
    for (const char* c = p; c < host_end; c++)
        if (*c == ':') port_colon = c;

    const char* hostname = p;
    u32 hostname_len = (u32)((port_colon ? port_colon : host_end) - p);
    u32 port = 0;
    if (port_colon) {
        for (const char* c = port_colon + 1; c < host_end; c++)
            if (*c >= '0' && *c <= '9') port = port * 10 + (u32)(*c - '0');
    } else if (scheme && scheme_len == 5 && !strncmp(scheme, "https", 5)) {
        port = 443;
    } else {
        port = 80;
    }

    /* The path keeps its query and fragment: it is the request target the
     * caller sends on the wire. */
    const char* path = host_end;
    u32 path_len = (u32)strlen(host_end);
    if (path_len == 0) {
        path = "/";
        path_len = 1;
    }

    const u32 needed = (scheme ? scheme_len + 1 : 0) +
                       (username ? username_len + 1 : 0) +
                       (password ? password_len + 1 : 0) +
                       hostname_len + 1 + path_len + 1;
    if (required_ea)
        vm_write32(required_ea, needed);
    if (!uri_ea)
        return CELL_OK;
    if (!pool_ea || poolSize < needed)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_MEMORY;

    uint32_t cursor = pool_ea;
    #define HTTP_URI_STORE(src, len) (                                        \
        memcpy(vm_translate(cursor), (src), (len)),                           \
        *((char*)vm_translate(cursor) + (len)) = '\0',                        \
        cursor += (len) + 1,                                                  \
        cursor - ((len) + 1))

    vm_write32(uri_ea + 0,  scheme   ? HTTP_URI_STORE(scheme, scheme_len)     : 0);
    vm_write32(uri_ea + 4,  HTTP_URI_STORE(hostname, hostname_len));
    vm_write32(uri_ea + 8,  username ? HTTP_URI_STORE(username, username_len) : 0);
    vm_write32(uri_ea + 12, password ? HTTP_URI_STORE(password, password_len) : 0);
    vm_write32(uri_ea + 16, HTTP_URI_STORE(path, path_len));
    vm_write32(uri_ea + 20, port);
    vm_write32(uri_ea + 24, 0);
    #undef HTTP_URI_STORE

    printf("[cellHttpUtil] ParseUri('%s') -> host='%.*s' port=%u path='%.*s'\n",
           text, (int)hostname_len, hostname, port, (int)path_len, path);
    return CELL_OK;
}

s32 cellHttpUtilBuildUri(char* urlBuf, u32 urlBufSize,
                          const CellHttpUtilUri* uri, u32* written)
{
    if (!urlBuf || !uri)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    int n = snprintf(urlBuf, urlBufSize, "%s://%s",
                     uri->scheme[0] ? uri->scheme : "http",
                     uri->hostname);
    if (n < 0) return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    /* Append port if non-default */
    int default_port = (strcmp(uri->scheme, "https") == 0) ? 443 : 80;
    if (uri->port && uri->port != (u32)default_port) {
        n += snprintf(urlBuf + n, urlBufSize - n, ":%u", uri->port);
    }

    /* Path */
    n += snprintf(urlBuf + n, urlBufSize - n, "%s",
                  uri->path[0] ? uri->path : "/");

    /* Query */
    if (uri->query[0])
        n += snprintf(urlBuf + n, urlBufSize - n, "?%s", uri->query);

    /* Fragment */
    if (uri->fragment[0])
        n += snprintf(urlBuf + n, urlBufSize - n, "#%s", uri->fragment);

    if (written)
        *written = (u32)n;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * URL encoding / decoding
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilEscapeUri(char* out, u32 outSize,
                           const u8* src, u32 srcSize, u32* written)
{
    if (!out || !src)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < srcSize; i++) {
        if (is_unreserved(src[i])) {
            if (pos >= outSize)
                return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
            out[pos++] = (char)src[i];
        } else {
            if (pos + 3 > outSize)
                return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
            out[pos++] = '%';
            out[pos++] = s_hex[(src[i] >> 4) & 0xF];
            out[pos++] = s_hex[src[i] & 0xF];
        }
    }

    if (pos < outSize)
        out[pos] = '\0';

    if (written)
        *written = pos;

    return CELL_OK;
}

s32 cellHttpUtilUnescapeUri(u8* out, u32 outSize,
                             const char* src, u32* required)
{
    if (!src)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;
    if (!out && !required)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 srcSize = (u32)strlen(src);
    u32 pos = 0;
    for (u32 i = 0; i < srcSize; i++) {
        if (out && pos >= outSize)
            return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

        u8 decoded;
        if (src[i] == '%' && i + 2 < srcSize) {
            int hi = hex_digit(src[i + 1]);
            int lo = hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded = (u8)((hi << 4) | lo);
                i += 2;
            } else {
                decoded = (u8)src[i];
            }
        } else {
            decoded = (u8)src[i];
        }

        if (out)
            out[pos] = decoded;
        pos++;
    }

    if (required)
        *required = pos;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Form URL encoding
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilFormUrlEncode(char* out, u32 outSize,
                               const char* key, const char* value, u32* written)
{
    if (!out || !key || !value)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 key_esc_len = outSize;
    u32 pos = 0;

    /* Encode key */
    s32 rc = cellHttpUtilEscapeUri(out, outSize, (const u8*)key,
                                    (u32)strlen(key), &key_esc_len);
    if (rc != CELL_OK) return rc;
    pos = key_esc_len;

    /* = */
    if (pos >= outSize)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    out[pos++] = '=';

    /* Encode value */
    u32 val_esc_len = outSize - pos;
    rc = cellHttpUtilEscapeUri(out + pos, outSize - pos, (const u8*)value,
                                (u32)strlen(value), &val_esc_len);
    if (rc != CELL_OK) return rc;
    pos += val_esc_len;

    if (pos < outSize)
        out[pos] = '\0';

    if (written)
        *written = pos;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Base64
 * -----------------------------------------------------------------------*/

static const char s_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

s32 cellHttpUtilBase64Encode(char* out, u32 outSize,
                              const u8* data, u32 dataSize, u32* written)
{
    if (!out || !data)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 needed = ((dataSize + 2) / 3) * 4;
    if (needed >= outSize)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    u32 pos = 0;
    for (u32 i = 0; i < dataSize; i += 3) {
        u32 n = ((u32)data[i]) << 16;
        if (i + 1 < dataSize) n |= ((u32)data[i + 1]) << 8;
        if (i + 2 < dataSize) n |= data[i + 2];

        out[pos++] = s_b64_table[(n >> 18) & 0x3F];
        out[pos++] = s_b64_table[(n >> 12) & 0x3F];
        out[pos++] = (i + 1 < dataSize) ? s_b64_table[(n >> 6) & 0x3F] : '=';
        out[pos++] = (i + 2 < dataSize) ? s_b64_table[n & 0x3F] : '=';
    }

    out[pos] = '\0';
    if (written)
        *written = pos;

    return CELL_OK;
}

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

s32 cellHttpUtilBase64Decode(u8* out, u32 outSize,
                              const char* encoded, u32 encodedLen, u32* written)
{
    if (!out || !encoded)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < encodedLen; i += 4) {
        int a = (i < encodedLen) ? b64_decode_char(encoded[i]) : 0;
        int b = (i + 1 < encodedLen) ? b64_decode_char(encoded[i + 1]) : 0;
        int c = (i + 2 < encodedLen) ? b64_decode_char(encoded[i + 2]) : -1;
        int d = (i + 3 < encodedLen) ? b64_decode_char(encoded[i + 3]) : -1;

        if (a < 0 || b < 0)
            return (s32)CELL_HTTP_UTIL_ERROR_PARSE_FAILED;

        u32 triple = ((u32)a << 18) | ((u32)b << 12);
        if (c >= 0) triple |= ((u32)c << 6);
        if (d >= 0) triple |= (u32)d;

        if (pos >= outSize) return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
        out[pos++] = (u8)((triple >> 16) & 0xFF);

        if (c >= 0 && pos < outSize)
            out[pos++] = (u8)((triple >> 8) & 0xFF);

        if (d >= 0 && pos < outSize)
            out[pos++] = (u8)(triple & 0xFF);
    }

    if (written)
        *written = pos;

    return CELL_OK;
}
