/*
 * Copyright 2018,2019 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * This is a standalone ESNIKeys Creator main file to start in on esni
 * in OpenSSL style, as per https://tools.ietf.org/html/draft-ietf-tls-esni-02
 * and now also https://tools.ietf.org/html/draft-ietf-tls-esni-02
 * Author: stephen.farrell@cs.tcd.ie
 * Date: 20190313
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/esni.h>
// for getopt()
#include <getopt.h>

// for getaddrinfo()
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define MAX_ESNIKEYS_BUFLEN 1024 ///< just for laughs, won't be that long
#define MAX_ESNI_COVER_NAME 254 ///< longer than this won't fit in SNI
#define MAX_ESNI_ADDRS   16 ///< max addresses to include in AddressSet
#define MAX_PADDING 40 ///< max padding to use when folding DNS records

/*
 * stdout version of esni_pbuf - just for odd/occasional debugging
 */
static void so_esni_pbuf(char *msg,unsigned char *buf,size_t blen,int indent)
{
    if (buf==NULL) {
        printf("OPENSSL: %s is NULL",msg);
        return;
    }
    printf("OPENSSL: %s (%zd):\n    ",msg,blen);
    int i;
    for (i=0;i!=blen;i++) {
        if ((i!=0) && (i%16==0))
            printf("\n    ");
        printf("%02x:",buf[i]);
    }
    printf("\n");
    return;
}

/*
 * stdout version of fp_esni_prr - also for debugging
 */
static void so_esni_prr(char *msg,         /* message string */
                        unsigned char *buf,     /* binary RDATA */
                        size_t blen,         /* length of RDATA */
                        int indent,         /* unused ? */
                        unsigned short typecode, /* numeric RRTYPE */
                        char *owner_name)     /* domain name to use */
{
    if (buf==NULL) {
        printf("OPENSSL: %s is NULL",msg);
        return;
    }
    printf("OPENSSL: %s (%zd):\n",msg,blen);
    if (blen>16) {        /* need to fold RDATA */
        char padding[1+MAX_ESNI_COVER_NAME];
        int i;
        for (i=0; i!=strlen(owner_name); i++) {
            padding[i]=' ';
        }
        padding[i]=0;
    
        printf("%s. IN TYPE%d \\# %ld (", owner_name, typecode, blen);
        for (i=0;i!=blen;i++) {
            if (i%16==0)
                printf("\n%s                  ", padding);
            else if (i%2==0)
                printf(" ");
            printf("%02x",buf[i]);
        }
        printf(" )\n");
    }
    else {            /* no need for folding */
        printf("%s. IN TYPE%d \\# %ld ", owner_name, typecode, blen);
        int i;
        for (i=0;i!=blen;i++) {
            printf("%02x",buf[i]);
        }
        printf("\n");
    }
    return;
}

/**
 * @brief write zone fragment to file
 *
 * @param fp handle on already-opened FILE
 * @param msg not used, kept for compatibility with debugging function
 * @param buf binary public key data
 * @param blen lenght of buf
 * @param indent not used, kept for compatibility with debugging function
 * @param typecode DNS TYPE code to use 
 * @param owner_name fully-qualified DNS owner, without trailing dot
 */
static void fp_esni_prr(FILE *fp,
                        char *msg,
                        unsigned char *buf,
                        size_t blen,
                        int indent,
                        unsigned short typecode,
                        char *owner_name)
{
    if (buf==NULL) {
        fprintf(stderr,"OPENSSL: %s is NULL",msg);
        exit(9);
    }
    if (blen>16) {        /* need to fold RDATA */
        char padding[1+MAX_ESNI_COVER_NAME];
        int i;
        for (i=0; i!=strlen(owner_name); i++) {
            padding[i]=' ';
        }
        padding[i]=0;
    
        fprintf(fp, "%s. IN TYPE%d \\# %ld (", owner_name, typecode, blen);
        for (i=0;i!=blen;i++) {
            if (i%16==0)
                fprintf(fp, "\n%s                  ", padding);
            else if (i%2==0)
                fprintf(fp, " ");
            fprintf(fp, "%02x",buf[i]);
        }
        fprintf(fp, " )\n");
    }
    else {            /* no need for folding */
        fprintf(fp, "%s. IN TYPE%d \\# %ld ", owner_name, typecode, blen);
        int i;
        for (i=0;i!=blen;i++) {
            fprintf(fp, "%02x",buf[i]);
        }
        fprintf(fp, "\n");
    }
    return;
}

/**
 * @brief generate the SHA256 checksum that should be in the DNS record
 *
 * Fixed SHA256 hash in this case, we work on the offset here,
 * (bytes 2 bytes then 4 checksum bytes then rest) with no other 
 * knowledge of the encoding.
 *
 * @param buf is the buffer
 * @param buf_len is obvous
 * @return 1 for success, not 1 otherwise
 */
static int esni_checksum_gen(unsigned char *buf, size_t buf_len, unsigned char cksum[4])
{
    /* 
     * copy input with zero'd checksum, do SHA256 hash, compare with
     * checksum, tedious but easy enough
     */
    unsigned char *buf_zeros=OPENSSL_malloc(buf_len);
    if (buf_zeros==NULL) {
        fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
        goto err;
    }
    memcpy(buf_zeros,buf,buf_len);
    memset(buf_zeros+2,0,4);
    unsigned char md[EVP_MAX_MD_SIZE];
    SHA256_CTX context;
    if(!SHA256_Init(&context)) {
        fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
        goto err;
    }
    if(!SHA256_Update(&context, buf_zeros, buf_len)) {
        fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
        goto err;
    }
    if(!SHA256_Final(md, &context)) {
        fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
        goto err;
    }
    OPENSSL_free(buf_zeros);
    memcpy(cksum,md,4);
    return 1;
 err:
    if (buf_zeros!=NULL) OPENSSL_free(buf_zeros);
    return 0;
}

void usage(char *prog) 
{
    printf("Create an ESNIKeys data structure as per draft-ietf-tls-esni-[02|03]\n");
    printf("Usage: \n");
    printf("\t%s [-V version] [-o <fname>] [-p <privfname>] [-d duration] \n",prog);
    printf("\t\t\t[-P public-/cover-name] [-A [file-name]] [-z zonefrag-file]\n");
    printf("where:\n");
    printf("-V specifies the ESNIKeys version to produce (default: 0xff01; 0xff02 allowed)\n");
    printf("-o specifies the output file name for the binary-encoded ESNIKeys (default: ./esnikeys.pub)\n");
    printf("-p specifies the output file name for the corresponding private key (default: ./esnikeys.priv)\n");
    printf("-d duration, specifies the duration in seconds from, now, for which the public share should be valid (default: 1 week)\n");
    printf("If <privfname> exists already and contains an appropriate value, then that key will be used without change.\n");
    printf("There is no support for crypto options - we only support TLS_AES_128_GCM_SHA256, X25519 and no extensions.\n");
    printf("Fix that if you like:-)\n");
    printf("The following are only valid with -V 0xff02:\n");
    printf("-P specifies the public-/cover-name value\n");
    printf("-A says to include an AddressSet extension\n");
    printf("-z says to output the zonefile fragment to the specified file\n");
    printf("\n");
    printf("-P, -A and -z are only supported for version 0xff02 and not 0xff01\n");
    printf("If a filename ie given with -A then that should contain one IP address per line.\n");
    printf("If no filename is given with -A then we'll look up the A and AAAA for the cover-/public-name and use those.\n");
    printf("If no zonefrag-file is provided a default zonedata.fragment file will be created\n");
    exit(1);
}

/**
 * @brief map version string like 0xff01 to unsigned short
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
 * @brief Add an adderess to the list if it's not there already
 * @param
 * @return 0 if added, 1 if already present, <0 for error
 */
static int add2alist(char *ips[], int *nips_p, char *line)
{
    int nips=0;
    int added=0;

    if (!ips || !nips_p || !line) {
        return -1;
    }
    nips=*nips_p;

    if (nips==0) {
        ips[0]=strdup(line);
        nips=1;
        added=1;
    } else {
        int found=0;
        for (int i=0;i!=nips;i++) {
            if (!strncmp(ips[i],line,strlen(line))) {
                found=1;
                return(1);
            }
        }
        if (!found) {
            if (nips==MAX_ESNI_ADDRS) {
                fprintf(stderr,"Too many addresses found (max is %d) - exiting\n",MAX_ESNI_ADDRS);
                exit(1);
            }
            ips[nips]=strdup(line);
            nips++;
            added=1;
        }
    }
    if (added) {
        *nips_p=nips;
        return(0);
    }
    return(-2);
}

/**
 * @brief Make an X25519 key pair and ESNIKeys structure for the public
 *
 * @todo TODO: check out NSS code to see if I can make same format private
 * @todo TODO: Decide if supporting private key re-use is even needed.
 */
static int mk_esnikeys(int argc, char **argv)
{
    // getopt vars
    int opt;

    char *pubfname=NULL; ///< public key file name
    char *privfname=NULL; ///< private key file name
    char *fragfname=NULL; ///< zone fragment file name
    unsigned short ekversion=0xff01; ///< ESNIKeys version value (default is for draft esni -02)
    char *cover_name=NULL; ///< ESNIKeys "public_name" field (here called cover name)
    size_t cnlen=0; ///< length of cover_name
    int includeaddrset=0; ///< whether or not to include an AddressSet extension
    char *asetfname=NULL; ///< optional file name for AddressSet values
    int duration=60*60*24*7; ///< 1 week in seconds
    int maxduration=duration*52*10; ///< 10 years max - draft -02 will definitely be deprecated by then:-)
    int minduration=3600; ///< less than one hour seems unwise

    int extlen=0; ///< length of overall ESNIKeys extension value (with all extensions included)
    unsigned char *extvals=NULL; ///< buffer with all encoded ESNIKeys extensions

    // check inputs with getopt
    while((opt = getopt(argc, argv, ":A:P:V:?ho:p:d:z:")) != -1) {
        switch(opt) {
        case 'h':
        case '?':
            usage(argv[0]);
            break;
        case 'o':
            pubfname=optarg;
            break;
        case 'p':
            privfname=optarg;
            break;
        case 'z':
            fragfname=optarg;
            break;
        case 'd':
            duration=atoi(optarg);
            break;
        case 'V':
            ekversion=verstr2us(optarg);
            break;
        case 'P':
            cover_name=optarg;
            break;
        case 'A':
            includeaddrset=1;
            asetfname=optarg;
            break;
        case ':':
            switch (optopt) {
            case 'A':
                includeaddrset=1;
                break;
            default: 
                fprintf(stderr, "Error - No such option: `%c'\n\n", optopt);
                usage(argv[0]);
            }
            break;
        default:
            fprintf(stderr, "Error - No such option: `%c'\n\n", optopt);
            usage(argv[0]);
        }
    }

    if (ekversion==0xff01 && cover_name != NULL) {
        fprintf(stderr,"Version 0xff01 doesn't support Cover name - exiting\n\n");
        usage(argv[0]);
    }
    if (ekversion==0xff01 && includeaddrset!=0) {
        fprintf(stderr,"Version 0xff01 doesn't support AddressSet - exiting\n\n");
        usage(argv[0]);
    }
    if (duration <=0) {
        fprintf(stderr,"Can't have negative duration (%d)\n\n",duration);
        usage(argv[0]);
    }
    if (duration>=maxduration) {
        fprintf(stderr,"Can't have >10 years duration (%d>%d)\n\n",duration,maxduration);
        usage(argv[0]);
    }
    if (duration<minduration) {
        fprintf(stderr,"Can't have <1 hour duration (%d<%d)\n\n",duration,minduration);
        usage(argv[0]);
    }
    switch(ekversion) {
    case 0xff01: /* esni draft -02 */
        break;
    case 0xff02: /* esni draft -03 */
        if (cover_name==NULL) {
            fprintf(stderr,"%x requires you to specify a cover/public-name - exiting\n\n",ekversion);
            usage(argv[0]);
        }
        cnlen=strlen(cover_name);
        if (cnlen > MAX_ESNI_COVER_NAME) {
            fprintf(stderr,"Cover name too long (%ld), max is %d\n\n",cnlen,MAX_ESNI_COVER_NAME);
            usage(argv[0]);
        }
        if (cover_name[cnlen-1]=='.') {
            cover_name[cnlen-1] = 0; /* strip trailing dot to canonicalize */
        }
        break;
    default:
        fprintf(stderr,"Bad version supplied: %x\n\n",ekversion);
        usage(argv[0]);
    }

    /* handle AddressSet stuff */
    if (ekversion==0xff02 && includeaddrset!=0) {
        int nips=0;
        char *ips[MAX_ESNI_ADDRS];
        memset(ips,0,MAX_ESNI_ADDRS*sizeof(char*));
        if (asetfname!=NULL) {
            /* open file and read 1 IP per line */
            FILE *fp=fopen(asetfname,"r");
            if (!fp) {
                fprintf(stderr,"Can't open address file (%s) - exiting\n",asetfname);
                exit(1);
            }
            char * line = NULL;
            size_t len = 0;
            ssize_t read;
            while ((read = getline(&line, &len, fp)) != -1) {
                if (line[0]=='#') {
                    continue;
                }
                line[read-1]='\0'; /* zap newline */
                int rv=add2alist(ips,&nips,line);
                if (rv<0) {
                    fprintf(stderr,"add2alist failed (%d) - exiting\n",rv);
                    exit(1);
                }
            }
            if (line)
                free(line);
            fclose(fp);
        } else {
            if (cnlen==0) {
                fprintf(stderr,"Can't get address as no public-/cover-name supplied.\n");
                exit(1);
            }
            /* try getaddrinfo() */
            struct addrinfo *ai,*rp=NULL;
            int rv=getaddrinfo(cover_name,NULL,NULL,&ai);
            if (rv!=-0) {
                fprintf(stderr,"getaddrinfo failed (%d) for %s\n",rv,cover_name);
                exit(1);
            }
            for (rp=ai;rp!=NULL;rp=rp->ai_next) {
                // just print first
                char astr[100];
                astr[0]='\0';
                struct sockaddr *sa=rp->ai_addr;
                if (rp->ai_family==AF_INET) {
                    inet_ntop(rp->ai_family, 
                              &((struct sockaddr_in *)sa)->sin_addr,
                              astr, sizeof astr);
                } else if (rp->ai_family==AF_INET6) {
                    inet_ntop(rp->ai_family, 
                              &((struct sockaddr_in6 *)sa)->sin6_addr,
                              astr, sizeof astr);
                }
                int rv=add2alist(ips,&nips,astr);
                if (rv<0) {
                    fprintf(stderr,"add2alist failed (%d) - exiting\n",rv);
                    exit(1);
                }

            }
            freeaddrinfo(ai);
        }
        /* 
         * put those into extension buffer
         */
        unsigned char tmpebuf[MAX_ESNIKEYS_BUFLEN]; 
        unsigned char *tp=tmpebuf;
        for (int i=0;i!=nips;i++) {
            /* 
             * it's IPv6 if it has a ':" otherwise IPv4
             * we do this here and not based on getaddrinfo because they may
             * have come from a file - could be better done later I guess
             */
            int rv=0;
            if (strrchr(ips[i],':')) {
                printf("IPv6 Address%d: %s\n",i,ips[i]);
                *tp++=0x06;
                rv=inet_pton(AF_INET6,ips[i],tp);
                if (rv!=1) {
                    fprintf(stderr,"Failed to convert string (%s) to IP address - exiting\n",ips[i]);
                    exit(1);
                }
                tp+=16;
            } else {
                printf("IPv4 Address%d: %s\n",i,ips[i]);
                *tp++=0x04;
                rv=inet_pton(AF_INET,ips[i],tp);
                if (rv!=1) {
                    fprintf(stderr,"Failed to convert string (%s) to IP address - exiting\n",ips[i]);
                    exit(1);
                }
                tp+=4;
            }
            if ((tp-tmpebuf)>(MAX_ESNIKEYS_BUFLEN-100)) {
                fprintf(stderr,"Out of space converting string (%s) to IP address - exiting\n",ips[i]);
                exit(1);
            }
        }
        /*
         * free strings
         */
        for (int i=0;i!=nips;i++) {
            free(ips[i]);
        }
        int nelen=(tp-tmpebuf);
        int exttype=0x1001;
        if (nelen>0xffff) {
            fprintf(stderr,"Encoded extensions too big (%d) - exiting\n",nelen);
            exit(1);
        }
        if (extvals==NULL) {
            extvals=(unsigned char*)malloc(6+nelen);
            if (!extvals) {
                fprintf(stderr,"Out of space converting string to IP address - exiting\n");
                exit(1);
            }
            extvals[0]=((nelen+4)>>8)%256;
            extvals[1]=(nelen+3)%256;
            extvals[2]=(exttype>>8)%256;
            extvals[3]=exttype%256;
            extvals[4]=(nelen>>8)%256;
            extvals[5]=nelen%256;
            memcpy(extvals+6,tmpebuf,nelen);
            extlen=nelen+6;
        } else {
            /* we only support 1 extension for now so this won't happen */
            fprintf(stderr,"Didn't implement realloc code yet - exiting!\n");
            exit(1);
        }
    }

    if (privfname==NULL) {
        privfname="esnikeys.priv";
    }
    EVP_PKEY *pkey = NULL;
    FILE *privfp=fopen(privfname,"rb");
    if (privfp!=NULL) {
        /*
         * read contents and re-use key if it's a good key
         *
         * The justification here is that we might need to handle public
         * values that overlap, e.g. due to TTLs being set differently
         * by different hidden domains or some such. (I.e. I don't know
         * yet if that's really needed or not.)
         *
         * Note though that re-using private keys like this could end
         * up being DANGEROUS, in terms of damaging forward secrecy
         * for hidden service names. Not sure if there're other possible
         * bad effects, but certainly likely safer operationally to 
         * use a new key pair every time. (Which is also supported of
         * course.)
         *
         */
        if (!PEM_read_PrivateKey(privfp,&pkey,NULL,NULL)) {
            fprintf(stderr,"Can't read private key - exiting\n");
            fclose(privfp);
            exit(1);
        }
        // don't close file yet, used as signal later
    } else {
        /* new private key please... */
        if (!RAND_set_rand_method(NULL)) {
            fprintf(stderr,"Can't init (P)RNG - exiting\n");
            exit(1);
        }
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(NID_X25519, NULL);
        if (pctx==NULL) {
            fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
            exit(2);
        }
        EVP_PKEY_keygen_init(pctx);
        EVP_PKEY_keygen(pctx, &pkey);
        if (pkey==NULL) {
            fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
            exit(3);
        }
        EVP_PKEY_CTX_free(pctx);

    }
    unsigned char *public=NULL;
    size_t public_len=0;
    public_len = EVP_PKEY_get1_tls_encodedpoint(pkey,&public); 
    if (public_len == 0) {
        fprintf(stderr,"Crypto error (line:%d)\n",__LINE__);
        exit(4);
    }

    // write private key to file, if we didn't just read private key file
    if (privfp==NULL) {
        privfp=fopen(privfname,"wb");
        if (privfp==NULL) {
            fprintf(stderr,"fopen error (line:%d)\n",__LINE__);
            exit(5);
        }
        if (!PEM_write_PrivateKey(privfp,pkey,NULL,NULL,0,NULL,NULL)) {
            fclose(privfp);
            fprintf(stderr,"file write error (line:%d)\n",__LINE__);
            exit(6);
        }
    }
    fclose(privfp);

    EVP_PKEY_free(pkey);

    time_t nb=time(0)-1;
    time_t na=nb+duration;

    /*
     * Here's a hexdump of one draft-02 cloudflare value:
     * 00000000  ff 01 c7 04 13 a8 00 24  00 1d 00 20 e1 84 9f 8d  |.......$... ....|
     * 00000010  2c 89 3c da f5 cf 71 7c  2a ac c1 34 19 cc 7a 38  |,.<...q|*..4..z8|
     * 00000020  a6 d2 62 59 68 f9 ab 89  ad d7 b2 27 00 02 13 01  |..bYh......'....|
     * 00000030  01 04 00 00 00 00 5b da  50 10 00 00 00 00 5b e2  |......[.P.....[.|
     * 00000040  39 10 00 00                                       |9...|
     * 00000044
     *
     * And here's the TLS presentation syntax:
     *     struct {
     *         uint16 version;
     *         uint8 checksum[4];
     *         KeyShareEntry keys<4..2^16-1>;
     *         CipherSuite cipher_suites<2..2^16-2>;
     *         uint16 padded_length;
     *         uint64 not_before;
     *         uint64 not_after;
     *         Extension extensions<0..2^16-1>;
     *     } ESNIKeys;
     *
     * draft-03 adds this just after the checksum:
     *         opaque public_name<1..2^16-1>;
     *
     * I don't yet have anyone else's example of a -03/ff02 value but here's one
     * of mine where this was called with "-P www.cloudflarecom -A":
     *
     * 00000000  ff 02 36 60 b9 a0 00 12  77 77 77 2e 63 6c 6f 75  |..6`....www.clou|
     * 00000010  64 66 6c 61 72 65 2e 63  6f 6d 00 24 00 1d 00 20  |dflare.com.$... |
     * 00000020  c7 e8 4b 92 59 d6 1c 58  36 6c eb 26 46 ec 9d 3d  |..K.Y..X6l.&F..=|
     * 00000030  fb 3d ab de 9a 94 ac 34  7e bd 7c 2a c4 ae e3 60  |.=.....4~.|*...`|
     * 00000040  00 02 13 01 01 04 00 00  00 00 5c 89 6e 0c 00 00  |..........\.n...|
     * 00000050  00 00 5c 92 a8 8c 00 2f  10 01 00 2c 06 26 06 47  |..\..../...,.&.G|
     * 00000060  00 00 00 00 00 00 00 00  00 c6 29 d6 a2 06 26 06  |..........)...&.|
     * 00000070  47 00 00 00 00 00 00 00  00 00 c6 29 d7 a2 04 c6  |G..........)....|
     * 00000080  29 d6 a2 04 c6 29 d7 a2                           |)....)..|
     * 00000088
     *
     */

    unsigned char bbuf[MAX_ESNIKEYS_BUFLEN]; ///< binary buffer
    unsigned char *bp=bbuf;
    memset(bbuf,0,MAX_ESNIKEYS_BUFLEN);
    *bp++=(ekversion>>8)%256; 
    *bp++=(ekversion%256);// version = 0xff01 or 0xff02
    memset(bp,0,4); bp+=4; // space for checksum
    if (ekversion==0xff02) {
        /* draft -03 has public_name here, -02 hasn't got that at all */
        *bp++=(cnlen>>8)%256;
        *bp++=cnlen%256;
        memcpy(bp,cover_name,cnlen); bp+=cnlen;
    }
    *bp++=0x00;
    *bp++=0x24; // length=36
    *bp++=0x00;
    *bp++=0x1d; // curveid=X25519= decimal 29
    *bp++=0x00;
    *bp++=0x20; // length=32
    memcpy(bp,public,32); bp+=32;
    *bp++=0x00;
    *bp++=0x02; // length=2
    *bp++=0x13;
    *bp++=0x01; // ciphersuite TLS_AES_128_GCM_SHA256
    *bp++=0x01;
    *bp++=0x04; // 2 bytes padded length - 260, same as CF for now
    memset(bp,0,4); bp+=4; // top zero 4 octets of time
    *bp++=(nb>>24)%256;
    *bp++=(nb>>16)%256;
    *bp++=(nb>>8)%256;
    *bp++=nb%256;
    memset(bp,0,4); bp+=4; // top zero 4 octets of time
    *bp++=(na>>24)%256;
    *bp++=(na>>16)%256;
    *bp++=(na>>8)%256;
    *bp++=na%256;
    if (extlen==0) {
        *bp++=0x00;
        *bp++=0x00; // no extensions
    } else {
        memcpy(bp,extvals,extlen);
        bp+=extlen;
        free(extvals);
    }
    size_t bblen=bp-bbuf;

    so_esni_pbuf("BP",bbuf,bblen,0);

    unsigned char cksum[4];
    if (esni_checksum_gen(bbuf,bblen,cksum)!=1) {
        fprintf(stderr,"fopen error (line:%d)\n",__LINE__);
        exit(7);
    }
    memcpy(bbuf+2,cksum,4);
    so_esni_pbuf("BP+cksum",bbuf,bblen,0);

    if (pubfname==NULL) {
        pubfname="esnikeys.pub";
    }
    FILE *pubfp=fopen(pubfname,"wb");
    if (pubfp==NULL) {
        fprintf(stderr,"fopen error (line:%d)\n",__LINE__);
        exit(7);
    }
    if (fwrite(bbuf,1,bblen,pubfp)!=bblen) {
        fprintf(stderr,"fwrite error (line:%d)\n",__LINE__);
        exit(8);
    }
    fclose(pubfp);

    if (ekversion==0xff02) {
        so_esni_prr("BP+cksum as DNS RR",bbuf,bblen,0,ESNI_RRTYPE,cover_name);

        if (fragfname==NULL) {
            fragfname="zonedata.fragment";
        }
        FILE *fragfp=fopen(fragfname,"w");
        if (fragfp==NULL) {
            fprintf(stderr,"fopen error (line:%d)\n",__LINE__);
            exit(7);
        }
        fp_esni_prr(fragfp, "BP+cksum as DNS RR",bbuf,bblen,0,ESNI_RRTYPE,cover_name);
        fclose(fragfp);
    }

    OPENSSL_free(public);

    return(0);
}


int main(int argc, char **argv)
{
    return mk_esnikeys(argc, argv);
}

// -*- Local Variables:
// -*- c-basic-offset: 4
// -*- indent-tabs-mode: nil
// -*- End: