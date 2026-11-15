#!/usr/bin/env python3
"""Writes the test mailbox into tests/imap/seed/<folder>/NNN[,FLAGS].eml (Maildir flags: S seen, F flagged,
R replied, D draft), replacing what an earlier run wrote there. Folder directories use Maildir++ names
(Projects.Bremen is Projects/Bremen). Deterministic: the same run gives the same files. seed.sh sets each
message's arrival time from its Date header."""

import email.utils
import random
import shutil
import struct
import zlib
from datetime import datetime, timedelta, timezone
from email.headerregistry import Address
from email.message import EmailMessage
from pathlib import Path

OUT = Path(__file__).resolve().parent / "seed"
NOW = datetime(2026, 11, 15, 18, 0, tzinfo=timezone(timedelta(hours=1)))
ME = Address("Jackdaw Tester", "jackdaw", "localhost")
rng = random.Random(7)

PEOPLE = {
    "ada": Address("Ada Lovelace", "ada", "example.org"),
    "bob": Address("Bob Builder", "bob", "example.org"),
    "carol": Address("Carol Danvers", "carol", "example.org"),
    "juergen": Address("Jürgen Müller", "juergen", "example.de"),
    "nikos": Address("Νίκος Παπαδόπουλος", "nikos", "example.gr"),
    "olga": Address("Ольга Иванова", "olga", "example.ru"),
    "priya": Address("Priya Raman", "priya", "uni-bremen.example"),
    "tom": Address("Tom Okafor", "tom", "uni-bremen.example"),
    "lena": Address("Lena Schmidt", "lena", "example.de"),
    "boss": Address("Dr. Hannah Weber", "weber", "uni-bremen.example"),
}

counter = 0


def png(width, height, color_a, color_b):
    """a small gradient PNG, written by hand with zlib"""
    rows = b""
    for y in range(height):
        rows += b"\x00"
        for x in range(width):
            t = (x + y) / (width + height)
            rows += bytes(int(a + (b - a) * t) for a, b in zip(color_a, color_b))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def pdf(title):
    """a one-page PDF with a line of text"""
    text = f"BT /F1 18 Tf 72 720 Td ({title}) Tj ET"
    objs = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
            "/Resources << /Font << /F1 5 0 R >> >> >>",
            f"<< /Length {len(text)} >>\nstream\n{text}\nendstream",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    out, offsets = "%PDF-1.4\n", []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n{o}\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n" + "".join(f"{o:010d} 00000 n \n" for o in offsets)
    out += f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n"
    return out.encode()


def write(folder, msg, flags=""):
    global counter
    counter += 1
    d = OUT / folder
    d.mkdir(parents=True, exist_ok=True)
    name = f"{counter:03d}" + (f",{flags}" if flags else "") + ".eml"
    (d / name).write_bytes(bytes(msg))


def message(frm, to, subject, when, body, cc=(), msgid=None, reply_to_msg=None, html=None, list_id=None):
    m = EmailMessage()
    m["From"] = frm
    m["To"] = to if isinstance(to, (list, tuple)) else [to]
    if cc:
        m["Cc"] = list(cc)
    m["Subject"] = subject
    m["Date"] = email.utils.format_datetime(when)
    m["Message-ID"] = msgid or email.utils.make_msgid(domain=frm.domain)
    if reply_to_msg is not None:
        m["In-Reply-To"] = reply_to_msg["Message-ID"]
        refs = (reply_to_msg.get("References", "") + " " + reply_to_msg["Message-ID"]).strip()
        m["References"] = refs
    if list_id:
        m["List-Id"] = list_id
        m["List-Unsubscribe"] = f"<mailto:unsubscribe@{list_id.strip('<>')}>"
    m.set_content(body)
    if html:
        m.add_alternative(html, subtype="html")
    return m


def quoted(prev, text):
    when = email.utils.parsedate_to_datetime(prev["Date"]).strftime("%a, %d %b %Y at %H:%M")
    who = prev["From"]
    body = prev.get_body(("plain",)).get_content()
    return text + f"\n\nOn {when}, {who} wrote:\n" + "".join("> " + line + "\n" for line in body.rstrip().split("\n"))


def ago(days, hours=0):
    return NOW - timedelta(days=days, hours=hours, minutes=rng.randint(0, 59))


def project_thread():
    """the Bremen conference: six messages, your replies in Sent, an agenda PDF halfway"""
    p = PEOPLE
    m1 = message(p["boss"], [ME, p["priya"], p["tom"]], "Bremen workshop: planning",
                 ago(9, 3), "Hi all,\n\nwe host the robotics workshop in Bremen on 4 December. Could Priya take the "
                 "programme, Tom the venue, and you the registration page?\n\nBest,\nHannah")
    write("INBOX", m1, "S")
    m2 = message(p["priya"], [p["boss"]], "Re: Bremen workshop: planning", ago(9, 1),
                 quoted(m1, "Happy to. I'll send a first programme by Friday."), cc=[ME, p["tom"]], reply_to_msg=m1)
    write("INBOX", m2, "S")
    m3 = message(ME, [p["boss"]], "Re: Bremen workshop: planning", ago(8, 20),
                 quoted(m2, "Registration page is up on the staging server; I'll open it once the programme is final."),
                 cc=[p["priya"], p["tom"]], reply_to_msg=m2)
    write("Sent", m3, "S")
    m4 = message(p["priya"], [p["boss"], ME, p["tom"]], "Re: Bremen workshop: planning", ago(6, 4),
                 quoted(m3, "First programme attached. Three sessions, two keynotes, posters in the afternoon."),
                 reply_to_msg=m3)
    m4.add_attachment(pdf("Workshop programme draft"), maintype="application", subtype="pdf",
                      filename="programme-draft.pdf")
    write("INBOX", m4, "SF")
    m5 = message(p["tom"], [p["boss"], ME, p["priya"]], "Re: Bremen workshop: planning", ago(2, 5),
                 quoted(m4, "Venue confirmed: lecture hall 2, coffee in the foyer. Catering needs numbers by the 28th."),
                 reply_to_msg=m4)
    m5.add_attachment(png(240, 160, (94, 129, 172), (163, 190, 140)), maintype="image", subtype="png",
                      filename="hall-2.png")
    write("INBOX", m5)
    m6 = message(p["boss"], [ME, p["priya"], p["tom"]], "Re: Bremen workshop: planning", ago(0, 3),
                 quoted(m5, "Great, thanks all. Please open registration tomorrow."), reply_to_msg=m5)
    write("INBOX", m6)


def mailing_list():
    lid = "<dev.lists.example.org>"
    lst = Address("dev list", "dev", "lists.example.org")
    threads = [("[dev] Release 2.4 freeze", "ada"), ("[dev] Flaky CI on arm64", "nikos"),
               ("[dev] RFC: drop the legacy config format", "olga")]
    for t, (subject, starter) in enumerate(threads):
        first = message(PEOPLE[starter], [lst], subject, ago(20 - t * 6, 2),
                        f"Hi everyone,\n\n{subject[6:]} — opinions welcome. Details in the tracker.\n", list_id=lid)
        write("INBOX", first, "S")
        prev = first
        for k, who in enumerate(rng.sample(["bob", "carol", "lena", "juergen", "olga", "nikos"], 3)):
            reply = message(PEOPLE[who], [lst], "Re: " + subject, ago(19 - t * 6 - k, 1),
                            quoted(prev, rng.choice(["+1 from me.", "We tried this last year; it broke the Windows "
                                                     "build. Can we gate it behind a flag first?",
                                                     "I can take this if nobody else does.",
                                                     "Logs attached in the tracker, see comment 14."])),
                            reply_to_msg=prev, list_id=lid)
            write("INBOX", reply, "" if t == 2 and k == 2 else "S")
            prev = reply


NEWSLETTER = """<html><head><style>
body {{ font-family: Helvetica, Arial, sans-serif; color: #222; }}
.wrap {{ max-width: 600px; margin: 0 auto; }}
.head {{ background: #2e3440; color: #eceff4; padding: 18px 24px; }}
.head h1 {{ margin: 0; font-size: 22px; }}
.item {{ border-bottom: 1px solid #d8dee9; padding: 14px 24px; }}
.item h2 {{ font-size: 17px; margin: 0 0 6px 0; color: #5e81ac; }}
.btn {{ display: inline-block; background: #5e81ac; color: #fff; padding: 8px 16px; text-decoration: none; }}
.foot {{ font-size: 12px; color: #777; padding: 12px 24px; }}
</style></head><body><div class="wrap">
<div class="head"><img src="cid:logo" width="48" height="48" alt=""> <h1>{title}</h1></div>
{items}
<div class="item"><a class="btn" href="https://example.org/news">Read on the web</a></div>
<div class="foot">You get this because you subscribed. <a href="https://example.org/unsubscribe">Unsubscribe</a>
<img src="https://tracker.example.com/open.gif" width="1" height="1" alt=""></div>
</div></body></html>"""


def newsletters():
    sender = Address("Corvid Weekly", "news", "corvids.example.com")
    for k, (title, topics) in enumerate([
        ("Corvid Weekly #41", ["Jackdaws recognise faces", "Why crows cache food", "Field notes from Bremen"]),
        ("Corvid Weekly #42", ["Tool use in New Caledonian crows", "Ravens and play", "Reader photos"]),
        ("Corvid Weekly #43", ["Winter roosts", "Counting birds with volunteers", "Book review: The Genius of Birds"]),
    ]):
        items = "".join(f'<div class="item"><h2>{t}</h2><p>{rng.choice(["Researchers in Cambridge", "A new study", "Our reader Lena"])} '
                        f'found something remarkable this week. <a href="https://example.org/{i}">Read more</a></p></div>'
                        for i, t in enumerate(topics))
        text = title + "\n\n" + "\n".join("* " + t for t in topics) + "\n\nRead on the web: https://example.org/news\n"
        m = message(sender, [ME], title, ago(21 - k * 7, 6), text, html=NEWSLETTER.format(title=title, items=items))
        html_part = m.get_body(("html",))
        html_part.add_related(png(48, 48, (136, 192, 208), (94, 129, 172)), maintype="image", subtype="png", cid="<logo>")
        write("INBOX", m, "S" if k < 2 else "")


RECEIPT = """<html><body style="font-family: Arial, sans-serif">
<h2 style="color:#2e3440">Thanks for your order</h2><p>Order <b>{order}</b>, placed {date}.</p>
<table style="border-collapse: collapse; width: 100%">
<tr style="background:#e5e9f0"><th style="text-align:left; padding:6px">Item</th><th style="padding:6px">Qty</th>
<th style="text-align:right; padding:6px">Price</th></tr>{rows}
<tr><td style="padding:6px"><b>Total</b></td><td></td><td style="text-align:right; padding:6px"><b>{total:.2f} EUR</b></td></tr>
</table><p style="color:#777; font-size:12px">Questions? Reply to this mail.</p></body></html>"""


def receipts():
    shop = Address("Field Gear Shop", "orders", "fieldgear.example")
    for k, items in enumerate([[("Binoculars 8x42", 1, 189.0), ("Lens cloth", 2, 4.5)],
                               [("Bird feeder", 1, 24.9), ("Sunflower seeds 5 kg", 2, 12.0)]]):
        total = sum(q * p for _, q, p in items)
        rows = "".join(f'<tr><td style="padding:6px">{n}</td><td style="text-align:center">{q}</td>'
                       f'<td style="text-align:right; padding:6px">{q * p:.2f} EUR</td></tr>' for n, q, p in items)
        when = ago(30 + k * 25)
        order = f"FG-{4711 + k}"
        text = f"Order {order}\n" + "\n".join(f"{n} x{q}: {q * p:.2f} EUR" for n, q, p in items) + f"\nTotal {total:.2f} EUR\n"
        m = message(shop, [ME], f"Your order {order}", when, text,
                    html=RECEIPT.format(order=order, date=when.strftime("%d %b %Y"), rows=rows, total=total))
        m.add_attachment(pdf(f"Invoice {order}"), maintype="application", subtype="pdf", filename=f"invoice-{order}.pdf")
        write("Receipts", m, "S")


def personal():
    p = PEOPLE
    m = message(p["juergen"], [ME], "Grüße aus Köln", ago(3, 2),
                "Hallo,\n\nschöne Grüße vom Rhein! Wir sind am Wochenende in Bremen — Kaffee am Samstag?\n\nJürgen")
    write("INBOX", m)
    m = message(p["nikos"], [ME], "Καλημέρα από την Αθήνα", ago(4, 8),
                "Γεια σου!\n\nΣου στέλνω τις φωτογραφίες από το συνέδριο.\n\nΝίκος")
    m.add_attachment(png(320, 200, (191, 97, 106), (235, 203, 139)), maintype="image", subtype="png",
                     filename="συνέδριο.png")
    write("INBOX", m, "S")
    m = message(p["olga"], [ME], "Отчёт за ноябрь", ago(1, 6),
                "Привет!\n\nОтчёт готов, посмотри, пожалуйста, до пятницы.\n\nОльга")
    m.add_attachment(pdf("Report November"), maintype="application", subtype="pdf", filename="отчёт-ноябрь.pdf")
    write("INBOX", m, "F")
    invite = ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//jackdaw test//EN\r\nMETHOD:REQUEST\r\nBEGIN:VEVENT\r\n"
              "UID:workshop-dry-run@uni-bremen.example\r\nDTSTART:20261203T140000Z\r\nDTEND:20261203T150000Z\r\n"
              "SUMMARY:Workshop dry run\r\nLOCATION:Lecture hall 2\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")
    m = message(p["boss"], [ME, p["tom"]], "Invitation: Workshop dry run (Thu 3 Dec, 15:00)", ago(1, 2),
                "Workshop dry run\nThursday 3 December, 15:00-16:00\nLecture hall 2\n")
    m.add_attachment(invite.encode(), maintype="text", subtype="calendar", filename="invite.ics", params={"method": "REQUEST"})
    write("INBOX", m)
    long_lines = " ".join(["The migration plan has three phases."] * 12)
    m = message(p["lena"], [ME], "Notes from the infrastructure meeting", ago(5, 3),
                f"Hi,\n\nhere are my notes.\n\n1. Storage\n{long_lines}\n\n2. Backups\n   - nightly to the NAS\n"
                f"   - weekly offsite\n\n3. Open questions\n   * who owns the DNS change?\n   * budget for Q1\n\nLena")
    write("INBOX", m, "S")
    m = message(p["carol"], [ME], "Quick question", ago(0, 1), "Do you have the slides from Tuesday? Thanks!")
    write("INBOX", m)
    m = message(ME, [p["carol"]], "Re: Lunch", ago(12), "Thursday works, see you at noon.")
    write("Sent", m, "S")
    m = message(p["ada"], [ME], "Tickets", ago(6, 1), "The tickets are attached.")
    m.add_attachment(b"Ticket 1: Bremen Hbf 09:00\nTicket 2: return 18:30\n", maintype="text", subtype="plain",
                     filename="tickets.txt")
    write("INBOX", m, "S")
    m = message(p["bob"], [ME], "Old note", ago(70), "This one is older than the prefetch window.")
    write("Archive", m, "S")
    for k in range(4):
        m = message(rng.choice(list(p.values())), [ME], rng.choice(["Minutes", "Slides", "Re: budget", "Photos"]),
                    ago(40 + k * 9), "Archived conversation.\n")
        write("Archive", m, "S")


def projects():
    p = PEOPLE
    m = message(p["priya"], [ME], "Atlas: data model review", ago(14, 2),
                "Can you review the Turtle shapes before Monday? The SHACL report is attached.")
    m.add_attachment(b"Validation report: 0 violations, 3 warnings\n", maintype="text", subtype="plain",
                     filename="shacl-report.txt")
    write("Projects.Atlas", m, "S")
    m = message(p["tom"], [ME], "Bremen: catering numbers", ago(3, 1), "We are at 46 registrations so far.")
    write("Projects.Bremen", m)
    m = message(p["boss"], [ME], "Project reports due", ago(16), "Quarterly reports for all projects by the 30th.")
    write("Projects", m, "S")


def junk_trash_drafts():
    m = message(Address("Prize Desk", "win", "lottery.example"), [ME], "You have WON 1.000.000 EUR!!!", ago(2),
                "Click here to claim: http://lottery.example/claim\n")
    write("Junk", m)
    m = message(Address("IT Support", "support", "it-helpdesk.example"), [ME], "Your mailbox is full",
                ago(5), "Verify your password at http://it-helpdesk.example/verify\n")
    write("Junk", m)
    m = message(PEOPLE["bob"], [ME], "Lunch?", ago(11), "Free on Thursday?")
    write("Trash", m, "S")
    m = message(ME, [PEOPLE["boss"]], "Registration numbers", ago(0, 2),
                "Hi Hannah,\n\nregistration is open; 12 people signed up in the first hour.\n")
    write("Drafts", m, "SD")


WWW = Path(__file__).resolve().parent / "www"
GALLERY = """<html><body style="font-family: Arial, sans-serif; color: #222">
<h2>Field photos: November</h2><p>Three pictures from our readers this month.</p>
<table><tr>{cells}</tr></table>
<p style="font-size:12px; color:#777">Pictures load from our server.
<img src="http://127.0.0.1:9080/pixel.png" width="1" height="1" alt=""></p></body></html>"""


def remote_gallery():
    """a newsletter whose pictures live on the test container's web server (127.0.0.1:9080)"""
    if WWW.exists():
        shutil.rmtree(WWW)
    (WWW / "photos").mkdir(parents=True)
    colors = [((46, 52, 64), (136, 192, 208)), ((191, 97, 106), (235, 203, 139)), ((163, 190, 140), (94, 129, 172))]
    cells = ""
    for i, (a, b) in enumerate(colors, 1):
        (WWW / "photos" / f"{i}.png").write_bytes(png(200, 140, a, b))
        cells += f'<td style="padding:4px"><img src="http://127.0.0.1:9080/photos/{i}.png" width="200" height="140" alt="photo {i}"></td>'
    (WWW / "pixel.png").write_bytes(png(1, 1, (255, 255, 255), (255, 255, 255)))
    sender = Address("Field Photos", "photos", "fieldphotos.example")
    m = message(sender, [ME], "Field photos: November", ago(0, 5), "Three reader photos this month (pictures on the web).",
                html=GALLERY.format(cells=cells))
    write("INBOX", m)


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    remote_gallery()
    project_thread()
    mailing_list()
    newsletters()
    receipts()
    personal()
    projects()
    junk_trash_drafts()
    print(f"{counter} messages in {OUT}")


if __name__ == "__main__":
    main()
