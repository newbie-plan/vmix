#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/frame.h>


#define MAX_STREAM_NUM 2
#define FILTERS_DESCR "[in0][in1]xstack=inputs=2:layout=0_0|0_h0[out]"


typedef struct Parameter
{
    int input_stream_count;
    char *input_file[MAX_STREAM_NUM];
    int input_width[MAX_STREAM_NUM];
    int input_height[MAX_STREAM_NUM];
    char *output_file;
    int output_width;
    int output_height;
}Parameter;


typedef struct Graph
{
    AVFilterGraph *filter_graph;

    AVFilter *buffersrc[MAX_STREAM_NUM];
    AVFilterContext *buffersrc_ctx[MAX_STREAM_NUM];
    AVFilterInOut *outputs[MAX_STREAM_NUM];

    AVFilter *buffersink;
    AVFilterContext *buffersink_ctx;
    AVFilterInOut *inputs;
}Graph;


static int init_filters(const char *filters_descr, Graph *graph, Parameter *param)
{
    int i = 0;
    int ret = -1;
    AVFilterInOut *outputs = NULL;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    for (i = 0; i < param->input_stream_count; i++)
    {
        graph->buffersrc[i] = avfilter_get_by_name("buffer");
        graph->outputs[i] = avfilter_inout_alloc();
        if (graph->buffersrc[i] == NULL || graph->outputs[i] == NULL)
        {
            printf("init buffersrc failed.\n");
            exit(0);
        }
    }

    graph->buffersink = avfilter_get_by_name("buffersink");
    graph->inputs = avfilter_inout_alloc();
    if (graph->buffersink == NULL || graph->inputs == NULL)
    {
        printf("init buffersink failed.\n");
        exit(0);
    }

    graph->filter_graph = avfilter_graph_alloc();
    if (graph->filter_graph == NULL)
    {
        printf("avfilter_graph_alloc() failed.\n");
        exit(0);
    }


    for (i = 0; i < param->input_stream_count; i++)
    {
        char name[16] = {0};
        char args[512] = {0};
        snprintf(name, sizeof(name), "in%d", i);
        snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
                 param->input_width[i], param->input_height[i], AV_PIX_FMT_YUV420P, 1, 90000);
        printf("args = [%s]\n", args);

        ret = avfilter_graph_create_filter(&graph->buffersrc_ctx[i], graph->buffersrc[i], name, args, NULL, graph->filter_graph);
        if (ret < 0)
        {
            printf("avfilter_graph_create_filter() abuffersrc[%d] failed : [%s]\n", i, av_err2str(ret));
            exit(0);
        }

        graph->outputs[i]->name       = av_strdup(name);
        graph->outputs[i]->filter_ctx = graph->buffersrc_ctx[i];
        graph->outputs[i]->pad_idx    = 0;
        graph->outputs[i]->next       = NULL;
        if (outputs == NULL)    outputs = graph->outputs[i];
        else                    outputs->next = graph->outputs[i];
    }

    ret = avfilter_graph_create_filter(&graph->buffersink_ctx, graph->buffersink, "out", NULL, NULL, graph->filter_graph);
    if (ret < 0)
    {
        printf("avfilter_graph_create_filter() failed : [%s]\n", av_err2str(ret));
        exit(0);
    }

    ret = av_opt_set_int_list(graph->buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0)
    {
        printf("av_opt_set_int_list() pix_fmts failed : [%s]\n", av_err2str(ret));
        exit(0);
    }


    graph->inputs->name       = av_strdup("out");
    graph->inputs->filter_ctx = graph->buffersink_ctx;
    graph->inputs->pad_idx    = 0;
    graph->inputs->next       = NULL;


    ret = avfilter_graph_parse_ptr(graph->filter_graph, filters_descr, &graph->inputs, &outputs, NULL);
    if (ret < 0)
    {
        printf("avfilter_graph_parse_ptr() failed : [%s]\n", av_err2str(ret));
        exit(0);
    }
    ret = avfilter_graph_config(graph->filter_graph, NULL);
    if (ret < 0)
    {
        printf("avfilter_graph_config() failed : [%s]\n", av_err2str(ret));
        exit(0);
    }


    for (i = 0; i < graph->filter_graph->nb_filters; i++)
    {
        printf("[%d] --> filter name = [%s]\n", i, graph->filter_graph->filters[i]->name);
    }

    return 0;
}


static void parse_options(int argc, const char *argv[], Parameter *param)
{
    int optc = -1;
    int opt_index = -1;
    int width = 0;
    int height = 0;
    
    while ((optc = getopt(argc, (char *const *)argv, "i:o:s:")) != -1)
    {
        switch (optc)
        {
            case 'i':
            {
                param->input_file[param->input_stream_count] = optarg;
                param->input_width[param->input_stream_count] = width;
                param->input_height[param->input_stream_count] = height;
                param->input_stream_count++;

                width = 0;
                height = 0;
                break;
            }
            case 'o':
            {
                param->output_file = optarg;
                param->output_width = width;
                param->output_height = height;

                width = 0;
                height = 0;
                break;
            }
            case 's':
            {
                sscanf(optarg, "%dx%d", &width, &height);
                break;
            }
            case '?':
            default:
            {
                printf("error...\n");
                exit(0);
            }
        }
    }
}


int main(int argc, const char *argv[])
{
    int i = 0, ret = -1, eof = 0;
    FILE *fp_o = NULL;
    FILE *fp[MAX_STREAM_NUM] = {0};
    AVFrame *filt_frame = NULL;
    AVFrame *frame[MAX_STREAM_NUM] = {0};
    Parameter param;
    Graph graph;
    memset(&graph, 0, sizeof(Graph));
    memset(&param, 0, sizeof(Parameter));


    parse_options(argc, argv, &param);
    if (param.input_stream_count > MAX_STREAM_NUM)
    {
        printf("only surport [%d] stream mixer\n", MAX_STREAM_NUM);
        return -1;
    }
    printf("input_file[%d] = [%s] : input_width[%d] = [%d] : input_height[%d] = [%d]\n\n",
                    0, param.input_file[0], 0, param.input_width[0], 0, param.input_height[0]);
    printf("input_file[%d] = [%s] : input_width[%d] = [%d] : input_height[%d] = [%d]\n\n",
                    1, param.input_file[1], 1, param.input_width[1], 1, param.input_height[1]);
    printf("output_file = [%s] : output_width = [%d] : output_height = [%d]\n\n",
                    param.output_file, param.output_width, param.output_height);


    for (i = 0; i < param.input_stream_count; i++)
    {
        fp[i] = fopen(param.input_file[i], "rb");
        if (fp[i] == NULL)
        {
            printf("fopen [%s] failed.\n", param.input_file[i]);
            return -1;
        }

        frame[i] = av_frame_alloc();
        if (frame[i] == NULL)
        {
            printf("av_frame_alloc() failed.\n");
            return -1;
        }
        frame[i]->width = param.input_width[i];
        frame[i]->height = param.input_height[i];
        frame[i]->format = AV_PIX_FMT_YUV420P;

        ret = av_frame_get_buffer(frame[i], 0);
        if (ret < 0)
        {
            printf("av_frame_get_buffer() failed : [%s]\n", av_err2str(ret));
            return -1;
        }
    }
    fp_o = fopen(param.output_file, "wb");
    if (fp_o == NULL)
    {
        printf("fopen [%s] failed.\n", param.output_file);
        return -1;
    }
    filt_frame = av_frame_alloc();
    if (filt_frame == NULL)
    {
        printf("av_frame_alloc() failed.\n");
        return -1;
    }


    init_filters(FILTERS_DESCR, &graph, &param);

    while (1)
    {
        for (i = 0; i < param.input_stream_count; i++)
        {
            int size = param.input_width[i] * param.input_height[i];
            if (feof(fp[i]) == 0)
            {
                fread(frame[i]->data[0], 1, size, fp[i]);  /*后续需要考虑planar*/
                fread(frame[i]->data[1], 1, size/4, fp[i]);  /*后续需要考虑planar*/
                fread(frame[i]->data[2], 1, size/4, fp[i]);  /*后续需要考虑planar*/

                ret = av_buffersrc_add_frame_flags(graph.buffersrc_ctx[i], frame[i], AV_BUFFERSRC_FLAG_KEEP_REF);
                if (ret < 0)
                {
                    printf("av_buffersrc_add_frame_flags() failed : [%s]\n", av_err2str(ret));
                    return -1;
                }
            }
            else
            {
                eof++;          /*mix结束*/
            }
        }


        while (1)
        {
            ret = av_buffersink_get_frame(graph.buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)   break;
            if (ret < 0)
            {
                printf("av_buffersink_get_frame() failed : [%s]\n", av_err2str(ret));
                return -1;
            }

            for (i = 0; i < filt_frame->height; i++)
            {
                fwrite(filt_frame->data[0]+filt_frame->linesize[0]*i, 1, filt_frame->width, fp_o);
            }
            for (i = 0; i < filt_frame->height/2; i++)
            {
                fwrite(filt_frame->data[1]+filt_frame->linesize[1]*i, 1, filt_frame->width/2, fp_o);
            }
            for (i = 0; i < filt_frame->height/2; i++)
            {
                fwrite(filt_frame->data[2]+filt_frame->linesize[2]*i, 1, filt_frame->width/2, fp_o);
            }

            av_frame_unref(filt_frame);
        }

        if (eof > 0)    break;  /*结束应该以长的音频流为准,要改*/
    }


    return 0;
}



