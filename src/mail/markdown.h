#pragma once

/* The HTML part of an outgoing message, from its plain text read as Markdown (CommonMark's common subset:
   headings, paragraphs, emphasis, code, links, lists, block quotes, rules). Line breaks inside a paragraph are
   kept, as mail readers expect; everything after the "-- " signature line is left as plain lines. */
char *jd_markdown_html(const char *text);
