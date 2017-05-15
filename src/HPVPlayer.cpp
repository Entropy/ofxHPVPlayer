#include <string.h>

#include "HPVPlayer.h"
#include "lz4.h"
#include "lz4hc.h"
#include "HPVManager.h"


namespace HPV {
	constexpr size_t PREBUFFER_SIZE = 4;

    static std::string HPVCompressionTypeStrings[] =
    {
        "DXT1 (no ALPHA)",
        "DXT5 (with ALPHA)",
        "SCALED DXT5 (CoCg_Y)"
    };

    // helper function to read HPV header from file
    static int readHeader(std::ifstream * const ifs, HPVHeader * const header)
    {
        int header_size = sizeof(uint32_t) * amount_header_fields;
        
        ifs->read((char *)header, header_size);
        
        if (ifs->fail())
            return -1;
        
        return 0;
    }

    HPVPlayer::HPVPlayer()
    : _id(0)
    , _gather_stats(true)
    , _num_bytes_in_header(0)
    , _num_bytes_in_sizes_table(0)
    , _filesize(0)
    , _frame_sizes_table(nullptr)
	, _frame_offsets_table(nullptr)
    , _bytes_per_frame(0)
    , _new_frame_time(0)
    , _global_time_per_frame(0)
    , _local_time_per_frame(0)
    , _curr_frame(0)
	, _send_frame(0)
	, _curr_buffered_frame(0)
    , _loop_in(0)
    , _loop_out(0)
    , _loop_mode(HPV_LOOPMODE_LOOP)
    , _state(HPV_STATE_NONE)
    , _direction(HPV_DIRECTION_FORWARDS)
	, _is_init(false)
    , _m_event_sink(nullptr)
    {
        _update_result.store(0, std::memory_order_relaxed);
        _was_seeked.store(false, std::memory_order_relaxed);
        _header.magic = 0;
        _header.version = 0;
        _header.video_width = 0;
        _header.video_height = 0;
        _header.number_of_frames = 0;
        _header.frame_rate = 0;
        _header.crc_frame_sizes = 0;
        _decode_stats.gpu_upload_time = 0;
        _decode_stats.hdd_read_time = 0;
        _decode_stats.l4z_decode_time = 0;
    }
    
    HPVPlayer::~HPVPlayer()
    {
        if (_is_init)
        {
            close();
        }
        HPV_VERBOSE("~HPVPLayer");
    }
    
    int HPVPlayer::open(const std::string& filepath)
    {
        _is_init = false;
        
        if (true == _ifs.is_open())
        {
            HPV_ERROR("Already loaded %s, call shutdown if you want to reload.", filepath.c_str());
            return HPV_RET_ERROR;
        }
        
        if (0 == filepath.size())
        {
            HPV_ERROR("Invalid filepath; size is 0.");
            return HPV_RET_ERROR;
        }
        
        // open the input filestream
        _ifs.open(filepath.c_str(), std::ios::binary | std::ios::in);
        if (!_ifs.is_open())
        {
            HPV_ERROR("Failed to open: %s", filepath.c_str());
            return HPV_RET_ERROR;
        }
        
        // get filesize of HPV file
        _ifs.seekg(0, std::ifstream::end);
        _filesize = _ifs.tellg();
        _ifs.seekg(0, std::ios_base::beg);
        
        if (0 == _filesize)
        {
            _ifs.close();
            HPV_ERROR("File size is 0 (%s).", filepath.c_str());
            return HPV_RET_ERROR;
        }
        
        // read the header
        if (0 != HPV::readHeader(&_ifs, &_header))
        {
            HPV_ERROR("Failed to read HPV header from %s", filepath.c_str());
            return HPV_RET_ERROR;
        }
        
        if (_header.magic != HPV_MAGIC)
        {
            HPV_ERROR("Wrong magic number")
            return HPV_RET_ERROR;
        }
        
        // check if dimensions are in correct range
        if (0 == _header.video_width || _header.video_width > HPV_MAX_SIDE_SIZE)
        {
            HPV_ERROR("Video width is invalid. Either 0 or bigger than what we don't support yet. Video width: %u", _header.video_width);
            _ifs.close();
            return HPV_RET_ERROR;
        }
        
        if (0 == _header.video_height || _header.video_height > HPV_MAX_SIDE_SIZE)
        {
            HPV_ERROR("Video height is invalid. either 0 or bigger than what we don't support yet. Video height: %u", _header.video_height);
            _ifs.close();
            return HPV_RET_ERROR;
        }
        
        // good file, save its path
        _file_path = filepath;
        
        // store file name
        _file_name = _file_path.substr(_file_path.find_last_of("\\/")+1);
        
        // ready reading the header...save our position
        _num_bytes_in_header = static_cast<uint32_t>(_ifs.tellg());
        _num_bytes_in_sizes_table = _header.number_of_frames * sizeof(uint32_t);
        
        // read in frame size table and check crc
        _frame_sizes_table = new uint32_t[_header.number_of_frames];
        _frame_offsets_table = new uint64_t[_header.number_of_frames];
        
        _ifs.read((char *)_frame_sizes_table, _num_bytes_in_sizes_table);
        
        uint32_t crc = 0;
		_frame_max_size = _frame_sizes_table[0];
        for (uint32_t i=0 ; i<_header.number_of_frames; ++i)
        {
            crc += _frame_sizes_table[i];
			if(_frame_sizes_table[i]>_frame_max_size){
				_frame_max_size = _frame_sizes_table[i];
			}
        }
		std::cout << "average frame size " << crc / double(_header.number_of_frames) << std::endl;
		std::cout << "at 60fps " << crc / double(_header.number_of_frames) * 60 / 1024. / 1024. << "MBps" << std::endl;
		std::cout << "max frame size " << _frame_max_size << std::endl;
		std::cout << "at 60fps " << _frame_max_size * 60 / 1024. / 1024. << "MBps" << std::endl;

		// create local buffer for storing L4Z compressed frame
		_l4z_buffer = new (std::nothrow) char[ _frame_max_size ];
        
        if (crc != _header.crc_frame_sizes)
        {
            HPV_ERROR("Frame sizes table CRC doesn't match, corrupt file")
            return HPV_RET_ERROR;
        }
        
        uint32_t start_offset = _num_bytes_in_header + _num_bytes_in_sizes_table;
        this->populateFrameOffsets(start_offset);
        
        // calculate frame size in bytes from compression type
        _bytes_per_frame = _header.video_width * _header.video_height;
        
        // this will need to be a bit worked out, for all different formats
        if (_header.compression_type == HPVCompressionType::HPV_TYPE_DXT1_NO_ALPHA)
        {
            _bytes_per_frame >>= 1;
        }
        
        // get the native frame rate of the file (was given as parameter during compression) and set initial speed to speed 1
        uint32_t fps = _header.frame_rate;
        _global_time_per_frame = static_cast<uint64_t>(double(1.0 / fps) * 1e9);
        
        // set to initial state
        this->resetPlayer();
        
        // allocate the frame buffer, happens only once. We re-use the same memory space
		for(size_t i=0;i<PREBUFFER_SIZE;i++){
			try{
				_frames.emplace_back(_bytes_per_frame,i);
			}catch(...){
				HPV_ERROR("Failed to allocate the frame buffer.");
				_ifs.close();
				return HPV_RET_ERROR;
			}
		}
        
		// read the first frame
		for(Frame & frame: _frames){
			_send_frame = frame._number;
			_m_frame_channel.send(&frame);
		}
		_send_frame += 1;
        
        this->launchUpdateThread();
        
        _is_init = true;
        
        HPV_VERBOSE("%s", this->getFileSummary().c_str());

        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::close()
    {
        if (_is_init)
        {
            // first let thread finish
            _update_result.store(0, std::memory_order_release);
			_m_read_frame_channel.close();
			_m_frame_channel.close();
            
            if (_update_thread.joinable())
            {
                _update_thread.join();
            }
            _update_result = 0;
            
            HPV_VERBOSE("Closed HPV worker thread for '%s'", _file_name.c_str());
            
            if (_ifs.is_open())
				_read_frames.push_back(&_frames[0]);
            {
                _ifs.close();
			}

			if (_l4z_buffer)
			{
				delete [] _l4z_buffer;
				_l4z_buffer = nullptr;
			}
            
            if (_frame_sizes_table)
            {
                delete [] _frame_sizes_table;
                _frame_sizes_table = nullptr;
            }
            
            if (_frame_offsets_table)
            {
                delete [] _frame_offsets_table;
                _frame_offsets_table = nullptr;
            }
            
            // clear out header
            memset(&_header, 0x00, HPV::amount_header_fields);
            
            _num_bytes_in_header = 0;
            _filesize = 0;
            _bytes_per_frame = 0;
            _new_frame_time = 0;
            _global_time_per_frame = 0;
            _local_time_per_frame = 0;
            
            _curr_frame = 0;
            _state = HPV_STATE_NONE;
            
            _is_init = false;
        }
        
        return HPV_RET_ERROR_NONE;
    }
    
    void HPVPlayer::populateFrameOffsets(uint32_t start_offset)
    {
        uint64_t offset_runner = (uint64_t)start_offset;
        _frame_offsets_table[0] = offset_runner;
        
        for (uint32_t frame_idx = 1; frame_idx < _header.number_of_frames; ++frame_idx)
        {
            offset_runner += _frame_sizes_table[frame_idx-1];
            _frame_offsets_table[frame_idx] = offset_runner;
        }
    }
    
	inline int HPVPlayer::readFrame(Frame & frame)
    {
        static uint64_t _before_read, _before_decode;
        static uint64_t _after_read, _after_decode;
        
        if (_gather_stats)
        {
            _before_read = ns();
			_before_decode = _before_read;
        }
        
		_ifs.seekg(_frame_offsets_table[frame._number]);
        
        if (!_ifs.good())
        {
			HPV_ERROR("Failed to seek to %lu", _frame_offsets_table[frame._number]);
            return HPV_RET_ERROR;
		}

		HPVManager::ReadChunk read_chunk{ &_ifs, _l4z_buffer, _frame_sizes_table[frame._number]};
		auto f = read_chunk.done.get_future();
		ManagerSingleton()->newChunk(&read_chunk);
		f.wait();
		if(!f.get()){
			HPV_ERROR("Couldn't read compressed data from disk!");
			return HPV_RET_ERROR;
		}

        // read L4Z data from disk into buffer
//		_ifs.read(_l4z_buffer, _frame_sizes_table[frame._number]);
        
//        if (!_ifs.good())
//        {
//            HPV_ERROR("Couldn't read compressed data from disk!");
//            return HPV_RET_ERROR;
//        }
        
        if (_gather_stats)
        {
            _after_read = ns();
            
            _decode_stats.hdd_read_time = _after_read - _before_read;
        }
        
        if (!_ifs.good())
        {
			HPV_ERROR("Failed to read frame %" PRId64, frame._number);
            return HPV_RET_ERROR;
        }
        
        if (_gather_stats)
        {
            _before_decode = ns();
        }
        
		// decompress L4Z
		int ret_decomp = LZ4_decompress_fast((const char *)_l4z_buffer, (char *)frame._buffer, static_cast<int>(_bytes_per_frame));
        
        if (ret_decomp == 0)
        {
			HPV_ERROR("Failed to decompress frame %" PRId64, frame._number);
            return HPV_RET_ERROR;
        }
        
        if (_gather_stats)
        {
            _after_decode = ns();
            
            _decode_stats.l4z_decode_time = _after_decode - _before_decode;
		}
        
        _update_result.store(1, std::memory_order_relaxed);
        
		//_curr_buffered_frame = _curr_frame;
        
        //HPV_VERBOSE("ID %d read frame %" PRId64, getID(), _curr_buffered_frame);
        
        return HPV_RET_ERROR_NONE;
    }
    
    void HPVPlayer::launchUpdateThread()
    {
		// start thread now that everything is set for this player
		_update_thread = std::thread(&HPVPlayer::thread_function, this);
    }
    
    int HPVPlayer::play()
    {
        if (!_ifs.is_open())
        {
            HPV_VERBOSE("Trying to play, but the file stream is not opened. Did you call init()?");
            return HPV_RET_ERROR;
        }
        
        if (isPlaying())
        {
            HPV_VERBOSE("Already playing");
            return HPV_RET_ERROR;
        }
        
        _new_frame_time = ns() + _local_time_per_frame;
        
        /* Rewind to first frame when we were stopped */
        if (isStopped())
        {
            _curr_frame = _loop_in;
        }
        
        _state = HPV_STATE_PLAYING;
        
        notifyHPVEvent(HPVEventType::HPV_EVENT_PLAY);
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::play(int fps)
    {
        _global_time_per_frame = static_cast<uint64_t>(double(1.0 / fps) * 1e9);
        _local_time_per_frame = _global_time_per_frame;
        
        return this->play();
    }
    
    int HPVPlayer::pause()
    {
        if (!isPlaying())
        {
            HPV_VERBOSE("Calling pause() on a video that's not playing");
            return HPV_RET_ERROR;
        }
        
        if (isPaused())
        {
            HPV_VERBOSE("Calling pause() on a video that's already paused");
            return HPV_RET_ERROR;
        }
        
        _state = HPV_STATE_PAUSED;
        
        notifyHPVEvent(HPVEventType::HPV_EVENT_PAUSE);
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::resume()
    {
        if (!isPaused())
        {
            HPV_VERBOSE("Calling resume() on a video that's not paused");
            return HPV_RET_ERROR;
        }
        
        _new_frame_time = ns() + _local_time_per_frame;
        
        _state = HPV_STATE_PLAYING;
        
        notifyHPVEvent(HPVEventType::HPV_EVENT_RESUME);
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::stop()
    {
        if (!isPlaying() && !isPaused())
        {
            HPV_ERROR("Cannot stop because we're not playing or paused");
            return HPV_RET_ERROR;
        }
        
        _state = HPV_STATE_STOPPED;
        
        _curr_frame = _loop_in;
        
        this->seek(_curr_frame);
                
        notifyHPVEvent(HPVEventType::HPV_EVENT_STOP);
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::setLoopMode(uint8_t loop_mode)
    {
        _loop_mode = loop_mode;
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::setLoopInPoint(int64_t loop_in)
    {
        if (loop_in >= 0 && loop_in < _header.number_of_frames)
        {
            _loop_in = loop_in;
            
            if (_curr_frame < _loop_in)
            {
                this->seek((int64_t)_loop_in);
            }
        }
        else
        {
            _loop_in = 0;
        }
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::setLoopOutPoint(int64_t loop_out)
    {
        if (loop_out > _loop_in && loop_out < _header.number_of_frames)
        {
            _loop_out = loop_out;
            
            if (_curr_frame > _loop_out)
            {
                this->seek((int64_t)_loop_in);
            }
        }
        else
        {
            _loop_out = _header.number_of_frames-1;
        }
        
        return HPV_RET_ERROR_NONE;
    }
    
    /*
     *  Threaded function. Updates the times until a new frame should be read.
     *
     *  The result is stored in std::atomic<bool> updateResult. This way, the main thread
     *  can query when a new frame is ready.
     *  updateResult > 0    -> new frame is ready
     *  updateResult = 0    -> not yet there, still iterating
     *
     */
	void HPVPlayer::thread_function()
    {
		uint64_t now;
		Frame * frame;
		while (_m_frame_channel.receive(frame))
		{
			_was_seeked.store(false, std::memory_order_relaxed);

			/* Read the frame from the file */
			if (!readFrame(*frame))
			{
				_seek_result.store(-1, std::memory_order_relaxed);
			}
			else
			{
				_seek_result.store(1, std::memory_order_relaxed);
				_m_read_frame_channel.send(frame);
			}

			now = ns();

//            else
//            {
//                /* When not playing, paused or stopped: sleep a bit an re-check condition */
//                if (!isPlaying() || isPaused() || isStopped())
//                {
//                    std::this_thread::sleep_for(std::chrono::nanoseconds(100));

//                    continue;
//                }
                
//                /* When playing: get delta time and check if we need to load next frame. */
//                now = ns();
                
//                if (!(now >= _new_frame_time))
//                {
//					//HPV_VERBOSE("%" PRIu64 " - %" PRIu64, now, _new_frame_time);
//                    continue;
//                }
//                // next actions depening on playback direction: forwards / backwards
//                else if (HPV_DIRECTION_FORWARDS == _direction)
//                {
//                    ++_curr_frame;
                    
//                    if (_curr_frame > _loop_out)
//                    {
//                        notifyHPVEvent(HPVEventType::HPV_EVENT_LOOP);
//                        if (HPV_LOOPMODE_NONE == _loop_mode)
//                        {
//                            stop();
//                            continue;
//                        }
//                        else if (HPV_LOOPMODE_LOOP == _loop_mode)
//                        {
//                            _curr_frame = _loop_in;
                            
//                        }
//                        else if (HPV_LOOPMODE_PALINDROME == _loop_mode)
//                        {
//                            _curr_frame = _loop_out;
//                            _direction = HPV_DIRECTION_REVERSE;
//                        }
//                        else
//                        {
//                            HPV_ERROR("Unhandled play mode.");
//                            continue;
//                        }
//                    }
//                }
//                else
//                {
//                    --_curr_frame;
                    
//                    if (_curr_frame < _loop_in)
//                    {
//                        notifyHPVEvent(HPVEventType::HPV_EVENT_LOOP);
//                        if (HPV_LOOPMODE_NONE == _loop_mode)
//                        {
//                            stop();
//                            continue;
//                        }
//                        else if (HPV_LOOPMODE_LOOP == _loop_mode)
//                        {
//                            _curr_frame = _loop_out;
//                        }
//                        else if (HPV_LOOPMODE_PALINDROME == _loop_mode)
//                        {
//                            _curr_frame = _loop_in;
//                            _direction = HPV_DIRECTION_FORWARDS;
//                        }
//                        else
//                        {
//                            HPV_ERROR("Unhandled play mode.");
//                            continue;
//                        }
//                    }
//                }
                
//                /* Set future time when new frame is needed */
//                //_new_frame_time = now + _local_time_per_frame;
//                _new_frame_time += _local_time_per_frame;
                
//                /* Read the frame from the file */
//                if (!readCurrentFrame())
//                {
//                    continue;
//                }
                                
//                // sleep thread and wake up in time for next frame, taking in account read time
//                std::this_thread::sleep_for(std::chrono::nanoseconds(_local_time_per_frame-(_decode_stats.hdd_read_time+_decode_stats.l4z_decode_time)-1000000));
//            }
		}
    }
    
    int HPVPlayer::setSpeed(double speed)
    {
        // don't take in account speeds too close to 0!
        if (speed < HPV_SPEED_EPSILON && speed > -HPV_SPEED_EPSILON)
        {
            // avoid stopping the video!
            return HPV_RET_ERROR;
        }
        
        // fwd
        if (speed > HPV_SPEED_EPSILON)
        {
            _direction = HPV_DIRECTION_FORWARDS;
        }
        // bwd
        else if (speed < -HPV_SPEED_EPSILON)
        {
            _direction = HPV_DIRECTION_REVERSE;
        }
        
        // the frame duration of course changes when the speed changes
        _local_time_per_frame = static_cast<uint64_t>(_global_time_per_frame / std::abs(speed));
        
        // resume if we were paused earlier (speed +/- 0)
        if (isPaused())
        {
            resume();
        }
        
        return HPV_RET_ERROR_NONE;
	}
    
    int HPVPlayer::setPlayDirection(uint8_t direction)
    {
        if (direction)
        {
            _direction = HPV_DIRECTION_FORWARDS;
        }
        else
        {
            _direction = HPV_DIRECTION_REVERSE;
        }
        
        return HPV_RET_ERROR_NONE;
    }
    
    int HPVPlayer::seek(double pos)
    {
        if (pos < 0.0 || pos > 1.0)
            return HPV_RET_ERROR;

		int64_t seek_to = 0;
        if (HPV::isNearlyEqual(pos, 0.0))
        {
			seek_to = 0;
        }
        else if (HPV::isNearlyEqual(pos, 1.0))
        {
			seek_to = _header.number_of_frames-1;
        }
        else
        {
			seek_to = clamp<int64_t>(static_cast<int64_t>(std::floor( (_header.number_of_frames-1) * pos)), _loop_in, _loop_out);
        }


    
		return this->seek(seek_to);
    }
    
	int HPVPlayer::seek(int64_t seek_to)
    {
//		if (seek_to < 0 || seek_to >= _header.number_of_frames)
//            return HPV_RET_ERROR;
        
		seek_to = clamp<int64_t>(seek_to, _loop_in, _loop_out);

		Frame * frame;
		while(_m_read_frame_channel.tryReceive(frame)){
			//std::cout << int(_id) << " receiving frame " << frame->_number << endl;
			_read_frames.push_back(frame);
		}

//		if(seek_to > _curr_frame || (_curr_frame+1 > _loop_out && seek_to < _curr_frame)){
//			_read_frames.erase(std::remove_if(_read_frames.begin(), _read_frames.end(),
//				[this, seek_to](Frame * frame){
//					bool remove = false;
//					if(seek_to > _curr_frame){
//						remove = frame->_number < seek_to && frame->_number > seek_to - PREBUFFER_SIZE;
//					}else{
//						remove = frame->_number >= _curr_frame || frame->_number < seek_to;
//					}
//					if(remove){
//						std::cout << int(_id) << " going to send frame " << frame->_number << " with seek_to " << seek_to << " and currframe " << _curr_frame << endl;
//						frame->_number += PREBUFFER_SIZE;
//						if(frame->_number > _loop_out){
//							if(_loop_mode != HPV_LOOPMODE_NONE){
//								frame->_number %= (_loop_out+1 - _loop_in);
//								frame->_number += _loop_in;
//							}
//						}
//						if(frame->_number>=_loop_in && frame->_number<=_loop_out){
//							std::cout << int(_id) << " sending frame " << frame->_number << endl;
//							_m_frame_channel.send(frame);
//						}else{
//							remove = false;
//						}
//					}
//					return remove;
//				}), _read_frames.end());
//		}

		if(ofWrap(ofWrap(_send_frame, _loop_in, _loop_out) - ofWrap(seek_to, _loop_in, _loop_out), _loop_in, _loop_out) > PREBUFFER_SIZE){
			_send_frame = seek_to;
		}

		if(seek_to!=_curr_frame){
			auto it = std::find_if(_read_frames.begin(), _read_frames.end(), [&](Frame * frame){
				return frame->_number == _curr_frame;
			});
			if(it!=_read_frames.end()){
				frame = *it;
				_read_frames.erase(it);
				//std::cout << int(_id) << " going to send frame " << frame->_number << " with seek_to " << seek_to << " and currframe " << _curr_frame << endl;
				frame->_number = _send_frame;
				if(frame->_number>=_loop_in && frame->_number<=_loop_out){
					//std::cout << int(_id) << " sending frame " << frame->_number << endl;
					_m_frame_channel.send(frame);
					_send_frame += 1;
					if(_send_frame > _loop_out){
						if(_loop_mode != HPV_LOOPMODE_NONE){
							_send_frame %= (_loop_out+1 - _loop_in);
							_send_frame += _loop_in;
						}
					}
				}
			}
		}

		auto it = std::find_if(_read_frames.begin(), _read_frames.end(), [&](Frame * frame){
			return frame->_number == seek_to;
		});
		bool received = it!=_read_frames.end();

		if(received){
			_curr_frame = seek_to;
		}else{
			if(seek_to>_curr_frame+PREBUFFER_SIZE || (_curr_frame+PREBUFFER_SIZE < _loop_out && seek_to < _curr_frame)){
				if(_read_frames.empty()){
					_m_read_frame_channel.receive(frame);
					//std::cout << int(_id) << " receiving frame after full clear " << frame->_number << endl;
					_read_frames.push_back(frame);
				}
				int64_t frame_num = seek_to;
				for(Frame * frame: _read_frames){
					frame->_number = frame_num;
					frame_num += 1;
					_m_frame_channel.send(frame);
				}
				_read_frames.clear();
			}
			while(!received){
				_m_read_frame_channel.receive(frame);
				//std::cout << int(_id) << " receiving frame after lock " << frame->_number << endl;
				_read_frames.push_back(frame);
				if(frame->_number==seek_to){
					received = true;
				}
			}
			_curr_frame = seek_to;
		}
		return true;
	}
    

	void HPVPlayer::update(){
//		Frame * frame = nullptr;
//		while(_m_read_frame_channel.tryReceive(frame)){
//			_read_frames.push_back(frame);
//		}
//		if(_read_frames.empty()){
//			_m_read_frame_channel.receive(frame);
//			_read_frames.push_back(frame);
//		}else{
//			auto it = std::find_if(_read_frames.begin(), _read_frames.end(), [&](Frame * frame){
//				return frame->_number == _curr_frame;
//			});
//			if(it!=_read_frames.end()){
//				frame = *it;
//				_read_frames.erase(it);
//				frame->_number += PREBUFFER_SIZE;
//				if(frame->_number > _loop_out){
//					if(_loop_mode != HPV_LOOPMODE_NONE){
//						frame->_number %= (_loop_out+1 - _loop_in);
//						frame->_number += _loop_in;
//					}
//				}
//				if(frame->_number>=_loop_in && frame->_number<=_loop_out){
//					_m_frame_channel.send(frame);
//				}
//			}
//		}
	}

    void HPVPlayer::resetPlayer()
    {
        _local_time_per_frame = _global_time_per_frame;
        _loop_in = 0;
        _loop_out = _header.number_of_frames - 1;
        _direction = HPV_DIRECTION_FORWARDS;
    }
    
    int HPVPlayer::isLoaded()
    {
        return _is_init;
    }
    
    int HPVPlayer::isPlaying()
    {
        return (_state == HPV_STATE_PLAYING);
    }
    
    int HPVPlayer::isPaused()
    {
        return (_state == HPV_STATE_PAUSED);
    }
    
    int HPVPlayer::isStopped()
    {
        return (_state == HPV_STATE_STOPPED);
    }
    
    void HPVPlayer::notifyHPVEvent(HPVEventType type)
    {
        if (_m_event_sink)
        {
            HPVEvent event(type, this);
            _m_event_sink->push(event);
        }
    }
    
    int HPVPlayer::getWidth()
    {
        return _header.video_width;
    }
    
    int HPVPlayer::getHeight()
    {
        return _header.video_height;
    }
    
    std::size_t HPVPlayer::getBytesPerFrame()
    {
        return _bytes_per_frame;
    }
    
    unsigned char* HPVPlayer::getBufferPtr()
    {
		auto it = std::find_if(_read_frames.begin(), _read_frames.end(), [&](Frame * frame){
			return frame->_number == _curr_frame;
		});
		if(it!=_read_frames.end()){
			return (*it)->_buffer;
		}else{
			HPV_ERROR("Empty buffer returning null");
			return nullptr;
		}
    }
    
    int HPVPlayer::getFrameRate()
    {
        return _header.frame_rate;
    }
    
    HPVCompressionType HPVPlayer::getCompressionType()
    {
        return _header.compression_type;
    }
    
    int HPVPlayer::getLoopMode()
    {
        return _loop_mode;
    }
    
    int64_t HPVPlayer::getLoopIn()
    {
        return _loop_in;
    }
    
    int64_t HPVPlayer::getLoopOut()
    {
        return _loop_out;
    }
    
    std::string HPVPlayer::getFilePath()
    {
        return _file_path;
    }
    
    int64_t HPVPlayer::getCurrentFrameNumber()
    {
        return _curr_buffered_frame;
    }
    
    uint64_t HPVPlayer::getNumberOfFrames()
    {
        return _header.number_of_frames;
    }
    
    std::string HPVPlayer::getFilename()
    {
        if (isLoaded())
        {
            return _file_name;
        }
        else
        {
            return "";
        }
    }
    
    uint8_t HPVPlayer::getID()
    {
        return _id;
    }
    
    float HPVPlayer::getPosition()
    {
        if (_header.number_of_frames <= 0)
        {
            return 0;
        }
        
        return _curr_frame / static_cast<float>(_header.number_of_frames-1);
    }
    
    float HPVPlayer::getSpeed()
    {
        return static_cast<float>(_global_time_per_frame / static_cast<float>(_local_time_per_frame));
    }
    
    bool HPVPlayer::hasNewFrame()
    {
        // only reset new frame flag when renderer has time to check for this new frame
        if (_update_result)
        {
            _update_result = 0;
            
            return true;
        }
        else
            return false;
    }
    
    int HPVPlayer::enableStats(bool _enable)
    {
        _gather_stats = _enable;
        
        return HPV_RET_ERROR_NONE;
    }
    
    std::string HPVPlayer::getFileSummary()
    {
        if (_is_init)
        {
            std::stringstream ss;

            ss  << "[ "
                << _file_name
                << " | dims: "
                << _header.video_width << "x" << _header.video_height
                << " | fps: "
                << _header.frame_rate
                << " | frames: "
                << _header.number_of_frames
                << " | type "
                << HPVCompressionTypeStrings[(uint8_t)_header.compression_type]
                << " | version: "
                << _header.version
                << " ] ";
            
            return ss.str();
        }
        else
        {
            return "No open file";
        }
    }
    
    void HPVPlayer::addHPVEventSink(ThreadSafe_Queue<HPVEvent> * sink)
    {
        _m_event_sink = sink;
    }
} /* End HPV namespace */
