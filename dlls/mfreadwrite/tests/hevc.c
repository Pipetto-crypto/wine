/*
 * HEVC source reader tests.
 *
 * Copyright 2026 Wine Developers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <string.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "mfapi.h"
#include "mfidl.h"
#include "mferror.h"
#include "mfreadwrite.h"
#include "propvarutil.h"
#include "codecapi.h"

#include "wine/test.h"

static HRESULT (WINAPI *pMFCreateMFByteStreamOnStream)(IStream *stream, IMFByteStream **bytestream);

static IMFByteStream *get_resource_stream(const char *name)
{
    IMFByteStream *bytestream;
    IStream *stream;
    DWORD written;
    HRESULT hr;
    HRSRC res;
    void *ptr;

    res = FindResourceA(NULL, name, (const char *)RT_RCDATA);
    ok(res != 0, "Resource %s wasn't found.\n", name);
    if (!res) return NULL;

    ptr = LockResource(LoadResource(GetModuleHandleA(NULL), res));
    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) return NULL;

    hr = IStream_Write(stream, ptr, SizeofResource(GetModuleHandleA(NULL), res), &written);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IStream_Release(stream);
        return NULL;
    }

    hr = pMFCreateMFByteStreamOnStream(stream, &bytestream);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    IStream_Release(stream);
    return SUCCEEDED(hr) ? bytestream : NULL;
}

static HRESULT read_source_reader_video_sample(IMFSourceReader *reader, DWORD *actual_index, IMFSample **sample)
{
    DWORD flags;
    LONGLONG timestamp;
    HRESULT hr;
    int i;

    *sample = NULL;
    for (i = 0; i < 8; ++i)
    {
        hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                actual_index, &flags, &timestamp, sample);
        if (FAILED(hr)) return hr;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return MF_E_NOTACCEPTING;
        if (*sample) return S_OK;
    }

    return MF_E_NOTACCEPTING;
}

static void check_annex_b(IMFSample *sample)
{
    unsigned int nal_types = 0, type;
    IMFMediaBuffer *buffer;
    DWORD i, length;
    BYTE *data;
    HRESULT hr;

    hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) return;

    hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &length);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(length >= 5 && !data[0] && !data[1] &&
                (data[2] == 1 || (!data[2] && data[3] == 1)), "Missing Annex B start code.\n");
        for (i = 0; i + 4 < length; ++i)
        {
            if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
            type = (data[i + 3] >> 1) & 0x3f;
            if (type >= 32 && type <= 34) nal_types |= 1u << (type - 32);
            if (type < 32) break;
        }
        ok(nal_types == 7, "Expected VPS, SPS and PPS before picture, got mask %#x.\n", nal_types);
        ok(i + 4 < length, "Missing coded picture NAL unit.\n");
        hr = IMFMediaBuffer_Unlock(buffer);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    }
    IMFMediaBuffer_Release(buffer);
}

static void check_decoded_pixels(IMFSample *sample, IMFMediaType *media_type, BOOL main10)
{
    IMFMediaBuffer *buffer;
    BYTE *data;
    DWORD max_length, length, stride_value;
    LONG stride;
    UINT32 first, second;
    HRESULT hr;
    unsigned int row, first_offset, second_offset, pixel_size;

    hr = IMFMediaType_GetUINT32(media_type, &MF_MT_DEFAULT_STRIDE, &stride_value);
    if (FAILED(hr)) stride_value = main10 ? 128 : 64;
    stride = (LONG)stride_value;
    if (!stride) stride = main10 ? 128 : 64;
    pixel_size = main10 ? 2 : 1;
    row = stride >= 0 ? 16 * stride : 47 * -stride;
    first_offset = row + 4 * pixel_size;
    second_offset = row + 60 * pixel_size;

    hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) return;

    hr = IMFMediaBuffer_Lock(buffer, &data, &max_length, &length);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(length >= (main10 ? 64 * 64 * 3 : 64 * 64 * 3 / 2), "Got short decoded buffer length %lu.\n", length);
        ok(second_offset + pixel_size <= length, "Pixel offsets exceed decoded buffer length %lu.\n", length);
        if (second_offset + pixel_size <= length)
        {
            if (main10)
            {
                first = ((const WORD *)(data + first_offset))[0] >> 6;
                second = ((const WORD *)(data + second_offset))[0] >> 6;
                ok(first > second + 200, "Unexpected Main10 luma values %u, %u.\n", first, second);
            }
            else
            {
                first = data[first_offset];
                second = data[second_offset];
                ok(first > second + 50, "Unexpected NV12 luma values %u, %u.\n", first, second);
            }
        }
        hr = IMFMediaBuffer_Unlock(buffer);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    }

    IMFMediaBuffer_Release(buffer);
}

static void test_source_reader_hevc(const char *filename, UINT32 profile, const GUID *output_subtype, BOOL main10)
{
    IMFMediaType *input_type = NULL, *output_type = NULL, *current_type = NULL;
    IMFSourceReader *reader = NULL;
    IMFByteStream *stream = NULL;
    IMFSample *sample = NULL;
    PROPVARIANT position;
    GUID subtype;
    UINT32 actual_profile;
    DWORD actual_index;
    HRESULT hr;

    winetest_push_context("%s", filename);

    if (!pMFCreateMFByteStreamOnStream)
    {
        if (winetest_platform_is_wine) ok(0, "MFCreateMFByteStreamOnStream() unavailable on Wine.\n");
        else win_skip("MFCreateMFByteStreamOnStream() unavailable.\n");
        goto done;
    }

    stream = get_resource_stream(filename);
    if (!stream) goto done;

    hr = MFCreateSourceReaderFromByteStream(stream, NULL, &reader);
    if (FAILED(hr))
    {
        if (winetest_platform_is_wine) ok(0, "Failed to create HEVC source reader, hr %#lx.\n", hr);
        else win_skip("HEVC source reader unavailable (HEVC extension/GStreamer components missing), hr %#lx.\n", hr);
        goto done;
    }

    hr = IMFSourceReader_GetNativeMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &input_type);
    if (FAILED(hr))
    {
        if (winetest_platform_is_wine) ok(0, "HEVC native media type unavailable, hr %#lx.\n", hr);
        else win_skip("HEVC native media type unavailable (HEVC extension/GStreamer components missing), hr %#lx.\n", hr);
        goto done;
    }

    hr = IMFMediaType_GetGUID(input_type, &MF_MT_SUBTYPE, &subtype);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        ok(IsEqualGUID(&subtype, &MFVideoFormat_HEVC), "Got subtype %s.\n", debugstr_guid(&subtype));

    hr = IMFMediaType_GetUINT32(input_type, &MF_MT_MPEG2_PROFILE, &actual_profile);
    if (FAILED(hr))
    {
        if (winetest_platform_is_wine) ok(0, "HEVC profile unavailable, hr %#lx.\n", hr);
        else win_skip("HEVC profile unavailable (HEVC extension/GStreamer components missing), hr %#lx.\n", hr);
        goto done;
    }
    ok(actual_profile == profile, "Got profile %u, expected %u.\n", actual_profile, profile);

    if (winetest_platform_is_wine)
    {
        hr = read_source_reader_video_sample(reader, &actual_index, &sample);
        ok(hr == S_OK, "Failed to read native HEVC sample, hr %#lx.\n", hr);
        if (FAILED(hr)) goto done;
        ok(actual_index == 0, "Unexpected stream index %lu.\n", actual_index);
        check_annex_b(sample);
        IMFSample_Release(sample);
        sample = NULL;

        /* Start decoding on an IDR, not the inter frame following the native read. */
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = 0;
        hr = IMFSourceReader_SetCurrentPosition(reader, &GUID_NULL, &position);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        if (FAILED(hr)) goto done;
    }

    hr = MFCreateMediaType(&output_type);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_SetGUID(output_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = IMFMediaType_SetGUID(output_type, &MF_MT_SUBTYPE, output_subtype);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = IMFSourceReader_SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, output_type);
    if (FAILED(hr))
    {
        if (winetest_platform_is_wine) ok(0, "Failed to select HEVC output type, hr %#lx.\n", hr);
        else win_skip("HEVC decoder/output type unavailable (HEVC extension/GStreamer components missing), hr %#lx.\n", hr);
        goto done;
    }

    hr = IMFSourceReader_GetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IMFMediaType_GetGUID(current_type, &MF_MT_SUBTYPE, &subtype);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        ok(IsEqualGUID(&subtype, output_subtype), "Got output subtype %s.\n", debugstr_guid(&subtype));

    hr = read_source_reader_video_sample(reader, &actual_index, &sample);
    if (FAILED(hr))
    {
        if (winetest_platform_is_wine) ok(0, "HEVC decode failed, hr %#lx.\n", hr);
        else win_skip("HEVC decode unavailable (HEVC extension/GStreamer components missing), hr %#lx.\n", hr);
        goto done;
    }
    ok(actual_index == 0, "Unexpected stream index %lu.\n", actual_index);
    check_decoded_pixels(sample, current_type, main10);
    IMFSample_Release(sample);
    sample = NULL;

    /* Flush and seek exercise the reader's decoder reset and MP4 seek path. */
    hr = IMFSourceReader_Flush(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = 10000000;
    hr = IMFSourceReader_SetCurrentPosition(reader, &GUID_NULL, &position);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = read_source_reader_video_sample(reader, &actual_index, &sample);
        ok(hr == S_OK, "Failed to read HEVC sample after seek, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            ok(actual_index == 0, "Unexpected stream index %lu.\n", actual_index);
            IMFSample_Release(sample);
            sample = NULL;
        }
    }
    PropVariantClear(&position);

done:
    if (sample) IMFSample_Release(sample);
    if (current_type) IMFMediaType_Release(current_type);
    if (output_type) IMFMediaType_Release(output_type);
    if (input_type) IMFMediaType_Release(input_type);
    if (reader) IMFSourceReader_Release(reader);
    if (stream) IMFByteStream_Release(stream);
    winetest_pop_context();
}

START_TEST(hevc)
{
    HRESULT hr;
    HMODULE mod;

    CoInitialize(NULL);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    mod = GetModuleHandleA("mfplat.dll");
    pMFCreateMFByteStreamOnStream = (void *)GetProcAddress(mod, "MFCreateMFByteStreamOnStream");

    test_source_reader_hevc("test-hevc-main.mp4", eAVEncH265VProfile_Main_420_8, &MFVideoFormat_NV12, FALSE);
    test_source_reader_hevc("test-hevc-main10.mp4", eAVEncH265VProfile_Main_420_10, &MFVideoFormat_P010, TRUE);

    hr = MFShutdown();
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    CoUninitialize();
}
