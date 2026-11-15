#define _GNU_SOURCE
#include "imap.h"

#include <libetpan/libetpan.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/util.h"
#include "mail/mime.h"

struct jd_imap {
    mailimap *s;
    int condstore;
};

static int ok(int r)
{
    return r == MAILIMAP_NO_ERROR || r == MAILIMAP_NO_ERROR_AUTHENTICATED || r == MAILIMAP_NO_ERROR_NON_AUTHENTICATED;
}

/* libetpan leaves certificates unchecked unless asked: verify the chain against the system's roots and the name
   against the host (RFC 6125), and send SNI. */
static void tls_setup(struct mailstream_ssl_context *ctx, void *data)
{
    const char *host = data;
    mailstream_ssl_set_server_name(ctx, (char *)host);
    SSL_CTX *ssl = mailstream_ssl_get_openssl_ssl_ctx(ctx);
    SSL_CTX_set_verify(ssl, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ssl);
    SSL_CTX_set_min_proto_version(ssl, TLS1_2_VERSION);
    X509_VERIFY_PARAM *param = SSL_CTX_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    X509_VERIFY_PARAM_set1_host(param, host, 0);
}

static void set_err(char *err, int errlen, mailimap *s, const char *what)
{
    const char *why = s && s->imap_response ? s->imap_response : "no answer";
    snprintf(err, (size_t)errlen, "%s: %s", what, why);
}

jd_imap *jd_imap_connect(const jd_account *a, const char *password, int *status, char *err, int errlen)
{
    *status = JD_IMAP_ERROR;
    mailimap *s = mailimap_new(0, NULL);
    mailimap_set_timeout(s, 60);
    int r;
    if (a->imap.security == JD_SEC_TLS) {
        r = mailimap_ssl_connect_with_callback(s, a->imap.host, (uint16_t)a->imap.port, tls_setup, a->imap.host);
    } else {
        r = mailimap_socket_connect(s, a->imap.host, (uint16_t)a->imap.port);
        if (ok(r)) r = mailimap_socket_starttls_with_callback(s, tls_setup, a->imap.host);
    }
    if (!ok(r)) {
        snprintf(err, (size_t)errlen, "cannot reach %s:%d securely (error %d)", a->imap.host, a->imap.port, r);
        mailimap_free(s);
        return NULL;
    }
    r = mailimap_login(s, a->username, password);
    if (!ok(r)) {
        set_err(err, errlen, s, "sign-in refused");
        *status = r == MAILIMAP_ERROR_LOGIN ? JD_IMAP_AUTH : JD_IMAP_ERROR;
        mailimap_free(s);
        return NULL;
    }
    struct mailimap_capability_data *caps = NULL;
    mailimap_capability(s, &caps);
    if (caps) mailimap_capability_data_free(caps);
    jd_imap *m = jd_alloc(sizeof *m);
    m->s = s;
    m->condstore = mailimap_has_condstore(s);
    *status = JD_IMAP_OK;
    return m;
}

void jd_imap_close(jd_imap *m)
{
    if (!m) return;
    mailimap_logout(m->s);
    mailimap_free(m->s);
    free(m);
}

int jd_imap_condstore(jd_imap *m) { return m->condstore; }
int jd_imap_can_idle(jd_imap *m) { return mailimap_has_idle(m->s); }

/* RFC 6154 attributes, and the names some servers use without it */
static const char *special_use_of(const struct mailimap_mailbox_list *mb, int *noselect)
{
    static const char *uses[] = {"All", "Archive", "Drafts", "Flagged", "Junk", "Sent", "Trash", NULL};
    *noselect = 0;
    const char *found = NULL;
    if (mb->mb_flag) {
        if (mb->mb_flag->mbf_type == MAILIMAP_MBX_LIST_FLAGS_SFLAG && mb->mb_flag->mbf_sflag == MAILIMAP_MBX_LIST_SFLAG_NOSELECT)
            *noselect = 1;
        for (clistiter *it = clist_begin(mb->mb_flag->mbf_oflags); it; it = clist_next(it)) {
            struct mailimap_mbx_list_oflag *f = clist_content(it);
            if (f->of_type != MAILIMAP_MBX_LIST_OFLAG_FLAG_EXT || !f->of_flag_ext) continue;
            if (strcasecmp(f->of_flag_ext, "NonExistent") == 0) *noselect = 1;
            for (int i = 0; uses[i]; i++)
                if (strcasecmp(f->of_flag_ext, uses[i]) == 0) found = uses[i];
        }
    }
    if (found) return found;
    static const struct {
        const char *name, *use;
    } names[] = {{"Sent", "Sent"}, {"Sent Items", "Sent"}, {"Sent Messages", "Sent"}, {"Drafts", "Drafts"},
                 {"Trash", "Trash"}, {"Deleted Items", "Trash"}, {"Junk", "Junk"}, {"Spam", "Junk"},
                 {"Archive", "Archive"}, {NULL, NULL}};
    const char *leaf = mb->mb_delimiter ? strrchr(mb->mb_name, mb->mb_delimiter) : NULL;
    leaf = leaf ? leaf + 1 : mb->mb_name;
    for (int i = 0; names[i].name; i++)
        if (strcasecmp(leaf, names[i].name) == 0) return names[i].use;
    return NULL;
}

int jd_imap_list(jd_imap *m, jd_mailbox **out, int *n)
{
    clist *list = NULL;
    *out = NULL;
    *n = 0;
    if (!ok(mailimap_list(m->s, "", "*", &list))) return JD_IMAP_ERROR;
    *out = jd_alloc(sizeof **out * (size_t)(clist_count(list) + 1));
    for (clistiter *it = clist_begin(list); it; it = clist_next(it)) {
        struct mailimap_mailbox_list *mb = clist_content(it);
        jd_mailbox *b = &(*out)[(*n)++];
        b->name = jd_strdup(mb->mb_name);
        b->delim = mb->mb_delimiter;
        b->special_use = jd_strdup(special_use_of(mb, &b->noselect));
    }
    mailimap_list_result_free(list);
    return JD_IMAP_OK;
}

void jd_mailboxes_free(jd_mailbox *b, int n)
{
    for (int i = 0; i < n; i++) free(b[i].name), free(b[i].special_use);
    free(b);
}

int jd_imap_select(jd_imap *m, const char *name, jd_selected *sel)
{
    uint64_t modseq = 0;
    int r = m->condstore ? mailimap_select_condstore(m->s, name, &modseq) : mailimap_select(m->s, name);
    if (!ok(r) || !m->s->imap_selection_info) return JD_IMAP_ERROR;
    struct mailimap_selection_info *si = m->s->imap_selection_info;
    *sel = (jd_selected){si->sel_uidvalidity, si->sel_uidnext, si->sel_exists, modseq};
    return JD_IMAP_OK;
}

static int cmp_uid(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int jd_imap_search(jd_imap *m, int64_t since, uint32_t **uids, int *n)
{
    struct mailimap_search_key *key;
    if (since > 0) {
        time_t t = (time_t)since;
        struct tm tm;
        gmtime_r(&t, &tm);
        key = mailimap_search_key_new_since(mailimap_date_new(tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900));
    } else {
        key = mailimap_search_key_new_all();
    }
    clist *res = NULL;
    int r = mailimap_uid_search(m->s, NULL, key, &res);
    mailimap_search_key_free(key);
    *uids = NULL;
    *n = 0;
    if (!ok(r)) return JD_IMAP_ERROR;
    *uids = jd_alloc(sizeof **uids * (size_t)(clist_count(res) + 1));
    for (clistiter *it = clist_begin(res); it; it = clist_next(it)) (*uids)[(*n)++] = *(uint32_t *)clist_content(it);
    mailimap_search_result_free(res);
    qsort(*uids, (size_t)*n, sizeof **uids, cmp_uid);
    return JD_IMAP_OK;
}

static struct mailimap_set *uid_set(const uint32_t *uids, int n)
{
    struct mailimap_set *set = mailimap_set_new_empty();
    for (int i = 0; i < n;) {
        int j = i;
        while (j + 1 < n && uids[j + 1] == uids[j] + 1) j++;
        mailimap_set_add_interval(set, uids[i], uids[j]);
        i = j + 1;
    }
    return set;
}

static int64_t imap_time(const struct mailimap_date_time *d)
{
    if (!d) return 0;
    struct tm tm = {.tm_year = d->dt_year - 1900, .tm_mon = d->dt_month - 1, .tm_mday = d->dt_day,
                    .tm_hour = d->dt_hour, .tm_min = d->dt_min, .tm_sec = d->dt_sec};
    return (int64_t)timegm(&tm) - (d->dt_zone / 100 * 60 + d->dt_zone % 100) * 60;
}

static void read_flags(const struct mailimap_msg_att_dynamic *dyn, int *flags, char **keywords)
{
    jd_buf kw = {0};
    *flags = 0;
    for (clistiter *it = dyn && dyn->att_list ? clist_begin(dyn->att_list) : NULL; it; it = clist_next(it)) {
        struct mailimap_flag_fetch *ff = clist_content(it);
        if (ff->fl_type != MAILIMAP_FLAG_FETCH_OTHER || !ff->fl_flag) continue;
        switch (ff->fl_flag->fl_type) {
        case MAILIMAP_FLAG_SEEN: *flags |= JD_SEEN; break;
        case MAILIMAP_FLAG_ANSWERED: *flags |= JD_ANSWERED; break;
        case MAILIMAP_FLAG_FLAGGED: *flags |= JD_FLAGGED; break;
        case MAILIMAP_FLAG_DELETED: *flags |= JD_DELETED; break;
        case MAILIMAP_FLAG_DRAFT: *flags |= JD_DRAFT; break;
        case MAILIMAP_FLAG_KEYWORD:
            if (ff->fl_flag->fl_data.fl_keyword) jd_buf_addf(&kw, "%s%s", kw.len ? " " : "", ff->fl_flag->fl_data.fl_keyword);
            break;
        }
    }
    *keywords = kw.s;
}

static void read_addrs(clist *list, jd_header *h, int role)
{
    if (!list) return;
    for (clistiter *it = clist_begin(list); it; it = clist_next(it)) {
        struct mailimap_address *ad = clist_content(it);
        if (!ad->ad_host_name || !ad->ad_mailbox_name) continue;
        h->addrs[role] = jd_realloc(h->addrs[role], sizeof(jd_addr) * (size_t)(h->naddrs[role] + 1));
        h->addrs[role][h->naddrs[role]++] = (jd_addr){
            .name = jd_mime_header(ad->ad_personal_name),
            .addr = jd_printf("%s@%s", ad->ad_mailbox_name, ad->ad_host_name),
        };
    }
}

static int body_has_attachment(const struct mailimap_body *b)
{
    if (!b) return 0;
    if (b->bd_type == MAILIMAP_BODY_MPART) {
        for (clistiter *it = clist_begin(b->bd_data.bd_body_mpart->bd_list); it; it = clist_next(it))
            if (body_has_attachment(clist_content(it))) return 1;
        return 0;
    }
    const struct mailimap_body_type_1part *p = b->bd_data.bd_body_1part;
    const struct mailimap_body_ext_1part *ext = p->bd_ext_1part;
    if (ext && ext->bd_disposition && strcasecmp(ext->bd_disposition->dsp_type, "attachment") == 0) return 1;
    if (p->bd_type != MAILIMAP_BODY_TYPE_1PART_BASIC) return 0;
    if (ext && ext->bd_disposition && strcasecmp(ext->bd_disposition->dsp_type, "inline") == 0) return 0;
    const struct mailimap_body_fld_param *params = p->bd_data.bd_type_basic->bd_fields->bd_parameter;
    for (clistiter *it = params ? clist_begin(params->pa_list) : NULL; it; it = clist_next(it)) {
        struct mailimap_single_body_fld_param *pa = clist_content(it);
        if (strcasecmp(pa->pa_name, "name") == 0) return 1;
    }
    return 0;
}

static uint64_t att_modseq(const struct mailimap_msg_att_item *item)
{
    const struct mailimap_extension_data *ext = item->att_data.att_extension_data;
    if (!ext || !ext->ext_extension || ext->ext_extension->ext_id != MAILIMAP_EXTENSION_CONDSTORE ||
        ext->ext_type != MAILIMAP_CONDSTORE_TYPE_FETCH_DATA)
        return 0;
    return ((struct mailimap_condstore_fetch_mod_resp *)ext->ext_data)->cs_modseq_value;
}

static void parse_header(const struct mailimap_msg_att *att, jd_header *h)
{
    for (clistiter *it = clist_begin(att->att_list); it; it = clist_next(it)) {
        struct mailimap_msg_att_item *item = clist_content(it);
        if (item->att_type == MAILIMAP_MSG_ATT_ITEM_DYNAMIC) {
            read_flags(item->att_data.att_dyn, &h->flags, &h->keywords);
            continue;
        }
        if (item->att_type == MAILIMAP_MSG_ATT_ITEM_EXTENSION) {
            h->modseq = att_modseq(item);
            continue;
        }
        if (item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC) continue;
        const struct mailimap_msg_att_static *st = item->att_data.att_static;
        switch (st->att_type) {
        case MAILIMAP_MSG_ATT_UID: h->uid = st->att_data.att_uid; break;
        case MAILIMAP_MSG_ATT_RFC822_SIZE: h->size = st->att_data.att_rfc822_size; break;
        case MAILIMAP_MSG_ATT_INTERNALDATE: h->internaldate = imap_time(st->att_data.att_internal_date); break;
        case MAILIMAP_MSG_ATT_BODYSTRUCTURE: h->has_attachment = body_has_attachment(st->att_data.att_bodystructure); break;
        case MAILIMAP_MSG_ATT_BODY_SECTION: {
            const struct mailimap_msg_att_body_section *sec = st->att_data.att_body_section;
            if (sec && sec->sec_body_part) h->refs = jd_mime_references(sec->sec_body_part, sec->sec_length, &h->nrefs);
            break;
        }
        case MAILIMAP_MSG_ATT_ENVELOPE: {
            const struct mailimap_envelope *env = st->att_data.att_env;
            if (!env) break;
            h->subject = jd_mime_header(env->env_subject);
            h->date = jd_mime_date(env->env_date);
            h->msgid = jd_mime_msgid(env->env_message_id);
            h->in_reply_to = jd_mime_msgid(env->env_in_reply_to);
            if (env->env_from) read_addrs(env->env_from->frm_list, h, JD_ROLE_FROM);
            if (env->env_to) read_addrs(env->env_to->to_list, h, JD_ROLE_TO);
            if (env->env_cc) read_addrs(env->env_cc->cc_list, h, JD_ROLE_CC);
            if (env->env_reply_to) read_addrs(env->env_reply_to->rt_list, h, JD_ROLE_REPLY_TO);
            break;
        }
        }
    }
    /* In-Reply-To counts as the last reference when References is missing (JWZ step 1) */
    if (!h->nrefs && h->in_reply_to) {
        h->refs = jd_alloc(sizeof *h->refs);
        h->refs[h->nrefs++] = jd_strdup(h->in_reply_to);
    }
}

int jd_imap_fetch_headers(jd_imap *m, const uint32_t *uids, int n, jd_header_cb *cb, void *ctx)
{
    if (n <= 0) return JD_IMAP_OK;
    struct mailimap_fetch_type *ft = mailimap_fetch_type_new_fetch_att_list_empty();
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_uid());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_flags());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_envelope());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_internaldate());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_rfc822_size());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_bodystructure());
    if (m->condstore) mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_modseq());
    clist *hdrs = clist_new();
    clist_append(hdrs, jd_strdup("References"));
    struct mailimap_section *sec = mailimap_section_new_header_fields(mailimap_header_list_new(hdrs));
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_body_peek_section(sec));
    struct mailimap_set *set = uid_set(uids, n);
    clist *res = NULL;
    int r = mailimap_uid_fetch(m->s, set, ft, &res);
    mailimap_set_free(set);
    mailimap_fetch_type_free(ft);
    if (!ok(r)) return JD_IMAP_ERROR;
    for (clistiter *it = clist_begin(res); it; it = clist_next(it)) {
        jd_header h = {0};
        parse_header(clist_content(it), &h);
        if (h.uid) cb(ctx, &h);
        jd_header_free(&h);
    }
    mailimap_fetch_list_free(res);
    return JD_IMAP_OK;
}

int jd_imap_fetch_flags(jd_imap *m, uint64_t changedsince, jd_flags_cb *cb, void *ctx)
{
    struct mailimap_fetch_type *ft = mailimap_fetch_type_new_fetch_att_list_empty();
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_uid());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_flags());
    struct mailimap_set *set = mailimap_set_new_interval(1, 0);
    clist *res = NULL;
    int r = changedsince && m->condstore ? mailimap_uid_fetch_changedsince(m->s, set, ft, changedsince, &res)
                                         : mailimap_uid_fetch(m->s, set, ft, &res);
    mailimap_set_free(set);
    mailimap_fetch_type_free(ft);
    if (!ok(r)) return JD_IMAP_ERROR;
    for (clistiter *it = clist_begin(res); it; it = clist_next(it)) {
        jd_header h = {0};
        parse_header(clist_content(it), &h);
        if (h.uid) cb(ctx, h.uid, h.flags, h.keywords, h.modseq);
        jd_header_free(&h);
    }
    mailimap_fetch_list_free(res);
    return JD_IMAP_OK;
}

int jd_imap_fetch_bodies(jd_imap *m, const uint32_t *uids, int n, jd_body_cb *cb, void *ctx)
{
    if (n <= 0) return JD_IMAP_OK;
    struct mailimap_fetch_type *ft = mailimap_fetch_type_new_fetch_att_list_empty();
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_uid());
    mailimap_fetch_type_new_fetch_att_list_add(ft, mailimap_fetch_att_new_body_peek_section(mailimap_section_new(NULL)));
    struct mailimap_set *set = uid_set(uids, n);
    clist *res = NULL;
    int r = mailimap_uid_fetch(m->s, set, ft, &res);
    mailimap_set_free(set);
    mailimap_fetch_type_free(ft);
    if (!ok(r)) return JD_IMAP_ERROR;
    for (clistiter *it = clist_begin(res); it; it = clist_next(it)) {
        struct mailimap_msg_att *att = clist_content(it);
        uint32_t uid = 0;
        const char *data = NULL;
        size_t len = 0;
        for (clistiter *a = clist_begin(att->att_list); a; a = clist_next(a)) {
            struct mailimap_msg_att_item *item = clist_content(a);
            if (item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC) continue;
            const struct mailimap_msg_att_static *st = item->att_data.att_static;
            if (st->att_type == MAILIMAP_MSG_ATT_UID) uid = st->att_data.att_uid;
            if (st->att_type == MAILIMAP_MSG_ATT_BODY_SECTION && st->att_data.att_body_section) {
                data = st->att_data.att_body_section->sec_body_part;
                len = st->att_data.att_body_section->sec_length;
            }
        }
        if (uid && data) cb(ctx, uid, data, len);
    }
    mailimap_fetch_list_free(res);
    return JD_IMAP_OK;
}

int jd_imap_set_flag(jd_imap *m, const uint32_t *uids, int n, int flag, int on)
{
    if (n <= 0) return JD_IMAP_OK;
    struct mailimap_flag_list *flags = mailimap_flag_list_new_empty();
    mailimap_flag_list_add(flags, flag == JD_FLAGGED    ? mailimap_flag_new_flagged()
                                  : flag == JD_DELETED  ? mailimap_flag_new_deleted()
                                  : flag == JD_ANSWERED ? mailimap_flag_new_answered()
                                  : flag == JD_DRAFT    ? mailimap_flag_new_draft()
                                                        : mailimap_flag_new_seen());
    struct mailimap_store_att_flags *att = on ? mailimap_store_att_flags_new_add_flags_silent(flags)
                                              : mailimap_store_att_flags_new_remove_flags_silent(flags);
    struct mailimap_set *set = uid_set(uids, n);
    int r = mailimap_uid_store(m->s, set, att);
    mailimap_set_free(set);
    mailimap_store_att_flags_free(att);
    return ok(r) ? JD_IMAP_OK : JD_IMAP_ERROR;
}

int jd_imap_search_text(jd_imap *m, const char *words, uint32_t **uids, int *n)
{
    struct mailimap_search_key *all = mailimap_search_key_new_multiple_empty();
    char *copy = jd_strdup(words), *save = NULL;
    for (char *w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save))
        mailimap_search_key_multiple_add(all, mailimap_search_key_new_text(jd_strdup(w)));
    free(copy);
    clist *res = NULL;
    int r = mailimap_uid_search(m->s, "UTF-8", all, &res);
    mailimap_search_key_free(all);
    *uids = NULL;
    *n = 0;
    if (!ok(r)) return JD_IMAP_ERROR;
    *uids = jd_alloc(sizeof **uids * (size_t)(clist_count(res) + 1));
    for (clistiter *it = clist_begin(res); it; it = clist_next(it)) (*uids)[(*n)++] = *(uint32_t *)clist_content(it);
    mailimap_search_result_free(res);
    return JD_IMAP_OK;
}

int jd_imap_create(jd_imap *m, const char *name)
{
    if (!ok(mailimap_create(m->s, name))) return JD_IMAP_ERROR;
    mailimap_subscribe(m->s, name);
    return JD_IMAP_OK;
}

int jd_imap_rename(jd_imap *m, const char *from, const char *to)
{
    if (!ok(mailimap_rename(m->s, from, to))) return JD_IMAP_ERROR;
    mailimap_subscribe(m->s, to);
    return JD_IMAP_OK;
}

int jd_imap_delete(jd_imap *m, const char *name)
{
    return ok(mailimap_delete(m->s, name)) ? JD_IMAP_OK : JD_IMAP_ERROR;
}

int jd_imap_copy(jd_imap *m, const uint32_t *uids, int n, const char *to)
{
    if (n <= 0) return JD_IMAP_OK;
    struct mailimap_set *set = uid_set(uids, n);
    int r = mailimap_uid_copy(m->s, set, to);
    mailimap_set_free(set);
    return ok(r) ? JD_IMAP_OK : JD_IMAP_ERROR;
}

int jd_imap_expunge(jd_imap *m, const uint32_t *uids, int n)
{
    if (n <= 0) return JD_IMAP_OK;
    if (!mailimap_has_uidplus(m->s)) return JD_IMAP_ERROR;
    int r = jd_imap_set_flag(m, uids, n, JD_DELETED, 1);
    if (r != JD_IMAP_OK) return r;
    struct mailimap_set *set = uid_set(uids, n);
    r = mailimap_uid_expunge(m->s, set);
    mailimap_set_free(set);
    return ok(r) ? JD_IMAP_OK : JD_IMAP_ERROR;
}

int jd_imap_append(jd_imap *m, const char *folder, const char *data, size_t len, int flags)
{
    struct mailimap_flag_list *fl = mailimap_flag_list_new_empty();
    if (flags & JD_SEEN) mailimap_flag_list_add(fl, mailimap_flag_new_seen());
    if (flags & JD_DRAFT) mailimap_flag_list_add(fl, mailimap_flag_new_draft());
    int r = mailimap_append(m->s, folder, fl, NULL, data, len);
    mailimap_flag_list_free(fl);
    return ok(r) ? JD_IMAP_OK : JD_IMAP_ERROR;
}

int jd_imap_move(jd_imap *m, const uint32_t *uids, int n, const char *to)
{
    if (n <= 0) return JD_IMAP_OK;
    if (mailimap_has_extension(m->s, "MOVE")) {
        struct mailimap_set *set = uid_set(uids, n);
        int r = mailimap_uid_move(m->s, set, to);
        mailimap_set_free(set);
        return ok(r) ? JD_IMAP_OK : JD_IMAP_ERROR;
    }
    if (!mailimap_has_uidplus(m->s)) return JD_IMAP_ERROR;
    int r = jd_imap_copy(m, uids, n, to);
    return r == JD_IMAP_OK ? jd_imap_expunge(m, uids, n) : r;
}

int jd_imap_idle_start(jd_imap *m)
{
    return ok(mailimap_idle(m->s)) ? JD_IMAP_OK : JD_IMAP_ERROR;
}

int jd_imap_idle_fd(jd_imap *m)
{
    return mailimap_idle_get_fd(m->s);
}

int jd_imap_idle_done(jd_imap *m)
{
    return ok(mailimap_idle_done(m->s)) ? JD_IMAP_OK : JD_IMAP_ERROR;
}
