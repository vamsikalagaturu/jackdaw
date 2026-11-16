#pragma once

/* The HTML part of an outgoing message, from its plain text read as Markdown (CommonMark's common subset:
   headings, paragraphs, emphasis, code, links, lists, block quotes, rules). Line breaks inside a paragraph are
   kept, as mail readers expect; everything after the "-- " signature line is left as plain lines. */
char *jd_markdown_html(const char *text);
/* A reply's HTML part: what was written (Markdown, or kept as typed when markdown is 0), the attribution line,
   and the original's own HTML body in a <blockquote type="cite">, as Thunderbird quotes */
char *jd_markdown_reply_html(const char *mine, int markdown, const char *attribution, const char *quoted);
