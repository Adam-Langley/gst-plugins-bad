/* Gstreamer
 * Copyright (C) <2011> Intel Corporation
 * Copyright (C) <2011> Collabora Ltd.
 * Copyright (C) <2011> Thibault Saunier <thibault.saunier@collabora.com>
 *
 * Some bits C-c,C-v'ed and s/4/3 from h264parse and videoparsers/h264parse.c:
 *    Copyright (C) <2010> Mark Nauwelaerts <mark.nauwelaerts@collabora.co.uk>
 *    Copyright (C) <2010> Collabora Multimedia
 *    Copyright (C) <2010> Nokia Corporation
 *
 *    (C) 2005 Michal Benes <michal.benes@itonis.tv>
 *    (C) 2008 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * Common code for NAL parsing from h264 and h265 parsers.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "nalutils.h"

/* Compute Ceil(Log2(v)) */
/* Derived from branchless code for integer log2(v) from:
   <http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog> */
guint
ceil_log2 (guint32 v)
{
  guint r, shift;

  v--;
  r = (v > 0xFFFF) << 4;
  v >>= r;
  shift = (v > 0xFF) << 3;
  v >>= shift;
  r |= shift;
  shift = (v > 0xF) << 2;
  v >>= shift;
  r |= shift;
  shift = (v > 0x3) << 1;
  v >>= shift;
  r |= shift;
  r |= (v >> 1);
  return r + 1;
}

/****** Nal parser ******/

void
nal_reader_init (NalReader * nr, const guint8 * data, guint size)
{
  nr->data = data;
  nr->size = size;
  nr->n_epb = 0;

  nr->byte = 0;
  nr->bits_in_cache = 0;
  /* fill with something other than 0 to detect emulation prevention bytes */
  nr->first_byte = 0xff;
  nr->cache = 0xff;
}

inline gboolean
nal_reader_read (NalReader * nr, guint nbits)
{
  if (G_UNLIKELY (nr->byte * 8 + (nbits - nr->bits_in_cache) > nr->size * 8)) {
    GST_DEBUG ("Can not read %u bits, bits in cache %u, Byte * 8 %u, size in "
        "bits %u", nbits, nr->bits_in_cache, nr->byte * 8, nr->size * 8);
    return FALSE;
  }

  while (nr->bits_in_cache < nbits) {
    guint8 byte;
    gboolean check_three_byte;

    check_three_byte = TRUE;
  next_byte:
    if (G_UNLIKELY (nr->byte >= nr->size))
      return FALSE;

    byte = nr->data[nr->byte++];

    /* check if the byte is a emulation_prevention_three_byte */
    if (check_three_byte && byte == 0x03 && nr->first_byte == 0x00 &&
        ((nr->cache & 0xff) == 0)) {
      /* next byte goes unconditionally to the cache, even if it's 0x03 */
      check_three_byte = FALSE;
      nr->n_epb++;
      goto next_byte;
    }
    nr->cache = (nr->cache << 8) | nr->first_byte;
    nr->first_byte = byte;
    nr->bits_in_cache += 8;
  }

  return TRUE;
}

inline gboolean
nal_reader_skip (NalReader * nr, guint nbits)
{
  if (G_UNLIKELY (!nal_reader_read (nr, nbits)))
    return FALSE;

  nr->bits_in_cache -= nbits;

  return TRUE;
}

inline gboolean
nal_reader_skip_to_next_byte (NalReader * nr)
{
  if (nr->bits_in_cache == 0) {
    if (G_LIKELY ((nr->size - nr->byte) > 0))
      nr->byte++;
    else
      return FALSE;
  }

  nr->bits_in_cache = 0;

  return TRUE;
}

inline guint
nal_reader_get_pos (const NalReader * nr)
{
  return nr->byte * 8 - nr->bits_in_cache;
}

inline guint
nal_reader_get_remaining (const NalReader * nr)
{
  return (nr->size - nr->byte) * 8 + nr->bits_in_cache;
}

inline guint
nal_reader_get_epb_count (const NalReader * nr)
{
  return nr->n_epb;
}

#define GST_NAL_READER_READ_BITS(bits) \
gboolean \
nal_reader_get_bits_uint##bits (NalReader *nr, guint##bits *val, guint nbits) \
{ \
  guint shift; \
  \
  if (!nal_reader_read (nr, nbits)) \
    return FALSE; \
  \
  /* bring the required bits down and truncate */ \
  shift = nr->bits_in_cache - nbits; \
  *val = nr->first_byte >> shift; \
  \
  *val |= nr->cache << (8 - shift); \
  /* mask out required bits */ \
  if (nbits < bits) \
    *val &= ((guint##bits)1 << nbits) - 1; \
  \
  nr->bits_in_cache = shift; \
  \
  return TRUE; \
} \

GST_NAL_READER_READ_BITS (8);
GST_NAL_READER_READ_BITS (16);
GST_NAL_READER_READ_BITS (32);

#define GST_NAL_READER_PEEK_BITS(bits) \
gboolean \
nal_reader_peek_bits_uint##bits (const NalReader *nr, guint##bits *val, guint nbits) \
{ \
  NalReader tmp; \
  \
  tmp = *nr; \
  return nal_reader_get_bits_uint##bits (&tmp, val, nbits); \
}

GST_NAL_READER_PEEK_BITS (8);

gboolean
nal_reader_get_ue (NalReader * nr, guint32 * val)
{
  guint i = 0;
  guint8 bit;
  guint32 value;

  if (G_UNLIKELY (!nal_reader_get_bits_uint8 (nr, &bit, 1))) {

    return FALSE;
  }

  while (bit == 0) {
    i++;
    if G_UNLIKELY
      ((!nal_reader_get_bits_uint8 (nr, &bit, 1)))
          return FALSE;
  }

  if (G_UNLIKELY (i > 32))
    return FALSE;

  if (G_UNLIKELY (!nal_reader_get_bits_uint32 (nr, &value, i)))
    return FALSE;

  *val = (1 << i) - 1 + value;

  return TRUE;
}

inline gboolean
nal_reader_get_se (NalReader * nr, gint32 * val)
{
  guint32 value;

  if (G_UNLIKELY (!nal_reader_get_ue (nr, &value)))
    return FALSE;

  if (value % 2)
    *val = (value / 2) + 1;
  else
    *val = -(value / 2);

  return TRUE;
}

gboolean
gst_nal_reader_is_byte_aligned (NalReader * nr)
{
  if (nr->bits_in_cache != 0)
    return FALSE;
  return TRUE;
}

gboolean
gst_nal_reader_has_more_data (NalReader * nr)
{
  guint remaining;

  remaining = nal_reader_get_remaining (nr);
  if (remaining == 0)
    return FALSE;

  if (remaining <= 8) {
    guint8 rbsp_stop_one_bit;

    if (!nal_reader_peek_bits_uint8 (nr, &rbsp_stop_one_bit, 1))
      return FALSE;

    if (rbsp_stop_one_bit == 1) {
      guint8 zero_bits;

      if (remaining == 1)
        return FALSE;

      if (!nal_reader_peek_bits_uint8 (nr, &zero_bits, remaining))
        return FALSE;

      if ((zero_bits - (1 << (remaining - 1))) == 0)
        return FALSE;
    }
  }

  return TRUE;
}

/***********  end of nal parser ***************/

inline gint
scan_for_start_codes (const guint8 * data, guint size)
{
  GstByteReader br;
  gst_byte_reader_init (&br, data, size);

  /* NALU not empty, so we can at least expect 1 (even 2) bytes following sc */
  return gst_byte_reader_masked_scan_uint32 (&br, 0xffffff00, 0x00000100,
      0, size);
}
