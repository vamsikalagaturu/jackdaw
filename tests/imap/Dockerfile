# Mail server for testing jackdaw: user jackdaw (jackdaw@localhost), password jackdaw-test.
# Dovecot: IMAPS 993, IMAP+STARTTLS 143, ManageSieve 4190, Sieve run on delivery.
# Postfix: submission over TLS 465 and STARTTLS 587, authenticated through Dovecot, delivering mail for
# @localhost into Dovecot over LMTP (other domains are accepted and stay queued: nothing leaves the container).
# busybox httpd: the remote images some test newsletters refer to, on port 80.
# The certificate is signed by a CA made here; tests/imap/start.sh copies ca.crt out so jackdaw can trust it.
FROM ubuntu:24.04
RUN echo 'postfix postfix/main_mailer_type select No configuration' | debconf-set-selections \
 && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends dovecot-imapd \
    dovecot-lmtpd dovecot-sieve dovecot-managesieved postfix openssl busybox && rm -rf /var/lib/apt/lists/*
RUN cd /etc/dovecot && openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=jackdaw-test-ca \
      -keyout ca.key -out ca.crt 2>/dev/null \
 && openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout test.key -out test.csr 2>/dev/null \
 && printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > san.ext \
 && openssl x509 -req -in test.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -extfile san.ext -out test.crt 2>/dev/null \
 && chmod 644 test.key \
 && useradd -r -u 5000 -d /srv/mail vmail && mkdir -p /srv/mail /srv/home && chown vmail:vmail /srv/mail /srv/home \
 && echo 'jackdaw:{PLAIN}jackdaw-test' > /etc/dovecot/users
COPY dovecot.conf /etc/dovecot/dovecot.conf
COPY postfix-main.cf /etc/postfix/main.cf
COPY postfix-master.cf /etc/postfix/master-extra.cf
RUN cat /etc/postfix/master-extra.cf >> /etc/postfix/master.cf && rm /etc/postfix/master-extra.cf \
 && chmod 644 /etc/postfix/main.cf /etc/postfix/master.cf && newaliases
COPY seed /seed
COPY seed.sh /seed.sh
COPY www /srv/www
CMD ["/bin/sh", "-c", "/bin/sh /seed.sh && dovecot && busybox httpd -p 80 -h /srv/www && exec postfix start-fg"]
