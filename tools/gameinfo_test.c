/* Tests for host/gameinfo.c (the Wikipedia parsing of the game card), with answers shaped as MediaWiki's API sends
 * them, and random input for robustness.
 *     cc -std=c11 -g -fsanitize=address,undefined -Ihost tools/gameinfo_test.c host/gameinfo.c -o gameinfo_test
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gameinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define EQ(a, b) do { if (strcmp((a), (b)) != 0) { printf("FAIL %s:%d:\n  got  [%s]\n  want [%s]\n", __FILE__, __LINE__, (a), (b)); fails++; } } while (0)

static const char kPage[] =
    "{\"batchcomplete\":true,\"query\":{\"normalized\":[{\"fromencoded\":false,\"from\":\"Mad_Dog_McCree\",\"to\":\"Mad Dog McCree\"}],"
    "\"pages\":[{\"pageid\":1234,\"ns\":0,\"title\":\"Mad Dog McCree\",\"description\":\"1990 video game\","
    "\"extract\":\"Mad Dog McCree is a 1990 laserdisc video game developed by American Laser Games ( ). It was the first of"
    " the company\\u2019s live-action games.\\n\\nThe game was ported to the Philips CD-i, the 3DO and DOS.\","
    "\"revisions\":[{\"slots\":{\"main\":{\"contentmodel\":\"wikitext\",\"content\":\"{{Short description|1990 video game}}\\n"
    "{{Infobox video game\\n| title = Mad Dog McCree\\n| platforms = [[Arcade game|Arcade]], [[CD-i]]\\n}}\\n"
    "== Reception ==\\n{{Video game reviews\\n| title = Reviews\\n| state = plain\\n"
    "| AllGame = {{Rating|2|5}}<ref>{{cite web|url=http://example.org/a|title=Mad Dog McCree - Review|publisher=[[AllGame]]}}</ref>\\n"
    "| rev1 = ''[[CD-i Magazine]]''\\n| rev1Score = 85%<ref name=\\\"cdimag\\\"/>\\n"
    "| CVG = 71%\\n| EGM = <!-- none -->\\n| rev2 = Dragon\\n| rev2Score = {{Rating|4}}\\n}}\\nThe game sold well.\"}}}]}]}}";

static const char kMissing[] = "{\"batchcomplete\":true,\"query\":{\"pages\":[{\"ns\":0,\"title\":\"No Such Game\",\"missing\":true}]}}";

static const char kSearch[] =
    "{\"batchcomplete\":true,\"continue\":{\"sroffset\":3,\"continue\":\"-||\"},\"query\":{\"searchinfo\":{\"totalhits\":42},"
    "\"search\":[{\"ns\":0,\"title\":\"Burn:Cycle\",\"pageid\":11},{\"ns\":0,\"title\":\"Burn Cycle (album)\",\"pageid\":12},"
    "{\"ns\":0,\"title\":\"Cycle \\\"Burn\\\"\",\"pageid\":13}]},\"other\":{\"title\":\"not a result\"}}";

int main(void)
{
    char out[2048];

    /* strings */
    CHECK(gi_json_string(kPage, sizeof kPage - 1, "title", out, sizeof out) > 0);
    EQ(out, "Mad Dog McCree");
    gi_json_string(kPage, sizeof kPage - 1, "description", out, sizeof out);
    EQ(out, "1990 video game");
    gi_json_string(kPage, sizeof kPage - 1, "extract", out, sizeof out);
    CHECK(strstr(out, "company\xE2\x80\x99s") != NULL);         /* \u2019 as UTF-8 */
    CHECK(strstr(out, "games.\n\nThe game") != NULL);           /* \n decoded */
    CHECK(gi_json_string(kPage, sizeof kPage - 1, "nothere", out, sizeof out) == -1 && out[0] == 0);
    /* a key that only appears inside a string value is not a key */
    {
        const char j[] = "{\"extract\":\"say \\\"title\\\": no\",\"title\":\"Yes\"}";
        gi_json_string(j, sizeof j - 1, "title", out, sizeof out);
        EQ(out, "Yes");
    }
    /* surrogate pairs, a broken one, and cutting at a character boundary */
    {
        const char j[] = "{\"t\":\"caf\\u00e9 \\ud83c\\udfae \\ud800x\"}";
        gi_json_string(j, sizeof j - 1, "t", out, sizeof out);
        EQ(out, "caf\xC3\xA9 \xF0\x9F\x8E\xAE \xEF\xBF\xBDx");
        char small[6];
        gi_json_string(j, sizeof j - 1, "t", small, sizeof small);
        EQ(small, "caf\xC3\xA9"); /* 5 bytes: the space would fit, the emoji would not; nothing half */
        const char raw[] = "{\"t\":\"\xC3\xA9\xC3\xA9\xC3\xA9\"}";
        char four[4];
        gi_json_string(raw, sizeof raw - 1, "t", four, sizeof four);
        EQ(four, "\xC3\xA9"); /* raw UTF-8 is not split either */
    }
    /* booleans */
    CHECK(gi_json_true(kMissing, sizeof kMissing - 1, "missing"));
    CHECK(!gi_json_true(kPage, sizeof kPage - 1, "missing"));
    CHECK(gi_json_true(kPage, sizeof kPage - 1, "batchcomplete"));

    /* search results, and not the "title" outside the list */
    {
        char t[5][256];
        const int n = gi_search_titles(kSearch, sizeof kSearch - 1, t, 5);
        CHECK(n == 3);
        if (n == 3) {
            EQ(t[0], "Burn:Cycle");
            EQ(t[1], "Burn Cycle (album)");
            EQ(t[2], "Cycle \"Burn\"");
        }
        CHECK(gi_search_titles(kSearch, sizeof kSearch - 1, t, 1) == 1);
        CHECK(gi_search_titles(kMissing, sizeof kMissing - 1, t, 5) == 0);
    }

    /* is it a game? */
    CHECK(gi_is_game("1990 video game", ""));
    CHECK(gi_is_game("", "Burn:Cycle is a 1994 interactive movie for the Philips CD-i."));
    CHECK(!gi_is_game("Topics referred to by the same term", "Burn may refer to:"));
    CHECK(!gi_is_game("English rock band", "Mad Dog is a band from London."));
    CHECK(!gi_is_game("", "Hotel Mario may refer to: a game"));

    /* the extract made fit */
    {
        char s[600];
        snprintf(s, sizeof s, "Zelda's Adventure ( ;  ) is a 1994  action-adventure game.\n\nIt was developed by Viridis ( ).  ");
        gi_clean_extract(s, 500);
        EQ(s, "Zelda's Adventure is a 1994 action-adventure game. It was developed by Viridis.");
        snprintf(s, sizeof s, "One sentence that is quite long indeed for the card. Second sentence follows here. Third.");
        gi_clean_extract(s, 70);
        EQ(s, "One sentence that is quite long indeed for the card.");
        snprintf(s, sizeof s, "averyveryverylongwordwithoutanyend and more words without a full stop at all here");
        gi_clean_extract(s, 40);
        CHECK(strlen(s) <= 40 && strstr(s, "\xE2\x80\xA6") != NULL);
    }

    /* reviews from the wikitext */
    {
        char wt[4096], rv[300];
        gi_json_string(kPage, sizeof kPage - 1, "content", wt, sizeof wt);
        gi_reviews(wt, rv, sizeof rv, 4);
        EQ(rv, "AllGame 2/5 \xC2\xB7 CD-i Magazine 85% \xC2\xB7 Computer and Video Games 71% \xC2\xB7 Dragon 4/5");
        gi_reviews(wt, rv, sizeof rv, 2);
        EQ(rv, "AllGame 2/5 \xC2\xB7 CD-i Magazine 85%");
        gi_reviews("No reviews here. {{Infobox video game|title=X}}", rv, sizeof rv, 4);
        EQ(rv, "");
        gi_reviews("{{VG Reviews\n|MC = 61/100<ref>x</ref>\n|GR=55%\n|IGN_PC = 7/10\n|na=\n}}", rv, sizeof rv, 4);
        EQ(rv, "Metacritic 61/100 \xC2\xB7 GameRankings 55% \xC2\xB7 IGN 7/10");
        gi_reviews("{{Video game reviews\n| rev1 = [[Edge (magazine)|Edge]]\n| rev1Score = 6/10<br/>\n}}", rv, sizeof rv, 4);
        EQ(rv, "Edge 6/10");
        char tiny[12];
        gi_reviews(wt, tiny, sizeof tiny, 4);
        EQ(tiny, "AllGame 2/5"); /* what does not fit is left out whole */
        gi_reviews("{{Video game reviews\n| AllGame = {{Rating|2|5", rv, sizeof rv, 4); /* never closed */
        CHECK(strlen(rv) < sizeof rv);
    }

    /* URL encoding */
    gi_url_encode("Burn:Cycle (CD-i) & Co", out, sizeof out);
    EQ(out, "Burn%3ACycle%20%28CD-i%29%20%26%20Co");

    /* random and cut-off input: nothing may read or write out of bounds (run under ASan) */
    srand(5);
    char buf[3000];
    for (int round = 0; round < 20000; round++) {
        const size_t n = (size_t)(rand() % 2999);
        const char *alphabet = "{}[]|=\"\\:,'<>/!-u0123456789abcdefRrtngtitleextractsearch \n";
        const size_t an = strlen(alphabet);
        for (size_t i = 0; i < n; i++)
            buf[i] = (rand() % 5 == 0) ? (char)(rand() % 256) : alphabet[(size_t)rand() % an];
        buf[n] = 0;
        char a[300], b[300], t[3][256];
        gi_json_string(buf, n, "title", a, sizeof a);
        gi_json_string(buf, n, "extract", b, 1 + (size_t)(rand() % 40));
        gi_json_true(buf, n, "missing");
        gi_search_titles(buf, n, t, 3);
        gi_is_game(a, buf);
        char c[400];
        snprintf(c, sizeof c, "%s", buf);
        gi_clean_extract(c, (size_t)(rand() % 300));
        gi_reviews(buf, a, 1 + (size_t)(rand() % 200), 4);
        /* cut-off copies of the real answers */
        const size_t cut = (size_t)(rand() % (sizeof kPage));
        gi_json_string(kPage, cut, "content", buf, sizeof buf);
        gi_reviews(buf, a, sizeof a, 4);
        gi_search_titles(kSearch, (size_t)(rand() % (sizeof kSearch)), t, 3);
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
