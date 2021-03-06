/*
 * Copyright 1995-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apps.h"
#include "progs.h"
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/echo.h>
#include <crypto/hpke.h>

#ifndef OPENSSL_NO_ECHO

typedef enum OPTION_choice {
    /* 
     * standard openssl options
     */
    OPT_ERR = -1, OPT_EOF = 0, OPT_HELP,
    OPT_PUBOUT, OPT_PRIVOUT, OPT_PEMOUT, 
    /*
     * ECHOCOnfig specifics
     */
    OPT_PUBLICNAME, OPT_ECHOVERSION
} OPTION_CHOICE;

const OPTIONS echo_options[] = {
    {"help", OPT_HELP, '-', "Display this summary"},
    {"pemout", OPT_PEMOUT, '>', "PEM output file with private key and ECHOConfig - default echoconfig.pem"},
    {"pubout", OPT_PUBOUT, '>', "Public key output file - default unset"},
    {"privout", OPT_PRIVOUT, '>', "Private key output file - default unset"},
    {"public_name", OPT_PUBLICNAME, 's', "public_name value"},
    {"echo_version", OPT_ECHOVERSION, 'n', "ECHOConfig version (default=0xff03)"},
    {NULL}
};

/**
 * @brief map version string like 0xff01 or 65291 to unsigned short
 * @param arg is the version string, from command line
 * @return is the unsigned short value (with zero for error cases)
 */
static unsigned short verstr2us(char *arg)
{
    long lv=strtol(arg,NULL,0);
    unsigned short rv=0;
    if (lv < 0xffff && lv > 0 ) {
        rv=(unsigned short)lv;
    }
    return(rv);
}

/**
 * @brief Make an X25519 key pair and ECHOConfig structure 
 * @param ekversion is the version to make
 * @param public_name is for inclusion within the ECHOConfig
 *
 * @return 1 for success, error otherwise
 */
static int mk_echoconfig(
        unsigned short ekversion,
        const char *public_name,
        size_t *echoconfig_len, unsigned char *echoconfig,
        size_t *privlen, unsigned char *priv)
{
    size_t pnlen=0; ///< length of public_name

    switch(ekversion) {
        case 0xff01: /* esni draft -02 */
        case 0xff02: /* esni draft -03 */
            return 0;
        case 0xff03: /* esni draft -04 */
            pnlen=(public_name==NULL?0:strlen(public_name));
            break;
        default:
            return 0;
    }

    /*
     * Placeholder - I'm gonna argue to exclude but it's in draft-06 for now
     */
    size_t extlen=0;
    unsigned char *extvals=NULL;

    /* new private key please... */
    if (priv==NULL) { return (__LINE__); }

    int hpke_mode=HPKE_MODE_BASE;
    hpke_suite_t hpke_suite = HPKE_SUITE_DEFAULT;
    size_t publen=HPKE_MAXSIZE; unsigned char pub[HPKE_MAXSIZE];
    int rv=hpke_kg(
        hpke_mode, hpke_suite,
        &publen, pub,
        privlen, priv); 
    if (rv!=1) { return(__LINE__); }
 

    /*
     * This is what's in draft-06:
     *
     * opaque HpkePublicKey<1..2^16-1>;
     * uint16 HkpeKemId; // Defined in I-D.irtf-cfrg-hpke
     *
     * struct {
     *     opaque public_name<1..2^16-1>;
     *     HpkePublicKey public_key;
     *     HkpeKemId kem_id;
     *     CipherSuite cipher_suites<2..2^16-2>;
     *     uint16 maximum_name_length;
     *     Extension extensions<0..2^16-1>;
     * } ECHOConfigContents;
     *
     * struct {
     *     uint16 version;
     *     uint16 length;
     *     select (ECHOConfig.version) {
     *       case 0xff03: ECHOConfigContents;
     *     }
     * } ECHOConfig;
     *
     * ECHOConfig ECHOConfigs<1..2^16-1>;
     */

    unsigned char bbuf[MAX_ECHOCONFIGS_BUFLEN]; ///< binary buffer
    unsigned char *bp=bbuf;
    memset(bbuf,0,MAX_ECHOCONFIGS_BUFLEN);
    *bp++=(ekversion>>8)%256; 
    *bp++=(ekversion%256);// version = 0xff01 or 0xff02
    if (ekversion==0xff01 || ekversion==0xff02) {
        memset(bp,0,4); bp+=4; // space for checksum
    }
    if (pnlen > 0 && (ekversion==0xff02 || ekversion == 0xff03)) {
        /* draft -03 and -04 have public_name here, -02 hasn't got that at all */
        *bp++=(pnlen>>8)%256;
        *bp++=pnlen%256;
        memcpy(bp,public_name,pnlen); bp+=pnlen;
    }
    /* keys */
    *bp++=0x00;
    *bp++=0x24; // length=36
    *bp++=0x00;
    *bp++=0x1d; // curveid=X25519= decimal 29
    *bp++=0x00;
    *bp++=0x20; // length=32
    memcpy(bp,pub,32); bp+=32;
    /* HPKE KEM id */
    *bp++=(HPKE_KEM_ID_25519/16);
    *bp++=(HPKE_KEM_ID_25519%16);
    /* cipher_suites */
    *bp++=0x00;
    *bp++=0x02; // length=2
    *bp++=0x13;
    *bp++=0x01; // ciphersuite TLS_AES_128_GCM_SHA256
    /* padded_length */
    *bp++=0x01;
    *bp++=0x04; // 2 bytes padded length - 260, same as CF for now
    if (extlen==0) {
        *bp++=0x00;
        *bp++=0x00; // no extensions
    } else {
        if (!extvals) {
            return(__LINE__);
        }
        memcpy(bp,extvals,extlen);
        bp+=extlen;
        free(extvals);
    }
    size_t bblen=bp-bbuf;

    int b64len = EVP_EncodeBlock((unsigned char*)echoconfig, (unsigned char *)bbuf, bblen);
    if (b64len >=(*echoconfig_len-1)) {
        return(__LINE__);
    }
    echoconfig[b64len]='\0';
    *echoconfig_len=b64len;

    return(1);
}

int echo_main(int argc, char **argv)
{
    BIO *pemf=NULL;
    char *prog=NULL;
    OPTION_CHOICE o;
    char *echoconfig_file = NULL, *keyfile = NULL, *pemfile=NULL;
    char *public_name=NULL;
    unsigned short echo_version=0xff03;

    prog = opt_init(argc, argv, echo_options);
    while ((o = opt_next()) != OPT_EOF) {
        switch (o) {
        case OPT_EOF:
        case OPT_ERR:
 opthelp:
            BIO_printf(bio_err, "%s: Use -help for summary.\n", prog);
            goto end;
        case OPT_HELP:
            opt_help(echo_options);
            goto end;
        case OPT_PUBOUT:
            echoconfig_file = opt_arg();
            break;
        case OPT_PRIVOUT:
            keyfile = opt_arg();
            break;
        case OPT_PEMOUT:
            pemfile = opt_arg();
            break;
        case OPT_PUBLICNAME:
            public_name = opt_arg();
            break;
        case OPT_ECHOVERSION:
            echo_version = verstr2us(opt_arg());
            break;
        }
    }
    argc = opt_num_rest();
    argv = opt_rest();
    if (argc != 0) {
        BIO_printf(bio_err, "%s: Unknown parameter %s\n", prog, argv[0]);
        goto opthelp;
    }

    /* 
     * Check ECHO-specific inputs
     */
    switch (echo_version) {
        case 0xff02:
        case 0xff01:
            BIO_printf(bio_err, "Unsupported version (0x%04x) - try using mk_esnikeys instead\n",echo_version);
            goto end;
        case 0xff03:
            break;
        default:
            BIO_printf(bio_err, "Unsupported version (0x%04x) - exiting\n",echo_version);
            goto end;
    }

    /*
     * Set default if needed
     */
    if (pemfile==NULL) {
        pemfile="echoconfig.pem";
    }

    /*
     * Generate a new ECHOConfig and spit that out
     */

    size_t echoconfig_len=MAX_ECHOCONFIGS_BUFLEN;
    unsigned char echoconfig[MAX_ECHOCONFIGS_BUFLEN];
    size_t privlen=HPKE_MAXSIZE; unsigned char priv[HPKE_MAXSIZE];
    int rv=mk_echoconfig(echo_version, public_name, &echoconfig_len, echoconfig, &privlen, priv);
    if (rv!=1) {
        BIO_printf(bio_err,"mk_echoconfig error: %d\n",rv);
        goto end;
    }
    
    /*
     * Write stuff to files, "proper" OpenSSL code needed
     */
    if (echoconfig_file!=NULL) {
        BIO *ecf=BIO_new_file(echoconfig_file,"w");
        if (ecf==NULL) goto end;
        BIO_write(ecf,echoconfig,echoconfig_len);
        BIO_printf(ecf,"\n");
        BIO_free_all(ecf);
        BIO_printf(bio_err,"Wrote ECHOConfig to %s\n",echoconfig_file);
    }
    if (keyfile!=NULL) {
        BIO *kf=BIO_new_file(keyfile,"w");
        if (kf==NULL) goto end;
        BIO_write(kf,priv,privlen);
        BIO_free_all(kf);
        BIO_printf(bio_err,"Wrote ECHO private key to %s\n",keyfile);
    }
    /*
     * If we didn't write out either of the above then
     * we'll create a PEM file
     */
    if (keyfile==NULL && echoconfig_file==NULL) {
        if ((pemf = BIO_new_file(pemfile, "w")) == NULL) goto end;
        BIO_write(pemf,priv,privlen);
        BIO_printf(pemf,"-----BEGIN ECHOCONFIG-----\n");
        BIO_write(pemf,echoconfig,echoconfig_len);
        BIO_printf(pemf,"\n");
        BIO_printf(pemf,"-----END ECHOCONFIG-----\n");
        BIO_free_all(pemf);
        BIO_printf(bio_err,"Wrote ECHO key pair to %s\n",pemfile);
    } else {
        if (keyfile==NULL) 
            BIO_printf(bio_err,"Didn't write private key anywhere! That's a bit silly\n");
        if (echoconfig_file==NULL) 
            BIO_printf(bio_err,"Didn't write ECHOConfig anywhere! That's a bit silly\n");
    }
    return(1);
end:
    return(0);
}

#endif

