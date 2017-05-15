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

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_buckets.h>

#include "md_crypt.h"
#include "md_json.h"
#include "md_jws.h"
#include "md_log.h"
#include "md_util.h"

static int header_set(void *data, const char *key, const char *val)
{
    md_json_sets(val, (md_json*)data, key, NULL);
    return 1;
}

apr_status_t md_jws_sign(md_json **pmsg, apr_pool_t *p,
                         const char *payload, size_t len, 
                         struct apr_table_t *protected, 
                         struct md_pkey *pkey, const char *key_id)
{
    md_json *msg;
    const char *prot64, *pay64, *sign64, *sign, *prot;
    apr_status_t status = APR_ENOMEM;

    prot = NULL;
    *pmsg = NULL;
    
    msg = md_json_create(p);
    if (msg) {
        md_json *jprotected = md_json_create(p);
        if (jprotected) {
            md_json_sets("RS256", jprotected, "alg", NULL);
            if (key_id) {
                md_json_sets(key_id, jprotected, "kid", NULL);
            }
            else {
                md_json_sets(md_crypt_pkey_get_rsa_e64(pkey, p), jprotected, "jwk", "e", NULL);
                md_json_sets("RSA", jprotected, "jwk", "kty", NULL);
                md_json_sets(md_crypt_pkey_get_rsa_n64(pkey, p), jprotected, "jwk", "n", NULL);
            }
            apr_table_do(header_set, jprotected, protected, NULL);
            prot = md_json_writep(jprotected, MD_JSON_FMT_INDENT, p);
            md_log_perror(MD_LOG_MARK, MD_LOG_TRACE4, 0, p, "protected: %s", prot); 
        }
    }
    
    if (prot) {
        prot64 = md_util_base64url_encode(prot, strlen(prot), p);
        if (prot64) {
            md_json_sets(prot64, msg, "protected", NULL);
            pay64 = md_util_base64url_encode(payload, len, p);
            if (pay64) {
                md_json_sets(pay64, msg, "payload", NULL);
                sign = apr_psprintf(p, "%s.%s", prot64, pay64);
                if (sign) {
                    status = md_crypt_sign64(&sign64, pkey, p, sign, strlen(sign));
                    if (status == APR_SUCCESS) {
                        md_json_sets(sign64, msg, "signature", NULL);
                        *pmsg = msg;
                        status = APR_SUCCESS;
                    }
                }
            }
        }
    }
    
    if (status != APR_SUCCESS) {
        md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, status, p, "jwk signed message");
    } 
    return status;
}