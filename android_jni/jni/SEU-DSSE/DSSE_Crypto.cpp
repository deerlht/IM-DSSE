#include "DSSE_Crypto.h"
#include "Miscellaneous.h"
#ifdef __cplusplus
extern "C" {
#endif

int ccm_128_enc_dec(int cipher, const unsigned char *key, unsigned long keylen, symmetric_key *uskey, const unsigned char *nonce,
                    unsigned long noncelen, const unsigned char *header, unsigned long headerlen, unsigned char *pt, unsigned long ptlen,
                    unsigned char *ct, unsigned char *tag, unsigned long *taglen, int  direction)
{

	int	err;

	/* register cipher */
	register_cipher(&aes_desc);
	/* encrypt it */
	if ((err = ccm_memory(cipher, key, keylen, uskey, nonce, noncelen, header, headerlen, pt, ptlen, ct, tag, taglen, direction)) != CRYPT_OK) {
		printf("ccm_memory error %s\n", error_to_string(err));
		return -1;
	}
	/* ct and tag now hold the output */
    unregister_cipher(&aes_desc);
	return 0;
}

//block_size and key_size are of 16 bytes & so outlen should be <=16
int omac_aes128(unsigned char *omac_out, int omac_length, const unsigned char *data, int datalen, unsigned char *key)
{
	int idx, err;
	omac_state omac;

	/* register Rijndael */
	if (register_cipher(&rijndael_desc) == -1) {
		printf("Error registering Rijndael\n");
		return -1;
	}
	/* get index of Rijndael in cipher descriptor table */
	idx = find_cipher("rijndael");
	/* we would make up our symmetric key in "key[]" here */
	/* start the OMAC */
#if defined (INTEL_AES_NI)
	if ((err = omac_aesni_init(&omac, idx, key)) != CRYPT_OK) {
		printf("Error setting up omac: %s\n", error_to_string(err));
		return -1;
	}
	/* process a few octets */
	if((err = omac_aesni_process(&omac, key, data, datalen)) != CRYPT_OK) {
		printf("Error processing omac: %s\n", error_to_string(err));
		return -1;
	}
	/* get result (presumably to use it somehow...) */
	if ((err = omac_aesni_done(&omac, key, omac_out, omac_length)) != CRYPT_OK) {
		printf("Error finishing omac: %s\n", error_to_string(err));
		return -1;
	}
#else
    if ((err = omac_init(&omac, idx, key,BLOCK_CIPHER_SIZE)) != CRYPT_OK) {
		printf("Error setting up omac: %s\n", error_to_string(err));
		return -1;
	}
	/* process a few octets */
	if((err = omac_process(&omac, data, datalen)) != CRYPT_OK) {
		printf("Error processing omac: %s\n", error_to_string(err));
		return -1;
	}
    unsigned long ul_omac_len = omac_length;
	/* get result (presumably to use it somehow...) */
	if ((err = omac_done(&omac, omac_out, &ul_omac_len)) != CRYPT_OK) {
		printf("Error finishing omac: %s\n", error_to_string(err));
		return -1;
	}
#endif
    unregister_cipher(&rijndael_desc);
	return 0;
}

int aes128_ctr_encdec( unsigned char *pt, unsigned char *ct, unsigned char *key, unsigned char *ctr,  size_t numBlocks) 
{
#if defined(INTEL_AES_NI)
    intel_AES_encdec128_CTR(pt,ct,key,numBlocks,ctr);
#else
    symmetric_CTR symmetric_ctr;
    int err;
    /*
    if (register_cipher(&aes_desc) == -1) 
    {
        printf("Error registering cipher.\n");
        return -1;
    }*/
    if ((err = ctr_start(find_cipher("aes"),ctr,key,BLOCK_CIPHER_SIZE,0,CTR_COUNTER_BIG_ENDIAN | 4,&symmetric_ctr))!=CRYPT_OK)
    {
        printf("Error starting AES-CTR: %s\n", error_to_string(err));
        //exit(0);
        return -1;
    }
    ctr_encrypt(pt,ct,BLOCK_CIPHER_SIZE,&symmetric_ctr);

    //unregister_cipher(&aes_desc);
#endif
    //memcpy(ct,pt,BLOCK_CIPHER_SIZE);
	return 0;
    
}

#if defined (INTEL_AES_NI)

int omac_aesni_init(omac_state *omac, int cipher, unsigned char *key)
{
	int err, x, y, mask, msb, len;

	LTC_ARGCHK(omac != NULL);
	LTC_ARGCHK(key  != NULL);

	if ((err = cipher_is_valid(cipher)) != CRYPT_OK) {
		return err;
	}

#ifdef LTC_FAST
	if (cipher_descriptor[cipher].block_length % sizeof(LTC_FAST_TYPE)) {
		return CRYPT_INVALID_ARG;
	}
#endif

	/* now setup the system */
	switch (cipher_descriptor[cipher].block_length) {
	case 8:  mask = 0x1B;
	len  = 8;
	break;
	case 16: mask = 0x87;
	len  = 16;
	break;
	default: return CRYPT_INVALID_ARG;
	}

	/* ok now we need Lu and Lu^2 [calc one from the other] */

	/* first calc L which is Ek(0) */
	zeromem(omac->Lu[0], cipher_descriptor[cipher].block_length);
	intel_AES_enc128(omac->Lu[0], omac->Lu[0], key, 1);


	/* now do the mults, whoopy! */
	for (x = 0; x < 2; x++) {
		/* if msb(L * u^(x+1)) = 0 then just shift, otherwise shift and xor constant mask */
		msb = omac->Lu[x][0] >> 7;

		/* shift left */
		for (y = 0; y < (len - 1); y++) {
			omac->Lu[x][y] = ((omac->Lu[x][y] << 1) | (omac->Lu[x][y+1] >> 7)) & 255;
		}
		omac->Lu[x][len - 1] = ((omac->Lu[x][len - 1] << 1) ^ (msb ? mask : 0)) & 255;

		/* copy up as require */
		if (x == 0) {
			XMEMCPY(omac->Lu[1], omac->Lu[0], sizeof(omac->Lu[0]));
		}
	}

	/* setup state */
	omac->cipher_idx = cipher;
	omac->buflen     = 0;
	omac->blklen     = len;
	zeromem(omac->prev,  sizeof(omac->prev));
	zeromem(omac->block, sizeof(omac->block));

	return CRYPT_OK;
}


/**
   Process data through LTC_OMAC
   @param omac     The LTC_OMAC state
   @param key     The secret key
   @param in       The input data to send through LTC_OMAC
   @param inlen    The length of the input (octets)
   @return CRYPT_OK if successful
 */
int omac_aesni_process(omac_state *omac, unsigned char *key, const unsigned char *in, unsigned long inlen)
{
	unsigned long n, x, blklen;
	int           err;

	LTC_ARGCHK(omac  != NULL);
	LTC_ARGCHK(in    != NULL);
	if ((err = cipher_is_valid(omac->cipher_idx)) != CRYPT_OK) 
    {
		return err;
	}

	if ((omac->buflen > (int)sizeof(omac->block)) || (omac->buflen < 0) ||
			(omac->blklen > (int)sizeof(omac->block)) || (omac->buflen > omac->blklen))
    {
		return CRYPT_INVALID_ARG;
	}

#ifdef LTC_FAST
    blklen = cipher_descriptor[omac->cipher_idx].block_length;
    if (omac->buflen == 0 && inlen > blklen) 
    {
        unsigned long y;
        for (x = 0; x < (inlen - blklen); x += blklen) {
            for (y = 0; y < blklen; y += sizeof(LTC_FAST_TYPE)) {
                *((LTC_FAST_TYPE*)(&omac->prev[y])) ^= *((LTC_FAST_TYPE*)(&in[y]));
            }
            in += blklen;
            intel_AES_enc128(omac->prev, omac->prev, key, 1);

        }
        inlen -= x;
    }
#endif
    while (inlen != 0) 
    {
        /* ok if the block is full we xor in prev, encrypt and replace prev */
        if (omac->buflen == omac->blklen) {
            for (x = 0; x < (unsigned long)omac->blklen; x++) {
                omac->block[x] ^= omac->prev[x];
            }
            intel_AES_enc128(omac->block, omac->prev, key, 1);

            omac->buflen = 0;
        }

        /* add bytes */
        n = MIN(inlen, (unsigned long)(omac->blklen - omac->buflen));
        XMEMCPY(omac->block + omac->buflen, in, n);
        omac->buflen  += n;
        inlen         -= n;
        in            += n;
    }
    return CRYPT_OK;
}


/**
  Terminate an LTC_OMAC stream
  @param omac   The LTC_OMAC state
  @param key     The secret key
  @param out    [out] Destination for the authentication tag
  @param outlen [in/out]  The max size and resulting size of the authentication tag
  @return CRYPT_OK if successful
 */
int omac_aesni_done(omac_state *omac, unsigned char *key, unsigned char *out, int outlen)
{
	int       err, mode;
	unsigned  x;

	LTC_ARGCHK(omac   != NULL);
	LTC_ARGCHK(out    != NULL);
	LTC_ARGCHK(outlen != NULL);
	if ((err = cipher_is_valid(omac->cipher_idx)) != CRYPT_OK) 
    {
		return err;
	}


	if ((omac->buflen > (int)sizeof(omac->block)) || (omac->buflen < 0) ||
			(omac->blklen > (int)sizeof(omac->block)) || (omac->buflen > omac->blklen)) 
    {
		return CRYPT_INVALID_ARG;
	}

	/* figure out mode */
	if (omac->buflen != omac->blklen) 
    {
		/* add the 0x80 byte */
		omac->block[omac->buflen++] = 0x80;

		/* pad with 0x00 */
		while (omac->buflen < omac->blklen) 
        {
            omac->block[omac->buflen++] = 0x00;
        }
		mode = 1;
	} 
    else 
    {
		mode = 0;
	}

	/* now xor prev + Lu[mode] */
	for (x = 0; x < (unsigned)omac->blklen; x++) 
    {
		omac->block[x] ^= omac->prev[x] ^ omac->Lu[mode][x];
	}

	/* encrypt it */
	intel_AES_enc128(omac->block, omac->block, key, 1);

	//cipher_descriptor[omac->cipher_idx].done(&omac->key);

	/* output it */
	for (x = 0; x < (unsigned long)omac->blklen && x < outlen; x++) 
    {
		out[x] = omac->block[x];
	}
	outlen = x;

#ifdef LTC_CLEAN_STACK
	zeromem(omac, sizeof(*omac));
#endif
	return CRYPT_OK;
}

#endif
int _rdrand64_asm(unsigned long long int *therand)
{
    
	unsigned char err;
	/*
    asm volatile("rdrand %0 ; setc %1"
			: "=r" (*therand), "=qm" (err));
	return (int) err;
     */
     memset(therand,0,sizeof(therand));
     unsigned long int k = random();
     memcpy(therand,&k,sizeof(k));
     return 1;
}

int rdrand(unsigned char* output, unsigned int output_len, unsigned int retry_limit)
{
	
    int qwords;
	int dwords;
	int i;

	unsigned long long int qrand;
	unsigned int drand;
    
    int n = output_len * sizeof(unsigned long int);
    unsigned long int* tmp = new unsigned long int[n];
    
	int success;
	int count;

	int total_uints;

	unsigned long int *qptr;

	total_uints = 0;
	
	
	for (i=0; i<n; i++)
	{
		count = 0;
		do
		{
			success=_rdrand64_asm(&qrand);
		} while((success == 0) && (count++ < retry_limit));

		if (success == 1)
		{
			*tmp = qrand;
			tmp++;
			total_uints++;
		}
		else 
            return -1;
	}
    memcpy(output,tmp,output_len);
	return 0;
}

int invokeFortuna_prng(unsigned char *seed, unsigned char *key, int seedlen, int keylen) 
{
	
	prng_state prng;
	int err, i;
	/* register prng */
	if ((err = register_prng(&fortuna_desc)) != CRYPT_OK) 
    {
		printf("Error registering Fortuna PRNG : %s\n", error_to_string(err));
	}

	if ((err = find_prng("fortuna")) != CRYPT_OK) 
    {
		printf("Invalid PRNG : %s\n", error_to_string(err));
	}

	/* start it */
	if ((err = fortuna_start(&prng)) != CRYPT_OK) 
    {
		printf("Start error: %s\n", error_to_string(err));
	}

	if ((err = fortuna_add_entropy(seed, seedlen, &prng)) != CRYPT_OK) 
    {
		printf("Add_entropy error: %s\n", error_to_string(err));
	}
	/* ready and read */
	if ((err = fortuna_ready(&prng)) != CRYPT_OK) 
    {
		printf("Ready error: %s\n", error_to_string(err));
	}

	if ((err = fortuna_done(&prng)) != CRYPT_OK) 
    {
		printf("Done error: %s\n", error_to_string(err));
	}
	//printf("Read %lu bytes from fortuna\n", fortuna_read(key, keylen, &prng));
    fortuna_read(key, keylen, &prng);
	return 0;
}

int rdrand_get_n_uints_retry(unsigned int n, unsigned int retry_limit, unsigned int *dest)
{
	int qwords;
	int dwords;
	int i;

	unsigned long long int qrand;
	unsigned int drand;

	int success;
	int count;

	int total_uints;

	unsigned long int *qptr;

	total_uints = 0;
	qptr = (unsigned long int*)dest;

	qwords = n/2;
	dwords = n -(qwords*2);

	for (i=0; i<qwords; i++)
	{
		count = 0;
		do
		{
			success=_rdrand64_step(&qrand);
		} while((success == 0) || (count++ < retry_limit));

		if (success == 1)
		{
			*qptr = qrand;
			qptr++;
			total_uints+=2;
		}
		else (i = qwords);
	}
	if ((qwords > 0) && (success == 0)) return total_uints;

	dest = (unsigned int*)qptr;
	for (i=0; i<dwords; i++)
	{
		count = 0;
		do
		{
			success=_rdrand32_step(&drand);
		} while((success == 0) || (count++ < retry_limit));

		if (success == 1)
		{
			*dest = qrand;
			dest++;
			total_uints++;
		}
		else (i = dwords);
	}
	return 0;
}

/**********************************************/
/* Gathers 32 bits of entropy through RDRAND  */
/*   Writes that entropy to *therand.         */
/*   Returns 1 on success, or 0 on undeerflow */
/**********************************************/

int _rdrand32_step(unsigned int *therand)
{
	//unsigned char err;
	//asm volatile("");
	//return (int) err;
    
    memset(therand,0,sizeof(therand));
    unsigned long int k = random();
    memcpy(therand,&k,sizeof(k));
    return 1;
}


/**********************************************/
/* Gathers 64 bits of entropy through RDRAND  */
/*   Writes that entropy to *therand.         */
/*   Returns 1 on success, or 0 on underflow  */
/**********************************************/

int _rdrand64_step(unsigned long long int *therand)
{
	//unsigned char err;
    
	//asm volatile("rdrand %0 ; setc %1"
	//		: "=r" (*therand), "=qm" (err));
	//return (int) err;
    memset(therand,0,sizeof(therand));
    unsigned long int k = random();
    memcpy(therand,&k,sizeof(k));
    return 1;
}

#ifdef __cplusplus
}
#endif