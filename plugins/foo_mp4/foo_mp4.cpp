/*
** FAAD - Freeware Advanced Audio Decoder
** Copyright (C) 2002-2003 M. Bakker
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** $Id: foo_mp4.cpp,v 1.27 2003/04/27 18:53:22 menno Exp $
**/

#include <mp4.h>
#include <faad.h>
#include "pfc/pfc.h"
#include "foobar2000/SDK/input.h"
#include "foobar2000/SDK/console.h"
#include "foobar2000/SDK/componentversion.h"
#include "foobar2000/SDK/tagread.h"

char *STRIP_REVISION(const char *str)
{
    char *tmp = strchr(str, ' ');
    tmp[strlen(tmp)-2] = '\0';
    return tmp;
}

DECLARE_COMPONENT_VERSION ("MPEG-4 AAC decoder",
                           STRIP_REVISION("$Revision: 1.27 $"),
                           "Based on FAAD2 v" FAAD2_VERSION "\nCopyright (C) 2002-2003 http://www.audiocoding.com" );

class input_mp4 : public input
{
public:

    virtual int test_filename(const char * fn,const char * ext)
    {
        return !stricmp(ext,"MP4");
    }

    virtual int open(reader *r, file_info *info, int full_open)
    {
        unsigned __int8 *buffer;
        unsigned __int32 buffer_size;
        unsigned __int8 channels;
        unsigned __int32 samplerate;

        faacDecConfigurationPtr config;
        mp4AudioSpecificConfig mp4ASC;

        m_reader = r;

        hDecoder = faacDecOpen();
        if (!hDecoder)
        {
            console::error("Failed to open FAAD2 library.", "foo_mp4");
            return 0;
        }

        config = faacDecGetCurrentConfiguration(hDecoder);
        config->outputFormat = FAAD_FMT_DOUBLE;
        faacDecSetConfiguration(hDecoder, config);

        hFile = MP4ReadCb(0, open_cb, close_cb, read_cb, write_cb,
            setpos_cb, getpos_cb, filesize_cb, (void*)m_reader);
        if (hFile == MP4_INVALID_FILE_HANDLE)
        {
            console::error("Failed to open MP4 file.", "foo_mp4");
            return 0;
        }

        track = GetAACTrack(hFile);
        if (track < 1)
        {
            console::error("No valid AAC track found.", "foo_mp4");
            return 0;
        }

        buffer = NULL;
        buffer_size = 0;
        MP4GetTrackESConfiguration(hFile, track, &buffer, &buffer_size);
        if (!buffer)
        {
            console::error("Unable to read track specific configuration.", "foo_mp4");
            return 0;
        }

        AudioSpecificConfig((unsigned char*)buffer, buffer_size, &mp4ASC);

        int rc = faacDecInit2(hDecoder, (unsigned char*)buffer, buffer_size,
            (unsigned long*)&samplerate, (unsigned char*)&channels);
        if (buffer) free(buffer);
        if (rc < 0)
        {
            console::error("Unable to initialise FAAD2 library.", "foo_mp4");
            return 0;
        }

        numSamples = MP4GetTrackNumberOfSamples(hFile, track);
        sampleId = 1;

        unsigned __int64 length = MP4GetTrackDuration(hFile, track);
        __int64 msDuration = MP4ConvertFromTrackDuration(hFile, track,
            length, MP4_MSECS_TIME_SCALE);
        info->set_length((double)msDuration/1000.0);

        info->info_set_int("bitrate",(__int64)(1.0/1000.0 *
            (double)(__int64)MP4GetTrackIntegerProperty(hFile,
            track, "mdia.minf.stbl.stsd.mp4a.esds.decConfigDescr.avgBitrate")) + 0.5);
        info->info_set_int("channels", (__int64)channels);
        info->info_set_int("samplerate", (__int64)samplerate);

#if 0
        if (mp4ASC.sbr_present_flag)
            info->info_set("codec", "AAC+SBR");
        else
#endif
            info->info_set("codec", "AAC");

        ReadMP4Tag(info);

        return 1;
    }

    input_mp4()
    {
        hFile = MP4_INVALID_FILE_HANDLE;
        hDecoder = NULL;
    }

    ~input_mp4()
    {
        if (hFile != MP4_INVALID_FILE_HANDLE)
            MP4Close(hFile);
        if (hDecoder)
            faacDecClose(hDecoder);
    }

    virtual int run(audio_chunk * chunk)
    {
        faacDecFrameInfo frameInfo;
        unsigned char *buffer;
        unsigned __int32 buffer_size;
        void *sample_buffer;

        if (sampleId == MP4_INVALID_SAMPLE_ID)
        {
            console::error("Invalid sampleId.", "foo_mp4");
            return 0;
        }

        do {
            buffer = NULL;
            buffer_size = 0;

            MP4ReadSample(hFile, track, sampleId,
                (unsigned __int8**)&buffer, &buffer_size,
                NULL, NULL, NULL, NULL);
            sampleId++;

            sample_buffer = faacDecDecode(hDecoder, &frameInfo, buffer, buffer_size);

            if (buffer) free(buffer);

        } while ((frameInfo.error == 0) && (frameInfo.samples == 0));

        if (frameInfo.error || (sampleId > numSamples))
        {
            if (frameInfo.error)
                console::error(faacDecGetErrorMessage(frameInfo.error), "foo_mp4");
            return 0;
        }

        chunk->data = (audio_sample*)sample_buffer;
        chunk->samples = frameInfo.samples/frameInfo.channels;
        chunk->nch = frameInfo.channels;
        chunk->srate = frameInfo.samplerate;

        return 1;
    }

    virtual int set_info(reader *r,const file_info * info)
    {
        m_reader = r;

        hFile = MP4ModifyCb(0, 0, open_cb, close_cb, read_cb, write_cb,
            setpos_cb, getpos_cb, filesize_cb, (void*)m_reader);
        if (hFile == MP4_INVALID_FILE_HANDLE) return 0;

        track = GetAACTrack(hFile);
        if (track < 1)
        {
            console::error("No valid AAC track found.", "foo_mp4");
            return 0;
        }

        MP4TagDelete(hFile, track);

        /* replay gain writing */
        const char *p = NULL;

        p = info->info_get("REPLAYGAIN_TRACK_PEAK");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_TRACK_PEAK", p);
        p = info->info_get("REPLAYGAIN_TRACK_GAIN");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_TRACK_GAIN", p);
        p = info->info_get("REPLAYGAIN_ALBUM_PEAK");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_ALBUM_PEAK", p);
        p = info->info_get("REPLAYGAIN_ALBUM_GAIN");
        if (p)
            MP4TagAddEntry(hFile, track, "REPLAYGAIN_ALBUM_GAIN", p);

        int numItems = info->meta_get_count();
        if (numItems > 0)
        {
            for (int i = 0; i < numItems; i++)
            {
                const char *n = info->meta_enum_name(i);
                const char *v = info->meta_enum_value(i);
                MP4TagAddEntry(hFile, track, n, v);
            }
        }

        numItems = MP4TagGetNumEntries(hFile, track);
        if (numItems == 0)
            MP4TagDelete(hFile, track);

        /* end */
        return 1;
    }

    virtual int seek(double seconds)
    {
        MP4Duration duration;

        duration = MP4ConvertToTrackDuration(hFile,
            track, seconds, MP4_SECS_TIME_SCALE);
        sampleId = MP4GetSampleIdFromTime(hFile,
            track, duration, 0);

        return 1;
    }
    
    virtual int is_our_content_type(const char *url, const char *type)
    {
        return !strcmp(type, "audio/mp4") || !strcmp(type, "audio/x-mp4") ||
            !strcmp(type, "audio/mp4a");
    }

private:

    reader *m_reader;

    faacDecHandle hDecoder;

    MP4FileHandle hFile;
    MP4SampleId sampleId, numSamples;
    MP4TrackId track;

    int ReadMP4Tag(file_info *info)
    {
        int numItems = MP4TagGetNumEntries(hFile, track);

        for (int i = 0; i < numItems; i++)
        {
            float f = 0.0;
            const char *n = NULL, *v = NULL;

            MP4TagGetEntry(hFile, track, i, &n, &v);

            if (!strcmp(n, "REPLAYGAIN_TRACK_PEAK"))
            {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_track_peak((double)f);
            } else if (!strcmp(n, "REPLAYGAIN_TRACK_GAIN")) {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_track_gain((double)f);
            } else if (!strcmp(n, "REPLAYGAIN_ALBUM_PEAK")) {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_album_peak((double)f);
            } else if (!strcmp(n, "REPLAYGAIN_ALBUM_GAIN")) {
                sscanf(v, "%f", &f);
                info->info_set_replaygain_album_gain((double)f);
            } else {
                info->meta_add(n, v);
            }
        }

        return 1;
    }

    int GetAACTrack(MP4FileHandle infile)
    {
        /* find AAC track */
        int i, rc;
        int numTracks = MP4GetNumberOfTracks(infile, NULL, /* subType */ 0);

        for (i = 0; i < numTracks; i++)
        {
            MP4TrackId trackId = MP4FindTrackId(infile, i, NULL, /* subType */ 0);
            const char* trackType = MP4GetTrackType(infile, trackId);

            if (!strcmp(trackType, MP4_AUDIO_TRACK_TYPE))
            {
                unsigned char *buff = NULL;
                int buff_size = 0;
                mp4AudioSpecificConfig mp4ASC;

                MP4GetTrackESConfiguration(infile, trackId,
                    (unsigned __int8**)&buff, (unsigned __int32*)&buff_size);

                if (buff)
                {
                    rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
                    free(buff);

                    if (rc < 0)
                        return -1;
                    return trackId;
                }
            }
        }

        /* can't decode this */
        return -1;
    }

    /* file callback stuff */
    static unsigned __int32 open_cb(const char *pName,
        const char *mode, void *userData)
    {
        return 1;
    }

    static void close_cb(void *userData)
    {
        return;
    }

    static unsigned __int32 read_cb(void *pBuffer, unsigned int nBytesToRead,
        void *userData)
    {
        reader *r = (reader*)userData;
        return r->read(pBuffer, nBytesToRead);
    }

    static unsigned __int32 write_cb(void *pBuffer, unsigned int nBytesToWrite,
        void *userData)
    {
        reader *r = (reader*)userData;
        return r->write(pBuffer, nBytesToWrite);
    }

    static __int64 getpos_cb(void *userData)
    {
        reader *r = (reader*)userData;
        return r->get_position();
    }

    static __int32 setpos_cb(unsigned __int32 pos, void *userData)
    {
        reader *r = (reader*)userData;
        return !(r->seek(pos));
    }

    static __int64 filesize_cb(void *userData)
    {
        reader *r = (reader*)userData;
        return r->get_length();
    }
};

struct seek_list
{
    seek_list *next;
    __int64 offset;
};

class input_aac : public input
{
public:

    virtual int test_filename(const char * fn,const char * ext)
    {
        return !stricmp(ext,"AAC");
    }

    virtual int open(reader *r, file_info *info, int full_open)
    {
        int tagsize = 0, tmp = 0;
        int bread = 0;
        double length = 1.;
        __int64 bitrate = 128;
        unsigned char channels = 0;
        unsigned long samplerate = 0;

        faacDecConfigurationPtr config;

        m_reader = r;

        hDecoder = faacDecOpen();
        if (!hDecoder)
        {
            console::error("Failed to open FAAD2 library.", "foo_mp4");
            return 0;
        }

        config = faacDecGetCurrentConfiguration(hDecoder);
        config->outputFormat = FAAD_FMT_DOUBLE;
        faacDecSetConfiguration(hDecoder, config);

        tag_reader::process_file(m_reader, info);

        m_at_eof = 0;

        if (!(m_aac_buffer = (unsigned char*)malloc(768*6)))
        {
            console::error("Memory allocation error.", "foo_mp4");
            return 0;
        }
        memset(m_aac_buffer, 0, 768*6);

        bread = m_reader->read(m_aac_buffer, 768*6);
        m_aac_bytes_into_buffer = bread;
        m_aac_bytes_consumed = 0;
        m_file_offset = 0;

        if (bread != 768*6)
            m_at_eof = 1;

        if (!stricmp((const char*)m_aac_buffer, "ID3"))
        {
            /* high bit is not used */
            tagsize = (m_aac_buffer[6] << 21) | (m_aac_buffer[7] << 14) |
                (m_aac_buffer[8] <<  7) | (m_aac_buffer[9] <<  0);

            tagsize += 10;
        }

        if (tagsize)
        {
            advance_buffer(tagsize);
            fill_buffer();
        }

        m_head = (struct seek_list*)malloc(sizeof(struct seek_list));
        m_tail = m_head;
        m_tail->next = NULL;

        m_header_type = 0;
        if ((m_aac_buffer[0] == 0xFF) && ((m_aac_buffer[1] & 0xF6) == 0xF0))
        {
            if (m_reader->can_seek())
            {
                adts_parse(&bitrate, &length);
                m_reader->seek(tagsize);

                bread = m_reader->read(m_aac_buffer, 768*6);
                if (bread != 768*6)
                    m_at_eof = 1;
                else
                    m_at_eof = 0;
                m_aac_bytes_into_buffer = bread;
                m_aac_bytes_consumed = 0;

                m_header_type = 1;
            }
        } else if (memcmp(m_aac_buffer, "ADIF", 4) == 0) {
            int skip_size = (m_aac_buffer[4] & 0x80) ? 9 : 0;
            bitrate =  ((unsigned int)(m_aac_buffer[4 + skip_size] & 0x0F)<<19) |
                ((unsigned int)m_aac_buffer[4 + skip_size]<<11) |
                ((unsigned int)m_aac_buffer[4 + skip_size]<<3) |
                ((unsigned int)m_aac_buffer[4 + skip_size] & 0xE0);

            length = (double)m_reader->get_length();
            if (length == -1.)
                length = 1.;
            else
                length = (double)bitrate/((double)length*8.) + 0.5;

            bitrate = (__int64)((double)bitrate/1000.0 + 0.5);

            m_header_type = 2;
        }

        if ((m_aac_bytes_consumed = faacDecInit(hDecoder,
            m_aac_buffer, m_aac_bytes_into_buffer,
            &samplerate, &channels)) < 0)
        {
            console::error("Can't initialize decoder library.", "foo_mp4");
            return 0;
        }
        advance_buffer(m_aac_bytes_consumed);

        info->info_set("codec", "AAC");
        info->set_length(length);

        info->info_set_int("bitrate", bitrate);
        info->info_set_int("channels", (__int64)channels);
        info->info_set_int("samplerate", (__int64)samplerate);

        m_samplerate = samplerate;

        return 1;
    }

    input_aac()
    {
        m_head = NULL;
        m_tail = NULL;
        cur_pos_sec = 0.0;
        m_samplerate = 0;
        hDecoder = NULL;
        m_aac_buffer = NULL;
    }

    ~input_aac()
    {
        struct seek_list *target = m_head;

        if (hDecoder)
            faacDecClose(hDecoder);
        if (m_aac_buffer)
            free(m_aac_buffer);

        while (target)
        {
            struct seek_list *tmp = target;
            target = target->next;
            if (tmp) free(tmp);
        }
    }

    virtual int run(audio_chunk * chunk)
    {
        faacDecFrameInfo frameInfo;
        void *sample_buffer;

        memset(&frameInfo, 0, sizeof(faacDecFrameInfo));

        do
        {
            fill_buffer();

            if (m_aac_bytes_into_buffer != 0)
            {
                sample_buffer = faacDecDecode(hDecoder, &frameInfo,
                    m_aac_buffer, m_aac_bytes_into_buffer);

                if (m_header_type != 1)
                {
                    m_tail->offset = m_file_offset;
                    m_tail->next = (struct seek_list*)malloc(sizeof(struct seek_list));
                    m_tail = m_tail->next;
                    m_tail->next = NULL;
                }

                advance_buffer(frameInfo.bytesconsumed);
            } else {
                break;
            }

        } while (!frameInfo.samples && !frameInfo.error);

        if (frameInfo.error || (m_aac_bytes_into_buffer == 0))
        {
            if (frameInfo.error)
            {
                if (faacDecGetErrorMessage(frameInfo.error) != NULL)
                    console::error(faacDecGetErrorMessage(frameInfo.error), "foo_mp4");
            }
            return 0;
        }

        if (chunk)
        {
            chunk->data = (audio_sample*)sample_buffer;
            chunk->samples = frameInfo.samples/frameInfo.channels;
            chunk->nch = frameInfo.channels;
            chunk->srate = frameInfo.samplerate;
        }
        m_samplerate = frameInfo.samplerate;

        cur_pos_sec += 1024.0/(double)m_samplerate;

        return 1;
    }

    virtual int set_info(reader *r,const file_info * info)
    {
        return tag_writer::process_file(r, info);
    }

    virtual int seek(double seconds)
    {
        int i, frames;
        int bread;
        struct seek_list *target = m_head;

        if (m_reader->can_seek() && ((m_header_type == 1) || (seconds < cur_pos_sec)))
        {
            frames = (int)(seconds*((double)m_samplerate/1024.0) + 0.5);

            for (i = 0; i < frames; i++)
            {
                if (target->next)
                    target = target->next;
                else
                    return 0;
            }
            m_reader->seek(target->offset);

            bread = m_reader->read(m_aac_buffer, 768*6);
            if (bread != 768*6)
                m_at_eof = 1;
            else
                m_at_eof = 0;
            m_aac_bytes_into_buffer = bread;
            m_aac_bytes_consumed = 0;

            return 1;
        } else {
            if (seconds > cur_pos_sec)
            {
                frames = (int)((seconds - cur_pos_sec)*((double)m_samplerate/1024.0) + 0.5);

                if (frames > 0)
                {
                    for (i = 0; i < frames; i++)
                    {
                        if (!run(NULL))
                            break;
                    }
                }

                return 1;
            } else {
                return 0;
            }
        }
    }
    
    virtual int is_our_content_type(const char *url, const char *type)
    {
        return !strcmp(type, "audio/aac") || !strcmp(type, "audio/x-aac");
    }

private:

    reader *m_reader;

    faacDecHandle hDecoder;

    long m_aac_bytes_into_buffer;
    long m_aac_bytes_consumed;
    __int64 m_file_offset;
    unsigned char *m_aac_buffer;
    int m_at_eof;

    unsigned long m_samplerate;
    double cur_pos_sec;
    int m_header_type;

    struct seek_list *m_head;
    struct seek_list *m_tail;

    int fill_buffer()
    {
        int bread;

        if (m_aac_bytes_consumed > 0)
        {
            if (m_aac_bytes_into_buffer)
            {
                memmove((void*)m_aac_buffer, (void*)(m_aac_buffer + m_aac_bytes_consumed),
                    m_aac_bytes_into_buffer*sizeof(unsigned char));
            }

            if (!m_at_eof)
            {
                bread = m_reader->read((void*)(m_aac_buffer + m_aac_bytes_into_buffer),
                    m_aac_bytes_consumed);

                if (bread != m_aac_bytes_consumed)
                    m_at_eof = 1;

                m_aac_bytes_into_buffer += bread;
            }

            m_aac_bytes_consumed = 0;

            if (m_aac_bytes_into_buffer > 3)
            {
                if (memcmp(m_aac_buffer, "TAG", 3) == 0)
                    m_aac_bytes_into_buffer = 0;
            }
            if (m_aac_bytes_into_buffer > 11)
            {
                if (memcmp(m_aac_buffer, "LYRICSBEGIN", 11) == 0)
                    m_aac_bytes_into_buffer = 0;
            }
            if (m_aac_bytes_into_buffer > 8)
            {
                if (memcmp(m_aac_buffer, "APETAGEX", 8) == 0)
                    m_aac_bytes_into_buffer = 0;
            }
        }

        return 1;
    }

    int advance_buffer(int bytes)
    {
        m_file_offset += bytes;
        m_aac_bytes_consumed = bytes;
        m_aac_bytes_into_buffer -= bytes;
    }

    int adts_parse(__int64 *bitrate, double *length)
    {
        static int sample_rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000};
        int frames, frame_length;
        int t_framelength = 0;
        int samplerate;
        double frames_per_sec, bytes_per_frame;

        /* Read all frames to ensure correct time and bitrate */
        for (frames = 0; /* */; frames++)
        {
            fill_buffer();

            if (m_aac_bytes_into_buffer > 7)
            {
                /* check syncword */
                if (!((m_aac_buffer[0] == 0xFF)&&((m_aac_buffer[1] & 0xF6) == 0xF0)))
                    break;

                m_tail->offset = m_file_offset;
                m_tail->next = (struct seek_list*)malloc(sizeof(struct seek_list));
                m_tail = m_tail->next;
                m_tail->next = NULL;

                if (frames == 0)
                    samplerate = sample_rates[(m_aac_buffer[2]&0x3c)>>2];

                frame_length = ((((unsigned int)m_aac_buffer[3] & 0x3)) << 11)
                    | (((unsigned int)m_aac_buffer[4]) << 3) | (m_aac_buffer[5] >> 5);

                t_framelength += frame_length;

                if (frame_length > m_aac_bytes_into_buffer)
                    break;

                advance_buffer(frame_length);
            } else {
                break;
            }
        }

        frames_per_sec = (double)samplerate/1024.0;
        if (frames != 0)
            bytes_per_frame = (double)t_framelength/(double)(frames*1000);
        else
            bytes_per_frame = 0;
        *bitrate = (__int64)(8. * bytes_per_frame * frames_per_sec + 0.5);
        if (frames_per_sec != 0)
            *length = (double)frames/frames_per_sec;
        else
            *length = 1;

        return 1;
    }
};

static service_factory_t<input,input_mp4> foo_mp4;
static service_factory_t<input,input_aac> foo_aac;
