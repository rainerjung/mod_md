/* Copyright 2017 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <apr_lib.h>
#include <apr_buckets.h>
#include <apr_file_io.h>
#include <apr_strings.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include "md.h"
#include "md_crypt.h"
#include "md_json.h"
#include "md_log.h"
#include "md_http.h"
#include "md_util.h"

/* getpid for *NIX */
#if APR_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif

/* getpid for Windows */
#if APR_HAVE_PROCESS_H
#include <process.h>
#endif

static int initialized;

struct md_pkey_t {
    apr_pool_t *pool;
    EVP_PKEY   *pkey;
};

#ifdef MD_HAVE_ARC4RANDOM

static void seed_RAND(int pid)
{
    char seed[128];
    arc4random_buf(seed, sizeof(seed));
    RAND_seed(seed, sizeof(seed));
}

#else /* ifdef MD_HAVE_ARC4RANDOM */

static int rand_choosenum(int l, int h)
{
    int i;
    char buf[50];

    apr_snprintf(buf, sizeof(buf), "%.0f",
                 (((double)(rand()%RAND_MAX)/RAND_MAX)*(h-l)));
    i = atoi(buf)+1;
    if (i < l) i = l;
    if (i > h) i = h;
    return i;
}

static void seed_RAND(int pid)
{   
    unsigned char stackdata[256];
    /* stolen from mod_ssl/ssl_engine_rand.c */
    int n;
    struct {
        time_t t;
        pid_t pid;
    } my_seed;
    
    /*
     * seed in the current time (usually just 4 bytes)
     */
    my_seed.t = time(NULL);
    
    /*
     * seed in the current process id (usually just 4 bytes)
     */
    my_seed.pid = pid;
    
    RAND_seed((unsigned char *)&my_seed, sizeof(my_seed));
    
    /*
     * seed in some current state of the run-time stack (128 bytes)
     */
#if HAVE_VALGRIND && 0
    if (ssl_running_on_valgrind) {
        VALGRIND_MAKE_MEM_DEFINED(stackdata, sizeof(stackdata));
    }
#endif
    n = rand_choosenum(0, sizeof(stackdata)-128-1);
    RAND_seed(stackdata+n, 128);
}

#endif /*ifdef MD_HAVE_ARC4RANDOM (else part) */

#ifdef LIBRESSL_VERSION_NUMBER

/* Extracted from OpenSSL 1.0.2 */

struct tm *OPENSSL_gmtime(const time_t *timer, struct tm *result)
{
    struct tm *ts = NULL;

#if defined(OPENSSL_THREADS) && defined(OPENSSL_SYS_VMS)
    {
        struct tm data, *ts2 = &data;
        if (gmtime_r(timer, ts2) == NULL)
            return NULL;
        memcpy(result, ts2, sizeof(struct tm));
        ts = result;
    }
#elif defined(OPENSSL_THREADS) && !defined(OPENSSL_SYS_WIN32) && !defined(OPENSSL_SYS_MACOSX)
    if (gmtime_r(timer, result) == NULL)
        return NULL;
    ts = result;
#else
    ts = gmtime(timer);
    if (ts == NULL)
        return NULL;

    memcpy(result, ts, sizeof(struct tm));
    ts = result;
#endif
    return ts;
}

#define SECS_PER_DAY (24 * 60 * 60)
# define ASN1_STRING_FLAG_X509_TIME 0x100

static int leap_year(const int year)
{
    if (year % 400 == 0 || (year % 100 != 0 && year % 4 == 0))
        return 1;
    return 0;
}

/*
 * Compute the day of the week and the day of the year from the year, month
 * and day.  The day of the year is straightforward, the day of the week uses
 * a form of Zeller's congruence.  For this months start with March and are
 * numbered 4 through 15.
 */
static void determine_days(struct tm *tm)
{
    static const int ydays[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon;
    int d = tm->tm_mday;
    int c;

    tm->tm_yday = ydays[m] + d - 1;
    if (m >= 2) {
        /* March and onwards can be one day further into the year */
        tm->tm_yday += leap_year(y);
        m += 2;
    } else {
        /* Treat January and February as part of the previous year */
        m += 14;
        y--;
    }
    c = y / 100;
    y %= 100;
    /* Zeller's congruance */
    tm->tm_wday = (d + (13 * m) / 5 + y + y / 4 + c / 4 + 5 * c + 6) % 7;
}

/*
 * Convert date to and from julian day Uses Fliegel & Van Flandern algorithm
 */
static long date_to_julian(int y, int m, int d)
{
    return (1461 * (y + 4800 + (m - 14) / 12)) / 4 +
        (367 * (m - 2 - 12 * ((m - 14) / 12))) / 12 -
        (3 * ((y + 4900 + (m - 14) / 12) / 100)) / 4 + d - 32075;
}

static void julian_to_date(long jd, int *y, int *m, int *d)
{
    long L = jd + 68569;
    long n = (4 * L) / 146097;
    long i, j;

    L = L - (146097 * n + 3) / 4;
    i = (4000 * (L + 1)) / 1461001;
    L = L - (1461 * i) / 4 + 31;
    j = (80 * L) / 2447;
    *d = L - (2447 * j) / 80;
    L = j / 11;
    *m = j + 2 - (12 * L);
    *y = 100 * (n - 49) + i + L;
}

/* Convert tm structure and offset into julian day and seconds */
static int julian_adj(const struct tm *tm, int off_day, long offset_sec,
                      long *pday, int *psec)
{
    int offset_hms, offset_day;
    long time_jd;
    int time_year, time_month, time_day;
    /* split offset into days and day seconds */
    offset_day = offset_sec / SECS_PER_DAY;
    /* Avoid sign issues with % operator */
    offset_hms = offset_sec - (offset_day * SECS_PER_DAY);
    offset_day += off_day;
    /* Add current time seconds to offset */
    offset_hms += tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    /* Adjust day seconds if overflow */
    if (offset_hms >= SECS_PER_DAY) {
        offset_day++;
        offset_hms -= SECS_PER_DAY;
    } else if (offset_hms < 0) {
        offset_day--;
        offset_hms += SECS_PER_DAY;
    }

    /*
     * Convert date of time structure into a Julian day number.
     */

    time_year = tm->tm_year + 1900;
    time_month = tm->tm_mon + 1;
    time_day = tm->tm_mday;

    time_jd = date_to_julian(time_year, time_month, time_day);

    /* Work out Julian day of new date */
    time_jd += offset_day;

    if (time_jd < 0)
        return 0;

    *pday = time_jd;
    *psec = offset_hms;
    return 1;
}

int OPENSSL_gmtime_adj(struct tm *tm, int off_day, long offset_sec)
{
    int time_sec, time_year, time_month, time_day;
    long time_jd;

    /* Convert time and offset into Julian day and seconds */
    if (!julian_adj(tm, off_day, offset_sec, &time_jd, &time_sec))
        return 0;

    /* Convert Julian day back to date */

    julian_to_date(time_jd, &time_year, &time_month, &time_day);

    if (time_year < 1900 || time_year > 9999)
        return 0;

    /* Update tm structure */

    tm->tm_year = time_year - 1900;
    tm->tm_mon = time_month - 1;
    tm->tm_mday = time_day;

    tm->tm_hour = time_sec / 3600;
    tm->tm_min = (time_sec / 60) % 60;
    tm->tm_sec = time_sec % 60;

    return 1;

}

int OPENSSL_gmtime_diff(int *pday, int *psec,
                        const struct tm *from, const struct tm *to)
{
    int from_sec, to_sec, diff_sec;
    long from_jd, to_jd, diff_day;
    if (!julian_adj(from, 0, 0, &from_jd, &from_sec))
        return 0;
    if (!julian_adj(to, 0, 0, &to_jd, &to_sec))
        return 0;
    diff_day = to_jd - from_jd;
    diff_sec = to_sec - from_sec;
    /* Adjust differences so both positive or both negative */
    if (diff_day > 0 && diff_sec < 0) {
        diff_day--;
        diff_sec += SECS_PER_DAY;
    }
    if (diff_day < 0 && diff_sec > 0) {
        diff_day++;
        diff_sec -= SECS_PER_DAY;
    }

    if (pday)
        *pday = (int)diff_day;
    if (psec)
        *psec = diff_sec;

    return 1;

}

int asn1_time_to_tm(struct tm *tm, const ASN1_TIME *d)
{
    static const int min[9] = { 0, 0, 1, 1, 0, 0, 0, 0, 0 };
    static const int max[9] = { 99, 99, 12, 31, 23, 59, 59, 12, 59 };
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    char *a;
    int n, i, i2, l, o, min_l = 11, strict = 0, end = 6, btz = 5, md;
    struct tm tmp;

    /*
     * ASN1_STRING_FLAG_X509_TIME is used to enforce RFC 5280
     * time string format, in which:
     *
     * 1. "seconds" is a 'MUST'
     * 2. "Zulu" timezone is a 'MUST'
     * 3. "+|-" is not allowed to indicate a time zone
     */
    if (d->type == V_ASN1_UTCTIME) {
        if (d->flags & ASN1_STRING_FLAG_X509_TIME) {
            min_l = 13;
            strict = 1;
        }
    } else if (d->type == V_ASN1_GENERALIZEDTIME) {
        end = 7;
        btz = 6;
        if (d->flags & ASN1_STRING_FLAG_X509_TIME) {
            min_l = 15;
            strict = 1;
        } else {
            min_l = 13;
        }
    } else {
        return 0;
    }

    l = d->length;
    a = (char *)d->data;
    o = 0;
    memset(&tmp, 0, sizeof(tmp));

    /*
     * GENERALIZEDTIME is similar to UTCTIME except the year is represented
     * as YYYY. This stuff treats everything as a two digit field so make
     * first two fields 00 to 99
     */

    if (l < min_l)
        goto err;
    for (i = 0; i < end; i++) {
        if (!strict && (i == btz) && ((a[o] == 'Z') || (a[o] == '+') || (a[o] == '-'))) {
            i++;
            break;
        }
        if (!isdigit(a[o]))
            goto err;
        n = a[o] - '0';
        /* incomplete 2-digital number */
        if (++o == l)
            goto err;

        if (!isdigit(a[o]))
            goto err;
        n = (n * 10) + a[o] - '0';
        /* no more bytes to read, but we haven't seen time-zone yet */
        if (++o == l)
            goto err;

        i2 = (d->type == V_ASN1_UTCTIME) ? i + 1 : i;

        if ((n < min[i2]) || (n > max[i2]))
            goto err;
        switch (i2) {
        case 0:
            /* UTC will never be here */
            tmp.tm_year = n * 100 - 1900;
            break;
        case 1:
            if (d->type == V_ASN1_UTCTIME)
                tmp.tm_year = n < 50 ? n + 100 : n;
            else
                tmp.tm_year += n;
            break;
        case 2:
            tmp.tm_mon = n - 1;
            break;
        case 3:
            /* check if tm_mday is valid in tm_mon */
            if (tmp.tm_mon == 1) {
                /* it's February */
                md = mdays[1] + leap_year(tmp.tm_year + 1900);
            } else {
                md = mdays[tmp.tm_mon];
            }
            if (n > md)
                goto err;
            tmp.tm_mday = n;
            determine_days(&tmp);
            break;
        case 4:
            tmp.tm_hour = n;
            break;
        case 5:
            tmp.tm_min = n;
            break;
        case 6:
            tmp.tm_sec = n;
            break;
        }
    }

    /*
     * Optional fractional seconds: decimal point followed by one or more
     * digits.
     */
    if (d->type == V_ASN1_GENERALIZEDTIME && a[o] == '.') {
        if (strict)
            /* RFC 5280 forbids fractional seconds */
            goto err;
        if (++o == l)
            goto err;
        i = o;
        while ((o < l) && isdigit(a[o]))
            o++;
        /* Must have at least one digit after decimal point */
        if (i == o)
            goto err;
        /* no more bytes to read, but we haven't seen time-zone yet */
        if (o == l)
            goto err;
    }

    /*
     * 'o' will never point to '\0' at this point, the only chance
     * 'o' can point to '\0' is either the subsequent if or the first
     * else if is true.
     */
    if (a[o] == 'Z') {
        o++;
    } else if (!strict && ((a[o] == '+') || (a[o] == '-'))) {
        int offsign = a[o] == '-' ? 1 : -1;
        int offset = 0;

        o++;
        /*
         * if not equal, no need to do subsequent checks
         * since the following for-loop will add 'o' by 4
         * and the final return statement will check if 'l'
         * and 'o' are equal.
         */
        if (o + 4 != l)
            goto err;
        for (i = end; i < end + 2; i++) {
            if (!isdigit(a[o]))
                goto err;
            n = a[o] - '0';
            o++;
            if (!isdigit(a[o]))
                goto err;
            n = (n * 10) + a[o] - '0';
            i2 = (d->type == V_ASN1_UTCTIME) ? i + 1 : i;
            if ((n < min[i2]) || (n > max[i2]))
                goto err;
            /* if tm is NULL, no need to adjust */
            if (tm != NULL) {
                if (i == end)
                    offset = n * 3600;
                else if (i == end + 1)
                    offset += n * 60;
            }
            o++;
        }
        if (offset && !OPENSSL_gmtime_adj(&tmp, 0, offset * offsign))
            goto err;
    } else {
        /* not Z, or not +/- in non-strict mode */
        goto err;
    }
    if (o == l) {
        /* success, check if tm should be filled */
        if (tm != NULL)
            *tm = tmp;
        return 1;
    }
 err:
    return 0;
}

int ASN1_TIME_to_tm(const ASN1_TIME *s, struct tm *tm)
{
    if (s == NULL) {
        time_t now_t;

        time(&now_t);
        memset(tm, 0, sizeof(*tm));
        if (OPENSSL_gmtime(&now_t, tm) != NULL)
            return 1;
        return 0;
    }

    return asn1_time_to_tm(tm, s);
}

int ASN1_TIME_diff(int *pday, int *psec,
                   const ASN1_TIME *from, const ASN1_TIME *to)
{
    struct tm tm_from, tm_to;

    if (!ASN1_TIME_to_tm(from, &tm_from))
        return 0;
    if (!ASN1_TIME_to_tm(to, &tm_to))
        return 0;
    return OPENSSL_gmtime_diff(pday, psec, &tm_from, &tm_to);
}

#endif /*ifdef LIBRESSL_VERSION_NUMBER*/


apr_status_t md_crypt_init(apr_pool_t *pool)
{
    (void)pool;
    
    if (!initialized) {
        int pid = getpid();
        
        ERR_load_crypto_strings();
        OpenSSL_add_all_algorithms();
        
        md_log_perror(MD_LOG_MARK, MD_LOG_TRACE2, 0, pool, "initializing RAND"); 
        while (!RAND_status()) {
            seed_RAND(pid);
	}

        initialized = 1;
    }
    return APR_SUCCESS;
}

typedef struct {
    char *data;
    apr_size_t len;
} buffer;

static apr_status_t fwrite_buffer(void *baton, apr_file_t *f, apr_pool_t *p) 
{
    buffer *buf = baton;
    return apr_file_write_full(f, buf->data, buf->len, &buf->len);
}

apr_status_t md_rand_bytes(unsigned char *buf, apr_size_t len, apr_pool_t *p)
{
    apr_status_t rv;
    
    if (len > INT_MAX) {
        return APR_ENOTIMPL;
    }
    if (APR_SUCCESS == (rv = md_crypt_init(p))) {
        RAND_bytes((unsigned char*)buf, (int)len);
    }
    return rv;
}

typedef struct {
    const char *pass_phrase;
    int pass_len;
} passwd_ctx;

static int pem_passwd(char *buf, int size, int rwflag, void *baton)
{
    passwd_ctx *ctx = baton;
    if (ctx->pass_len > 0) {
        if (ctx->pass_len < size) {
            size = (int)ctx->pass_len;
        }
        memcpy(buf, ctx->pass_phrase, (size_t)size);
    }
    return ctx->pass_len;
}

/**************************************************************************************************/
/* private keys */

md_json_t *md_pkey_spec_to_json(const md_pkey_spec_t *spec, apr_pool_t *p)
{
    md_json_t *json = md_json_create(p);
    if (json) {
        switch (spec->type) {
            case MD_PKEY_TYPE_DEFAULT:
                md_json_sets("Default", json, MD_KEY_TYPE, NULL);
                break;
            case MD_PKEY_TYPE_RSA:
                md_json_sets("RSA", json, MD_KEY_TYPE, NULL);
                if (spec->params.rsa.bits >= MD_PKEY_RSA_BITS_MIN) {
                    md_json_setl(spec->params.rsa.bits, json, MD_KEY_BITS, NULL);
                }
                break;
            default:
                md_json_sets("Unsupported", json, MD_KEY_TYPE, NULL);
                break;
        }
    }
    return json;    
}

md_pkey_spec_t *md_pkey_spec_from_json(struct md_json_t *json, apr_pool_t *p)
{
    md_pkey_spec_t *spec = apr_pcalloc(p, sizeof(*spec));
    const char *s;
    long l;
    
    if (spec) {
        s = md_json_gets(json, MD_KEY_TYPE, NULL);
        if (!s || !apr_strnatcasecmp("Default", s)) {
            spec->type = MD_PKEY_TYPE_DEFAULT;
        }
        else if (!apr_strnatcasecmp("RSA", s)) {
            spec->type = MD_PKEY_TYPE_RSA;
            l = md_json_getl(json, MD_KEY_BITS, NULL);
            if (l >= MD_PKEY_RSA_BITS_MIN) {
                spec->params.rsa.bits = (unsigned int)l;
            }
            else {
                spec->params.rsa.bits = MD_PKEY_RSA_BITS_DEF;
            }
        }
    }
    return spec;
}

int md_pkey_spec_eq(md_pkey_spec_t *spec1, md_pkey_spec_t *spec2)
{
    if (spec1 == spec2) {
        return 1;
    }
    if (spec1 && spec2 && spec1->type == spec2->type) {
        switch (spec1->type) {
            case MD_PKEY_TYPE_DEFAULT:
                return 1;
            case MD_PKEY_TYPE_RSA:
                if (spec1->params.rsa.bits == spec2->params.rsa.bits) {
                    return 1;
                }
                break;
        }
    }
    return 0;
}

static md_pkey_t *make_pkey(apr_pool_t *p) 
{
    md_pkey_t *pkey = apr_pcalloc(p, sizeof(*pkey));
    pkey->pool = p;
    return pkey;
}

static apr_status_t pkey_cleanup(void *data)
{
    md_pkey_t *pkey = data;
    if (pkey->pkey) {
        EVP_PKEY_free(pkey->pkey);
        pkey->pkey = NULL;
    }
    return APR_SUCCESS;
}

void md_pkey_free(md_pkey_t *pkey)
{
    pkey_cleanup(pkey);
}

void *md_pkey_get_EVP_PKEY(struct md_pkey_t *pkey)
{
    return pkey->pkey;
}

apr_status_t md_pkey_fload(md_pkey_t **ppkey, apr_pool_t *p, 
                           const char *key, apr_size_t key_len,
                           const char *fname)
{
    apr_status_t rv = APR_ENOENT;
    md_pkey_t *pkey;
    BIO *bf;
    passwd_ctx ctx;
    
    pkey =  make_pkey(p);
    if (NULL != (bf = BIO_new_file(fname, "r"))) {
        ctx.pass_phrase = key;
        ctx.pass_len = (int)key_len;
        
        ERR_clear_error();
        pkey->pkey = PEM_read_bio_PrivateKey(bf, NULL, pem_passwd, &ctx);
        BIO_free(bf);
        
        if (pkey->pkey != NULL) {
            rv = APR_SUCCESS;
            apr_pool_cleanup_register(p, pkey, pkey_cleanup, apr_pool_cleanup_null);
        }
        else {
            unsigned long err = ERR_get_error();
            rv = APR_EINVAL;
            md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, rv, p, 
                          "error loading pkey %s: %s (pass phrase was %snull)", fname,
                          ERR_error_string(err, NULL), key? "not " : ""); 
        }
    }
    *ppkey = (APR_SUCCESS == rv)? pkey : NULL;
    return rv;
}

static apr_status_t pkey_to_buffer(buffer *buffer, md_pkey_t *pkey, apr_pool_t *p,
                                   const char *pass, apr_size_t pass_len)
{
    BIO *bio = BIO_new(BIO_s_mem());
    const EVP_CIPHER *cipher = NULL;
    pem_password_cb *cb = NULL;
    void *cb_baton = NULL;
    passwd_ctx ctx;
    unsigned long err;
    int i;
    
    if (!bio) {
        return APR_ENOMEM;
    }
    if (pass_len > INT_MAX) {
        return APR_EINVAL;
    }
    if (pass && pass_len > 0) {
        ctx.pass_phrase = pass;
        ctx.pass_len = (int)pass_len;
        cb = pem_passwd;
        cb_baton = &ctx;
        cipher = EVP_aes_256_cbc();
        if (!cipher) {
            return APR_ENOTIMPL;
        }
    }
    
    ERR_clear_error();
    if (!PEM_write_bio_PrivateKey(bio, pkey->pkey, cipher, NULL, 0, cb, cb_baton)) {
        BIO_free(bio);
        err = ERR_get_error();
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "PEM_write key: %ld %s", 
                      err, ERR_error_string(err, NULL)); 
        return APR_EINVAL;
    }

    i = BIO_pending(bio);
    if (i > 0) {
        buffer->data = apr_palloc(p, (apr_size_t)i + 1);
        i = BIO_read(bio, buffer->data, i);
        buffer->data[i] = '\0';
        buffer->len = (apr_size_t)i;
    }
    BIO_free(bio);
    return APR_SUCCESS;
}

apr_status_t md_pkey_fsave(md_pkey_t *pkey, apr_pool_t *p, 
                           const char *pass_phrase, apr_size_t pass_len,
                           const char *fname, apr_fileperms_t perms)
{
    buffer buffer;
    apr_status_t rv;
    
    if (APR_SUCCESS == (rv = pkey_to_buffer(&buffer, pkey, p, pass_phrase, pass_len))) {
        return md_util_freplace(fname, perms, p, fwrite_buffer, &buffer); 
    }
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, p, "save pkey %s (%s pass phrase, len=%d)",
                  fname, pass_len > 0? "with" : "without", (int)pass_len); 
    return rv;
}

static apr_status_t gen_rsa(md_pkey_t **ppkey, apr_pool_t *p, unsigned int bits)
{
    EVP_PKEY_CTX *ctx = NULL;
    apr_status_t rv;
    
    *ppkey = make_pkey(p);
    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (ctx 
        && EVP_PKEY_keygen_init(ctx) >= 0
        && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)bits) >= 0
        && EVP_PKEY_keygen(ctx, &(*ppkey)->pkey) >= 0) {
        rv = APR_SUCCESS;
    }
    else {
        md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, 0, p, "error generate pkey RSA %d", bits); 
        *ppkey = NULL;
        rv = APR_EGENERAL;
    }
    
    if (ctx != NULL) {
        EVP_PKEY_CTX_free(ctx);
    }
    return rv;
}

apr_status_t md_pkey_gen(md_pkey_t **ppkey, apr_pool_t *p, md_pkey_spec_t *spec)
{
    md_pkey_type_t ptype = spec? spec->type : MD_PKEY_TYPE_DEFAULT;
    switch (ptype) {
        case MD_PKEY_TYPE_DEFAULT:
            return gen_rsa(ppkey, p, MD_PKEY_RSA_BITS_DEF);
        case MD_PKEY_TYPE_RSA:
            return gen_rsa(ppkey, p, spec->params.rsa.bits);
        default:
            return APR_ENOTIMPL;
    }
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)

#ifndef NID_tlsfeature
#define NID_tlsfeature          1020
#endif

static void RSA_get0_key(const RSA *r,
                         const BIGNUM **n, const BIGNUM **e, const BIGNUM **d)
{
    if (n != NULL)
        *n = r->n;
    if (e != NULL)
        *e = r->e;
    if (d != NULL)
        *d = r->d;
}

#endif

static const char *bn64(const BIGNUM *b, apr_pool_t *p) 
{
    if (b) {
         apr_size_t len = (apr_size_t)BN_num_bytes(b);
         char *buffer = apr_pcalloc(p, len);
         if (buffer) {
            BN_bn2bin(b, (unsigned char *)buffer);
            return md_util_base64url_encode(buffer, len, p);
         }
    }
    return NULL;
}

const char *md_pkey_get_rsa_e64(md_pkey_t *pkey, apr_pool_t *p)
{
    const BIGNUM *e;
    RSA *rsa = EVP_PKEY_get1_RSA(pkey->pkey);
    
    if (!rsa) {
        return NULL;
    }
    RSA_get0_key(rsa, NULL, &e, NULL);
    return bn64(e, p);
}

const char *md_pkey_get_rsa_n64(md_pkey_t *pkey, apr_pool_t *p)
{
    const BIGNUM *n;
    RSA *rsa = EVP_PKEY_get1_RSA(pkey->pkey);
    
    if (!rsa) {
        return NULL;
    }
    RSA_get0_key(rsa, &n, NULL, NULL);
    return bn64(n, p);
}

apr_status_t md_crypt_sign64(const char **psign64, md_pkey_t *pkey, apr_pool_t *p, 
                             const char *d, size_t dlen)
{
    EVP_MD_CTX *ctx = NULL;
    char *buffer;
    unsigned int blen;
    const char *sign64 = NULL;
    apr_status_t rv = APR_ENOMEM;
    
    buffer = apr_pcalloc(p, (apr_size_t)EVP_PKEY_size(pkey->pkey));
    if (buffer) {
        ctx = EVP_MD_CTX_create();
        if (ctx) {
            rv = APR_ENOTIMPL;
            if (EVP_SignInit_ex(ctx, EVP_sha256(), NULL)) {
                rv = APR_EGENERAL;
                if (EVP_SignUpdate(ctx, d, dlen)) {
                    if (EVP_SignFinal(ctx, (unsigned char*)buffer, &blen, pkey->pkey)) {
                        sign64 = md_util_base64url_encode(buffer, blen, p);
                        if (sign64) {
                            rv = APR_SUCCESS;
                        }
                    }
                }
            }
        }
        
        if (ctx) {
            EVP_MD_CTX_destroy(ctx);
        }
    }
    
    if (rv != APR_SUCCESS) {
        md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, rv, p, "signing"); 
    }
    
    *psign64 = sign64;
    return rv;
}

static apr_status_t sha256_digest(unsigned char **pdigest, size_t *pdigest_len,
                                  apr_pool_t *p, const char *d, size_t dlen)
{
    EVP_MD_CTX *ctx = NULL;
    unsigned char *buffer;
    apr_status_t rv = APR_ENOMEM;
    unsigned int blen;
    
    buffer = apr_pcalloc(p, EVP_MAX_MD_SIZE);
    if (buffer) {
        ctx = EVP_MD_CTX_create();
        if (ctx) {
            rv = APR_ENOTIMPL;
            if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) {
                rv = APR_EGENERAL;
                if (EVP_DigestUpdate(ctx, d, dlen)) {
                    if (EVP_DigestFinal(ctx, buffer, &blen)) {
                        rv = APR_SUCCESS;
                    }
                }
            }
        }
        
        if (ctx) {
            EVP_MD_CTX_destroy(ctx);
        }
    }
    
    if (APR_SUCCESS == rv) {
        *pdigest = buffer;
        *pdigest_len = blen;
    }
    else {
        md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, rv, p, "digest"); 
        *pdigest = NULL;
        *pdigest_len = 0;
    }
    return rv;
}

apr_status_t md_crypt_sha256_digest64(const char **pdigest64, apr_pool_t *p, 
                                      const char *d, size_t dlen)
{
    const char *digest64 = NULL;
    unsigned char *buffer;
    size_t blen;
    apr_status_t rv;
    
    if (APR_SUCCESS == (rv = sha256_digest(&buffer, &blen, p, d, dlen))) {
        if (NULL == (digest64 = md_util_base64url_encode((const char*)buffer, blen, p))) {
            rv = APR_EGENERAL;
        }
    }
    *pdigest64 = digest64;
    return rv;
}

static const char * const hex_const[] = {
    "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "0a", "0b", "0c", "0d", "0e", "0f", 
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "1a", "1b", "1c", "1d", "1e", "1f", 
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "2a", "2b", "2c", "2d", "2e", "2f", 
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39", "3a", "3b", "3c", "3d", "3e", "3f", 
    "40", "41", "42", "43", "44", "45", "46", "47", "48", "49", "4a", "4b", "4c", "4d", "4e", "4f", 
    "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "5a", "5b", "5c", "5d", "5e", "5f", 
    "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "6a", "6b", "6c", "6d", "6e", "6f", 
    "70", "71", "72", "73", "74", "75", "76", "77", "78", "79", "7a", "7b", "7c", "7d", "7e", "7f", 
    "80", "81", "82", "83", "84", "85", "86", "87", "88", "89", "8a", "8b", "8c", "8d", "8e", "8f", 
    "90", "91", "92", "93", "94", "95", "96", "97", "98", "99", "9a", "9b", "9c", "9d", "9e", "9f", 
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "aa", "ab", "ac", "ad", "ae", "af", 
    "b0", "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "b9", "ba", "bb", "bc", "bd", "be", "bf", 
    "c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9", "ca", "cb", "cc", "cd", "ce", "cf", 
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "da", "db", "dc", "dd", "de", "df", 
    "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8", "e9", "ea", "eb", "ec", "ed", "ee", "ef", 
    "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "fa", "fb", "fc", "fd", "fe", "ff", 
};

apr_status_t md_crypt_sha256_digest_hex(const char **pdigesthex, apr_pool_t *p, 
                                        const char *d, size_t dlen)
{
    char *dhex = NULL, *cp;
    const char * x;
    unsigned char *buffer;
    size_t blen;
    apr_status_t rv;
    int i;
    
    if (APR_SUCCESS == (rv = sha256_digest(&buffer, &blen, p, d, dlen))) {
        cp = dhex = apr_pcalloc(p,  2 * blen + 1);
        if (!dhex) {
            rv = APR_EGENERAL;
        }
        for (i = 0; i < blen; ++i, cp += 2) {
            x = hex_const[buffer[i]];
            cp[0] = x[0];
            cp[1] = x[1];
        }
    }
    *pdigesthex = dhex;
    return rv;
}

/**************************************************************************************************/
/* certificates */

struct md_cert_t {
    apr_pool_t *pool;
    X509 *x509;
    apr_array_header_t *alt_names;
};

static apr_status_t cert_cleanup(void *data)
{
    md_cert_t *cert = data;
    if (cert->x509) {
        X509_free(cert->x509);
        cert->x509 = NULL;
    }
    return APR_SUCCESS;
}

static md_cert_t *make_cert(apr_pool_t *p, X509 *x509) 
{
    md_cert_t *cert = apr_pcalloc(p, sizeof(*cert));
    cert->pool = p;
    cert->x509 = x509;
    apr_pool_cleanup_register(p, cert, cert_cleanup, apr_pool_cleanup_null);
    
    return cert;
}

void md_cert_free(md_cert_t *cert)
{
    cert_cleanup(cert);
}

void *md_cert_get_X509(struct md_cert_t *cert)
{
    return cert->x509;
}

int md_cert_is_valid_now(const md_cert_t *cert)
{
    return ((X509_cmp_current_time(X509_get_notBefore(cert->x509)) < 0)
            && (X509_cmp_current_time(X509_get_notAfter(cert->x509)) > 0));
}

int md_cert_has_expired(const md_cert_t *cert)
{
    return (X509_cmp_current_time(X509_get_notAfter(cert->x509)) <= 0);
}

apr_time_t md_cert_get_not_after(md_cert_t *cert)
{
    int secs, days;
    apr_time_t time = apr_time_now();
    ASN1_TIME *not_after = X509_get_notAfter(cert->x509);
    
    if (ASN1_TIME_diff(&days, &secs, NULL, not_after)) {
        time += apr_time_from_sec((days * MD_SECS_PER_DAY) + secs); 
    }
    return time;
}

apr_time_t md_cert_get_not_before(md_cert_t *cert)
{
    int secs, days;
    apr_time_t time = apr_time_now();
    ASN1_TIME *not_after = X509_get_notBefore(cert->x509);
    
    if (ASN1_TIME_diff(&days, &secs, NULL, not_after)) {
        time += apr_time_from_sec((days * MD_SECS_PER_DAY) + secs); 
    }
    return time;
}

int md_cert_covers_domain(md_cert_t *cert, const char *domain_name)
{
    if (!cert->alt_names) {
        md_cert_get_alt_names(&cert->alt_names, cert, cert->pool);
    }
    if (cert->alt_names) {
        return md_array_str_index(cert->alt_names, domain_name, 0, 0) >= 0;
    }
    return 0;
}

int md_cert_covers_md(md_cert_t *cert, const md_t *md)
{
    const char *name;
    int i;
    
    if (!cert->alt_names) {
        md_cert_get_alt_names(&cert->alt_names, cert, cert->pool);
    }
    if (cert->alt_names) {
        md_log_perror(MD_LOG_MARK, MD_LOG_TRACE4, 0, cert->pool, "cert has %d alt names",
                      cert->alt_names->nelts); 
        for (i = 0; i < md->domains->nelts; ++i) {
            name = APR_ARRAY_IDX(md->domains, i, const char *);
            if (md_array_str_index(cert->alt_names, name, 0, 0) < 0) {
                md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, 0, cert->pool, 
                              "md domain %s not covered by cert", name);
                return 0;
            }
        }
        return 1;
    }
    else {
        md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, 0, cert->pool, "cert has NO alt names");
    }
    return 0;
}

apr_status_t md_cert_get_issuers_uri(const char **puri, md_cert_t *cert, apr_pool_t *p)
{
    int i, ext_idx, nid = NID_info_access;
    X509_EXTENSION *ext;
    X509V3_EXT_METHOD *ext_cls;
    void *ext_data;
    const char *uri = NULL;
    apr_status_t rv = APR_ENOENT;
    
    /* Waddle through x509  API history to get someone that may be able
     * to hand us the issuer url for the cert chain */
    ext_idx = X509_get_ext_by_NID(cert->x509, nid, -1);
    ext = (ext_idx >= 0)? X509_get_ext(cert->x509, ext_idx) : NULL;
    ext_cls = ext? (X509V3_EXT_METHOD*)X509V3_EXT_get(ext) : NULL;
    if (ext_cls && (ext_data = X509_get_ext_d2i(cert->x509, nid, 0, 0))) {
        CONF_VALUE *cval;
        STACK_OF(CONF_VALUE) *ext_vals = ext_cls->i2v(ext_cls, ext_data, 0);
        
        for (i = 0; i < sk_CONF_VALUE_num(ext_vals); ++i) {
            cval = sk_CONF_VALUE_value(ext_vals, i);
            if (!strcmp("CA Issuers - URI", cval->name)) {
                uri = apr_pstrdup(p, cval->value);
                rv = APR_SUCCESS;
                break;
            }
        }
    } 
    *puri = (APR_SUCCESS == rv)? uri : NULL;
    return rv;
}

apr_status_t md_cert_get_alt_names(apr_array_header_t **pnames, md_cert_t *cert, apr_pool_t *p)
{
    apr_array_header_t *names;
    apr_status_t rv = APR_ENOENT;
    STACK_OF(GENERAL_NAME) *xalt_names;
    unsigned char *buf;
    int i;
    
    xalt_names = (GENERAL_NAMES*)X509_get_ext_d2i(cert->x509, NID_subject_alt_name, NULL, NULL);
    if (xalt_names) {
        GENERAL_NAME *cval;
        
        names = apr_array_make(p, sk_GENERAL_NAME_num(xalt_names), sizeof(char *));
        for (i = 0; i < sk_GENERAL_NAME_num(xalt_names); ++i) {
            cval = sk_GENERAL_NAME_value(xalt_names, i);
            switch (cval->type) {
                case GEN_DNS:
                case GEN_URI:
                case GEN_IPADD:
                    ASN1_STRING_to_UTF8(&buf, cval->d.ia5);
                    APR_ARRAY_PUSH(names, const char *) = apr_pstrdup(p, (char*)buf);
                    OPENSSL_free(buf);
                    break;
                default:
                    break;
            }
        }
        rv = APR_SUCCESS;
    }
    *pnames = (APR_SUCCESS == rv)? names : NULL;
    return rv;
}

apr_status_t md_cert_fload(md_cert_t **pcert, apr_pool_t *p, const char *fname)
{
    FILE *f;
    apr_status_t rv;
    md_cert_t *cert;
    X509 *x509;
    
    rv = md_util_fopen(&f, fname, "r");
    if (rv == APR_SUCCESS) {
    
        x509 = PEM_read_X509(f, NULL, NULL, NULL);
        rv = fclose(f);
        if (x509 != NULL) {
            cert =  make_cert(p, x509);
        }
        else {
            rv = APR_EINVAL;
        }
    }

    *pcert = (APR_SUCCESS == rv)? cert : NULL;
    return rv;
}

static apr_status_t cert_to_buffer(buffer *buffer, md_cert_t *cert, apr_pool_t *p)
{
    BIO *bio = BIO_new(BIO_s_mem());
    int i;
    
    if (!bio) {
        return APR_ENOMEM;
    }

    ERR_clear_error();
    PEM_write_bio_X509(bio, cert->x509);
    if (ERR_get_error() > 0) {
        BIO_free(bio);
        return APR_EINVAL;
    }

    i = BIO_pending(bio);
    if (i > 0) {
        buffer->data = apr_palloc(p, (apr_size_t)i + 1);
        i = BIO_read(bio, buffer->data, i);
        buffer->data[i] = '\0';
        buffer->len = (apr_size_t)i;
    }
    BIO_free(bio);
    return APR_SUCCESS;
}

apr_status_t md_cert_fsave(md_cert_t *cert, apr_pool_t *p, 
                           const char *fname, apr_fileperms_t perms)
{
    buffer buffer;
    apr_status_t rv;
    
    if (APR_SUCCESS == (rv = cert_to_buffer(&buffer, cert, p))) {
        return md_util_freplace(fname, perms, p, fwrite_buffer, &buffer); 
    }
    return rv;
}

apr_status_t md_cert_to_base64url(const char **ps64, md_cert_t *cert, apr_pool_t *p)
{
    buffer buffer;
    apr_status_t rv;
    
    if (APR_SUCCESS == (rv = cert_to_buffer(&buffer, cert, p))) {
        *ps64 = md_util_base64url_encode(buffer.data, buffer.len, p);
        return APR_SUCCESS;
    }
    *ps64 = NULL;
    return rv;
}

apr_status_t md_cert_read_http(md_cert_t **pcert, apr_pool_t *p, 
                               const md_http_response_t *res)
{
    const char *ct;
    apr_off_t data_len;
    apr_size_t der_len;
    apr_status_t rv;
    
    ct = apr_table_get(res->headers, "Content-Type");
    if (!res->body || !ct  || strcmp("application/pkix-cert", ct)) {
        return APR_ENOENT;
    }
    
    if (APR_SUCCESS == (rv = apr_brigade_length(res->body, 1, &data_len))) {
        char *der;
        if (data_len > 1024*1024) { /* certs usually are <2k each */
            return APR_EINVAL;
        }
        if (APR_SUCCESS == (rv = apr_brigade_pflatten(res->body, &der, &der_len, p))) {
            const unsigned char *bf = (const unsigned char*)der;
            X509 *x509;
            
            if (NULL == (x509 = d2i_X509(NULL, &bf, (long)der_len))) {
                rv = APR_EINVAL;
            }
            else {
                *pcert = make_cert(p, x509);
                rv = APR_SUCCESS;
            }
        }
        md_log_perror(MD_LOG_MARK, MD_LOG_TRACE3, rv, p, "cert parsed");
    }
    return rv;
}

md_cert_state_t md_cert_state_get(md_cert_t *cert)
{
    if (cert->x509) {
        return md_cert_is_valid_now(cert)? MD_CERT_VALID : MD_CERT_EXPIRED;
    }
    return MD_CERT_UNKNOWN;
}

apr_status_t md_chain_fappend(struct apr_array_header_t *certs, apr_pool_t *p, const char *fname)
{
    FILE *f;
    apr_status_t rv;
    X509 *x509;
    md_cert_t *cert;
    unsigned long err;
    
    rv = md_util_fopen(&f, fname, "r");
    if (rv == APR_SUCCESS) {
        ERR_clear_error();
        while (NULL != (x509 = PEM_read_X509(f, NULL, NULL, NULL))) {
            cert = make_cert(p, x509);
            APR_ARRAY_PUSH(certs, md_cert_t *) = cert;
        }
        fclose(f);
        
        if (0 < (err =  ERR_get_error())
            && !(ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE)) {
            /* not the expected one when no more PEM encodings are found */
            rv = APR_EINVAL;
            goto out;
        }
        
        if (certs->nelts == 0) {
            /* Did not find any. This is acceptable unless the file has a certain size
             * when we no longer accept it as empty chain file. Something seems to be
             * wrong then. */
            apr_finfo_t info;
            if (APR_SUCCESS == apr_stat(&info, fname, APR_FINFO_SIZE, p) && info.size >= 1024) {
                /* "Too big for a moon." */
                rv = APR_EINVAL;
                md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, 
                              "no certificates in non-empty chain %s", fname);
                goto out;
            }
        }        
    }
out:
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE3, rv, p, "read chain file %s, found %d certs", 
                  fname, certs? certs->nelts : 0);
    return rv;
}

apr_status_t md_chain_fload(apr_array_header_t **pcerts, apr_pool_t *p, const char *fname)
{
    apr_array_header_t *certs;
    apr_status_t rv;

    certs = apr_array_make(p, 5, sizeof(md_cert_t *));
    rv = md_chain_fappend(certs, p, fname);
    *pcerts = (APR_SUCCESS == rv)? certs : NULL;
    return rv;
}

apr_status_t md_chain_fsave(apr_array_header_t *certs, apr_pool_t *p, 
                            const char *fname, apr_fileperms_t perms)
{
    FILE *f;
    apr_status_t rv;
    const md_cert_t *cert;
    unsigned long err = 0;
    int i;
    
    rv = md_util_fopen(&f, fname, "w");
    if (rv == APR_SUCCESS) {
        apr_file_perms_set(fname, perms);
        ERR_clear_error();
        for (i = 0; i < certs->nelts; ++i) {
            cert = APR_ARRAY_IDX(certs, i, const md_cert_t *);
            assert(cert->x509);
            
            PEM_write_X509(f, cert->x509);
            
            if (0 < (err = ERR_get_error())) {
                break;
            }
            
        }
        rv = fclose(f);
        if (err) {
            rv = APR_EINVAL;
        }
    }
    return rv;
}

/**************************************************************************************************/
/* certificate signing requests */

static const char *alt_name(const char *domain, apr_pool_t *p)
{
    return apr_psprintf(p, "DNS:%s", domain);
}

static const char *alt_names(apr_array_header_t *domains, apr_pool_t *p)
{
    const char *alts = "", *sep = "", *domain;
    int i;
    
    for (i = 0; i < domains->nelts; ++i) {
        domain = APR_ARRAY_IDX(domains, i, const char *);
        alts = apr_psprintf(p, "%s%sDNS:%s", alts, sep, domain);
        sep = ",";
    }
    return alts;
}

static apr_status_t add_ext(X509 *x, int nid, const char *value, apr_pool_t *p)
{
    X509_EXTENSION *ext = NULL;
    X509V3_CTX ctx;
    apr_status_t rv;

    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, NULL, NULL, 0);
    if (NULL == (ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, (char*)value))) {
        return APR_EGENERAL;
    }
    
    ERR_clear_error();
    rv = X509_add_ext(x, ext, -1)? APR_SUCCESS : APR_EINVAL;
    if (APR_SUCCESS != rv) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "add_ext nid=%dd value='%s'", 
                      nid, value); 
        
    }
    X509_EXTENSION_free(ext);
    return rv;
}

static apr_status_t sk_add_alt_names(STACK_OF(X509_EXTENSION) *exts,
                                     apr_array_header_t *domains, apr_pool_t *p)
{
    if (domains->nelts > 0) {
        X509_EXTENSION *x;
        
        x = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, (char*)alt_names(domains, p));
        if (NULL == x) {
            return APR_EGENERAL;
        }
        sk_X509_EXTENSION_push(exts, x);
    }
    return APR_SUCCESS;
}

static apr_status_t add_must_staple(STACK_OF(X509_EXTENSION) *exts, const md_t *md, apr_pool_t *p)
{
    
    if (md->must_staple) {
        X509_EXTENSION *x = X509V3_EXT_conf_nid(NULL, NULL, 
                                                NID_tlsfeature, (char*)"DER:30:03:02:01:05");
        if (NULL == x) {
            return APR_EGENERAL;
        }
        sk_X509_EXTENSION_push(exts, x);
    }
    return APR_SUCCESS;
}

apr_status_t md_cert_req_create(const char **pcsr_der_64, const md_t *md, 
                                md_pkey_t *pkey, apr_pool_t *p)
{
    const char *s, *csr_der, *csr_der_64 = NULL;
    const unsigned char *domain;
    X509_REQ *csr;
    X509_NAME *n = NULL;
    STACK_OF(X509_EXTENSION) *exts = NULL;
    apr_status_t rv;
    int csr_der_len;
    
    assert(md->domains->nelts > 0);
    
    if (NULL == (csr = X509_REQ_new()) 
        || NULL == (exts = sk_X509_EXTENSION_new_null())
        || NULL == (n = X509_NAME_new())) {
        rv = APR_ENOMEM;
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: openssl alloc X509 things", md->name);
        goto out; 
    }

    /* subject name == first domain */
    domain = APR_ARRAY_IDX(md->domains, 0, const unsigned char *);
    if (!X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, domain, -1, -1, 0)
        || !X509_REQ_set_subject_name(csr, n)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "%s: REQ name add entry", md->name);
        rv = APR_EGENERAL; goto out;
    }
    /* collect extensions, such as alt names and must staple */
    if (APR_SUCCESS != (rv = sk_add_alt_names(exts, md->domains, p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: collecting alt names", md->name);
        rv = APR_EGENERAL; goto out;
    }
    if (APR_SUCCESS != (rv = add_must_staple(exts, md, p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: must staple", md->name);
        rv = APR_EGENERAL; goto out;
    }
    /* add extensions to csr */
    if (sk_X509_EXTENSION_num(exts) > 0 && !X509_REQ_add_extensions(csr, exts)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: adding exts", md->name);
        rv = APR_EGENERAL; goto out;
    }
    /* add our key */
    if (!X509_REQ_set_pubkey(csr, pkey->pkey)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: set pkey in csr", md->name);
        rv = APR_EGENERAL; goto out;
    }
    /* sign, der encode and base64url encode */
    if (!X509_REQ_sign(csr, pkey->pkey, EVP_sha256())) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: sign csr", md->name);
        rv = APR_EGENERAL; goto out;
    }
    if ((csr_der_len = i2d_X509_REQ(csr, NULL)) < 0) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: der length", md->name);
        rv = APR_EGENERAL; goto out;
    }
    s = csr_der = apr_pcalloc(p, (apr_size_t)csr_der_len + 1);
    if (i2d_X509_REQ(csr, (unsigned char**)&s) != csr_der_len) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: csr der enc", md->name);
        rv = APR_EGENERAL; goto out;
    }
    csr_der_64 = md_util_base64url_encode(csr_der, (apr_size_t)csr_der_len, p);
    rv = APR_SUCCESS;
    
out:
    if (exts) {
        sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    }
    if (csr) {
        X509_REQ_free(csr);
    }
    if (n) {
        X509_NAME_free(n);
    }
    *pcsr_der_64 = (APR_SUCCESS == rv)? csr_der_64 : NULL;
    return rv;
}

apr_status_t md_cert_self_sign(md_cert_t **pcert, const char *cn, 
                               const char *domain, md_pkey_t *pkey,
                               apr_interval_time_t valid_for, apr_pool_t *p)
{
    X509 *x;
    X509_NAME *n = NULL;
    md_cert_t *cert = NULL;
    apr_status_t rv;
    int days;
    BIGNUM *big_rnd = NULL;
    ASN1_INTEGER *asn1_rnd = NULL;
    unsigned char rnd[20];
    
    assert(domain);
    
    if (NULL == (x = X509_new()) 
        || NULL == (n = X509_NAME_new())) {
        rv = APR_ENOMEM;
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "%s: openssl alloc X509 things", domain);
        goto out; 
    }
    
    if (APR_SUCCESS != (rv = md_rand_bytes(rnd, sizeof(rnd), p))
        || !(big_rnd = BN_bin2bn(rnd, sizeof(rnd), NULL))
        || !(asn1_rnd = BN_to_ASN1_INTEGER(big_rnd, NULL))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "%s: setup random serial", domain);
        rv = APR_EGENERAL; goto out;
    } 
    if (!X509_set_serialNumber(x, asn1_rnd)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "%s: set serial number", domain);
        rv = APR_EGENERAL; goto out;
    }
    /* set common name and issue */
    if (!X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char*)cn, -1, -1, 0)
        || !X509_set_subject_name(x, n)
        || !X509_set_issuer_name(x, n)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "%s: name add entry", domain);
        rv = APR_EGENERAL; goto out;
    }
    /* cert are uncontrained (but not very trustworthy) */
    if (APR_SUCCESS != (rv = add_ext(x, NID_basic_constraints, "CA:TRUE, pathlen:0", p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: set basic constraints ext", domain);
        goto out;
    }
    /* add the domain as alt name */
    if (APR_SUCCESS != (rv = add_ext(x, NID_subject_alt_name, alt_name(domain, p), p))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: set alt_name ext", domain);
        goto out;
    }
    /* add our key */
    if (!X509_set_pubkey(x, pkey->pkey)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: set pkey in x509", domain);
        rv = APR_EGENERAL; goto out;
    }
    
    days = (int)((apr_time_sec(valid_for) + MD_SECS_PER_DAY - 1)/ MD_SECS_PER_DAY);
    if (!X509_set_notBefore(x, ASN1_TIME_set(NULL, time(NULL)))) {
        rv = APR_EGENERAL; goto out;
    }
    if (!X509_set_notAfter(x, ASN1_TIME_adj(NULL, time(NULL), days, 0))) {
        rv = APR_EGENERAL; goto out;
    }

    /* sign with same key */
    if (!X509_sign(x, pkey->pkey, EVP_sha256())) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: sign x509", domain);
        rv = APR_EGENERAL; goto out;
    }

    cert = make_cert(p, x);
    rv = APR_SUCCESS;
    
out:
    if (!cert && x) {
        X509_free(x);
    }
    if (n) {
        X509_NAME_free(n);
    }
    if (big_rnd) {
        BN_free(big_rnd);
    }
    if (asn1_rnd) {
        ASN1_INTEGER_free(asn1_rnd);
    }
    *pcert = (APR_SUCCESS == rv)? cert : NULL;
    return rv;
}

