#include <cassert>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <sys/stat.h>
#include <cmath>
#include <time.h>
#include <cuda_runtime_api.h>
#include <memory>
#include <cstring>
#include <algorithm>

#include "NvCaffeParser.h"
#include "NvInferPlugin.h"
#include "common.h"
#include "factoryFasterRCNN.h"

//add by me

//#include <cuda.h>
#include "NvDecoder.h"

#include "NvCodecUtils.h"

#include "FFmpegDemuxer.h"



static Logger gLogger;
using namespace nvinfer1;
using namespace nvcaffeparser1;
using namespace plugin;

// stuff we know about the network and the caffe input/output blobs
static const int INPUT_C = 3;
static const int INPUT_H = 375;
static const int INPUT_W = 500;
static const int IM_INFO_SIZE = 3;
static const int OUTPUT_CLS_SIZE = 21;
static const int OUTPUT_BBOX_SIZE = OUTPUT_CLS_SIZE * 4;
static int gUseDLACore{-1};

const std::string CLASSES[OUTPUT_CLS_SIZE]{"background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "diningtable", "dog", "horse", "motorbike", "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor"};

const char* INPUT_BLOB_NAME0 = "data";
const char* INPUT_BLOB_NAME1 = "im_info";
const char* OUTPUT_BLOB_NAME0 = "bbox_pred";
const char* OUTPUT_BLOB_NAME1 = "cls_prob";
const char* OUTPUT_BLOB_NAME2 = "rois";

struct PPM
{
    std::string magic, fileName;
    int h, w, max;
    uint8_t buffer[INPUT_C * INPUT_H * INPUT_W];
};

struct BBox
{
    float x1, y1, x2, y2;
};

std::string locateFile(const std::string& input)
{
    std::vector<std::string> dirs{"data/samples/faster-rcnn/", "data/faster-rcnn/"};
    return locateFile(input, dirs);
}

// simple PPM (portable pixel map) reader
void readPPMFile(const std::string& filename, PPM& ppm)
{
    ppm.fileName = filename;
    std::ifstream infile(locateFile(filename), std::ifstream::binary);
    infile >> ppm.magic >> ppm.w >> ppm.h >> ppm.max;
    infile.seekg(1, infile.cur);
    infile.read(reinterpret_cast<char*>(ppm.buffer), ppm.w * ppm.h * 3);
}

void writePPMFileWithBBox(const std::string& filename, PPM& ppm, const BBox& bbox)
{
    std::ofstream outfile("./" + filename, std::ofstream::binary);
    assert(!outfile.fail());
    outfile << "P6"
            << "\n"
            << ppm.w << " " << ppm.h << "\n"
            << ppm.max << "\n";
    auto round = [](float x) -> int { return int(std::floor(x + 0.5f)); };
    for (int x = int(bbox.x1); x < int(bbox.x2); ++x)
    {
        // bbox top border
        ppm.buffer[(round(bbox.y1) * ppm.w + x) * 3] = 255;
        ppm.buffer[(round(bbox.y1) * ppm.w + x) * 3 + 1] = 0;
        ppm.buffer[(round(bbox.y1) * ppm.w + x) * 3 + 2] = 0;
        // bbox bottom border
        ppm.buffer[(round(bbox.y2) * ppm.w + x) * 3] = 255;
        ppm.buffer[(round(bbox.y2) * ppm.w + x) * 3 + 1] = 0;
        ppm.buffer[(round(bbox.y2) * ppm.w + x) * 3 + 2] = 0;
    }
    for (int y = int(bbox.y1); y < int(bbox.y2); ++y)
    {
        // bbox left border
        ppm.buffer[(y * ppm.w + round(bbox.x1)) * 3] = 255;
        ppm.buffer[(y * ppm.w + round(bbox.x1)) * 3 + 1] = 0;
        ppm.buffer[(y * ppm.w + round(bbox.x1)) * 3 + 2] = 0;
        // bbox right border
        ppm.buffer[(y * ppm.w + round(bbox.x2)) * 3] = 255;
        ppm.buffer[(y * ppm.w + round(bbox.x2)) * 3 + 1] = 0;
        ppm.buffer[(y * ppm.w + round(bbox.x2)) * 3 + 2] = 0;
    }
    outfile.write(reinterpret_cast<char*>(ppm.buffer), ppm.w * ppm.h * 3);
}

void caffeToTRTModel(const std::string& deployFile,                   // name for caffe prototxt
                     const std::string& modelFile,                    // name for model
                     const std::vector<std::string>& outputs,         // network outputs
                     unsigned int maxBatchSize,                       // batch size - NB must be at least as large as the batch we want to run with)
                     nvcaffeparser1::IPluginFactoryV2* pluginFactory, // factory for plugin layers
                     IHostMemory** trtModelStream)                    // output stream for the TensorRT model
{
    // create the builder
    IBuilder* builder = createInferBuilder(gLogger);

    // parse the caffe model to populate the network, then set the outputs
    INetworkDefinition* network = builder->createNetwork();
    ICaffeParser* parser = createCaffeParser();
    parser->setPluginFactoryV2(pluginFactory);

    std::cout << "Begin parsing model..." << std::endl;
    const IBlobNameToTensor* blobNameToTensor = parser->parse(locateFile(deployFile).c_str(),
                                                              locateFile(modelFile).c_str(),
                                                              *network,
                                                              DataType::kFLOAT);
    std::cout << "End parsing model..." << std::endl;
    // specify which tensors are outputs
    for (auto& s : outputs)
        network->markOutput(*blobNameToTensor->find(s.c_str()));

    // Build the engine
    builder->setMaxBatchSize(maxBatchSize);
    builder->setMaxWorkspaceSize(10 << 20); // we need about 6MB of scratch space for the plugin layer for batch size 5

    samplesCommon::enableDLA(builder, gUseDLACore);

    std::cout << "Begin building engine..." << std::endl;
    ICudaEngine* engine = builder->buildCudaEngine(*network);
    assert(engine);
    std::cout << "End building engine..." << std::endl;

    // we don't need the network any more, and we can destroy the parser
    network->destroy();
    parser->destroy();

    // serialize the engine, then close everything down
    (*trtModelStream) = engine->serialize();

    engine->destroy();
    builder->destroy();
    shutdownProtobufLibrary();
}

void doInference(IExecutionContext& context, float* inputData, float* inputImInfo, float* outputBboxPred, float* outputClsProb, float* outputRois, int batchSize)
{
    const ICudaEngine& engine = context.getEngine();
    // input and output buffer pointers that we pass to the engine - the engine requires exactly IEngine::getNbBindings(),
    // of these, but in this case we know that there is exactly 2 inputs and 3 outputs.
    assert(engine.getNbBindings() == 5);
    void* buffers[5];

    // In order to bind the buffers, we need to know the names of the input and output tensors.
    // note that indices are guaranteed to be less than IEngine::getNbBindings()
    int inputIndex0 = engine.getBindingIndex(INPUT_BLOB_NAME0),
        inputIndex1 = engine.getBindingIndex(INPUT_BLOB_NAME1),
        outputIndex0 = engine.getBindingIndex(OUTPUT_BLOB_NAME0),
        outputIndex1 = engine.getBindingIndex(OUTPUT_BLOB_NAME1),
        outputIndex2 = engine.getBindingIndex(OUTPUT_BLOB_NAME2);

    // create GPU buffers and a stream
    CHECK(cudaMalloc(&buffers[inputIndex0], batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float)));   // data
    CHECK(cudaMalloc(&buffers[inputIndex1], batchSize * IM_INFO_SIZE * sizeof(float)));                  // im_info
    CHECK(cudaMalloc(&buffers[outputIndex0], batchSize * nmsMaxOut * OUTPUT_BBOX_SIZE * sizeof(float))); // bbox_pred
    CHECK(cudaMalloc(&buffers[outputIndex1], batchSize * nmsMaxOut * OUTPUT_CLS_SIZE * sizeof(float)));  // cls_prob
    CHECK(cudaMalloc(&buffers[outputIndex2], batchSize * nmsMaxOut * 4 * sizeof(float)));                // rois

    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));

    // DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
    CHECK(cudaMemcpyAsync(buffers[inputIndex0], inputData, batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
    CHECK(cudaMemcpyAsync(buffers[inputIndex1], inputImInfo, batchSize * IM_INFO_SIZE * sizeof(float), cudaMemcpyHostToDevice, stream));
    context.enqueue(batchSize, buffers, stream, nullptr);
    CHECK(cudaMemcpyAsync(outputBboxPred, buffers[outputIndex0], batchSize * nmsMaxOut * OUTPUT_BBOX_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(outputClsProb, buffers[outputIndex1], batchSize * nmsMaxOut * OUTPUT_CLS_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(outputRois, buffers[outputIndex2], batchSize * nmsMaxOut * 4 * sizeof(float), cudaMemcpyDeviceToHost, stream));
    cudaStreamSynchronize(stream);

    // release the stream and the buffers
    cudaStreamDestroy(stream);
    CHECK(cudaFree(buffers[inputIndex0]));
    CHECK(cudaFree(buffers[inputIndex1]));
    CHECK(cudaFree(buffers[outputIndex0]));
    CHECK(cudaFree(buffers[outputIndex1]));
    CHECK(cudaFree(buffers[outputIndex2]));
}

void bboxTransformInvAndClip(float* rois, float* deltas, float* predBBoxes, float* imInfo,
                             const int N, const int nmsMaxOut, const int numCls)
{
    float width, height, ctr_x, ctr_y;
    float dx, dy, dw, dh, pred_ctr_x, pred_ctr_y, pred_w, pred_h;
    float *deltas_offset, *predBBoxes_offset, *imInfo_offset;
    for (int i = 0; i < N * nmsMaxOut; ++i)
    {
        width = rois[i * 4 + 2] - rois[i * 4] + 1;
        height = rois[i * 4 + 3] - rois[i * 4 + 1] + 1;
        ctr_x = rois[i * 4] + 0.5f * width;
        ctr_y = rois[i * 4 + 1] + 0.5f * height;
        deltas_offset = deltas + i * numCls * 4;
        predBBoxes_offset = predBBoxes + i * numCls * 4;
        imInfo_offset = imInfo + i / nmsMaxOut * 3;
        for (int j = 0; j < numCls; ++j)
        {
            dx = deltas_offset[j * 4];
            dy = deltas_offset[j * 4 + 1];
            dw = deltas_offset[j * 4 + 2];
            dh = deltas_offset[j * 4 + 3];
            pred_ctr_x = dx * width + ctr_x;
            pred_ctr_y = dy * height + ctr_y;
            pred_w = exp(dw) * width;
            pred_h = exp(dh) * height;
            predBBoxes_offset[j * 4] = max(min(pred_ctr_x - 0.5f * pred_w, imInfo_offset[1] - 1.f), 0.f);
            predBBoxes_offset[j * 4 + 1] = max(min(pred_ctr_y - 0.5f * pred_h, imInfo_offset[0] - 1.f), 0.f);
            predBBoxes_offset[j * 4 + 2] = max(min(pred_ctr_x + 0.5f * pred_w, imInfo_offset[1] - 1.f), 0.f);
            predBBoxes_offset[j * 4 + 3] = max(min(pred_ctr_y + 0.5f * pred_h, imInfo_offset[0] - 1.f), 0.f);
        }
    }
}

std::vector<int> nms(std::vector<std::pair<float, int>>& score_index, float* bbox, const int classNum, const int numClasses, const float nms_threshold)
{
    auto overlap1D = [](float x1min, float x1max, float x2min, float x2max) -> float {
        if (x1min > x2min)
        {
            std::swap(x1min, x2min);
            std::swap(x1max, x2max);
        }
        return x1max < x2min ? 0 : min(x1max, x2max) - x2min;
    };
    auto computeIoU = [&overlap1D](float* bbox1, float* bbox2) -> float {
        float overlapX = overlap1D(bbox1[0], bbox1[2], bbox2[0], bbox2[2]);
        float overlapY = overlap1D(bbox1[1], bbox1[3], bbox2[1], bbox2[3]);
        float area1 = (bbox1[2] - bbox1[0]) * (bbox1[3] - bbox1[1]);
        float area2 = (bbox2[2] - bbox2[0]) * (bbox2[3] - bbox2[1]);
        float overlap2D = overlapX * overlapY;
        float u = area1 + area2 - overlap2D;
        return u == 0 ? 0 : overlap2D / u;
    };

    std::vector<int> indices;
    for (auto i : score_index)
    {
        const int idx = i.second;
        bool keep = true;
        for (unsigned k = 0; k < indices.size(); ++k)
        {
            if (keep)
            {
                const int kept_idx = indices[k];
                float overlap = computeIoU(&bbox[(idx * numClasses + classNum) * 4],
                                           &bbox[(kept_idx * numClasses + classNum) * 4]);
                keep = overlap <= nms_threshold;
            }
            else
                break;
        }
        if (keep)
            indices.push_back(idx);
    }
    return indices;
}

int main(int argc, char** argv)
{
    gUseDLACore = samplesCommon::parseDLA(argc, argv);
    // create a TensorRT model from the caffe model and serialize it to a stream

    FRCNNPluginFactory pluginFactorySerialize;
    IHostMemory* trtModelStream{nullptr};
    initLibNvInferPlugins(&gLogger, "");

    // batch size
    const int N = 2;
    caffeToTRTModel("faster_rcnn_test_iplugin.prototxt",
                    "VGG16_faster_rcnn_final.caffemodel",
                    std::vector<std::string>{OUTPUT_BLOB_NAME0, OUTPUT_BLOB_NAME1, OUTPUT_BLOB_NAME2},
                    N, &pluginFactorySerialize, &trtModelStream);
    assert(trtModelStream != nullptr);
    pluginFactorySerialize.destroyPlugin();

    // read a random sample image
    srand(unsigned(time(nullptr)));
    // available images
    std::vector<std::string> imageList = {"000456.ppm", "000542.ppm", "001150.ppm", "001763.ppm", "004545.ppm"};
    std::vector<PPM> ppms(N);

    float imInfo[N * 3]; // input im_info
    std::random_shuffle(imageList.begin(), imageList.end(), [](int i) { return rand() % i; });
    assert(ppms.size() <= imageList.size());
    for (int i = 0; i < N; ++i)
    {
        readPPMFile(imageList[i], ppms[i]);
        imInfo[i * 3] = float(ppms[i].h);     // number of rows
        imInfo[i * 3 + 1] = float(ppms[i].w); // number of columns
        imInfo[i * 3 + 2] = 1;                // image scale
    }

    float* data = new float[N * INPUT_C * INPUT_H * INPUT_W];
    // pixel mean used by the Faster R-CNN's author
    float pixelMean[3]{102.9801f, 115.9465f, 122.7717f}; // also in BGR order
    for (int i = 0, volImg = INPUT_C * INPUT_H * INPUT_W; i < N; ++i)
    {
        for (int c = 0; c < INPUT_C; ++c)
        {
            // the color image to input should be in BGR order
            for (unsigned j = 0, volChl = INPUT_H * INPUT_W; j < volChl; ++j)
                data[i * volImg + c * volChl + j] = float(ppms[i].buffer[j * INPUT_C + 2 - c]) - pixelMean[c];
        }
    }

    // deserialize the engine
    IRuntime* runtime = createInferRuntime(gLogger);
    assert(runtime != nullptr);
    if (gUseDLACore >= 0)
    {
        runtime->setDLACore(gUseDLACore);
    }
    FRCNNPluginFactory pluginFactory;
    ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream->data(), trtModelStream->size(), nullptr);
    assert(engine != nullptr);
    trtModelStream->destroy();
    IExecutionContext* context = engine->createExecutionContext();
    assert(context != nullptr);

    // host memory for outputs
    float* rois = new float[N * nmsMaxOut * 4];
    float* bboxPreds = new float[N * nmsMaxOut * OUTPUT_BBOX_SIZE];
    float* clsProbs = new float[N * nmsMaxOut * OUTPUT_CLS_SIZE];

    // predicted bounding boxes
    float* predBBoxes = new float[N * nmsMaxOut * OUTPUT_BBOX_SIZE];

#if 1
	//add by me-------
	CUdevice cuDevice = 0;
	CUcontext cuContext = NULL;
	cuCtxCreate(&cuContext, 0, cuDevice);
	bool bOutPlanar = false;
	Rect cropRect = {};
	Dim resizeDim = {};
	char szInFilePath[256] = "data/test.mp4";
	FFmpegDemuxer demuxer(szInFilePath);
	NvDecoder dec(cuContext, demuxer.GetWidth(), demuxer.GetHeight(), true, FFmpeg2NvCodecId(demuxer.GetVideoCodec()));// , NULL, false, false, &cropRect, &resizeDim);
	int nVideoBytes = 0, nFrameReturned = 0, nFrame = 0;
	uint8_t *pVideo = NULL, **ppFrame;


	int src_height_ = demuxer.GetHeight();
	int src_width_ = demuxer.GetWidth();
	uint8_t* frame_;
	cudaMalloc((void **)&frame_, INPUT_H * INPUT_W * 4);
	//because only need bgr channels, new three channels mem.cudamemcpy three channels
	uint8_t* planner_ = (uint8_t*)malloc(INPUT_W * INPUT_H * 3*sizeof(uint8_t));
	do {
		demuxer.Demux(&pVideo, &nVideoBytes);
		dec.Decode(pVideo, nVideoBytes, &ppFrame, &nFrameReturned);
		if (!nFrame && nFrameReturned)
			LOG(INFO) << dec.GetVideoInfo();

		for (int i = 0; i < nFrameReturned; i++) {
			ResizeNv12(frame_, INPUT_W, INPUT_W, INPUT_H,
				ppFrame[i], src_width_, src_width_, src_height_, nullptr);
			Nv12ToBgrPlanar(ppFrame[i], INPUT_W, frame_, INPUT_W, INPUT_W, INPUT_H);
			cudaMemcpy(planner_, frame_, INPUT_W * INPUT_H * 3, cudaMemcpyDeviceToHost);
			//ConvertToPlanar(ppFrame[i], dec.GetWidth(), dec.GetHeight(), dec.GetBitDepth());

			// run inference
			doInference(*context, data, imInfo, bboxPreds, clsProbs, rois, N);
			//fpOut.write(reinterpret_cast<char*>(ppFrame[i]), dec.GetFrameSize());
		}
		nFrame += nFrameReturned;
	} while (nVideoBytes);
	cudaFree(frame_);
	free(planner_);
#else
	// run inference
	doInference(*context, data, imInfo, bboxPreds, clsProbs, rois, N);
#endif
    // Destroy the engine
    context->destroy();
    engine->destroy();
    runtime->destroy();

    // unscale back to raw image space
    for (int i = 0; i < N; ++i)
    {
        float* rois_offset = rois + i * nmsMaxOut * 4;
        for (int j = 0; j < nmsMaxOut * 4 && imInfo[i * 3 + 2] != 1; ++j)
            rois_offset[j] /= imInfo[i * 3 + 2];
    }

    bboxTransformInvAndClip(rois, bboxPreds, predBBoxes, imInfo, N, nmsMaxOut, OUTPUT_CLS_SIZE);

    const float nms_threshold = 0.3f;
    const float score_threshold = 0.8f;

    for (int i = 0; i < N; ++i)
    {
        float* bbox = predBBoxes + i * nmsMaxOut * OUTPUT_BBOX_SIZE;
        float* scores = clsProbs + i * nmsMaxOut * OUTPUT_CLS_SIZE;
        for (int c = 1; c < OUTPUT_CLS_SIZE; ++c) // skip the background
        {
            std::vector<std::pair<float, int>> score_index;
            for (int r = 0; r < nmsMaxOut; ++r)
            {
                if (scores[r * OUTPUT_CLS_SIZE + c] > score_threshold)
                {
                    score_index.push_back(std::make_pair(scores[r * OUTPUT_CLS_SIZE + c], r));
                    std::stable_sort(score_index.begin(), score_index.end(),
                                     [](const std::pair<float, int>& pair1,
                                        const std::pair<float, int>& pair2) {
                                         return pair1.first > pair2.first;
                                     });
                }
            }

            // apply NMS algorithm
            std::vector<int> indices = nms(score_index, bbox, c, OUTPUT_CLS_SIZE, nms_threshold);
            // Show results
            for (unsigned k = 0; k < indices.size(); ++k)
            {
                int idx = indices[k];
                std::string storeName = CLASSES[c] + "-" + std::to_string(scores[idx * OUTPUT_CLS_SIZE + c]) + ".ppm";
                std::cout << "Detected " << CLASSES[c] << " in " << ppms[i].fileName << " with confidence " << scores[idx * OUTPUT_CLS_SIZE + c] * 100.0f << "% "
                          << " (Result stored in " << storeName << ")." << std::endl;

                BBox b{bbox[idx * OUTPUT_BBOX_SIZE + c * 4], bbox[idx * OUTPUT_BBOX_SIZE + c * 4 + 1], bbox[idx * OUTPUT_BBOX_SIZE + c * 4 + 2], bbox[idx * OUTPUT_BBOX_SIZE + c * 4 + 3]};
                writePPMFileWithBBox(storeName, ppms[i], b);
            }
        }
    }

    delete[] data;
    delete[] rois;
    delete[] bboxPreds;
    delete[] clsProbs;
    delete[] predBBoxes;
    return 0;
}
