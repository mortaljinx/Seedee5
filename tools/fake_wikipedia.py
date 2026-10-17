#!/usr/bin/env python3
"""A local stand-in for Wikipedia's API, for tools/extras_test.c: HTTPS on 127.0.0.1:8443 with a throwaway
certificate (the title's client does not check certificates). It answers the two requests the engine makes
(the article query, and the search) for a few games, as MediaWiki does with format=json&formatversion=2.

    python3 tools/fake_wikipedia.py <dir for the certificate>
"""
import json
import os
import ssl
import subprocess
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

ARTICLES = {
    "Mad Dog McCree": {
        "description": "1990 video game",
        "extract": "Mad Dog McCree is a 1990 laserdisc video game developed by American Laser Games ( ). "
                   "It was the first of the company’s live-action shooting games.\n\n"
                   "The game was ported to several home systems, including the Philips CD-i.",
        "content": "{{Infobox video game|title=Mad Dog McCree}}\n== Reception ==\n{{Video game reviews\n"
                   "| AllGame = {{Rating|2|5}}<ref>{{cite web|url=http://example.org}}</ref>\n"
                   "| rev1 = ''[[CD-i Magazine]]''\n| rev1Score = 85%\n}}\n",
    },
    "Burn": {"description": "English rock band", "extract": "Burn are an English rock band.", "content": ""},
    "Burn:Cycle": {
        "description": "1994 video game",
        "extract": "Burn:Cycle is a 1994 interactive movie adventure game for the Philips CD-i, developed by TripMedia.",
        "content": "{{Video game reviews\n| Edge = 8/10\n| rev1 = CD-i Magazine\n| rev1Score = 90%\n}}",
    },
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if q.get("list") == ["search"]:
            term = q.get("srsearch", [""])[0].lower()
            hits = [t for t in ARTICLES if t.lower().split(":")[0] in term and t != "Burn"]
            body = {"batchcomplete": True, "query": {"searchinfo": {"totalhits": len(hits)},
                                                     "search": [{"ns": 0, "title": t, "pageid": 1} for t in hits]}}
        else:
            title = q.get("titles", [""])[0]
            a = ARTICLES.get(title)
            if a:
                page = {"pageid": 1, "ns": 0, "title": title, "description": a["description"],
                        "extract": a["extract"],
                        "revisions": [{"slots": {"main": {"contentmodel": "wikitext", "content": a["content"]}}}]}
            else:
                page = {"ns": 0, "title": title, "missing": True}
            body = {"batchcomplete": True, "query": {"pages": [page]}}
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        sys.stderr.write("fake wikipedia: " + (fmt % args) + "\n")


def main():
    d = sys.argv[1]
    cert, key = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")
    if not os.path.exists(cert):
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=127.0.0.1",
                        "-keyout", key, "-out", cert], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    srv = HTTPServer(("127.0.0.1", 8443), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
