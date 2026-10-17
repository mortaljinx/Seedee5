/* Seedee5 - what a game's card says about it, from Wikipedia: the parsing (gameinfo.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gameinfo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ JSON */

/* where "key" starts a member: the quote is at the start, or after {, [, a comma or white space */
static const char *find_key(const char *json, size_t len, const char *from, const char *key)
{
    const size_t kl = strlen(key);
    const char *end = json + len;
    for (const char *p = from; p + kl + 2 < end; p++) {
        if (*p != '"' || memcmp(p + 1, key, kl) != 0 || p[kl + 1] != '"')
            continue;
        if (p > json) {
            const char b = p[-1];
            if (b != '{' && b != ',' && b != '[' && !isspace((unsigned char)b))
                continue;
        }
        const char *q = p + kl + 2;
        while (q < end && isspace((unsigned char)*q))
            q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && isspace((unsigned char)*q))
                q++;
            return q; /* the value */
        }
    }
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *p, const char *end)
{
    if (end - p < 4)
        return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        const int h = hexval(p[i]);
        if (h < 0)
            return -1;
        v = v * 16 + h;
    }
    return v;
}

/* a code point as UTF-8, if it fits; returns the bytes written (0: no room) */
static size_t put_utf8(char *out, size_t room, unsigned cp)
{
    char b[4];
    size_t k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); k = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); k = 4;
    }
    if (k > room)
        return 0;
    memcpy(out, b, k);
    return k;
}

/* the JSON string starting at v (its opening quote), decoded; returns the length, or -1. *next: after it */
static int decode_string(const char *v, const char *end, char *out, size_t n, const char **next)
{
    if (n == 0)
        return -1;
    if (v >= end || *v != '"') {
        out[0] = 0;
        return -1;
    }
    size_t o = 0;
    int full = 0; /* out is full: keep reading to the closing quote, write nothing more */
    const char *p = v + 1;
    while (p < end && *p != '"') {
        unsigned cp;
        if (*p == '\\') {
            if (p + 1 >= end)
                break;
            const char e = p[1];
            p += 2;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u': {
                int h = hex4(p, end);
                if (h < 0) { cp = '?'; break; }
                p += 4;
                cp = (unsigned)h;
                if (cp >= 0xD800 && cp < 0xDC00) { /* a surrogate pair */
                    int l = (end - p >= 6 && p[0] == '\\' && p[1] == 'u') ? hex4(p + 2, end) : -1;
                    if (l >= 0xDC00 && l < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)l - 0xDC00);
                        p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                break;
            }
            default: cp = (unsigned char)e; break; /* \" \\ \/ */
            }
            if (cp == 0)
                cp = ' ';
            if (!full) {
                const size_t k = put_utf8(out + o, n - 1 - o, cp);
                if (k == 0) full = 1;
                o += k;
            }
        } else {
            /* raw bytes (UTF-8 as sent): a multi-byte character is copied whole or not at all */
            const unsigned char c = (unsigned char)*p;
            size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (p + k > end)
                k = (size_t)(end - p);
            if (!full) {
                if (o + k <= n - 1) {
                    memcpy(out + o, p, k);
                    o += k;
                } else {
                    full = 1;
                }
            }
            p += k;
        }
    }
    out[o] = 0;
    if (next)
        *next = p < end ? p + 1 : end;
    return p < end ? (int)o : -1;
}

int gi_json_string(const char *json, size_t len, const char *key, char *out, size_t n)
{
    if (n)
        out[0] = 0;
    if (!json || !n)
        return -1;
    for (const char *from = json;;) {
        const char *v = find_key(json, len, from, key);
        if (!v)
            return -1;
        if (*v == '"')
            return decode_string(v, json + len, out, n, NULL);
        from = v; /* the key with a value that is not a string: look further */
    }
}

int gi_json_true(const char *json, size_t len, const char *key)
{
    const char *v = json ? find_key(json, len, json, key) : NULL;
    return v && (size_t)(json + len - v) >= 4 && !memcmp(v, "true", 4);
}

int gi_search_titles(const char *json, size_t len, char titles[][256], int max)
{
    const char *end = json + len;
    const char *p = json ? find_key(json, len, json, "search") : NULL;
    int found = 0;
    if (!p || *p != '[')
        return 0;
    while (found < max) {
        const char *v = find_key(json, len, p, "title");
        if (!v || *v != '"')
            break;
        /* still inside the search list? (no ']' that closes it before this title, outside strings) */
        int inside = 1, in_str = 0;
        for (const char *q = p + 1; q < v; q++) {
            if (*q == '\\' && in_str) { q++; continue; }
            if (*q == '"') in_str = !in_str;
            else if (!in_str && *q == ']') { inside = 0; break; }
        }
        if (!inside)
            break;
        const char *next = end;
        if (decode_string(v, end, titles[found], 256, &next) >= 0 && titles[found][0])
            found++;
        p = next;
    }
    return found;
}

/* ------------------------------------------------------------------ text */

static const char *ci_find(const char *hay, const char *needle)
{
    const size_t nl = strlen(needle);
    if (!hay || !nl)
        return NULL;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return p;
    }
    return NULL;
}

int gi_is_game(const char *description, const char *extract)
{
    if (ci_find(extract, "may refer to") || ci_find(description, "same term") || ci_find(description, "disambiguation"))
        return 0;
    if (ci_find(description, "video game") || ci_find(description, "game") || ci_find(description, "cd-i"))
        return 1;
    /* the first sentence or so */
    char head[400] = "";
    if (extract)
        snprintf(head, sizeof head, "%s", extract);
    return ci_find(head, "video game") || ci_find(head, "cd-i") || ci_find(head, "interactive movie") ||
           ci_find(head, "edutainment") || ci_find(head, "game for the philips");
}

void gi_clean_extract(char *s, size_t max)
{
    if (!s)
        return;
    /* white space to single spaces */
    size_t o = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ' && (o == 0 || s[o - 1] == ' '))
            continue;
        s[o++] = c;
    }
    s[o] = 0;
    /* brackets left empty where a pronunciation was taken out: "( )", "(; )", "()" */
    for (char *p = s; (p = strchr(p, '(')) != NULL;) {
        char *q = p + 1;
        while (*q == ' ' || *q == ';' || *q == ',')
            q++;
        if (*q == ')')
            memmove(p, q + 1, strlen(q + 1) + 1);
        else
            p++;
    }
    /* "word  ," and "word ." after the removals */
    o = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == ' ' && (s[i + 1] == ' ' || s[i + 1] == ',' || s[i + 1] == '.' || s[i + 1] == ';' || s[i + 1] == 0))
            continue;
        s[o++] = s[i];
    }
    s[o] = 0;
    while (o > 0 && s[o - 1] == ' ')
        s[--o] = 0;
    if (o <= max)
        return;
    /* cut at the last sentence end that fits, else at a space with an ellipsis */
    size_t cut = 0;
    for (size_t i = 0; i + 1 < max; i++)
        if (s[i] == '.' && (s[i + 1] == ' ' || s[i + 1] == 0) && i > 40)
            cut = i + 1;
    if (cut) {
        s[cut] = 0;
        return;
    }
    size_t sp = max > 4 ? max - 4 : 0;
    while (sp > 0 && s[sp] != ' ')
        sp--;
    memcpy(s + sp, "\xE2\x80\xA6", 4); /* "…" and the end */
}

void gi_url_encode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (!n)
        return;
    for (; in && *in && o + 4 < n; in++) {
        const unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ the review table */

/* wikitext made plain: references, comments and tags out, {{Rating|a|b}} as "a/b", [[x|y]] as y, quotes out */
static void wiki_plain(const char *in, size_t len, char *out, size_t n)
{
    size_t o = 0;
    const char *end = in + len;
    for (const char *p = in; p < end && o + 8 < n;) {
        if (!strncmp(p, "<!--", 4)) {
            const char *e = strstr(p, "-->");
            p = e && e < end ? e + 3 : end;
        } else if (!strncasecmp(p, "<ref", 4) && (p[4] == '>' || p[4] == ' ' || p[4] == '/')) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            if (!gt) break;
            if (gt[-1] == '/') { p = gt + 1; continue; } /* <ref name=x /> */
            const char *close = NULL;
            for (const char *q = gt; q + 6 <= end; q++)
                if (!strncasecmp(q, "</ref>", 6)) { close = q; break; }
            p = close ? close + 6 : end;
        } else if (!strncasecmp(p, "<br", 3)) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            p = gt ? gt + 1 : end;
            if (o && out[o - 1] != ' ') out[o++] = ',';
            out[o++] = ' ';
        } else if (*p == '<') {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            p = gt ? gt + 1 : end;
        } else if (!strncmp(p, "{{", 2)) {
            /* the template, to its matching end */
            const char *q = p + 2;
            int depth = 1;
            while (q + 1 < end && depth) {
                if (q[0] == '{' && q[1] == '{') { depth++; q += 2; }
                else if (q[0] == '}' && q[1] == '}') { depth--; q += 2; }
                else q++;
            }
            if (depth) q = end;
            const char *inner = p + 2, *inner_end = q - 2 > inner ? q - 2 : inner;
            if (!strncasecmp(inner, "rating|", 7) || !strncasecmp(inner, "rating |", 8)) {
                /* {{Rating|3.5|5}} or {{Rating|4}} */
                char a[16] = "", b[16] = "5";
                const char *s = memchr(inner, '|', (size_t)(inner_end - inner));
                if (s) {
                    s++;
                    size_t k = 0;
                    while (s < inner_end && *s != '|' && k + 1 < sizeof a) { if (*s != ' ') a[k++] = *s; s++; }
                    a[k] = 0;
                    if (s < inner_end && *s == '|') {
                        s++;
                        k = 0;
                        while (s < inner_end && *s != '|' && k + 1 < sizeof b) { if (*s != ' ') b[k++] = *s; s++; }
                        b[k] = 0;
                    }
                    o += (size_t)snprintf(out + o, n - o, "%s/%s", a, b);
                }
            }
            p = q;
        } else if (!strncmp(p, "[[", 2)) {
            const char *q = p + 2;
            const char *close = NULL;
            for (const char *t = q; t + 1 < end; t++)
                if (t[0] == ']' && t[1] == ']') { close = t; break; }
            if (!close) { p = end; break; }
            const char *bar = NULL;
            for (const char *t = q; t < close; t++)
                if (*t == '|') bar = t;
            const char *s = bar ? bar + 1 : q;
            while (s < close && o + 2 < n)
                out[o++] = *s++;
            p = close + 2;
        } else if (!strncmp(p, "''", 2)) {
            p += 2;
        } else if (!strncmp(p, "&nbsp;", 6)) {
            out[o++] = ' ';
            p += 6;
        } else {
            const char c = *p++;
            out[o++] = (c == '\n' || c == '\t') ? ' ' : c;
        }
    }
    out[o] = 0;
    /* trim and single spaces */
    size_t w = 0;
    for (size_t i = 0; out[i]; i++) {
        if (out[i] == ' ' && (w == 0 || out[w - 1] == ' '))
            continue;
        out[w++] = out[i];
    }
    while (w && (out[w - 1] == ' ' || out[w - 1] == ','))
        w--;
    out[w] = 0;
}

static const char *reviewer_name(const char *key)
{
    static const char *const map[][2] = {
        {"MC", "Metacritic"}, {"GR", "GameRankings"}, {"OC", "OpenCritic"}, {"AllGame", "AllGame"},
        {"CVG", "Computer and Video Games"}, {"EGM", "EGM"}, {"Edge", "Edge"}, {"Fam", "Famitsu"},
        {"GameFan", "GameFan"}, {"GamePro", "GamePro"}, {"GI", "Game Informer"}, {"GSpot", "GameSpot"},
        {"IGN", "IGN"}, {"NGen", "Next Generation"}, {"MMonthly", "Mean Machines"}, {"ACE", "ACE"},
        {"CGW", "Computer Gaming World"}, {"Hyper", "Hyper"}, {"GameRev", "Game Revolution"},
        {"1Up", "1Up.com"}, {"EuroG", "Eurogamer"}, {"GT", "GameTrailers"}, {"GameZone", "GameZone"},
        {"PCGUK", "PC Gamer UK"}, {"PCGUS", "PC Gamer US"}, {"PCZone", "PC Zone"}, {"Joystick", "Joystick"},
        {"Gen4", "Génération 4"}, {"Tilt", "Tilt"}, {"Consoles+", "Consoles +"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(key, map[i][0]))
            return map[i][1];
    return key;
}

static int skip_key(const char *key)
{
    static const char *const skip[] = {"title", "subtitle", "state", "width", "align", "na", "noagg", "platforms",
        "editorials", "aggregators", "reviewers", "award", "collapse", "headerstyle", "bodystyle", "titlestyle",
        "uniform", "MCYear", "showplatforms", "compilation", "column", "qid"};
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (!strncasecmp(key, skip[i], strlen(skip[i])))
            return 1;
    return 0;
}

static int has_digit(const char *s)
{
    for (; *s; s++)
        if (isdigit((unsigned char)*s))
            return 1;
    return 0;
}

void gi_reviews(const char *wikitext, char *out, size_t n, int most)
{
    if (!n)
        return;
    out[0] = 0;
    if (!wikitext)
        return;
    static const char *const names[] = {"{{video game reviews", "{{vg reviews", "{{vgreviews", "{{video game review score"};
    const char *start = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !start; i++)
        start = ci_find(wikitext, names[i]);
    if (!start)
        return;
    /* the template's end */
    const char *end = wikitext + strlen(wikitext);
    const char *p = start + 2;
    int depth = 1;
    while (p + 1 < end && depth) {
        if (p[0] == '{' && p[1] == '{') { depth++; p += 2; }
        else if (p[0] == '}' && p[1] == '}') { depth--; p += 2; }
        else p++;
    }
    const char *tend = depth ? end : p - 2;
    /* its parameters: split at the '|' outside {{ }} and [[ ]] */
    struct { char key[40]; char val[64]; } prm[64];
    int np = 0;
    const char *seg = NULL;
    int d2 = 0, d3 = 0;
    for (const char *q = start + 2; q <= tend && np < 64; q++) {
        const int at_end = q == tend;
        if (!at_end) {
            if (q[0] == '{' && q + 1 < tend && q[1] == '{') { d2++; q++; continue; }
            if (q[0] == '}' && q + 1 < tend && q[1] == '}') { d2--; q++; continue; }
            if (q[0] == '[' && q + 1 < tend && q[1] == '[') { d3++; q++; continue; }
            if (q[0] == ']' && q + 1 < tend && q[1] == ']') { d3--; q++; continue; }
        }
        if (at_end || (*q == '|' && d2 == 0 && d3 == 0)) {
            if (seg) {
                /* key = value */
                const char *eq = NULL;
                int e2 = 0;
                for (const char *t = seg; t < q; t++) {
                    if (t[0] == '{' && t + 1 < q && t[1] == '{') e2++;
                    else if (t[0] == '}' && t + 1 < q && t[1] == '}') e2--;
                    else if (*t == '=' && e2 == 0) { eq = t; break; }
                }
                if (eq) {
                    const char *ks = seg, *ke = eq;
                    while (ks < ke && isspace((unsigned char)*ks)) ks++;
                    while (ke > ks && isspace((unsigned char)ke[-1])) ke--;
                    if (ke > ks && (size_t)(ke - ks) < sizeof prm[0].key) {
                        memcpy(prm[np].key, ks, (size_t)(ke - ks));
                        prm[np].key[ke - ks] = 0;
                        wiki_plain(eq + 1, (size_t)(q - eq - 1), prm[np].val, sizeof prm[np].val);
                        if (prm[np].val[0])
                            np++;
                    }
                }
            }
            seg = q + 1;
        }
    }
    /* the scores, in the table's order: "revN" names "revNScore"; a named reviewer gives its own */
    size_t o = 0;
    int shown = 0;
    for (int i = 0; i < np && shown < most; i++) {
        const char *key = prm[i].key;
        const char *val = prm[i].val;
        char name[64];
        if (!has_digit(val) || skip_key(key))
            continue;
        if (!strncasecmp(key, "rev", 3) && isdigit((unsigned char)key[3])) {
            const char *s = key + 3;
            while (isdigit((unsigned char)*s)) s++;
            if (strcasecmp(s, "score") != 0)
                continue; /* "rev1" itself is a name */
            char want[16];
            snprintf(want, sizeof want, "%.*s", (int)(s - key), key);
            name[0] = 0;
            for (int j = 0; j < np; j++)
                if (!strcasecmp(prm[j].key, want))
                    snprintf(name, sizeof name, "%s", prm[j].val);
            if (!name[0])
                continue;
        } else {
            /* "IGN_PC" -> IGN */
            char base[40];
            snprintf(base, sizeof base, "%s", key);
            char *us = strchr(base, '_');
            if (us)
                *us = 0;
            snprintf(name, sizeof name, "%s", reviewer_name(base));
        }
        char v[40];
        snprintf(v, sizeof v, "%.39s", val); /* a score is short: anything longer is cut */
        const int wrote = snprintf(out + o, n - o, "%s%s %s", shown ? " \xC2\xB7 " : "", name, v);
        if (wrote < 0 || (size_t)wrote >= n - o) {
            out[o] = 0;
            break;
        }
        o += (size_t)wrote;
        shown++;
    }
}
