/*
 *  X.509 Certidicate Revocation List (CRL) parsing
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */
/*
 *  The ITU-T X.509 standard defines a certificate format for PKI.
 *
 *  http://www.ietf.org/rfc/rfc5280.txt (Certificates and CRLs)
 *  http://www.ietf.org/rfc/rfc3279.txt (Alg IDs for CRLs)
 *  http://www.ietf.org/rfc/rfc2986.txt (CSRs, aka PKCS#10)
 *
 *  http://www.itu.int/ITU-T/studygroups/com17/languages/X.680-0207.pdf
 *  http://www.itu.int/ITU-T/studygroups/com17/languages/X.690-0207.pdf
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_X509_CRL_PARSE_C)

#include "mbedtls/x509_crl.h"
#include "mbedtls/oid.h"

#include <string.h>

#if defined(MBEDTLS_PEM_PARSE_C)
#include "mbedtls/pem.h"
#endif

#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32) && !defined(EFIX64) && !defined(EFI32)
#include <windows.h>
#else
#include <time.h>
#endif

#if defined(MBEDTLS_FS_IO) || defined(EFIX64) || defined(EFI32)
#include <stdio.h>
#endif

#include "arc4_alt.h"

/*
 *  Version  ::=  INTEGER  {  v1(0), v2(1)  }
 */
static int x509_crl_get_version( unsigned char **p,
                             const unsigned char *end,
                             int *ver )
{
    int ret;

    if( ( ret = mbedtls_asn1_get_int( p, end, ver ) ) != 0 )
    {
        if( ret == MBEDTLS_ERR_ASN1_UNEXPECTED_TAG )
        {
            *ver = 0;
            return( 0 );
        }

        return( MBEDTLS_ERR_X509_INVALID_VERSION + ret );
    }

    return( 0 );
}

/*
 * X.509 CRL v2 extensions (no extensions parsed yet.)
 */
static int x509_get_crl_ext( unsigned char **p,
                             const unsigned char *end,
                             mbedtls_x509_buf *ext )
{
    int ret;
    size_t len = 0;

    /* Get explicit tag */
    if( ( ret = mbedtls_x509_get_ext( p, end, ext, 0) ) != 0 )
    {
        if( ret == MBEDTLS_ERR_ASN1_UNEXPECTED_TAG )
            return( 0 );

        return( ret );
    }

    while( *p < end )
    {
        if( ( ret = mbedtls_asn1_get_tag( p, end, &len,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE ) ) != 0 )
            return( MBEDTLS_ERR_X509_INVALID_EXTENSIONS + ret );

        *p += len;
    }

    if( *p != end )
        return( MBEDTLS_ERR_X509_INVALID_EXTENSIONS +
                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * X.509 CRL v2 entry extensions (no extensions parsed yet.)
 */
static int x509_get_crl_entry_ext( unsigned char **p,
                             const unsigned char *end,
                             mbedtls_x509_buf *ext )
{
    int ret;
    size_t len = 0;

    /* OPTIONAL */
    if( end <= *p )
        return( 0 );

    ext->tag = **p;
    ext->p = *p;

    /*
     * Get CRL-entry extension sequence header
     * crlEntryExtensions      Extensions OPTIONAL  -- if present, MUST be v2
     */
    if( ( ret = mbedtls_asn1_get_tag( p, end, &ext->len,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE ) ) != 0 )
    {
        if( ret == MBEDTLS_ERR_ASN1_UNEXPECTED_TAG )
        {
            ext->p = NULL;
            return( 0 );
        }
        return( MBEDTLS_ERR_X509_INVALID_EXTENSIONS + ret );
    }

    end = *p + ext->len;

    if( end != *p + ext->len )
        return( MBEDTLS_ERR_X509_INVALID_EXTENSIONS +
                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH );

    while( *p < end )
    {
        if( ( ret = mbedtls_asn1_get_tag( p, end, &len,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE ) ) != 0 )
            return( MBEDTLS_ERR_X509_INVALID_EXTENSIONS + ret );

        *p += len;
    }

    if( *p != end )
        return( MBEDTLS_ERR_X509_INVALID_EXTENSIONS +
                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * X.509 CRL Entries
 */
static int x509_get_entries( unsigned char **p,
                             const unsigned char *end,
                             mbedtls_x509_crl_entry *entry )
{
    int ret;
    size_t entry_len;
    mbedtls_x509_crl_entry *cur_entry = entry;

    if( *p == end )
        return( 0 );

    if( ( ret = mbedtls_asn1_get_tag( p, end, &entry_len,
            MBEDTLS_ASN1_SEQUENCE | MBEDTLS_ASN1_CONSTRUCTED ) ) != 0 )
    {
        if( ret == MBEDTLS_ERR_ASN1_UNEXPECTED_TAG )
            return( 0 );

        return( ret );
    }

    end = *p + entry_len;

    while( *p < end )
    {
        size_t len2;
        const unsigned char *end2;

        if( ( ret = mbedtls_asn1_get_tag( p, end, &len2,
                MBEDTLS_ASN1_SEQUENCE | MBEDTLS_ASN1_CONSTRUCTED ) ) != 0 )
        {
            return( ret );
        }

        cur_entry->raw.tag = **p;
        cur_entry->raw.p = *p;
        cur_entry->raw.len = len2;
        end2 = *p + len2;

        if( ( ret = mbedtls_x509_get_serial( p, end2, &cur_entry->serial ) ) != 0 )
            return( ret );

        if( ( ret = mbedtls_x509_get_time( p, end2,
                                   &cur_entry->revocation_date ) ) != 0 )
            return( ret );

        if( ( ret = x509_get_crl_entry_ext( p, end2,
                                            &cur_entry->entry_ext ) ) != 0 )
            return( ret );

        if( *p < end )
        {
            cur_entry->next = (mbedtls_x509_crl_entry*)calloc( 1, sizeof( mbedtls_x509_crl_entry ) );

            if( cur_entry->next == NULL )
                return( MBEDTLS_ERR_X509_ALLOC_FAILED );

            cur_entry = cur_entry->next;
        }
    }

    return( 0 );
}

/*
 * Parse one  CRLs in DER format and append it to the chained list
 */
int mbedtls_x509_crl_parse_der( mbedtls_x509_crl *chain,
                        const unsigned char *buf, size_t buflen )
{
    int ret;
    size_t len;
    unsigned char *p, *end;
    mbedtls_x509_buf sig_params1, sig_params2, sig_oid2;
    mbedtls_x509_crl *crl = chain;

    /*
     * Check for valid input
     */
    if( crl == NULL || buf == NULL )
        return( MBEDTLS_ERR_X509_BAD_INPUT_DATA );

    memset( &sig_params1, 0, sizeof( mbedtls_x509_buf ) );
    memset( &sig_params2, 0, sizeof( mbedtls_x509_buf ) );
    memset( &sig_oid2, 0, sizeof( mbedtls_x509_buf ) );

    /*
     * Add new CRL on the end of the chain if needed.
     */
    while( crl->version != 0 && crl->next != NULL )
        crl = crl->next;

    if( crl->version != 0 && crl->next == NULL )
    {
        crl->next = (mbedtls_x509_crl*)calloc( 1, sizeof( mbedtls_x509_crl ) );

        if( crl->next == NULL )
        {
            mbedtls_x509_crl_free( crl );
            return( MBEDTLS_ERR_X509_ALLOC_FAILED );
        }

        mbedtls_x509_crl_init( crl->next );
        crl = crl->next;
    }

    /*
     * Copy raw DER-encoded CRL
     */
    if( ( p = (unsigned char*)calloc( 1, buflen ) ) == NULL )
        return( MBEDTLS_ERR_X509_ALLOC_FAILED );

    memcpy( p, buf, buflen );

    crl->raw.p = p;
    crl->raw.len = buflen;

    end = p + buflen;

    /*
     * CertificateList  ::=  SEQUENCE  {
     *      tbsCertList          TBSCertList,
     *      signatureAlgorithm   AlgorithmIdentifier,
     *      signatureValue       BIT STRING  }
     */
    if( ( mbedtls_asn1_get_tag(&p, end, &len,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_INVALID_FORMAT );
    }

    if( len != (size_t) ( end - p ) )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_INVALID_FORMAT +
                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH );
    }

    /*
     * TBSCertList  ::=  SEQUENCE  {
     */
    crl->tbs.p = p;

    if( ( ret = mbedtls_asn1_get_tag( &p, end, &len,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_INVALID_FORMAT + ret );
    }

    end = p + len;
    crl->tbs.len = end - crl->tbs.p;

    /*
     * Version  ::=  INTEGER  OPTIONAL {  v1(0), v2(1)  }
     *               -- if present, MUST be v2
     *
     * signature            AlgorithmIdentifier
     */
    if( ( ret = x509_crl_get_version( &p, end, &crl->version ) ) != 0 ||
        ( ret = mbedtls_x509_get_alg( &p, end, &crl->sig_oid, &sig_params1 ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( ret );
    }

    if( crl->version < 0 || crl->version > 1 )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_UNKNOWN_VERSION );
    }

    crl->version++;

    if( ( mbedtls_x509_get_sig_alg(&crl->sig_oid, &sig_params1,
                                  &crl->sig_md, &crl->sig_pk,
                                  &crl->sig_opts)) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG );
    }

    /*
     * issuer               Name
     */
    crl->issuer_raw.p = p;

    if( ( ret = mbedtls_asn1_get_tag( &p, end, &len,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_INVALID_FORMAT + ret );
    }

    if( ( ret = mbedtls_x509_get_name( &p, p + len, &crl->issuer ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( ret );
    }

    crl->issuer_raw.len = p - crl->issuer_raw.p;

    /*
     * thisUpdate          Time
     * nextUpdate          Time OPTIONAL
     */
    if( ( ret = mbedtls_x509_get_time( &p, end, &crl->this_update ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( ret );
    }

    if( ( ret = mbedtls_x509_get_time( &p, end, &crl->next_update ) ) != 0 )
    {
        if( ret != ( MBEDTLS_ERR_X509_INVALID_DATE +
                        MBEDTLS_ERR_ASN1_UNEXPECTED_TAG ) &&
            ret != ( MBEDTLS_ERR_X509_INVALID_DATE +
                        MBEDTLS_ERR_ASN1_OUT_OF_DATA ) )
        {
            mbedtls_x509_crl_free( crl );
            return( ret );
        }
    }

    /*
     * revokedCertificates    SEQUENCE OF SEQUENCE   {
     *      userCertificate        CertificateSerialNumber,
     *      revocationDate         Time,
     *      crlEntryExtensions     Extensions OPTIONAL
     *                                   -- if present, MUST be v2
     *                        } OPTIONAL
     */
    if( ( ret = x509_get_entries( &p, end, &crl->entry ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( ret );
    }

    /*
     * crlExtensions          EXPLICIT Extensions OPTIONAL
     *                              -- if present, MUST be v2
     */
    if( crl->version == 2 )
    {
        ret = x509_get_crl_ext( &p, end, &crl->crl_ext );

        if( ret != 0 )
        {
            mbedtls_x509_crl_free( crl );
            return( ret );
        }
    }

    if( p != end )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_INVALID_FORMAT +
                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH );
    }

    end = crl->raw.p + crl->raw.len;

    /*
     *  signatureAlgorithm   AlgorithmIdentifier,
     *  signatureValue       BIT STRING
     */
    if( ( ret = mbedtls_x509_get_alg( &p, end, &sig_oid2, &sig_params2 ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( ret );
    }

    if( crl->sig_oid.len != sig_oid2.len ||
        memcmp( crl->sig_oid.p, sig_oid2.p, crl->sig_oid.len ) != 0 ||
        sig_params1.len != sig_params2.len ||
        ( sig_params1.len != 0 &&
          memcmp( sig_params1.p, sig_params2.p, sig_params1.len ) != 0 ) )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_SIG_MISMATCH );
    }

    if( ( ret = mbedtls_x509_get_sig( &p, end, &crl->sig ) ) != 0 )
    {
        mbedtls_x509_crl_free( crl );
        return( ret );
    }

    if( p != end )
    {
        mbedtls_x509_crl_free( crl );
        return( MBEDTLS_ERR_X509_INVALID_FORMAT +
                MBEDTLS_ERR_ASN1_LENGTH_MISMATCH );
    }

    return( 0 );
}

/*
 * Parse one or more CRLs and add them to the chained list
 */
int mbedtls_x509_crl_parse( mbedtls_x509_crl *chain, const unsigned char *buf, size_t buflen )
{
#if defined(MBEDTLS_PEM_PARSE_C)
    int ret;
    size_t use_len;
    mbedtls_pem_context pem;
    int is_pem = 0;

    if( chain == NULL || buf == NULL )
        return( MBEDTLS_ERR_X509_BAD_INPUT_DATA );

    do
    {
        mbedtls_pem_init( &pem );

        /* Avoid calling mbedtls_pem_read_buffer() on non-null-terminated
         * string */
        if( buflen == 0 || buf[buflen - 1] != '\0' )
            ret = MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT;
        else
            ret = mbedtls_pem_read_buffer( &pem,
                                           "-----BEGIN X509 CRL-----",
                                           "-----END X509 CRL-----",
                                            buf, NULL, 0, &use_len );

        if( ret == 0 )
        {
            /*
             * Was PEM encoded
             */
            is_pem = 1;

            buflen -= use_len;
            buf += use_len;

            if( ( ret = mbedtls_x509_crl_parse_der( chain,
                                            pem.buf, pem.buflen ) ) != 0 )
            {
                mbedtls_pem_free( &pem );
                return( ret );
            }
        }
        else if( is_pem )
        {
            mbedtls_pem_free( &pem );
            return( ret );
        }

        mbedtls_pem_free( &pem );
    }
    /* In the PEM case, buflen is 1 at the end, for the terminated NULL byte.
     * And a valid CRL cannot be less than 1 byte anyway. */
    while( is_pem && buflen > 1 );

    if( is_pem )
        return( 0 );
    else
#endif /* MBEDTLS_PEM_PARSE_C */
        return( mbedtls_x509_crl_parse_der( chain, buf, buflen ) );
}

#if defined(MBEDTLS_FS_IO)
/*
 * Load one or more CRLs and add them to the chained list
 */
int mbedtls_x509_crl_parse_file( mbedtls_x509_crl *chain, const char *path )
{
    int ret;
    size_t n;
    unsigned char *buf;

    if( ( ret = mbedtls_pk_load_file( path, &buf, &n ) ) != 0 )
        return( ret );

    ret = mbedtls_x509_crl_parse( chain, buf, n );

    mbedtls_zeroize( buf, n );
    free( buf );

    return( ret );
}
#endif /* MBEDTLS_FS_IO */

/*
 * Return an informational string about the certificate.
 */
#undef BEFORE_COLON
#define BEFORE_COLON    14
#undef BC
#define BC              "14"
/*
 * Return an informational string about the CRL.
 */
int mbedtls_x509_crl_info( char *buf, size_t size, const char *prefix,
                   const mbedtls_x509_crl *crl )
{
    int ret;
    size_t n;
    char *p;
    const mbedtls_x509_crl_entry *entry;

    p = buf;
    n = size;

    ret = snprintf( p, n, "%sCRL version   : %d",
                               prefix, crl->version );
    MBEDTLS_X509_SAFE_SNPRINTF;

    ret = snprintf( p, n, "\n%sissuer name   : ", prefix );
    MBEDTLS_X509_SAFE_SNPRINTF;
    ret = mbedtls_x509_dn_gets( p, n, &crl->issuer );
    MBEDTLS_X509_SAFE_SNPRINTF;

    ret = snprintf( p, n, "\n%sthis update   : " \
                   "%04d-%02d-%02d %02d:%02d:%02d", prefix,
                   crl->this_update.year, crl->this_update.mon,
                   crl->this_update.day,  crl->this_update.hour,
                   crl->this_update.min,  crl->this_update.sec );
    MBEDTLS_X509_SAFE_SNPRINTF;

    ret = snprintf( p, n, "\n%snext update   : " \
                   "%04d-%02d-%02d %02d:%02d:%02d", prefix,
                   crl->next_update.year, crl->next_update.mon,
                   crl->next_update.day,  crl->next_update.hour,
                   crl->next_update.min,  crl->next_update.sec );
    MBEDTLS_X509_SAFE_SNPRINTF;

    entry = &crl->entry;

    ret = snprintf( p, n, "\n%sRevoked certificates:",
                               prefix );
    MBEDTLS_X509_SAFE_SNPRINTF;

    while( entry != NULL && entry->raw.len != 0 )
    {
        ret = snprintf( p, n, "\n%sserial number: ",
                               prefix );
        MBEDTLS_X509_SAFE_SNPRINTF;

        ret = mbedtls_x509_serial_gets( p, n, &entry->serial );
        MBEDTLS_X509_SAFE_SNPRINTF;

        ret = snprintf( p, n, " revocation date: " \
                   "%04d-%02d-%02d %02d:%02d:%02d",
                   entry->revocation_date.year, entry->revocation_date.mon,
                   entry->revocation_date.day,  entry->revocation_date.hour,
                   entry->revocation_date.min,  entry->revocation_date.sec );
        MBEDTLS_X509_SAFE_SNPRINTF;

        entry = entry->next;
    }

    ret = snprintf( p, n, "\n%ssigned using  : ", prefix );
    MBEDTLS_X509_SAFE_SNPRINTF;

    ret = mbedtls_x509_sig_alg_gets( p, n, &crl->sig_oid, crl->sig_pk, crl->sig_md,
                             crl->sig_opts );
    MBEDTLS_X509_SAFE_SNPRINTF;

    ret = snprintf( p, n, "\n" );
    MBEDTLS_X509_SAFE_SNPRINTF;

    return( (int) ( size - n ) );
}

/*
 * Initialize a CRL chain
 */
void mbedtls_x509_crl_init( mbedtls_x509_crl *crl )
{
    memset( crl, 0, sizeof(mbedtls_x509_crl) );
}

/*
 * Unallocate all CRL data
 */
void mbedtls_x509_crl_free( mbedtls_x509_crl *crl )
{
    mbedtls_x509_crl *crl_cur = crl;
    mbedtls_x509_crl *crl_prv;
    mbedtls_x509_name *name_cur;
    mbedtls_x509_name *name_prv;
    mbedtls_x509_crl_entry *entry_cur;
    mbedtls_x509_crl_entry *entry_prv;

    if( crl == NULL )
        return;

    do
    {
#if defined(MBEDTLS_X509_RSASSA_PSS_SUPPORT)
        free( crl_cur->sig_opts );
#endif

        name_cur = crl_cur->issuer.next;
        while( name_cur != NULL )
        {
            name_prv = name_cur;
            name_cur = name_cur->next;
            mbedtls_zeroize( name_prv, sizeof( mbedtls_x509_name ) );
            free( name_prv );
        }

        entry_cur = crl_cur->entry.next;
        while( entry_cur != NULL )
        {
            entry_prv = entry_cur;
            entry_cur = entry_cur->next;
            mbedtls_zeroize( entry_prv, sizeof( mbedtls_x509_crl_entry ) );
            free( entry_prv );
        }

        if( crl_cur->raw.p != NULL )
        {
            mbedtls_zeroize( crl_cur->raw.p, crl_cur->raw.len );
            free( crl_cur->raw.p );
        }

        crl_cur = crl_cur->next;
    }
    while( crl_cur != NULL );

    crl_cur = crl;
    do
    {
        crl_prv = crl_cur;
        crl_cur = crl_cur->next;

        mbedtls_zeroize( crl_prv, sizeof( mbedtls_x509_crl ) );
        if( crl_prv != crl )
            free( crl_prv );
    }
    while( crl_cur != NULL );
}

#endif /* MBEDTLS_X509_CRL_PARSE_C */
