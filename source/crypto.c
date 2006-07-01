/* $EPIC: crypto.c,v 1.4 2006/07/01 04:17:12 jnelson Exp $ */
/*
 * crypto.c: SED/CAST5/BLOWFISH/AES encryption and decryption routines.
 *
 * Copyright (c) 1990 Michael Sandroff.
 * Copyright (c) 1991, 1992 Troy Rollo.
 * Copyright (c) 1992-1996 Matthew Green.
 * Copyright � 2006 EPIC Software Labs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notices, the above paragraph (the one permitting redistribution),
 *    this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the author(s) may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 *	-About CAST5
 *
 * IRCII includes support for cast5-cbc (ircII calls it cast128) which is a
 * symmetric cipher that uses a 128 bit key (hense cast128) and a 64 bit 
 * initialization vector.
 *
 * IRCII sends the a CTCP message:
 *	PRIVMSG <target> :\001CAST128ED-CBC <payload>\001\r\n
 *
 * Where the <payload> is composed of:
 *	<Initialization Vector>		An 8-byte random Cast IV
 *	<Blocks>			Some number of 8 byte blocks
 *	<Final Block>			A final block with up to 7 bytes of 
 *					data. The final byte tells you how many
 *					bytes should be ignored.
 *
 * The whole thing is encrypted with CAST5-CBC.  Well, the IV is not encrypted
 * but you still have to run it through the decrypter because the CBC won't be
 * set up correctly if you don't.
 *
 * If a message is not divisible by 8 chars, and does not have at least two
 * blocks (an IV and a Final Block), it is probably a rogue message and should 
 * be discarded.
 *
 * The ircII format is almost compatable with openssl -- whereas openssl
 * expects the all of the fill bytes in <Final Block> to be the same, ircII
 * fills them in with random chars.  We must decrypt the string in unbuffered
 * modeor openssl will throw fits.  We handle the fill bytes ourself.
 *
 * When we encrypt a message, we let openssl do the buffering, so the fill 
 * bytes are not random, but ircII doesn't care.
 */

/*
 *	-About BlowFish
 *
 * EPIC supports BlowFish-cbc in the same format as Cast5-CBC:
 *	PRIVMSG <target> :\001BLOWFISH-CBC <payload>\001\r\n
 * with the same <payload> as cast5.  This is only supported with EPIC for
 * now.  It is not compatable with FiSH (a plugin for mirc, irssi, and xchat)
 */

/*
 *	-About AES-256
 *
 * EPIC supports AES-256-cbc in the same format as Cast5-CBC:
 *	PRIVMSG <target> :\001AES256-CBC <payload>\001\r\n
 * except that the <payload> is different:
 *	<Initialization Vector>		A 16-byte random IV (unused by AES)
 *	<Blocks>			Some number of 16 byte blocks
 *	<Final Block>			A final block with up to 15 bytes of
 *					data.  The final byte tells you how 
 *					many bytes should be ignored.
 *
 * The key for AES256 is always 256 bits (32 chars), no matter what.  EPIC 
 * fills short keys with nuls (ie, a password of 5 chars is padded with 27 
 * nuls to fill it out.)  Long keys are truncated at 32 chars.
 */

/*
 *	-About AES-SHA-256
 *
 * EPIC supports AES-SHA-256-cbc in the same format as AES256:
 *	PRIVMSG <target> :\001AESSHA256-CBC <payload>\001\r\n
 * except that the encryption key is generated by taking the sha256 of the
 * key instead of padding it out with nuls (as AES256 does).  This allows
 * an effectively unlimited key length.  
 *
 * Perhaps someday you'll be able to be able to key with a file instead of 
 * a passphrase.
 */

/*
 *	- About FiSH
 *
 * EPIC does not support FiSH yet.  Fish looks like this:
 *	PRIVMSG <target> :+OK <payload>\r\n
 * where <payload> are 64 byte blocks encoded with base64 into 12 characters.
 */

/*
 *	- About SED
 *
 * Some will look at these routines and recoil in horror with their insecurity.
 * It is true that SED ("Simple Encrypted Data", although maybe "Slightly 
 * Entropic Data" is closer to truth) is an exceptionally weak CBC cipher, and
 * does not provide much of a defense against cryptoanalysis of your messages.
 *
 * But what SED does provide is an effective defense against server-side 
 * pattern matching.  This is useful for discretely sending /DCC offers.
 * Because a DCC SEND reveals not only your real IP address, but also a port
 * from which you will send a file to the first taker, and because the server
 * sees this information before anyone else does, the security of your DCCs
 * are only as secure as your server admin.  For those who use rogue networks,
 * using SED to send twiddle your DCC offers is better than nothing.
 *
 * Alas, CTCP-over-SED is only compatable with EPIC and BitchX.
 */

#include "irc.h"
#include "sedcrypt.h"
#include "ctcp.h"
#include "ircaux.h"
#include "list.h"
#include "output.h"
#include "vars.h"
#include "words.h"
#ifdef HAVE_SSL
#include <openssl/evp.h>
#include <openssl/err.h>

static char *	decipher_evp (const unsigned char *key, int keylen, const unsigned char *ciphertext, int cipherlen, const EVP_CIPHER *type, int *outlen, int ivsize);
static void     decrypt_sed (unsigned char *str, int len, const unsigned char *key, int key_len);
static char *	decrypt_by_prog (const unsigned char *str, size_t *len, Crypt *key);

static char *	cipher_evp (const unsigned char *key, int keylen, const unsigned char *plaintext, int plaintextlen, const EVP_CIPHER *type, int *retsize, int ivsize);
static void     encrypt_sed (unsigned char *str, int len, char *key, int key_len);
static char *	encrypt_by_prog (const unsigned char *str, size_t *len, Crypt *key);
#endif

unsigned char *	decipher_message (const unsigned char *ciphertext, size_t len, Crypt *key, int *retlen)
{
	unsigned char *	copy, *free_it;
	size_t	copylen = 0, decipher_len = 0;

    do
    {
	if (key->type == CAST5CRYPT || key->type == BLOWFISHCRYPT ||
	    key->type == AES256CRYPT || key->type == AESSHA256CRYPT)
	{
	    unsigned char *	outbuf = NULL;
#ifdef HAVE_SSL
	    const EVP_CIPHER *type;
	    int	bytes_to_trim;
	    int blocksize, keylen;

	    if (key->type == CAST5CRYPT)
	    {
		blocksize = 8;
	    }
	    else if (key->type == BLOWFISHCRYPT)
	    {
		blocksize = 8;
	    }
	    else if (key->type == AES256CRYPT || key->type == AESSHA256CRYPT)
	    {
		blocksize = 16;
	    }
	    else
		return NULL;

	    if (len % blocksize != 0)
	    {
		yell("Encrypted message [%s] isn't multiple of %d! (is %d)", 
				ciphertext, blocksize, len);
		break;
	    }
	    if ((int)len < blocksize * 2)
	    {
		yell("Encrypted message [%s] doesn't contain message! "
				"(len is %d)", ciphertext, len);
		break;
	    }

	    if (key->type == CAST5CRYPT)
		type = EVP_cast5_cbc();
	    else if (key->type == BLOWFISHCRYPT)
		type = EVP_bf_cbc();
	    else if (key->type == AES256CRYPT || key->type == AESSHA256CRYPT)
		type = EVP_aes_256_cbc();
	    else
		break;		/* Not supported */

	    if (!(outbuf = decipher_evp(key->key, key->keylen,
					ciphertext, len, 
					type, retlen, blocksize)))
	    {
		yell("bummer");
		break;
	    }

	    bytes_to_trim = outbuf[len - 1] & (blocksize - 1);
	    outbuf[len - bytes_to_trim - 1] = 0;
	    memmove(outbuf, outbuf + blocksize, len - blocksize);
#endif
	    return outbuf;
	}
	else if (key->type == SEDCRYPT || key->type == SEDSHACRYPT)
	{
		unsigned char *	text;

		text = new_malloc(len + 1);
		memmove(text, ciphertext, len);
		decrypt_sed(text, len, key->key, key->keylen);
		*retlen = len;
		return text;
	}
	else if (key->type == PROGCRYPT)
	{
		unsigned char *retval;

		retval = decrypt_by_prog(ciphertext, &len, key);
		*retlen = len;
		return retval;
	}
	else
		panic("decipher_message: key->type %d is not valid", key->type);
    }
    while (0);

	return NULL;
}

#ifdef HAVE_SSL
static char *	decipher_evp (const unsigned char *key, int keylen, const unsigned char *ciphertext, int cipherlen, const EVP_CIPHER *type, int *outlen, int ivsize)
{
        unsigned char *outbuf;
	unsigned char	*iv;
	unsigned long errcode;
        EVP_CIPHER_CTX a;
        EVP_CIPHER_CTX_init(&a);
	EVP_CIPHER_CTX_set_padding(&a, 0);

	iv = new_malloc(ivsize);
	outbuf = new_malloc(1024);
	memcpy(iv, ciphertext, ivsize);

        EVP_DecryptInit_ex(&a, type, NULL, NULL, iv);
	EVP_CIPHER_CTX_set_key_length(&a, keylen);
        EVP_DecryptInit_ex(&a, NULL, NULL, key, NULL);
        EVP_DecryptUpdate(&a, outbuf, outlen, ciphertext, cipherlen);
        EVP_CIPHER_CTX_cleanup(&a);

	ERR_load_crypto_strings();
	while ((errcode = ERR_get_error()))
	{
	    char r[256];
	    ERR_error_string_n(errcode, r, 256);
	    yell("ERROR: %s", r);
	}

	new_free(&iv);
	return outbuf;
}
#endif

static void     decrypt_sed (unsigned char *str, int len, const unsigned char *key, int key_len)
{
        int	key_pos,
                i;
        char    mix,
                tmp;

        if (!key)
                return;                 /* no decryption */

        key_len = strlen(key);
        key_pos = 0;
        mix = 0;

        for (i = 0; i < len; i++)
        {
                tmp = mix ^ str[i] ^ key[key_pos]; 
                str[i] = tmp;
                mix ^= tmp;
                key_pos = (key_pos + 1) % key_len;
        }
        str[i] = (char) 0;
}

static char *	decrypt_by_prog (const unsigned char *str, size_t *len, Crypt *key)
{
        char    *ret = NULL, *input;
        char *  args[3];
        int     iplen;

        args[0] = malloc_strdup(key->prog);
        args[1] = malloc_strdup("decrypt");
        args[2] = NULL;
        input = malloc_strdup2(key->key, "\n");

        iplen = strlen(input);
        new_realloc((void**)&input, *len + iplen);
        memmove(input + iplen, str, *len);

        *len += iplen;
        ret = exec_pipe(key->prog, input, len, args);

        new_free(&args[0]);
        new_free(&args[1]);
        new_free((char**)&input);

        new_realloc((void**)&ret, 1+*len);
        ret[*len] = 0;
        return ret;
}

/*************************************************************************/
unsigned char *	cipher_message (const unsigned char *orig_message, size_t len, Crypt *key, int *retlen)
{
	unsigned char *	copy, *free_it;
	size_t	copylen = 0, decipher_len = 0;
	int	bytes_to_trim;
	size_t	ivlen;

	if (retlen)
		*retlen = 0;
	if (!orig_message || !key || !retlen)
		return NULL;

	if (key->type == CAST5CRYPT || key->type == BLOWFISHCRYPT ||
	    key->type == AES256CRYPT || key->type == AESSHA256CRYPT)
	{
	    unsigned char *ciphertext = NULL;
#ifdef HAVE_SSL
	    const EVP_CIPHER *type;

	    if (key->type == CAST5CRYPT)
	    {
		type = EVP_cast5_cbc();
		ivlen = 8;
	    }
	    else if (key->type == BLOWFISHCRYPT)
	    {
		type = EVP_bf_cbc();
		ivlen = 8;
	    }
	    else if (key->type == AES256CRYPT || key->type == AESSHA256CRYPT)
	    {
		type = EVP_aes_256_cbc();
		ivlen = 16;
	    }
	    else
		return NULL;	/* Not supported */

	    if (!(ciphertext = cipher_evp(key->key, key->keylen,
				      orig_message, len, 
				      type, retlen, ivlen)))
	    {
		yell("bummer");
		return NULL;
	    }
#endif
	    return ciphertext;
	}
	else if (key->type == SEDCRYPT || key->type == SEDSHACRYPT)
	{
		unsigned char *	ciphertext;

		ciphertext = new_malloc(len + 1);
		memmove(ciphertext, orig_message, len);
		encrypt_sed(ciphertext, len, key->key, strlen(key->key));
		*retlen = len;
		return ciphertext;
	}
	else if (key->type == PROGCRYPT)
	{
		unsigned char *ciphertext;

		ciphertext = encrypt_by_prog(orig_message, &len, key);
		*retlen = len;
		return ciphertext;
	}
	else
		panic("cipher_message: key->type %d is not valid", key->type);

	return NULL;
}

#ifdef HAVE_SSL
static char *	cipher_evp (const unsigned char *key, int keylen, const unsigned char *plaintext, int plaintextlen, const EVP_CIPHER *type, int *retsize, int ivsize)
{
        unsigned char *outbuf;
        int     outlen = 0;
	int	extralen = 0;
	unsigned char	*iv;
	unsigned long errcode;
	u_32int_t	randomval;
	int		iv_count;
        EVP_CIPHER_CTX a;
        EVP_CIPHER_CTX_init(&a);
	EVP_CIPHER_CTX_set_padding(&a, 0);

	iv = new_malloc(ivsize);
	for (iv_count = 0; iv_count < ivsize; iv_count += sizeof(u_32int_t))
	{
		randomval = arc4random();
		memmove(iv + iv_count, &randomval, sizeof(u_32int_t));
	}

	outbuf = new_malloc(1024);
	memcpy(outbuf, iv, ivsize);

        EVP_EncryptInit_ex(&a, type, NULL, NULL, iv);
	EVP_CIPHER_CTX_set_key_length(&a, keylen);
        EVP_EncryptInit_ex(&a, NULL, NULL, key, NULL);
        EVP_EncryptUpdate(&a, outbuf + ivsize, &outlen, plaintext, plaintextlen);
	EVP_EncryptFinal_ex(&a, outbuf + ivsize + outlen, &extralen);
        EVP_CIPHER_CTX_cleanup(&a);
	outlen += extralen;

	ERR_load_crypto_strings();
	while ((errcode = ERR_get_error()))
	{
	    char r[256];
	    ERR_error_string_n(errcode, r, 256);
	    yell("ERROR: %s", r);
	}

	*retsize = outlen + ivsize;
	return outbuf;
}
#endif

static void     encrypt_sed (unsigned char *str, int len, char *key, int key_len)
{
        int     key_pos,
                i;
        char    mix, 
                tmp;

        if (!key)
                return;                 /* no encryption */

        key_len = strlen(key);
        key_pos = 0;
        mix = 0;

        for (i = 0; i < len; i++)
        {
                tmp = str[i];
                str[i] = mix ^ tmp ^ key[key_pos];
                mix ^= tmp;
                key_pos = (key_pos + 1) % key_len;
        }
        str[i] = (char) 0;
}

static char *	encrypt_by_prog (const unsigned char *str, size_t *len, Crypt *key)
{
        char    *ret = NULL, *input;
        char *  args[3];
        int     iplen;

        args[0] = malloc_strdup(key->prog);
        args[1] = malloc_strdup("encrypt");
        args[2] = NULL;
        input = malloc_strdup2(key->key, "\n");

        iplen = strlen(input);
        new_realloc((void**)&input, *len + iplen);
        memmove(input + iplen, str, *len);

        *len += iplen;
        ret = exec_pipe(key->prog, input, len, args);

        new_free(&args[0]);
        new_free(&args[1]);
        new_free((char**)&input);

        new_realloc((void**)&ret, 1+*len);
        ret[*len] = 0;
        return ret;
}
