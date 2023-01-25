/* cm-input-stream.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-input-stream"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>

#include "cm-utils-private.h"
#include "cm-input-stream-private.h"

struct _CmInputStream
{
  GFilterInputStream parent_instance;

  gcry_cipher_hd_t   cipher_hd;

  char              *aes_key_base64;
  char              *aes_iv_base64;
  char              *sha256_base64;

  /* For files that will be used to upload */
  GFile             *file;
  GFileInfo         *file_info;
  GChecksum         *checksum;
  gboolean           checksum_complete;
  gboolean           encrypt;

  char              *buffer;
  int                buffer_len;

  gcry_error_t       gcr_error;
};

G_DEFINE_TYPE (CmInputStream, cm_input_stream, G_TYPE_FILTER_INPUT_STREAM)

static char *
value_to_unpadded_base64 (const guchar *data,
                          gsize         data_len,
                          gboolean      url_safe)
{
  char *base64;

  g_assert (data);
  g_assert (data_len);

  base64 = g_base64_encode (data, data_len);
  g_strdelimit (base64, "=", '\0');

  if (url_safe)
    {
      g_strdelimit (base64, "/", '_');
      g_strdelimit (base64, "+", '-');
    }

  return base64;
}

static void
parse_base64_value (const char  *unpadded_base64,
                    guchar     **out,
                    gsize       *out_len)
{
  g_autofree char *base64 = NULL;
  gsize len, padded_len;

  g_assert (out);
  g_assert (out_len);

  if (!unpadded_base64)
    return;

  len = strlen (unpadded_base64);
  /* base64 is always multiple of 4, so add space for padding */
  if (len % 4)
    padded_len = len + 4 - len % 4;
  else
    padded_len = len;
  base64 = malloc (padded_len + 1);
  strcpy (base64, unpadded_base64);
  memset (base64 + len, '=', padded_len - len);
  base64[padded_len] = '\0';

  *out = g_base64_decode (base64, out_len);
}

static gssize
cm_input_stream_read_fn (GInputStream  *stream,
                         void          *buffer,
                         gsize          count,
                         GCancellable  *cancellable,
                         GError       **error)
{
  CmInputStream *self = (CmInputStream *)stream;
  gssize n_read = -1;

  if (self->gcr_error)
    goto end;

  n_read = G_INPUT_STREAM_CLASS (cm_input_stream_parent_class)->read_fn (stream, buffer, count,
                                                                         cancellable, error);

  if (self->cipher_hd && n_read > 0)
    {
      /* We need sha256 checksums only for encrypted/to be encrypted files */
      if (!self->checksum)
        self->checksum = g_checksum_new (G_CHECKSUM_SHA256);

      if (G_UNLIKELY (self->buffer_len < n_read))
        {
          self->buffer_len = MAX (n_read, 1024 * 8);
          self->buffer = g_realloc (self->buffer, self->buffer_len);
        }
    }

  /* Since it's CTR mode, the encrypted and decrypted always have the same size */
  if (self->cipher_hd && n_read > 0)
    {
      if (self->encrypt)
        {
          self->gcr_error = gcry_cipher_encrypt (self->cipher_hd, self->buffer,
                                                 n_read, buffer, n_read);
          /* we are encrypting, calculate the checksum after encryption */
          if (!self->gcr_error)
            g_checksum_update (self->checksum, (gpointer)self->buffer, n_read);
        }
      else
        {
          /* we are decrypting, calculate the checksum before decryption */
          g_checksum_update (self->checksum, buffer, n_read);
          self->gcr_error = gcry_cipher_decrypt (self->cipher_hd, self->buffer,
                                                 n_read, buffer, n_read);
        }

      if (!self->gcr_error)
        memcpy (buffer, self->buffer, n_read);
    }

  if (!self->gcr_error && self->cipher_hd && n_read == 0)
    self->checksum_complete = TRUE;

 end:
  if (self->gcr_error)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed decrypting buffer: %s", gcry_strerror (self->gcr_error));
      return -1;
    }

  return n_read;
}

static void
cm_input_stream_finalize (GObject *object)
{
  CmInputStream *self = (CmInputStream *)object;

  if (self->cipher_hd)
    gcry_cipher_close (self->cipher_hd);

  if (self->checksum)
    g_checksum_free (self->checksum);

  g_free (self->buffer);

  g_clear_object (&self->file);
  g_clear_object (&self->file_info);

  G_OBJECT_CLASS (cm_input_stream_parent_class)->finalize (object);
}

static void
cm_input_stream_class_init (CmInputStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *input_stream_class = G_INPUT_STREAM_CLASS (klass);

  object_class->finalize = cm_input_stream_finalize;

  input_stream_class->read_fn = cm_input_stream_read_fn;
}

static void
cm_input_stream_init (CmInputStream *self)
{
}

CmInputStream *
cm_input_stream_new (GInputStream *base_stream)
{
  return g_object_new (CM_TYPE_INPUT_STREAM,
                       "base-stream", base_stream,
                       NULL);
}

CmInputStream *
cm_input_stream_new_from_file (GFile         *file,
                               gboolean       encrypt,
                               GCancellable  *cancellable,
                               GError       **error)
{
  CmInputStream *self;
  GInputStream *stream;
  GFileInfo *file_info;

  g_return_val_if_fail (G_IS_FILE (file), NULL);
  g_return_val_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable), NULL);

  stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

  if (!stream)
    return NULL;

  file_info = g_file_query_info (file,
                                 G_FILE_ATTRIBUTE_STANDARD_SIZE","G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                 G_FILE_QUERY_INFO_NONE,
                                 cancellable,
                                 error);
  if (!file_info)
    return NULL;

  self = cm_input_stream_new (stream);
  self->file_info = file_info;
  self->file = g_object_ref (file);
  self->encrypt = !!encrypt;

  if (encrypt)
    cm_input_stream_set_encrypt (self);

  return self;
}

void
cm_input_stream_set_file_enc (CmInputStream *self,
                              CmEncFileInfo *file)
{
  gcry_cipher_hd_t cipher_hd;
  gsize len;

  g_return_if_fail (CM_IS_INPUT_STREAM (self));

  if (!file)
    return;

  g_return_if_fail (file->mxc_uri);
  g_return_if_fail (file->aes_key_base64);
  g_return_if_fail (!self->cipher_hd);

  self->gcr_error = gcry_cipher_open (&cipher_hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CTR, 0);

  if (!self->gcr_error)
    self->cipher_hd = cipher_hd;

  if (!self->gcr_error)
    {
      g_autofree char *key_base64 = NULL;
      g_autofree guchar *key = NULL;

      /* uses unpadded base64url */
      key_base64 = g_strdup (file->aes_key_base64);
      g_strdelimit (key_base64, "_", '/');
      g_strdelimit (key_base64, "-", '+');
      parse_base64_value (key_base64, &key, &len);
      self->gcr_error = gcry_cipher_setkey (cipher_hd, key, len);

      cm_utils_clear ((char *)key, len);
      cm_utils_clear (key_base64, -1);
    }

  if (!self->gcr_error)
    {
      g_autofree char *iv_base64 = NULL;
      g_autofree guchar *iv = NULL;

      iv_base64 = g_strdup (file->aes_iv_base64);
      /* uses unpadded base64 */
      parse_base64_value (iv_base64, &iv, &len);
      self->gcr_error = gcry_cipher_setctr (cipher_hd, iv, len);

      cm_utils_clear ((char *)iv, len);
      cm_utils_clear (iv_base64, -1);
    }
}

void
cm_input_stream_set_encrypt (CmInputStream *self)
{
  gcry_cipher_hd_t cipher_hd;

  g_return_if_fail (CM_IS_INPUT_STREAM (self));
  g_return_if_fail (!self->cipher_hd);

  self->gcr_error = gcry_cipher_open (&cipher_hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CTR, 0);

  if (!self->gcr_error)
    self->cipher_hd = cipher_hd;

  if (!self->gcr_error)
    {
      g_autofree guchar *key = NULL;

      key = gcry_random_bytes (32, GCRY_STRONG_RANDOM);
      self->aes_key_base64 = value_to_unpadded_base64 (key, 32, TRUE);
      self->gcr_error = gcry_cipher_setkey (cipher_hd, key, 32);

      cm_utils_clear ((char *)key, 32);
    }

  if (!self->gcr_error)
    {
      g_autofree guchar *iv = NULL;

      /* The first 8 bytes has to be random, and the rest (counter) has to be 0 */
      iv = g_malloc0 (16);
      gcry_randomize (iv, 8, GCRY_STRONG_RANDOM);
      self->aes_iv_base64 = value_to_unpadded_base64 (iv, 16, FALSE);
      self->gcr_error = gcry_cipher_setctr (cipher_hd, iv, 16);

      cm_utils_clear ((char *)iv, 16);
    }
}

char *
cm_input_stream_get_sha256 (CmInputStream *self)
{
  guint8 *buffer;
  gsize digest_len;

  g_return_val_if_fail (CM_IS_INPUT_STREAM (self), NULL);

  if (!self->checksum || !self->checksum_complete)
    return NULL;

  digest_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
  buffer = g_malloc (digest_len);
  g_checksum_get_digest (self->checksum, buffer, &digest_len);

  return value_to_unpadded_base64 (buffer, digest_len, FALSE);
}

const char *
cm_input_stream_get_content_type (CmInputStream *self)
{
  const char *content_type;

  g_return_val_if_fail (CM_IS_INPUT_STREAM (self), NULL);

  if (!self->file_info)
    return NULL;

  content_type = g_file_info_get_content_type (self->file_info);

  if (content_type && !self->encrypt)
    return content_type;

  return "application/octect-stream";
}

goffset
cm_input_stream_get_size (CmInputStream *self)
{
  g_return_val_if_fail (CM_IS_INPUT_STREAM (self), 0);

  if (!self->file_info)
    return 0;

  return g_file_info_get_size (self->file_info);
}

JsonObject *
cm_input_stream_get_file_json (CmInputStream *self)
{
  g_autofree char *sha256 = NULL;
  JsonObject *root, *child;
  JsonArray *array;
  const char *url;

  g_return_val_if_fail (CM_IS_INPUT_STREAM (self), NULL);

  /* Return JSON only if the stream is used to encrypt and after file
   * has been read completely
   */
  if (!self->encrypt || !self->checksum_complete || !self->cipher_hd)
    return NULL;

  /* The mxc url should have set somewhere else */
  if (!g_object_get_data (G_OBJECT (self), "uri"))
    return NULL;

  url = g_object_get_data (G_OBJECT (self), "uri");
  root = json_object_new ();
  json_object_set_string_member (root, "v", "v2");
  json_object_set_string_member (root, "url", url);
  json_object_set_string_member (root, "iv", self->aes_iv_base64);

  sha256 = cm_input_stream_get_sha256 (self);
  child = json_object_new ();
  json_object_set_string_member (child, "sha256", sha256);
  json_object_set_object_member (root, "hashes", child);

  array = json_array_new ();
  json_array_add_string_element (array, "encrypt");
  json_array_add_string_element (array, "decrypt");

  child = json_object_new ();
  json_object_set_array_member (child, "key_ops", array);
  json_object_set_string_member (child, "alg", "A256CTR");
  json_object_set_string_member (child, "kty", "oct");
  json_object_set_string_member (child, "k", self->aes_key_base64);
  json_object_set_boolean_member (child, "ext", TRUE);
  json_object_set_object_member (root, "key", child);

  return root;
}
