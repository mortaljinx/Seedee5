/* Seedee5 - what a game's card says about it, from Wikipedia: the parsing (no network here).
 *
 * The engine asks the MediaWiki API (en.wikipedia.org/w/api.php) for a game's article: its short description, the
 * plain text of its lead section, and its wikitext (for the review scores). These functions read those answers.
 * The text comes over our own HTTPS client, which does not check certificates, so nothing here trusts it: every
 * read is bounded, and what comes out is only ever drawn as text.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GAMEINFO_H
#define GAMEINFO_H

#include <stddef.h>

/* The first string value of "key" in a JSON text, decoded (escapes, \uXXXX and surrogate pairs to UTF-8) into
 * out (n bytes, always terminated). Returns the length written, or -1 when the key is not there. A value longer
 * than out is cut at a character boundary. */
int gi_json_string(const char *json, size_t len, const char *key, char *out, size_t n);

/* 1 when "key":true is in the JSON text (as MediaWiki marks "missing" and "invalid" pages) */
int gi_json_true(const char *json, size_t len, const char *key);

/* The titles a search answered, in order ("search":[{"title":...},...]), at most max of them, each up to 255
 * bytes in titles[i]. Returns how many. */
int gi_search_titles(const char *json, size_t len, char titles[][256], int max);

/* Is this article about a video game? Its short description or its first sentence says so, and it is not a list
 * of meanings ("may refer to"). */
int gi_is_game(const char *description, const char *extract);

/* The lead section's text made fit for the card, in place: whitespace and line breaks made single spaces, empty
 * brackets left behind by removed pronunciations dropped, cut at a sentence end before max bytes. */
void gi_clean_extract(char *s, size_t max);

/* The scores of the article's review table ({{Video game reviews}}) as one line, "AllGame 3/5 · Edge 6/10", at
 * most `most` of them; out is "" when there is no table or no scores. */
void gi_reviews(const char *wikitext, char *out, size_t n, int most);

/* "Mad Dog McCree" -> "Mad%20Dog%20McCree" (UTF-8 bytes percent-encoded) */
void gi_url_encode(const char *in, char *out, size_t n);

#endif
