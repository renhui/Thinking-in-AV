// FFmpegTest.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "pch.h"
#include <iostream>
#include "AACFormat.h"

extern "C" {
#include <stdio.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/timestamp.h"
#include "libavutil/log.h"
}

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

// For extractVideo
#ifndef AV_WB32
#   define AV_WB32(p, val) do {                 \
        uint32_t d = (val);                     \
        ((uint8_t*)(p))[3] = (d);               \
        ((uint8_t*)(p))[2] = (d)>>8;            \
        ((uint8_t*)(p))[1] = (d)>>16;           \
        ((uint8_t*)(p))[0] = (d)>>24;           \
    } while(0)
#endif

// For extractVideo
#ifndef AV_RB16
#   define AV_RB16(x)                           \
    ((((const uint8_t*)(x))[0] << 8) |          \
      ((const uint8_t*)(x))[1])
#endif




// For extractVideo
static int alloc_and_copy(AVPacket *out, const uint8_t *sps_pps, uint32_t sps_pps_size, const uint8_t *in, uint32_t in_size) {
	uint32_t offset = out->size;
	uint8_t nal_header_size = offset ? 3 : 4;
	int err;

	err = av_grow_packet(out, sps_pps_size + in_size + nal_header_size);
	if (err < 0)
		return err;

	if (sps_pps)
		memcpy(out->data + offset, sps_pps, sps_pps_size);
	memcpy(out->data + sps_pps_size + nal_header_size + offset, in, in_size);
	if (!offset) {
		AV_WB32(out->data + sps_pps_size, 1);
	}
	else {
		(out->data + offset + sps_pps_size)[0] =
			(out->data + offset + sps_pps_size)[1] = 0;
		(out->data + offset + sps_pps_size)[2] = 1;
	}

	return 0;
}

// For extractVideo
int h264_extradata_to_annexb(const uint8_t *codec_extradata, const int codec_extradata_size, AVPacket *out_extradata, int padding)
{
	uint16_t unit_size;
	uint64_t total_size = 0;
	uint8_t *out = NULL, unit_nb, sps_done = 0,
		sps_seen = 0, pps_seen = 0, sps_offset = 0, pps_offset = 0;
	const uint8_t *extradata = codec_extradata + 4;
	static const uint8_t nalu_header[4] = { 0, 0, 0, 1 };
	int length_size = (*extradata++ & 0x3) + 1; // retrieve length coded size, 用于指示表示编码数据长度所需字节数

	sps_offset = pps_offset = -1;

	/* retrieve sps and pps unit(s) */
	unit_nb = *extradata++ & 0x1f; /* number of sps unit(s) */
	if (!unit_nb) {
		goto pps;
	}
	else {
		sps_offset = 0;
		sps_seen = 1;
	}

	while (unit_nb--) {
		int err;

		unit_size = AV_RB16(extradata);
		total_size += unit_size + 4;
		if (total_size > INT_MAX - padding) {
			av_log(NULL, AV_LOG_ERROR,
				"Too big extradata size, corrupted stream or invalid MP4/AVCC bitstream\n");
			av_free(out);
			return AVERROR(EINVAL);
		}
		if (extradata + 2 + unit_size > codec_extradata + codec_extradata_size) {
			av_log(NULL, AV_LOG_ERROR, "Packet header is not contained in global extradata, "
				"corrupted stream or invalid MP4/AVCC bitstream\n");
			av_free(out);
			return AVERROR(EINVAL);
		}
		if ((err = av_reallocp(&out, total_size + padding)) < 0)
			return err;
		memcpy(out + total_size - unit_size - 4, nalu_header, 4);
		memcpy(out + total_size - unit_size, extradata + 2, unit_size);
		extradata += 2 + unit_size;
	pps:
		if (!unit_nb && !sps_done++) {
			unit_nb = *extradata++; /* number of pps unit(s) */
			if (unit_nb) {
				pps_offset = total_size;
				pps_seen = 1;
			}
		}
	}

	if (out)
		memset(out + total_size, 0, padding);

	if (!sps_seen)
		av_log(NULL, AV_LOG_WARNING,
			"Warning: SPS NALU missing or invalid. "
			"The resulting stream may not play.\n");

	if (!pps_seen)
		av_log(NULL, AV_LOG_WARNING,
			"Warning: PPS NALU missing or invalid. "
			"The resulting stream may not play.\n");

	out_extradata->data = out;
	out_extradata->size = total_size;

	return length_size;
}

// For extractVideo

// 参数1：AVFormatContext上下文；参数2：读取的编码状态下的帧；参数3：目标文件
int h264_mp4toannexb(AVFormatContext *fmt_ctx, AVPacket *in, FILE *dst_fd) {

	AVPacket *out = NULL;
	AVPacket spspps_pkt;

	int len;
	uint8_t unit_type;
	int32_t nal_size;
	uint32_t cumul_size = 0;
	const uint8_t *buf;
	const uint8_t *buf_end;
	int            buf_size;
	int ret = 0, i;

	out = av_packet_alloc();

	buf = in->data;
	buf_size = in->size;
	buf_end = in->data + in->size;

	do {
		ret = AVERROR(EINVAL);
		if (buf + 4 /*s->length_size*/ > buf_end)
			goto fail;

		for (nal_size = 0, i = 0; i < 4/*s->length_size*/; i++)
			nal_size = (nal_size << 8) | buf[i];

		buf += 4; /*s->length_size;*/
		unit_type = *buf & 0x1f;

		if (nal_size > buf_end - buf || nal_size < 0)
			goto fail;

		/*
		if (unit_type == 7)
			s->idr_sps_seen = s->new_idr = 1;
		else if (unit_type == 8) {
			s->idr_pps_seen = s->new_idr = 1;
			*/
			/* if SPS has not been seen yet, prepend the AVCC one to PPS */
			/*
			if (!s->idr_sps_seen) {
				if (s->sps_offset == -1)
					av_log(ctx, AV_LOG_WARNING, "SPS not present in the stream, nor in AVCC, stream may be unreadable\n");
				else {
					if ((ret = alloc_and_copy(out,
										 ctx->par_out->extradata + s->sps_offset,
										 s->pps_offset != -1 ? s->pps_offset : ctx->par_out->extradata_size - s->sps_offset,
										 buf, nal_size)) < 0)
						goto fail;
					s->idr_sps_seen = 1;
					goto next_nal;
				}
			}
		}
		*/

		/* if this is a new IDR picture following an IDR picture, reset the idr flag.
		 * Just check first_mb_in_slice to be 0 as this is the simplest solution.
		 * This could be checking idr_pic_id instead, but would complexify the parsing. */
		 /*
		 if (!s->new_idr && unit_type == 5 && (buf[1] & 0x80))
			 s->new_idr = 1;

		 */
		 /* prepend only to the first type 5 NAL unit of an IDR picture, if no sps/pps are already present */
		if (/*s->new_idr && */unit_type == 5 /*&& !s->idr_sps_seen && !s->idr_pps_seen*/) {

			h264_extradata_to_annexb(fmt_ctx->streams[in->stream_index]->codec->extradata,
				fmt_ctx->streams[in->stream_index]->codec->extradata_size, &spspps_pkt, AV_INPUT_BUFFER_PADDING_SIZE);

			if ((ret = alloc_and_copy(out, spspps_pkt.data, spspps_pkt.size, buf, nal_size)) < 0)
				goto fail;
			/*s->new_idr = 0;*/
		/* if only SPS has been seen, also insert PPS */
		}
		/*else if (s->new_idr && unit_type == 5 && s->idr_sps_seen && !s->idr_pps_seen) {
			if (s->pps_offset == -1) {
				av_log(ctx, AV_LOG_WARNING, "PPS not present in the stream, nor in AVCC, stream may be unreadable\n");
				if ((ret = alloc_and_copy(out, NULL, 0, buf, nal_size)) < 0)
					goto fail;
			} else if ((ret = alloc_and_copy(out,
										ctx->par_out->extradata + s->pps_offset, ctx->par_out->extradata_size - s->pps_offset,
										buf, nal_size)) < 0)
				goto fail;
		}*/ else {
			if ((ret = alloc_and_copy(out, NULL, 0, buf, nal_size)) < 0)
				goto fail;
			/*
			if (!s->new_idr && unit_type == 1) {
				s->new_idr = 1;
				s->idr_sps_seen = 0;
				s->idr_pps_seen = 0;
			}
			*/
		}

		len = fwrite(out->data, 1, out->size, dst_fd);
		if (len != out->size) {
			av_log(NULL, AV_LOG_DEBUG, "warning, length of writed data isn't equal pkt.size(%d, %d)\n",
				len,
				out->size);
		}
		fflush(dst_fd);

	next_nal:
		buf += nal_size;
		cumul_size += nal_size + 4;//s->length_size;
	} while (cumul_size < buf_size);

	/*
	ret = av_packet_copy_props(out, in);
	if (ret < 0)
		goto fail;

	*/
fail:
	av_packet_free(&out);

	return ret;
}

// 自定义日志输出
void my_logoutput(void* ptr, int level, const char* fmt, va_list vl) {
	printf("FFmepeg Log = %s", fmt);
}

// FFmpeg 删除文件操作
void ffmpegDelFile() {
	int ret;
	ret = avpriv_io_delete("1.txt");  // 在项目目录下创建的文件（测试时需要创建好）
	printf("Del File Code : %d \n", ret);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to delete file \n");
	} else {
		av_log(NULL, AV_LOG_INFO, "Delete File Success！\n ");
	}
}

// FFmpeg 重命名或移动文件
void ffmpegMoveFile(char* src, char* dst) {
	int ret;

	ret = avpriv_io_move(src, dst);

	printf("Move File Code : %d \n", ret);

	// 重命名时，如果文件不存在，ret也会0
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to Move File %s!\n ", src);
	} else {
		av_log(NULL, AV_LOG_INFO, "Success Move File %s!\n", src);
	}
}

// FFmpeg 目录操作
void ffmpegDir() {

	int ret;
	
	// 上下文
	AVIODirContext *dirCtx = NULL;
	AVIODirEntry *dirEntry = NULL;
	
	// 注意Windows下会返回-40，也就是Function not implement，方法未实现，也就是说windows下不支持此方法
	ret = avio_open_dir(&dirCtx, "./", NULL);  

	if (ret < 0) {
		// 输出错误日志
		printf("cant open dir，msg = %s", av_err2str(ret));
		return;
	}

	av_log(NULL, AV_LOG_INFO, "Open Dir Success!");

	while (1){
		ret = avio_read_dir(dirCtx, &dirEntry);
		if (ret < 0) {
			printf("cant read dir : %s", av_err2str(ret));
			// 防止内存泄漏
			goto __failed;
		}
		av_log(NULL, AV_LOG_INFO, "read dir success");
		if (!dirEntry) {
			break;
		}
		printf("Entry Name = %s", dirEntry->name);
		// 释放资源
		avio_free_directory_entry(&dirEntry);
	}
// 释放资源
__failed:
	avio_close_dir(&dirCtx);
}

// 使用FFmpeg打印多媒体文件的Meta信息
void ffmpegVideoMeta() {

	av_log_set_level(AV_LOG_INFO);

	AVFormatContext *fmt_ctx = NULL;

	av_register_all();

	int ret;
	// 参数为 AVFormatContext上下文、文件名、指定的输入格式（一般为NULL，由ffmpeg自行解析）、附加参数（一般为NULL）
	ret = avformat_open_input(&fmt_ctx, "111.mp4", NULL, NULL);

	if (ret < 0) {
		printf("Cant open File: %s\n", av_err2str(ret));
	}

	// 参数为AVFormatContext上下文、流索引值（一般不用关心，直接写0）、文件名、是否是输入出文件（1：是  0：不是）	
	av_dump_format(fmt_ctx, 0, "111.mp4", 0);

	// 关闭打开的多媒体文件
	avformat_close_input(&fmt_ctx);
}

// 使用FFmpeg从视频中抽取音频
void extractAudio_HE_ACC() {
	/************************************************************
	 * 目前只能抽取HE-AAC格式的MP4文件(LC及其他格式抽取后不能播放)。
	 * 
	 * TODO：研究HE-AAC和LC-AAC的区别，整理一下AAC的格式相关的知识
	 ************************************************************/

	// 设置日志输出等级
	av_log_set_level(AV_LOG_INFO); 

	AVFormatContext *fmt_ctx = NULL;
	AVPacket pkt;

	av_register_all();

	int ret;
	int len;
	int audio_index = -1;

	// 打开输入文件
	ret = avformat_open_input(&fmt_ctx, "111.mp4", NULL, NULL);

	// 检查打开输入文件是否成功
	if (ret < 0) {
		printf("cant open file，error message = %s", av_err2str(ret));
		return;
	}

	// 打开输出文件
	FILE* dst_fd = fopen("111.aac", "wb");  // w 写入  b 二进制文件

	// 检查输出文件打开是否成功，如果失败，就输出日志，并关闭输出文件的引用
	if (!dst_fd) {
		av_log(NULL, AV_LOG_ERROR, "Can't Open Out File!\n");
		avformat_close_input(&fmt_ctx);
	}

	// 获取到音频流(使用av_find_best_stream：多媒体文件中拿到想使用的最好的一路流）

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	
	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0); 
	for (int i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_index = i;
			break;
		}
 	}

	printf("Audio Stream Index = %d", audio_index);
	
	// 检查发现音频流的结果
	if (audio_index < 0) {
		av_log(NULL, AV_LOG_ERROR, "Can't find Best Audio Stream!\n");
		//printf("Reason = %s", av_err2str(ret));
		// 关闭输出文件和输出文件的引用
		avformat_close_input(&fmt_ctx);
		fclose(dst_fd);
		return;
	}

	while (av_read_frame(fmt_ctx, &pkt) >= 0) {
		if (pkt.stream_index == audio_index) {
			printf("Has Read An Audio Packet\n");
			char adts_header_buf[7];
			adts_header(adts_header_buf, pkt.size);
			fwrite(adts_header_buf, 1, 7, dst_fd);
			len = fwrite(pkt.data, 1, pkt.size, dst_fd);
			if (len != pkt.size) {
				av_log(NULL, AV_LOG_WARNING, "Waring! Length of data not equal size of pkt!\n");
			}
			fflush(dst_fd);
		}
		// 将引用基数减一
		av_packet_unref(&pkt);
		//av_free_packet(&pkt);
	}

	// 关闭文件（输入/输出）
	avformat_close_input(&fmt_ctx);
	if (dst_fd) {
		fclose(dst_fd);
	}
}

// 使用FFmpeg的API从视频中抽取音频
void extractAudio() {
	/**
	* 调用 av_guess_format 让ffmpeg帮你找到一个合适的文件格式。
	* 调用 avformat_new_stream 为输出文件创建一个新流。
	* 调用 avio_open 打开新创建的文件。
	* 调用 avformat_write_header 写文件头。
	* 调用 av_interleaved_write_frame 写文件内容。
	* 调用 av_write_trailer 写文件尾。
	* 调用 avio_close 关闭文件。
	**/

	av_log_set_level(AV_LOG_INFO);
	av_register_all();

	FILE *dst_fd = NULL;

	int ret;

	AVFormatContext *fmt_ctx = NULL;  // 输入文件的AVFormatContext上下文
	AVStream *in_stream = NULL;

	AVPacket pkt;

	AVFormatContext *ofmt_ctx = NULL;
	AVOutputFormat *output_fmt = NULL; 
	AVStream *out_stream = NULL;
	
	int audio_stream_index = -1;

	ret = avformat_open_input(&fmt_ctx, "1111.mp4", NULL, NULL);

	// 检查打开输入文件是否成功
	if (ret < 0) {
		printf("cant open file，error message = %s", av_err2str(ret));
		return;
	}

	/*retrieve audio stream*/
	if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_DEBUG, "failed to find stream information!\n");
		return;
	}

	in_stream = fmt_ctx->streams[1];
	
	AVCodecParameters *in_codecpar = in_stream->codecpar;
	if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
		av_log(NULL, AV_LOG_ERROR, "The Codec type is invalid!\n");
		return;
	}

	//out file
	ofmt_ctx = avformat_alloc_context();
	output_fmt = av_guess_format(NULL, "222.aac", NULL);
	if (!output_fmt) {
		av_log(NULL, AV_LOG_DEBUG, "Cloud not guess file format \n");
		return;
	}

	ofmt_ctx->oformat = output_fmt;

	out_stream = avformat_new_stream(ofmt_ctx, NULL);
	if (!out_stream) {
		av_log(NULL, AV_LOG_DEBUG, "Failed to create out stream!\n");
		return;
	}

	if (fmt_ctx->nb_streams < 2) {
		av_log(NULL, AV_LOG_ERROR, "the number of stream is too less!\n");
		return;
	}

	if ((ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar)) < 0) {
		av_log(NULL, AV_LOG_ERROR,"Failed to copy codec parameter, %d(%s)\n");
	}

	out_stream->codecpar->codec_tag = 0;

	if ((ret = avio_open(&ofmt_ctx->pb, "222.aac", AVIO_FLAG_WRITE)) < 0) {
		av_log(NULL, AV_LOG_DEBUG, "Could not open file! \n");
	}

	/*initialize packet*/
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	/*find best audio stream*/
	audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (audio_stream_index < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to find Audio Stream!\n");
	}

	if (avformat_write_header(ofmt_ctx, NULL) < 0) {
		av_log(NULL, AV_LOG_DEBUG, "Error occurred when opening output file");
	}

	/*read frames from media file*/
	while (av_read_frame(fmt_ctx, &pkt) >= 0) {
		if (pkt.stream_index == audio_stream_index) {
			pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
			pkt.dts = pkt.pts;
			pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
			pkt.pos = -1;
			pkt.stream_index = 0;
			av_interleaved_write_frame(ofmt_ctx, &pkt);
			av_packet_unref(&pkt);
		}
	}

	av_write_trailer(ofmt_ctx);

	/*close input media file*/
	avformat_close_input(&fmt_ctx);
	if (dst_fd) {
		fclose(dst_fd);
	}

	avio_close(ofmt_ctx->pb);
	return;


}

// 使用FFmpeg从视频中抽取视频
void extractVideo() {

	// Start Code 特征码
	// PSP PPS(从codec->extradata即在数据空间中存放)

	// 设置日志输出等级
	av_log_set_level(AV_LOG_INFO);

	AVFormatContext *fmt_ctx = NULL;
	AVPacket pkt;

	// 注册所有的格式和编解码器
	av_register_all();

	int ret;
	int len;
	int video_index = -1;

	// 打开输入文件
	ret = avformat_open_input(&fmt_ctx, "111.mp4", NULL, NULL);

	// 检查打开输入文件是否成功
	if (ret < 0) {
		printf("cant open file，error message = %s", av_err2str(ret));
		return;
	}

	// 打开输出文件
	FILE* dst_fd = fopen("111.H264", "wb");  // w 写入  b 二进制文件

	// 检查输出文件打开是否成功，如果失败，就输出日志，并关闭输出文件的引用
	if (!dst_fd) {
		av_log(NULL, AV_LOG_ERROR, "Can't Open Out File!\n");
		avformat_close_input(&fmt_ctx);
	}

	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	for (int i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_index = i;
			break;
		}
	}

	printf("Video Stream Index = %d", video_index);

	// 检查发现视频流的结果
	if (video_index < 0) {
		av_log(NULL, AV_LOG_ERROR, "Can't find Best Video Stream!\n");
		printf("Reason = %s", av_err2str(ret));
		// 关闭输出文件和输出文件的引用
		avformat_close_input(&fmt_ctx);
		fclose(dst_fd);
		return;
	}

	while (av_read_frame(fmt_ctx, &pkt) >= 0) {
		if (pkt.stream_index == video_index) {
			// TODO 核心操作，思路待理顺
			h264_mp4toannexb(fmt_ctx, &pkt, dst_fd);
		}
		// 将引用基数减一
		av_packet_unref(&pkt);
		//av_free_packet(&pkt);
	}

	// 关闭文件（输入/输出）
	avformat_close_input(&fmt_ctx);
	if (dst_fd) {
		fclose(dst_fd);
	}
}


// 使用FFmpeg将MP4转成FLV
void ffmpegMP42FLV() {

	// 核心API

	// avformat_alloc_output_context2() / avformat_free_context
	// avformat_new_stream  
	// avcodec_parameters_copy  
	// avformat_write_header
	// av_write_frame / av_interleaved_write_frame
	// av_write_trailer

	AVOutputFormat *ofmt = NULL;
	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	AVPacket pkt;
	const char *in_filename, *out_filename;
	int ret, i;
	int stream_index = 0;
	int *stream_mapping = NULL;
	int stream_mapping_size = 0;

	av_register_all();

	if ((ret = avformat_open_input(&ifmt_ctx, "111.mp4", 0, 0)) < 0) {
		goto end;
	}

	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
		goto end;
	}

	av_dump_format(ifmt_ctx, 0, "111.mp4", 0);

	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, "111.flv");

	if (!ofmt_ctx) {
		fprintf(stderr, "Could not create output context\n");
		ret = AVERROR_UNKNOWN;
		goto end;
	}

	stream_mapping_size = ifmt_ctx->nb_streams;
	stream_mapping = (int*) av_mallocz_array(stream_mapping_size, sizeof(*stream_mapping));
	if (!stream_mapping) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	ofmt = ofmt_ctx->oformat;

	for (i = 0; i < ifmt_ctx->nb_streams; i++) {
		AVStream *out_stream;
		AVStream *in_stream = ifmt_ctx->streams[i];
		AVCodecParameters *in_codecpar = in_stream->codecpar;

		if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
			in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
			in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
			stream_mapping[i] = -1;
			continue;
		}

		stream_mapping[i] = stream_index++;

		out_stream = avformat_new_stream(ofmt_ctx, NULL);
		if (!out_stream) {
			fprintf(stderr, "Failed allocating output stream\n");
			ret = AVERROR_UNKNOWN;
			goto end;
		}

		ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
		if (ret < 0) {
			fprintf(stderr, "Failed to copy codec parameters\n");
			goto end;
		}
		out_stream->codecpar->codec_tag = 0;
	}
	av_dump_format(ofmt_ctx, 0, "111.flv", 1);

	if (!(ofmt->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, "111.flv", AVIO_FLAG_WRITE);
		if (ret < 0) {
			goto end;
		}
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Error occurred when opening output file\n");
		goto end;
	}

	while (1) {
		AVStream *in_stream, *out_stream;

		ret = av_read_frame(ifmt_ctx, &pkt);
		if (ret < 0)
			break;

		in_stream = ifmt_ctx->streams[pkt.stream_index];
		if (pkt.stream_index >= stream_mapping_size ||
			stream_mapping[pkt.stream_index] < 0) {
			av_packet_unref(&pkt);
			continue;
		}

		pkt.stream_index = stream_mapping[pkt.stream_index];
		out_stream = ofmt_ctx->streams[pkt.stream_index];

		/* copy packet */
		pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base,(AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base,(AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;

		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
		if (ret < 0) {
			fprintf(stderr, "Error muxing packet\n");
			break;
		}
		av_packet_unref(&pkt);
	}

	av_write_trailer(ofmt_ctx);
end:

	avformat_close_input(&ifmt_ctx);

	/* close output */
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);

	av_freep(&stream_mapping);

	if (ret < 0 && ret != AVERROR_EOF) {
		fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
		return;
	}
}


// 使用FFmpeg剪辑视频
void ffmpegCutVideo() {

	// 定义剪切的秒数区间
	double from_seconds = 10;  // 剪切的起始时间
	double end_seconds = 59;   // 剪切的终止时间

	// 输入输出的格式上下文
	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;

	// 输出格式
	AVOutputFormat *ofmt = NULL;
	
	AVPacket pkt;

	// 返回值，接受各阶段的数据返回值
	int ret;

	av_register_all();

	if ((ret = avformat_open_input(&ifmt_ctx, "111.mp4", 0, 0)) < 0) {
		fprintf(stderr, "Could not open input file!\n");
		return;
	}

	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
		fprintf(stderr, "Failed to retrieve input stream information");
		return;
	}

	av_dump_format(ifmt_ctx, 0, "111.mp4", 0);

	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, "222.mp4");
	if (!ofmt_ctx) {
		fprintf(stderr, "Could not create output context\n");
		ret = AVERROR_UNKNOWN;
		return;
	}

	ofmt = ofmt_ctx->oformat;

	for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
		AVStream *in_stream = ifmt_ctx->streams[i];
		AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
		if (!out_stream) {
			fprintf(stderr, "Failed allocating output stream\n");
			ret = AVERROR_UNKNOWN;
			return;
		}

		ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
		if (ret < 0) {
			fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
			return;
		}
		out_stream->codec->codec_tag = 0;
		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	av_dump_format(ofmt_ctx, 0, "222.mp4", 1);

	if (!(ofmt->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, "222.mp4", AVIO_FLAG_WRITE);
		if (ret < 0) {
			fprintf(stderr, "Could not open output file '%s'", "222.mp4");
			return;
		}
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Error occurred when opening output file\n");
		return;
	}
	
	ret = av_seek_frame(ifmt_ctx, -1, from_seconds*AV_TIME_BASE, AVSEEK_FLAG_ANY);
	if (ret < 0) {
		fprintf(stderr, "Error seek\n");
		return;
	}

	int64_t *dts_start_from = (int64_t *) malloc(sizeof(int64_t) * ifmt_ctx->nb_streams);
	memset(dts_start_from, 0, sizeof(int64_t) * ifmt_ctx->nb_streams);
	int64_t *pts_start_from = (int64_t *) malloc(sizeof(int64_t) * ifmt_ctx->nb_streams);
	memset(pts_start_from, 0, sizeof(int64_t) * ifmt_ctx->nb_streams);

	while (1) {
		AVStream *in_stream, *out_stream;

		ret = av_read_frame(ifmt_ctx, &pkt);
		if (ret < 0)
			break;

		in_stream = ifmt_ctx->streams[pkt.stream_index];
		out_stream = ofmt_ctx->streams[pkt.stream_index];

		if (av_q2d(in_stream->time_base) * pkt.pts > end_seconds) {
			av_free_packet(&pkt);
			break;
		}

		if (dts_start_from[pkt.stream_index] == 0) {
			dts_start_from[pkt.stream_index] = pkt.dts;
		}
		if (pts_start_from[pkt.stream_index] == 0) {
			pts_start_from[pkt.stream_index] = pkt.pts;
		}

		/* copy packet */
		pkt.pts = av_rescale_q_rnd(pkt.pts - pts_start_from[pkt.stream_index], in_stream->time_base, out_stream->time_base, (AVRounding) ( AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		pkt.dts = av_rescale_q_rnd(pkt.dts - dts_start_from[pkt.stream_index], in_stream->time_base, out_stream->time_base, (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		if (pkt.pts < 0) {
			pkt.pts = 0;
		}
		if (pkt.dts < 0) {
			pkt.dts = 0;
		}
		pkt.duration = (int)av_rescale_q((int64_t)pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;

		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
		if (ret < 0) {
			fprintf(stderr, "Error muxing packet\n");
			break;
		}
		av_free_packet(&pkt);
	}
	free(dts_start_from);
	free(pts_start_from);

	av_write_trailer(ofmt_ctx);

	// 关闭释放相关资源
	avformat_close_input(&ifmt_ctx);

	/* close output */
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);

	if (ret < 0 && ret != AVERROR_EOF) {
		fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
		return;
	}
	return;
}

int main(int argc, char* argv[]) {
	
	/** 0.FFmpeg Hello World **/
	//av_register_all();
	//printf("%s\n", avcodec_configuration());

	/** 1.FFmpeg Log System **/
	//av_log_set_level(AV_LOG_INFO);

	/** 2.设置自定义的日志输出方法 **/
	//av_log_set_callback(my_logoutput);  
	//av_log(NULL, AV_LOG_INFO, "Hello World\n");

	/** 3.使用FFmpeg删除文件 **/
	//ffmpegDelFile();  // 删除文件

	/** 4.使用FFmpeg重命名文件 **/
	//char src[] = "111.txt";
	//char dst[] = "222.txt";
	//ffmpegMoveFile(src, dst);  //  重命名文件

	/** 5.使用FFmpeg操作文件目录（注:此操作可能在Windows下面不支持） **/
	//ffmpegDir();  // 文件目录操作

	/** 6.使用FFmpeg操作文件目录使用FFmpeg打印多媒体的Mate信息 **/
	//ffmpegVideoMeta(); // FFmpeg打印多媒体文件的Meta信息

	/** 7.使用FFmpeg从视频中抽取音频 **/
	//extractAudio_HE_ACC(); // (仅支持音频为HE-ACC格式的MP4)
	//extractAudio();  // 不限格式，使用FFmpeg写入头

	/** 8.使用FFmpeg从视频中抽取视频 **/
	//extractVideo();

	/** 9.使用FFmpeg将视频从MP4转封装为FLV格式 **/
	//ffmpegMP42FLV();

	/** 10.使用FFmpeg剪切视频 **/
	ffmpegCutVideo();

	/** 至此基本的单元的操作内容基本完成，后续要学习的是更深一层的知识 */

	/** 未来可能需要更进一步学习的是，这10个Demo涉及到的知识点，然后再往后继续学习 */

	return 0;
}



// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门提示: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
