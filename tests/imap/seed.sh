#!/bin/sh
# Seeds the Maildirs from /seed/<folder>/*.eml once (tests/imap/make_mail.py writes them). A message's arrival
# time (IMAP INTERNALDATE) is its file's mtime, set from its Date header; the letters after "," in the file name
# are its Maildir flags (S seen, F flagged, R replied, D draft).
set -e
base=/srv/mail/jackdaw
[ -d "$base" ] && exit 0
for dir in /seed/*/; do
    folder=$(basename "$dir")
    if [ "$folder" = INBOX ]; then target=$base; else target=$base/.$folder; fi
    mkdir -p "$target/cur" "$target/new" "$target/tmp"
    n=0
    for f in "$dir"*.eml; do
        n=$((n + 1))
        name=$(basename "$f" .eml)
        flags=
        case "$name" in *,*) flags=${name#*,} ;; esac
        out="$target/cur/$(date +%s).M$n.seed:2,$flags"
        cp "$f" "$out"
        when=$(sed -n 's/^Date: //p' "$f" | head -n 1)
        [ -n "$when" ] && touch -d "$when" "$out"
    done
done
chown -R vmail:vmail /srv/mail
