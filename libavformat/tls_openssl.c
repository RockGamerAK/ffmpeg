/*
 * TLS/SSL Protocol
 * Copyright (c) 2011 Martin Storsjo
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avformat.h"
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include "url.h"
#include "tls.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

typedef struct TLSContext {
    const AVClass *class;
    TLSShared tls_shared;
    SSL_CTX *ctx;
    SSL *ssl;
} TLSContext;

static int print_tls_error(URLContext *h, int ret)
{
    av_log(h, AV_LOG_ERROR, "%s\n", ERR_error_string(ERR_get_error(), NULL));
    return AVERROR(EIO);
}

static int tls_close(URLContext *h)
{
    TLSContext *c = h->priv_data;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    if (c->tls_shared.tcp)
        ffurl_close(c->tls_shared.tcp);
    ff_tls_deinit();
    return 0;
}

static int url_bio_create(BIO *b)
{
    b->init = 1;
    b->ptr = NULL;
    b->flags = 0;
    return 1;
}

static int url_bio_destroy(BIO *b)
{
    return 1;
}

static int url_bio_bread(BIO *b, char *buf, int len)
{
    URLContext *h = b->ptr;
    int ret = ffurl_read(h, buf, len);
    if (ret >= 0)
        return ret;
    BIO_clear_retry_flags(b);
    if (ret == AVERROR_EXIT)
        return 0;
    return -1;
}

static int url_bio_bwrite(BIO *b, const char *buf, int len)
{
    URLContext *h = b->ptr;
    int ret = ffurl_write(h, buf, len);
    if (ret >= 0)
        return ret;
    BIO_clear_retry_flags(b);
    if (ret == AVERROR_EXIT)
        return 0;
    return -1;
}

static long url_bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    if (cmd == BIO_CTRL_FLUSH) {
        BIO_clear_retry_flags(b);
        return 1;
    }
    return 0;
}

static int url_bio_bputs(BIO *b, const char *str)
{
    return url_bio_bwrite(b, str, strlen(str));
}

static BIO_METHOD url_bio_method = {
    .type = BIO_TYPE_SOURCE_SINK,
    .name = "urlprotocol bio",
    .bwrite = url_bio_bwrite,
    .bread = url_bio_bread,
    .bputs = url_bio_bputs,
    .bgets = NULL,
    .ctrl = url_bio_ctrl,
    .create = url_bio_create,
    .destroy = url_bio_destroy,
};

static int tls_open(URLContext *h, const char *uri, int flags, AVDictionary **options)
{
    TLSContext *p = h->priv_data;
    TLSShared *c = &p->tls_shared;
    BIO *bio;
    int ret;

    ff_tls_init();

    if ((ret = ff_tls_open_underlying(c, h, uri, options)) < 0)
        goto fail;

    p->ctx = SSL_CTX_new(c->listen ? TLSv1_server_method() : TLSv1_client_method());
    if (!p->ctx) {
        av_log(h, AV_LOG_ERROR, "%s\n", ERR_error_string(ERR_get_error(), NULL));
        ret = AVERROR(EIO);
        goto fail;
    }
    if (c->ca_file)
        SSL_CTX_load_verify_locations(p->ctx, c->ca_file, NULL);
    if (c->cert_file && !SSL_CTX_use_certificate_chain_file(p->ctx, c->cert_file)) {
        av_log(h, AV_LOG_ERROR, "Unable to load cert file %s: %s\n",
               c->cert_file, ERR_error_string(ERR_get_error(), NULL));
        ret = AVERROR(EIO);
        goto fail;
    }
    if (c->key_file && !SSL_CTX_use_PrivateKey_file(p->ctx, c->key_file, SSL_FILETYPE_PEM)) {
        av_log(h, AV_LOG_ERROR, "Unable to load key file %s: %s\n",
               c->key_file, ERR_error_string(ERR_get_error(), NULL));
        ret = AVERROR(EIO);
        goto fail;
    }
    // Note, this doesn't check that the peer certificate actually matches
    // the requested hostname.
    if (c->verify)
        SSL_CTX_set_verify(p->ctx, SSL_VERIFY_PEER, NULL);
    p->ssl = SSL_new(p->ctx);
    if (!p->ssl) {
        av_log(h, AV_LOG_ERROR, "%s\n", ERR_error_string(ERR_get_error(), NULL));
        ret = AVERROR(EIO);
        goto fail;
    }
    bio = BIO_new(&url_bio_method);
    bio->ptr = c->tcp;
    SSL_set_bio(p->ssl, bio, bio);
    if (!c->listen && !c->numerichost)
        SSL_set_tlsext_host_name(p->ssl, c->host);
    ret = c->listen ? SSL_accept(p->ssl) : SSL_connect(p->ssl);
    if (ret == 0) {
        av_log(h, AV_LOG_ERROR, "Unable to negotiate TLS/SSL session\n");
        ret = AVERROR(EIO);
        goto fail;
    } else if (ret < 0) {
        ret = print_tls_error(h, ret);
        goto fail;
    }

    return 0;
fail:
    tls_close(h);
    return ret;
}

static int tls_read(URLContext *h, uint8_t *buf, int size)
{
    TLSContext *c = h->priv_data;
    int ret = SSL_read(c->ssl, buf, size);
    if (ret > 0)
        return ret;
    if (ret == 0)
        return AVERROR_EOF;
    return print_tls_error(h, ret);
}

static int tls_write(URLContext *h, const uint8_t *buf, int size)
{
    TLSContext *c = h->priv_data;
    int ret = SSL_write(c->ssl, buf, size);
    if (ret > 0)
        return ret;
    if (ret == 0)
        return AVERROR_EOF;
    return print_tls_error(h, ret);
}

static const AVOption options[] = {
    TLS_COMMON_OPTIONS(TLSContext, tls_shared),
    { NULL }
};

static const AVClass tls_class = {
    .class_name = "tls",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

URLProtocol ff_tls_openssl_protocol = {
    .name           = "tls",
    .url_open2      = tls_open,
    .url_read       = tls_read,
    .url_write      = tls_write,
    .url_close      = tls_close,
    .priv_data_size = sizeof(TLSContext),
    .flags          = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class = &tls_class,
};
