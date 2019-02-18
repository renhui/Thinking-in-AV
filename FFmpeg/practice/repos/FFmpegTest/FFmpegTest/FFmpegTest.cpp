// FFmpegTest.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "pch.h"
#include <iostream>
#include "AACFormat.h"

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

extern "C"{
#include <stdio.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/log.h"
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
void extractAudio() {

	/******************************************************
	 * TODO：目前存在抽取后不能播放的问题
	 * 推测1：Header 不对，建议研究AAC后尝试调试
	 * 推测2：转码的MP4的ACC格式不对，也需要研究ACC
	 * 感觉代码写的没问题，此段代码非常需要调好，但是需要学习
	 * 的知识也很多，慢慢来吧。
	 ******************************************************/

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

	// 打开输入文件成功，打印一下Meta信息
	// av_dump_format(fmt_ctx, 0, "111.flv", 0);


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

int main(int argc, char* argv[]) {
	
	/** 0.FFmpeg Hello World **/
	av_register_all();
	printf("%s\n", avcodec_configuration());

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
	//extractAudio();


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
