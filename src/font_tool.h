/* File foo.  */
#ifndef FONT_TOOL
#define FONT_TOOL


#include "mupdf/fitz.h"
//#include <ft2build.h>
//#include FT_FREETYPE_H
//#include FT_SFNT_NAMES_H

//#include <string.h>
//#include <stdlib.h>

typedef struct FT_FaceRec_
{
    // Ключевые поля, которые могут понадобиться
    const char *family_name;
    const char *style_name;

    // Добавь другие поля по необходимости, но лучше не делать полный дубль
} *FT_Face;

typedef struct {
    char tag[4];
    unsigned char *data;
    uint32_t length;
} TableEntry;

static void write_u16(unsigned char **p, uint16_t v) {
    *(*p)++ = v >> 8;
    *(*p)++ = v & 0xFF;
}
static void write_u32(unsigned char **p, uint32_t v) {
    *(*p)++ = v >> 24;
    *(*p)++ = (v >> 16) & 0xFF;
    *(*p)++ = (v >> 8) & 0xFF;
    *(*p)++ = v & 0xFF;
}
static int ulog2(uint16_t v) {
    int r = 0;
    while ((1 << r) < v) r++;
    return r;
}
static fz_buffer *build_cmap_table(fz_context *ctx, FT_Face face)
{
    uint16_t segCount = 1;
    uint16_t segCountX2 = segCount * 2;
    uint16_t searchRange = 2;
    uint16_t entrySelector = 1;
    uint16_t rangeShift = segCountX2 - searchRange;

    size_t size = 14 + segCount * 8 + 2; // header + seg arrays + reservedPad + glyphIdArray
    fz_buffer *buf = fz_new_buffer(ctx, size);
    fz_resize_buffer(ctx, buf, size);

    unsigned char *p;
    fz_buffer_storage(ctx, buf, &p);

    write_u16(&p, 4);        // format
    write_u16(&p, (uint16_t)size);
    write_u16(&p, 0);        // language
    write_u16(&p, segCountX2);
    write_u16(&p, searchRange);
    write_u16(&p, entrySelector);
    write_u16(&p, rangeShift);

    write_u16(&p, 0xFFFF);   // endCode
    write_u16(&p, 0);        // reservedPad
    write_u16(&p, 0xFFFF);   // startCode
    write_u16(&p, 1);        // idDelta
    write_u16(&p, 0);        // idRangeOffset
    write_u16(&p, 0);        // glyphIdArray

    return buf;
}

static fz_buffer *build_name_table(fz_context *ctx, FT_Face face)
{
    const char *family = face->family_name ? face->family_name : "Unknown";
    const char *style = face->style_name ? face->style_name : family;

    size_t family_len = strlen(family);
    size_t style_len = strlen(style);

    size_t string_offset = 6 + 2 * 12;
    size_t total_len = string_offset + family_len + style_len;

    fz_buffer *buf = fz_new_buffer(ctx, total_len);
    fz_resize_buffer(ctx, buf, total_len);

    unsigned char *p;
    fz_buffer_storage(ctx, buf, &p);

    write_u16(&p, 0);      // format
    write_u16(&p, 2);      // count (2 записи)
    write_u16(&p, (uint16_t)string_offset);  // stringOffset

    // Запись 1: Font Family name (NameID=1)
    write_u16(&p, 3);      // platformID (Windows)
    write_u16(&p, 1);      // encodingID (Unicode BMP)
    write_u16(&p, 0x0409); // languageID (en-US)
    write_u16(&p, 1);      // nameID (Font Family)
    write_u16(&p, (uint16_t)family_len * 2);  // length (UTF-16BE)
    write_u16(&p, 0);      // offset в строковый блок (от string_offset)

    // Запись 2: Full Font Name (NameID=4)
    write_u16(&p, 3);      // platformID (Windows)
    write_u16(&p, 1);      // encodingID (Unicode BMP)
    write_u16(&p, 0x0409); // languageID (en-US)
    write_u16(&p, 4);      // nameID (Full Font Name)
    write_u16(&p, (uint16_t)style_len * 2);   // length
    write_u16(&p, (uint16_t)(family_len * 2)); // offset (после family)

    unsigned char *str_ptr = p;

    for (size_t i = 0; i < family_len; i++) {
        *str_ptr++ = 0;
        *str_ptr++ = (unsigned char)family[i];
    }
    for (size_t i = 0; i < style_len; i++) {
        *str_ptr++ = 0;
        *str_ptr++ = (unsigned char)style[i];
    }

    return buf;
}

fz_buffer *ccf_font_as_otf(fz_context *ctx, fz_font *font)
{
    if (!font || !font->ft_face)
        return NULL;

    FT_Face face = font->ft_face;

    fz_buffer *cff_buf = font->buffer;
    fz_buffer *cmap_buf = build_cmap_table(ctx, face);
    fz_buffer *name_buf = build_name_table(ctx, face);

    unsigned char *cff_data, *cmap_data, *name_data;
    size_t cff_len = fz_buffer_storage(ctx, cff_buf, &cff_data);
    size_t cmap_len = fz_buffer_storage(ctx, cmap_buf, &cmap_data);
    size_t name_len = fz_buffer_storage(ctx, name_buf, &name_data);

    TableEntry tables[3] = {
        { { 'C','F','F',' ' }, cff_data, (uint32_t)cff_len },
        { { 'c','m','a','p' }, cmap_data, (uint32_t)cmap_len },
        { { 'n','a','m','e' }, name_data, (uint32_t)name_len },
    };

    int numTables = 3;
    uint16_t maxPow2 = 1 << ulog2(numTables);
    uint16_t searchRange = maxPow2 * 16;
    uint16_t entrySelector = ulog2(maxPow2);
    uint16_t rangeShift = numTables * 16 - searchRange;

    size_t offset = 12 + numTables * 16;
    for (int i = 0; i < numTables; i++) {
        offset = (offset + 3) & ~3; // align 4
        offset += (tables[i].length + 3) & ~3;
    }

    fz_buffer *out = fz_new_buffer(ctx, offset);
    fz_resize_buffer(ctx, out, offset);

    unsigned char *p;
    fz_buffer_storage(ctx, out, &p);

    // Header
    *p++ = 'O'; *p++ = 'T'; *p++ = 'T'; *p++ = 'O';
    write_u16(&p, numTables);
    write_u16(&p, searchRange);
    write_u16(&p, entrySelector);
    write_u16(&p, rangeShift);

    size_t header_size = 12 + numTables * 16;
    size_t data_offset = header_size;

    for (int i = 0; i < numTables; i++) {
        TableEntry *t = &tables[i];
        uint32_t aligned_len = (t->length + 3) & ~3;
        data_offset = (data_offset + 3) & ~3;

        memcpy(p, t->tag, 4); p += 4;
        write_u32(&p, 0); // checksum (placeholder)
        write_u32(&p, (uint32_t)data_offset);
        write_u32(&p, t->length);

        data_offset += aligned_len;
    }

    // Write data
    for (int i = 0; i < numTables; i++) {
        uint32_t len = tables[i].length;
        unsigned char *data = tables[i].data;
        p = out->data + (char)(tables[i].tag[0] == 'C' ? header_size : 0);
        size_t pos = (p - out->data);
        pos = (pos + 3) & ~3;
        memcpy(out->data + pos, data, len);
    }

    fz_drop_buffer(ctx, cff_buf);
    fz_drop_buffer(ctx, cmap_buf);
    fz_drop_buffer(ctx, name_buf);

    return out;
}

fz_buffer* export_font_as_otf(fz_context *ctx, fz_font *font)
{
    fz_buffer *buf = font->buffer;

    if (!buf)
        return NULL;

    unsigned char *data;
    size_t size = fz_buffer_storage(ctx, buf, &data);

    // Проверка на TrueType (sfnt с glyf)
    if (size >= 4 && data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00)
    {
        // Уже валидный TTF — вернуть как есть (копию, чтобы caller мог освободить)
        return fz_keep_buffer(ctx, buf);
    }

    // Проверка на OTF с CFF (начинается с 'OTTO')
    if (size >= 4 && data[0] == 'O' && data[1] == 'T' && data[2] == 'T' && data[3] == 'O')
    {
        return fz_keep_buffer(ctx, buf);
    }

    // Проверка на чистый CFF (01 00 04 00)
    if (size >= 4 && data[0] == 0x01 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00)
    {
        return ccf_font_as_otf(ctx, font);
    }

    // Возможно Type1 (pfb/pfa) — можно здесь добавить конвертацию в CFF, если потребуется

    // fz_drop_buffer(ctx, buf);
    return NULL; // Неизвестный формат
}

#endif