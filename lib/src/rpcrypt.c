/*
 * This file is part of Chiaki.
 *
 * Chiaki is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chiaki is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Chiaki.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <chiaki/rpcrypt.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <string.h>
#include <stdbool.h>


CHIAKI_EXPORT void chiaki_rpcrypt_bright_ambassador(uint8_t *bright, uint8_t *ambassador, const uint8_t *nonce, const uint8_t *morning)
{
	static const uint8_t echo_a[] = { 0x01, 0x49, 0x87, 0x9b, 0x65, 0x39, 0x8b, 0x39, 0x4b, 0x3a, 0x8d, 0x48, 0xc3, 0x0a, 0xef, 0x51 };
	static const uint8_t echo_b[] = { 0xe1, 0xec, 0x9c, 0x3a, 0xdd, 0xbd, 0x08, 0x85, 0xfc, 0x0e, 0x1d, 0x78, 0x90, 0x32, 0xc0, 0x04 };

	for(uint8_t i=0; i<CHIAKI_KEY_BYTES; i++)
	{
		uint8_t v = nonce[i];
		v -= i;
		v -= 0x27;
		v ^= echo_a[i];
		ambassador[i] = v;
	}

	for(uint8_t i=0; i<CHIAKI_KEY_BYTES; i++)
	{
		uint8_t v = morning[i];
		v -= i;
		v += 0x34;
		v ^= echo_b[i];
		v ^= nonce[i];
		bright[i] = v;
	}
}


CHIAKI_EXPORT void chiaki_rpcrypt_init(ChiakiRPCrypt *rpcrypt, const uint8_t *nonce, const uint8_t *morning)
{
	chiaki_rpcrypt_bright_ambassador(rpcrypt->bright, rpcrypt->ambassador, nonce, morning);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_rpcrypt_generate_iv(ChiakiRPCrypt *rpcrypt, uint8_t *iv, uint64_t counter)
{
	uint8_t hmac_key[] = { 0xac, 0x07, 0x88, 0x83, 0xc8, 0x3a, 0x1f, 0xe8, 0x11, 0x46, 0x3a, 0xf3, 0x9e, 0xe3, 0xe3, 0x77 };

	uint8_t buf[CHIAKI_KEY_BYTES + 8];
	memcpy(buf, rpcrypt->ambassador, CHIAKI_KEY_BYTES);
	buf[CHIAKI_KEY_BYTES + 0] = (uint8_t)((counter >> 0x38) & 0xff);
	buf[CHIAKI_KEY_BYTES + 1] = (uint8_t)((counter >> 0x30) & 0xff);
	buf[CHIAKI_KEY_BYTES + 2] = (uint8_t)((counter >> 0x28) & 0xff);
	buf[CHIAKI_KEY_BYTES + 3] = (uint8_t)((counter >> 0x20) & 0xff);
	buf[CHIAKI_KEY_BYTES + 4] = (uint8_t)((counter >> 0x18) & 0xff);
	buf[CHIAKI_KEY_BYTES + 5] = (uint8_t)((counter >> 0x10) & 0xff);
	buf[CHIAKI_KEY_BYTES + 6] = (uint8_t)((counter >> 0x08) & 0xff);
	buf[CHIAKI_KEY_BYTES + 7] = (uint8_t)((counter >> 0x00) & 0xff);

	uint8_t hmac[32];
	unsigned int hmac_len = 0;
	if(!HMAC(EVP_sha256(), hmac_key, CHIAKI_KEY_BYTES, buf, sizeof(buf), hmac, &hmac_len))
		return CHIAKI_ERR_UNKNOWN;

	if(hmac_len < CHIAKI_KEY_BYTES)
		return CHIAKI_ERR_UNKNOWN;

	memcpy(iv, hmac, CHIAKI_KEY_BYTES);
	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode chiaki_rpcrypt_crypt(ChiakiRPCrypt *rpcrypt, uint64_t counter, uint8_t *buf, size_t buf_size, bool encrypt)
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if(!ctx)
		return CHIAKI_ERR_UNKNOWN;

	uint8_t iv[CHIAKI_KEY_BYTES];
	ChiakiErrorCode err = chiaki_rpcrypt_generate_iv(rpcrypt, iv, counter);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

#define FAIL(err) do { EVP_CIPHER_CTX_free(ctx); return (err); } while(0);

	if(encrypt)
	{
		if(!EVP_EncryptInit_ex(ctx, EVP_aes_128_cfb128(), NULL, rpcrypt->bright, iv))
			FAIL(CHIAKI_ERR_UNKNOWN);
	}
	else
	{
		if(!EVP_DecryptInit_ex(ctx, EVP_aes_128_cfb128(), NULL, rpcrypt->bright, iv))
			FAIL(CHIAKI_ERR_UNKNOWN);
	}

	if(!EVP_CIPHER_CTX_set_padding(ctx, 0))
		FAIL(CHIAKI_ERR_UNKNOWN);

	if(buf_size % CHIAKI_KEY_BYTES)
	{
		size_t padded_size = ((buf_size + CHIAKI_KEY_BYTES - 1) / CHIAKI_KEY_BYTES) * CHIAKI_KEY_BYTES;
		uint8_t *tmp = malloc(padded_size);
		if(!tmp)
			FAIL(CHIAKI_ERR_MEMORY);
		memcpy(tmp, buf, buf_size);
		memset(tmp + buf_size, 0, padded_size - buf_size);
		int outl = (int)padded_size;

		int success;
		if(encrypt)
			success = EVP_EncryptUpdate(ctx, tmp, &outl, tmp, outl);
		else
			success = EVP_DecryptUpdate(ctx, tmp, &outl, tmp, outl);

		if(!success || outl != (int)padded_size)
		{
			free(tmp);
			FAIL(CHIAKI_ERR_UNKNOWN);
		}

		memcpy(buf, tmp, buf_size);
		free(tmp);
	}
	else
	{
		int outl = (int)buf_size;
		if(encrypt)
		{
			if(!EVP_EncryptUpdate(ctx, buf, &outl, buf, outl))
				FAIL(CHIAKI_ERR_UNKNOWN);
		}
		else
		{
			if(!EVP_DecryptUpdate(ctx, buf, &outl, buf, outl))
				FAIL(CHIAKI_ERR_UNKNOWN);
		}

		if(outl != (int)buf_size)
			FAIL(CHIAKI_ERR_UNKNOWN);
	}

#undef FAIL
	EVP_CIPHER_CTX_free(ctx);
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_rpcrypt_encrypt(ChiakiRPCrypt *rpcrypt, uint64_t counter, uint8_t *buf, size_t buf_size)
{
	return chiaki_rpcrypt_crypt(rpcrypt, counter, buf, buf_size, true);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_rpcrypt_decrypt(ChiakiRPCrypt *rpcrypt, uint64_t counter, uint8_t *buf, size_t buf_size)
{
	return chiaki_rpcrypt_crypt(rpcrypt, counter, buf, buf_size, false);
}
