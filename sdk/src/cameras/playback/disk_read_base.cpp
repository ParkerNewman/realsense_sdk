// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2016 Intel Corporation. All Rights Reserved.

#include "disk_read_base.h"
#include <limits>
#include <vector>
#include "rs/core/metadata_interface.h"
#include "include/file.h"
#include "rs/utils/log_utils.h"
#include "rs_sdk_version.h"

using namespace rs::core;
using namespace rs::playback;

disk_read_base::disk_read_base(const char * file_path) : m_file_path(file_path), m_file_header(), m_pause(true),
    m_realtime(true), m_streams_infos(), m_base_ts(0), m_is_index_complete(false),
    m_samples_desc_index(0), m_is_motion_tracking_enabled(false)
{

}

disk_read_base::~disk_read_base(void)
{
    LOG_FUNC_SCOPE();
}

rs::playback::file_info disk_read_base::query_file_info()
{
    std::stringstream sdk_version;
    std::stringstream librealsense_version;
    sdk_version << m_sw_info.sdk.major << "." << m_sw_info.sdk.minor << "." << m_sw_info.sdk.patch;
    librealsense_version << m_sw_info.librealsense.major << "." << m_sw_info.librealsense.minor << "." <<
                            m_sw_info.librealsense.patch;

    playback::file_info file_info = {};
    file_info.capture_mode = m_file_header.capture_mode;
    file_info.version = m_file_header.version;
    memcpy(&file_info.sdk_version, sdk_version.str().c_str(), sdk_version.str().size());
    memcpy(&file_info.librealsense_version, librealsense_version.str().c_str(), librealsense_version.str().size());
    switch(m_file_header.id)
    {
        case UID('R', 'S', 'C', 'F'): file_info.type = playback::file_format::rs_rssdk_format; break;
        case UID('R', 'S', 'L', '1'):
        case UID('R', 'S', 'L', '2'): file_info.type = playback::file_format::rs_linux_format; break;
    }
    return file_info;
}

capture_mode disk_read_base::get_capture_mode()
{
    if(m_streams_infos.size() == 1)
        return capture_mode::synced;
    const uint32_t MIN_NUM_OF_FRAMES_TO_VALIDATE = 10;
    std::map<rs_stream,uint64_t> capture_times;
    //index MIN_NUM_OF_FRAMES_TO_VALIDATE samples for each stream type
    while(!m_is_index_complete)
    {
        index_next_samples(NUMBER_OF_SAMPLES_TO_INDEX);
        if(m_image_indices.size() < m_streams_infos.size())
            continue;

        bool done = true;
        for(auto indices : m_image_indices)
        {
            if(indices.second.size() < MIN_NUM_OF_FRAMES_TO_VALIDATE)
            {
                done = false;
            }
        }
        if(done)
            break;
    }

    //match capture times between the differnt streams
    for(auto sample_desc : m_samples_desc)
    {
        if(sample_desc->info.type != file_types::sample_type::st_image)
            continue;

        auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(sample_desc);
        capture_times[frame->finfo.stream] = frame->info.capture_time;
        if(capture_times.size() > 0 && capture_times.size() == m_streams_infos.size())
        {
            bool match = true;
            auto base_ct = capture_times.begin()->second;
            for(auto ct : capture_times)
            {
                if(ct.second != base_ct)
                    match = false;
            }
            if(match)
                return capture_mode::synced;
        }
    }
    return capture_mode::asynced;
}

status disk_read_base::init()
{
    if (m_file_path.empty()) return status_file_open_failed;

    m_file_data_read = std::unique_ptr<file>(new file());
    status init_status = m_file_data_read->open(m_file_path.c_str(), (open_file_option)(open_file_option::read));
    if (init_status < status_no_error)
    {
        return init_status;
    }

    init_status = read_headers();

    m_file_indexing = std::unique_ptr<file>(new file());
    init_status = m_file_indexing->open(m_file_path.c_str(), (open_file_option)(open_file_option::read));
    if (init_status < status_no_error) return init_status;

    /* Be prepared to index the frames */
    m_file_indexing->set_position(m_file_header.first_frame_offset, move_method::begin);
    LOG_INFO("init " << (init_status == status_no_error ? "succeeded" : "failed") << "(status - " << init_status << ")");

    if(m_file_header.capture_mode == 0)
        m_file_header.capture_mode = get_capture_mode();

    return init_status;
}

void disk_read_base::resume()
{
    LOG_FUNC_SCOPE();

    m_pause = false;
    //reset time base on resume
    update_time_base();

    //resume while streaming is not allowed
    if(m_thread.joinable())
        throw std::runtime_error("resume while streaming is not allowed");

    m_thread = std::thread(&disk_read_base::read_thread, this);
}

void disk_read_base::pause()
{
    LOG_FUNC_SCOPE();

    m_pause = true;

    if (m_thread.joinable())
        m_thread.join();
}

void disk_read_base::read_thread()
{
    LOG_FUNC_SCOPE();
    m_base_sys_time = std::chrono::high_resolution_clock::now();
    auto eof = false;
    while (!m_pause && !eof)
    {
        eof = !read_next_sample();
        if(eof)
        {
            //notify that reached the end of file
            if(!m_eof_callback)
                throw std::runtime_error("end of file callback is null");
            m_eof_callback();
            m_pause = true;
        }
    }
    LOG_INFO("Total number of dropped frames during playback - " << m_properties[rs_option::RS_OPTION_TOTAL_FRAME_DROPS]);
    LOG_INFO("Total number of dropped IMUs during playback - " << m_motion_drop_count);
}

void disk_read_base::init_decoder()
{
    std::map<rs_stream,file_types::compression_type> compression_config;
    uint32_t buffer_size = 0;
    for(auto it = m_active_streams_info.begin(); it != m_active_streams_info.end(); ++it)
    {
        uint32_t size = it->second.m_stream_info.profile.info.width * it->second.m_stream_info.profile.info.height;
        buffer_size = size > buffer_size ? size : buffer_size;
        compression_config.emplace(it->first, it->second.m_stream_info.ctype);
    }

    m_decoder.reset(new compression::decoder(compression_config));
    m_encoded_data = std::vector<uint8_t>(buffer_size * 4);//stride is not availabe, taking worst case.
}

void disk_read_base::set_total_frame_drop_count(double value)
{
    m_properties[rs_option::RS_OPTION_TOTAL_FRAME_DROPS] = value;
}

void disk_read_base::update_frame_drop_count(rs_stream stream, uint32_t frame_drop)
{
    m_frame_drop_count[stream] += frame_drop;
    m_properties[rs_option::RS_OPTION_TOTAL_FRAME_DROPS] += frame_drop;
}

void disk_read_base::update_imu_drop_count(uint32_t drop_count)
{
    m_motion_drop_count += drop_count;
}

void disk_read_base::reset()
{
    LOG_FUNC_SCOPE();
    pause();
    std::lock_guard<std::mutex> guard(m_mutex);
    m_file_data_read->reset();
    m_samples_desc_index = 0;
    std::queue<std::shared_ptr<core::file_types::sample>> empty_queue;
    std::swap(m_prefetched_samples, empty_queue);
    for(auto it = m_active_streams_info.begin(); it != m_active_streams_info.end(); ++it)
    {
        active_stream_info & asi = it->second;
        asi.m_image_indices = m_image_indices[it->first];
        asi.m_prefetched_samples_count = 0;
        asi.m_stream_info = m_streams_infos[it->first];
    }
    m_decoder.reset();
}

void disk_read_base::enable_stream(rs_stream stream, bool state)
{
    if(m_streams_infos.find(stream) == m_streams_infos.end())
        throw std::runtime_error("unsupported stream");
    if(state)
    {
        active_stream_info asi;
        asi.m_image_indices = m_image_indices[stream];
        asi.m_prefetched_samples_count = 0;
        asi.m_stream_info = m_streams_infos[stream];
        m_active_streams_info[stream] = asi;
    }
    else
    {
        if(m_active_streams_info.find(stream) != m_active_streams_info.end())
            m_active_streams_info.erase(m_active_streams_info.find(stream));
    }
}

void disk_read_base::enable_motions_callback(bool state)
{
    m_is_motion_tracking_enabled = state;
}

void disk_read_base::notify_available_samples()
{
    int64_t time_to_next_sample = 0;
    while(!m_pause)
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if(m_prefetched_samples.empty())break;
        time_to_next_sample = calc_sleep_time(m_prefetched_samples.front());
        if(time_to_next_sample > 0 && m_realtime)break;

        //handle next sample if its time has come
        if(m_prefetched_samples.front()->info.type == file_types::sample_type::st_image)
        {
            auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(m_prefetched_samples.front());
            if (frame)
            {
                m_active_streams_info[frame->finfo.stream].m_prefetched_samples_count--;
                LOG_VERBOSE("calling callback, frame stream type - " << frame->finfo.stream);
            }
        }
        LOG_VERBOSE("calling callback, sample type - " << m_prefetched_samples.front()->info.type);
        LOG_VERBOSE("calling callback, sample capture time - " << m_prefetched_samples.front()->info.capture_time);
        m_sample_callback(m_prefetched_samples.front());
        m_prefetched_samples.pop();
    }
}

void disk_read_base::prefetch_sample()
{
    if(m_samples_desc_index >= m_samples_desc.size() || all_samples_bufferd())
        return;
    LOG_VERBOSE("process sample - " << m_samples_desc_index);
    auto sample = m_samples_desc[m_samples_desc_index];
    m_samples_desc_index++;
    std::lock_guard<std::mutex> guard(m_mutex);
    switch(sample->info.type)
    {
        case file_types::sample_type::st_image:
        {
            auto frame = std::static_pointer_cast<file_types::frame_sample>(sample);
            if (frame)
            {
                //don't prefatch frame if stream is disabled.
                if(m_active_streams_info.find(frame->finfo.stream) == m_active_streams_info.end()) return;
                auto curr = read_image_buffer(frame);
                if(curr)
                {
                    m_active_streams_info[frame->finfo.stream].m_prefetched_samples_count++;
                    m_prefetched_samples.push(curr);
                }
            }
        }
        break;
        case file_types::sample_type::st_motion:
        case file_types::sample_type::st_time:
        {
            if(m_is_motion_tracking_enabled)
                m_prefetched_samples.push(sample);
        }
        break;
        case file_types::sample_type::st_debug_event:
        break;
        default:
            throw std::runtime_error("undefind sample type");
    }


    LOG_VERBOSE("sample prefetched, sample type - " << sample->info.type);
    LOG_VERBOSE("sample prefetched, sample capture time - " << sample->info.capture_time);
}

bool disk_read_base::read_next_sample()
{
    //indicate to device all samples which time elapsed (timestamp is in the past of the playback clock)
    notify_available_samples();
    while(m_samples_desc_index >= m_samples_desc.size() && !m_is_index_complete)
        index_next_samples(NUMBER_OF_SAMPLES_TO_INDEX);
    if(m_samples_desc_index >= m_samples_desc.size() && m_prefetched_samples.size() == 0)
        return false;
    //optimize next reads - prefetch a single sample.
    //This sample will be indicated to the device on the next iteration of the calling function if its time arrived.
    //Can't fetch more than 1 sample without checking if need to indicate any sample from the prefetched queue
    prefetch_sample();
    //goto sleep in case we have at least one frame ready for each stream, and playing in realtime
    if(all_samples_bufferd() && m_realtime)
    {
        int64_t time_to_next_sample;
        while((time_to_next_sample  = calc_sleep_time(m_prefetched_samples.front())) > 1000)
        {
            if(m_is_index_complete)
                std::this_thread::sleep_for(std::chrono::microseconds(time_to_next_sample));
            else
               index_next_samples(NUMBER_OF_SAMPLES_TO_INDEX);
        }
    }
    return true;
}

bool disk_read_base::all_samples_bufferd()
{
    //no more samples to prefetch - all available samples are buffered
    if(m_is_index_complete && m_samples_desc_index >= m_samples_desc.size() && m_prefetched_samples.size() > 0) return true;

    for(auto it = m_active_streams_info.begin(); it != m_active_streams_info.end(); ++it)
    {
        if(it->second.m_prefetched_samples_count > 0) continue;//continue if at least one frame is ready
        return false;
    }
    //no images streams enabled, only motions samples available.
    return m_prefetched_samples.size() > (m_is_motion_tracking_enabled ? NUMBER_OF_REQUIRED_PREFETCHED_SAMPLES : 0);
}

bool disk_read_base::is_stream_profile_available(rs_stream stream, int width, int height, rs_format format, int framerate)
{
    for(auto it = m_streams_infos.begin(); it != m_streams_infos.end(); ++it)
    {
        file_types::stream_info si = it->second;
        if(si.stream != stream) continue;
        if(si.profile.info.width != width) continue;
        if(si.profile.info.height != height) continue;
        if(si.profile.info.format != format) continue;
        if(si.profile.frame_rate != framerate) continue;
        return true;
    }
    return false;
}

std::map<rs_stream, std::shared_ptr<rs::core::file_types::frame_sample>> disk_read_base::set_frame_by_index(uint32_t index, rs_stream stream_type)
{
    std::map<rs_stream, std::shared_ptr<rs::core::file_types::frame_sample>> rv;
    auto previous_state = m_pause;

    pause();

    while(index >= m_image_indices[stream_type].size() && !m_is_index_complete) index_next_samples(NUMBER_OF_SAMPLES_TO_INDEX);

    if (index >= m_image_indices[stream_type].size()) return rv;


    //return current frames for all streams.
    rv = find_nearest_frames(m_image_indices[stream_type][index], stream_type);

    LOG_VERBOSE("set index to - " << index << " ,stream - " << stream_type);

    if(!previous_state)
        resume();

    return rv;
}

std::map<rs_stream, std::shared_ptr<rs::core::file_types::frame_sample>> disk_read_base::set_frame_by_time_stamp(uint64_t ts)
{
    std::map<rs_stream, std::shared_ptr<core::file_types::frame_sample>> rv;
    auto previous_state = m_pause;

    pause();
    rs_stream stream = rs_stream::RS_STREAM_COUNT;
    uint32_t index = 0;
    // Index the streams until we have at least a stream whose time stamp is bigger than ts.
    do
    {
        if(index >= m_samples_desc.size())
        {
            if(m_is_index_complete)return rv;
            index_next_samples(NUMBER_OF_SAMPLES_TO_INDEX);
        }
        {
            if(index >= m_samples_desc.size())continue;
            std::lock_guard<std::mutex> guard(m_mutex);
            if(m_samples_desc[index]->info.type != file_types::sample_type::st_image)continue;
            auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(m_samples_desc[index]);
            if(frame->finfo.time_stamp >= (double)ts)
            {
                stream = frame->finfo.stream;
                break;
            }
        }
    }
    while(++index);

    if(stream == rs_stream::RS_STREAM_COUNT) return rv;


    //return current frames for all streams.
    rv = find_nearest_frames(index, stream);

    LOG_VERBOSE("requested time stamp - " << ts << " ,set index to - " << index);

    if(!previous_state)
        resume();

    return rv;
}

std::map<rs_stream, std::shared_ptr<file_types::frame_sample> > disk_read_base::find_nearest_frames(uint32_t sample_index, rs_stream stream)
{
    std::map<rs_stream, std::shared_ptr<core::file_types::frame_sample>> rv;

    std::map<rs_stream, uint64_t> prev_index;
    std::map<rs_stream, uint64_t> next_index;
    auto index = sample_index;
    while(index > 0 && prev_index.size() < m_active_streams_info.size())
    {
        index--;
        if(m_samples_desc[index]->info.type != file_types::sample_type::st_image)continue;
        auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(m_samples_desc[index]);
        if(prev_index.find(frame->finfo.stream) != prev_index.end())continue;
        prev_index[frame->finfo.stream] = index;
    }
    index = sample_index;
    while(next_index.size() < m_active_streams_info.size())
    {
        if(index + 1 >= m_samples_desc.size())
        {
            if(m_is_index_complete)break;
            index_next_samples(NUMBER_OF_SAMPLES_TO_INDEX);
        }
        if(index + 1 >= m_samples_desc.size())continue;
        index++;
        if(m_samples_desc[index]->info.type != file_types::sample_type::st_image)continue;
        auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(m_samples_desc[index]);
        if(next_index.find(frame->finfo.stream) != next_index.end())continue;
        next_index[frame->finfo.stream] = index;
    }
    auto capture_time = m_samples_desc[sample_index]->info.capture_time;
    for(auto it = m_active_streams_info.begin(); it != m_active_streams_info.end(); ++it)
    {
        std::lock_guard<std::mutex> guard(m_mutex);

        std::shared_ptr<file_types::sample> sample;
        if(it->first == stream)
            sample = m_samples_desc[sample_index];
        else
        {
            auto prev = capture_time > m_samples_desc[prev_index[it->first]]->info.capture_time ?
                (capture_time - m_samples_desc[prev_index[it->first]]->info.capture_time ):
                ( m_samples_desc[prev_index[it->first]]->info.capture_time - capture_time );
            auto next = capture_time > m_samples_desc[next_index[it->first]]->info.capture_time ?
                capture_time - m_samples_desc[next_index[it->first]]->info.capture_time :
                m_samples_desc[next_index[it->first]]->info.capture_time - capture_time;
            sample = prev > next ? m_samples_desc[next_index[it->first]] : m_samples_desc[prev_index[it->first]];
        }
        auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(sample);
        if (frame)
        {
            auto curr = read_image_buffer(frame);
            if(curr)
                rv[frame->finfo.stream] = curr;
        }
    }
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_samples_desc_index = sample_index;
    }
    std::queue<std::shared_ptr<core::file_types::sample>> empty_queue;
    std::swap(m_prefetched_samples, empty_queue);
    prefetch_sample();
    LOG_VERBOSE("update " << rv.size() << " frames");
    return rv;
}

void disk_read_base::set_realtime(bool realtime)
{
    this->m_realtime = realtime;

    //set time base to currnt sample time
    update_time_base();
    LOG_INFO((realtime ? "enable" : "disable") << " realtime");
}

uint32_t disk_read_base::query_number_of_frames(rs_stream stream_type)
{
    uint32_t nframes = m_streams_infos[stream_type].nframes;

    if (nframes > 0) return nframes;

    /* If not able to get from the header, let's count */
    while (!m_is_index_complete) index_next_samples(std::numeric_limits<uint32_t>::max());

    return (int32_t)m_image_indices[stream_type].size();
}

uint64_t disk_read_base::query_run_time()
{
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now - m_base_sys_time).count();
}

int64_t disk_read_base::calc_sleep_time(std::shared_ptr<file_types::sample> sample)
{
    auto time_span = query_run_time();
    auto time_stamp = sample->info.capture_time;
    //number of miliseconds to wait - the diff in milisecond between the last call for streaming resume
    //and the recorded time.
    int64_t wait_for = static_cast<int>(time_stamp - m_base_ts - time_span);
    LOG_VERBOSE("sleep length " << wait_for << " miliseconds");
    LOG_VERBOSE("total run time - " << time_span);
    return wait_for;
}

void disk_read_base::update_time_base()
{
    m_base_sys_time = std::chrono::high_resolution_clock::now();

    std::lock_guard<std::mutex> guard(m_mutex);
    if(m_samples_desc_index > 0)
    {
        if(m_prefetched_samples.size() > 0)
            m_base_ts = m_prefetched_samples.front()->info.capture_time;
        else
            m_base_ts = m_samples_desc_index < m_samples_desc.size() ?
                        m_samples_desc[m_samples_desc_index]->info.capture_time : 0;
    }
    else
        m_base_ts = 0;

    LOG_VERBOSE("new time base - " << m_base_ts);
}

file_types::version disk_read_base::query_sdk_version()
{
    return m_sw_info.sdk;
}

file_types::version disk_read_base::query_librealsense_version()
{
    return m_sw_info.librealsense;
}

std::shared_ptr<file_types::frame_sample> disk_read_base::read_image_buffer(std::shared_ptr<file_types::frame_sample> &frame)
{
    status sts = m_file_data_read->set_position(frame->info.offset, move_method::begin);

    if(!m_decoder)
        init_decoder();

    if(sts != status::status_no_error)
        return nullptr;

    uint32_t num_bytes_read = 0;
    unsigned long num_bytes_to_read = 0;

    file_types::chunk_info chunk = {};
    for (;;)
    {
        m_file_data_read->read_bytes(&chunk, sizeof(chunk), num_bytes_read);
        num_bytes_to_read = chunk.size;
        switch (chunk.id)
        {
            case file_types::chunk_id::chunk_image_metadata:
            {
                if(num_bytes_to_read > 0)
                {
                    read_frame_metadata(frame, num_bytes_to_read);
                }
                else
                {
                    LOG_ERROR("failed to read frame metadata, metadata size is not valid");
                }
                break;
            }
            case file_types::chunk_id::chunk_sample_data:
            {
                m_file_data_read->set_position(size_of_pitches(),move_method::current);
                num_bytes_to_read -= size_of_pitches();
                switch (frame->finfo.ctype)
                {
                    case file_types::compression_type::none:
                    {
                        auto rv = std::shared_ptr<file_types::frame_sample>(
                        new file_types::frame_sample(frame.get()), [](file_types::frame_sample* f) { delete[] f->data; delete f;});
                        auto data = new uint8_t[num_bytes_to_read];
                        m_file_data_read->read_bytes(data, static_cast<uint32_t>(num_bytes_to_read), num_bytes_read);
                        num_bytes_to_read -= num_bytes_read;
                        rv->data = data;
                        return rv;
                    }
                    case file_types::compression_type::lz4:
                    case file_types::compression_type::h264:
                    {
                        uint8_t * data = m_encoded_data.data();
                        m_file_data_read->read_bytes(data, static_cast<uint32_t>(num_bytes_to_read), num_bytes_read);
                        num_bytes_to_read -= num_bytes_read;
                        auto rv = m_decoder->decode_frame(frame, data, num_bytes_read);
                        return rv;
                    }
                    default:
                    {
                        throw std::runtime_error("unsupported compression type");
                    }
                }
                if (num_bytes_to_read > 0)
                {
                    LOG_ERROR("image size failed to match the data size");
                }
                return nullptr;
            }
            default:
            {
                if(num_bytes_to_read == 0)
                    return nullptr;
                m_file_data_read->set_position(num_bytes_to_read, move_method::current);
            }
            num_bytes_to_read = 0;
        }
    }
}
